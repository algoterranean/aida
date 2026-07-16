#include "Recipes/AIDAFactoryPlanner.h"

#include "Tools/AIDAToolJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// Fallback power law when a building's extracted exponent is unknown (all vanilla producers use log2(2.5)).
	constexpr double DefaultPowerExponent = 1.321928;
	// Worklist cap: vanilla chains are < 20 items deep; only a cyclic alternate graph can approach this.
	constexpr int32 MaxDemandPops = 4096;
	constexpr int32 MaxStepsInJson = 30;
	constexpr int32 MaxAlternatesPerStep = 3;

	FString LowerKey(const FString& S) { return S.ToLower(); }

	bool IsAlternateRecipe(const FAIDARecipeInfo& R) { return R.RecipeName.StartsWith(TEXT("Alternate")); }

	/** The product line for Item inside a recipe (null when the recipe doesn't make it). */
	const FAIDAItemAmount* ProductLine(const FAIDARecipeInfo& R, const FString& ItemKey)
	{
		return R.Products.FindByPredicate([&ItemKey](const FAIDAItemAmount& P) { return LowerKey(P.Item) == ItemKey; });
	}

	/**
	 * Smallest transport that carries Rate: belts for solids, pipes for fluids, rates straight from
	 * the catalog. When even the fastest lane is too slow, report the lane count ("2× ..."). Empty
	 * when the catalog has no belt/pipe entries (they unlock with the first conveyor milestone).
	 */
	FString PickTransport(const TArray<FAIDABuildingInfo>& Buildings, double Rate, bool bFluid)
	{
		const FAIDABuildingInfo* Best = nullptr;    // smallest that fits
		const FAIDABuildingInfo* Fastest = nullptr; // fallback for multi-lane
		for (const FAIDABuildingInfo& B : Buildings)
		{
			const double Cap = bFluid ? B.PipeM3PerMin : B.BeltItemsPerMin;
			if (Cap <= 0.0) { continue; }
			if (!bFluid && !B.Name.Contains(TEXT("Belt"))) { continue; } // lifts share belt rates; name belts
			if (!Fastest || Cap > (bFluid ? Fastest->PipeM3PerMin : Fastest->BeltItemsPerMin)) { Fastest = &B; }
			if (Cap + 1e-9 >= Rate && (!Best || Cap < (bFluid ? Best->PipeM3PerMin : Best->BeltItemsPerMin))) { Best = &B; }
		}
		if (Best) { return Best->Name; }
		if (Fastest)
		{
			const double Cap = bFluid ? Fastest->PipeM3PerMin : Fastest->BeltItemsPerMin;
			const int32 Lanes = static_cast<int32>(FMath::CeilToDouble(Rate / Cap - 1e-9));
			return FString::Printf(TEXT("%d× %s"), Lanes, *Fastest->Name);
		}
		return FString();
	}
}

FAIDAFactoryPlan FAIDAFactoryPlanner::Plan(const FString& Item, double RatePerMin,
	const TArray<FAIDARecipeInfo>& Recipes, const TArray<FAIDABuildingInfo>& Buildings)
{
	FAIDAFactoryPlan Plan;
	Plan.TargetItem = Item;
	Plan.TargetPerMin = RatePerMin;

	if (Item.TrimStartAndEnd().IsEmpty()) { Plan.Error = TEXT("plan_factory needs an 'item'."); return Plan; }
	if (RatePerMin <= 0.0) { Plan.Error = TEXT("'rate_per_min' must be > 0."); return Plan; }

	// Index: item -> recipes producing it, ranked standard-first, then name==item, then lexicographic.
	TMap<FString, TArray<int32>> ProducersOf;
	for (int32 i = 0; i < Recipes.Num(); ++i)
	{
		for (const FAIDAItemAmount& P : Recipes[i].Products)
		{
			if (P.Amount > 0.0) { ProducersOf.FindOrAdd(LowerKey(P.Item)).Add(i); }
		}
	}
	for (TPair<FString, TArray<int32>>& Pair : ProducersOf)
	{
		const FString& ItemKey = Pair.Key;
		Pair.Value.Sort([&Recipes, &ItemKey](int32 A, int32 B)
		{
			const bool AltA = IsAlternateRecipe(Recipes[A]), AltB = IsAlternateRecipe(Recipes[B]);
			if (AltA != AltB) { return !AltA; }
			const bool NameA = LowerKey(Recipes[A].RecipeName) == ItemKey, NameB = LowerKey(Recipes[B].RecipeName) == ItemKey;
			if (NameA != NameB) { return NameA; }
			return Recipes[A].RecipeName < Recipes[B].RecipeName;
		});
	}

	const FString TargetKey = LowerKey(Item);
	if (!ProducersOf.Contains(TargetKey))
	{
		Plan.Error = FString::Printf(TEXT("No unlocked recipe produces '%s'."), *Item);
		return Plan;
	}

	// Building lookup by display name.
	TMap<FString, const FAIDABuildingInfo*> BuildingByName;
	for (const FAIDABuildingInfo& B : Buildings) { BuildingByName.Add(LowerKey(B.Name), &B); }

	// Demand accumulation: pop (item, rate), fold it into the item's step (or raws), push ingredients.
	// Shared intermediates accumulate naturally; a cycle can only spin the worklist, so cap the pops.
	TMap<FString, double> Demand;       // produced items -> total rate
	TMap<FString, double> RawDemand;    // items with no producing recipe
	TMap<FString, bool> RawFluid;
	TMap<FString, FString> DisplayName; // preserve first-seen display casing
	TArray<FString> DiscoveryOrder;     // produced items, target first

	TArray<TPair<FString, double>> Queue;
	Queue.Add({ TargetKey, RatePerMin });
	DisplayName.Add(TargetKey, Item);

	int32 Pops = 0;
	bool bTruncated = false;
	while (Queue.Num() > 0)
	{
		if (++Pops > MaxDemandPops) { bTruncated = true; break; }
		const TPair<FString, double> Entry = Queue[0];
		Queue.RemoveAt(0);
		const FString& Key = Entry.Key;
		const double Rate = Entry.Value;

		const TArray<int32>* Producers = ProducersOf.Find(Key);
		if (!Producers)
		{
			RawDemand.FindOrAdd(Key) += Rate;
			continue;
		}

		if (!Demand.Contains(Key)) { DiscoveryOrder.Add(Key); }
		Demand.FindOrAdd(Key) += Rate;

		const FAIDARecipeInfo& Recipe = Recipes[(*Producers)[0]];
		const FAIDAItemAmount* Primary = ProductLine(Recipe, Key);
		if (!Primary || Primary->Amount <= 0.0 || Recipe.DurationSeconds <= 0.0) { continue; }
		const double Crafts = Rate / Primary->Amount; // crafts/min needed for this slice of demand
		for (const FAIDAItemAmount& Ing : Recipe.Ingredients)
		{
			if (Ing.Amount <= 0.0) { continue; }
			const FString IngKey = LowerKey(Ing.Item);
			if (!DisplayName.Contains(IngKey)) { DisplayName.Add(IngKey, Ing.Item); }
			if (!ProducersOf.Contains(IngKey)) { RawFluid.FindOrAdd(IngKey) = Ing.bFluid; }
			Queue.Add({ IngKey, Crafts * Ing.Amount });
		}
	}
	if (bTruncated)
	{
		Plan.Notes.Add(TEXT("Plan truncated: the recipe graph did not converge (a recipe cycle, e.g. recycled plastic/rubber alternates). Rates below the cycle point are incomplete."));
	}

	// Steps, in discovery order.
	bool bVariablePowerNote = false;
	TMap<FString, double> ByproductRate;
	TMap<FString, bool> ByproductFluid;
	for (const FString& Key : DiscoveryOrder)
	{
		const double Rate = Demand[Key];
		const TArray<int32>& Producers = ProducersOf[Key];
		const FAIDARecipeInfo& Recipe = Recipes[Producers[0]];
		const FAIDAItemAmount* Primary = ProductLine(Recipe, Key);
		if (!Primary || Primary->Amount <= 0.0 || Recipe.DurationSeconds <= 0.0) { continue; }

		FAIDAPlanStep Step;
		Step.Item = DisplayName.Contains(Key) ? DisplayName[Key] : Primary->Item;
		Step.RatePerMin = Rate;
		Step.bFluid = Primary->bFluid;
		Step.Recipe = Recipe.RecipeName;
		for (int32 i = 1; i < Producers.Num() && Step.AlternateRecipes.Num() < MaxAlternatesPerStep; ++i)
		{
			Step.AlternateRecipes.Add(Recipes[Producers[i]].RecipeName);
		}

		const double PerMachine = Primary->Amount * 60.0 / Recipe.DurationSeconds;
		Step.Machines = static_cast<int32>(FMath::CeilToDouble(Rate / PerMachine - 1e-9));
		Step.Clock = Step.Machines > 0 ? Rate / (Step.Machines * PerMachine) : 1.0;

		// Building: prefer a ProducedIn entry we have power data for (skips craft-bench style entries).
		const FAIDABuildingInfo* Info = nullptr;
		for (const FString& Name : Recipe.ProducedIn)
		{
			const FAIDABuildingInfo* const* Found = BuildingByName.Find(LowerKey(Name));
			if (Found && ((*Found)->PowerConsumptionMW > 0.0 || (*Found)->bVariablePower)) { Info = *Found; break; }
			if (Found && !Info) { Info = *Found; }
		}
		Step.Building = Info ? Info->Name : (Recipe.ProducedIn.Num() > 0 ? Recipe.ProducedIn[0] : FString());
		if (Info)
		{
			double BaseMW = Info->PowerConsumptionMW;
			if (Info->bVariablePower)
			{
				BaseMW = (Info->MinPowerMW + Info->MaxPowerMW) * 0.5;
				bVariablePowerNote = true;
			}
			const double Exponent = Info->PowerExponent > 0.0 ? Info->PowerExponent : DefaultPowerExponent;
			Step.PowerMW = BaseMW * Step.Machines * FMath::Pow(Step.Clock, Exponent);
			Step.FootprintXM = Info->FootprintXM;
			Step.FootprintYM = Info->FootprintYM;
		}
		Step.Transport = PickTransport(Buildings, Rate, Step.bFluid);

		// Secondary products of the chosen recipe are byproducts at this step's craft rate.
		const double Crafts = Rate / Primary->Amount;
		for (const FAIDAItemAmount& P : Recipe.Products)
		{
			if (LowerKey(P.Item) == Key || P.Amount <= 0.0) { continue; }
			ByproductRate.FindOrAdd(LowerKey(P.Item)) += Crafts * P.Amount;
			ByproductFluid.FindOrAdd(LowerKey(P.Item)) = P.bFluid;
			if (!DisplayName.Contains(LowerKey(P.Item))) { DisplayName.Add(LowerKey(P.Item), P.Item); }
		}

		Plan.TotalMachines += Step.Machines;
		Plan.TotalPowerMW += Step.PowerMW;
		Plan.Steps.Add(MoveTemp(Step));
	}
	if (bVariablePowerNote)
	{
		Plan.Notes.Add(TEXT("A step runs on variable-power machines; its MW figure is the min/max average."));
	}

	for (const TPair<FString, double>& Pair : RawDemand)
	{
		FAIDAPlanResource Raw;
		Raw.Item = DisplayName.Contains(Pair.Key) ? DisplayName[Pair.Key] : Pair.Key;
		Raw.RatePerMin = Pair.Value;
		Raw.bFluid = RawFluid.Contains(Pair.Key) && RawFluid[Pair.Key];
		Plan.RawInputs.Add(MoveTemp(Raw));
	}
	Plan.RawInputs.Sort([](const FAIDAPlanResource& A, const FAIDAPlanResource& B) { return A.RatePerMin > B.RatePerMin; });

	for (const TPair<FString, double>& Pair : ByproductRate)
	{
		FAIDAPlanResource By;
		By.Item = DisplayName.Contains(Pair.Key) ? DisplayName[Pair.Key] : Pair.Key;
		By.RatePerMin = Pair.Value;
		By.bFluid = ByproductFluid.Contains(Pair.Key) && ByproductFluid[Pair.Key];
		Plan.Byproducts.Add(By);
		if (Demand.Contains(Pair.Key) || RawDemand.Contains(Pair.Key))
		{
			Plan.Notes.Add(FString::Printf(TEXT("%s is both consumed and produced as a byproduct; the plan does not net them, so machine counts upstream of it are an upper bound."), *By.Item));
		}
	}
	Plan.Byproducts.Sort([](const FAIDAPlanResource& A, const FAIDAPlanResource& B) { return A.RatePerMin > B.RatePerMin; });

	return Plan;
}

FString FAIDAFactoryPlanner::BuildPlanJson(const FAIDAFactoryPlan& Plan)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	if (!Plan.Error.IsEmpty())
	{
		Root->SetStringField(TEXT("error"), Plan.Error);
		return AIDAToCompactJson(Root);
	}

	Root->SetStringField(TEXT("target"), Plan.TargetItem);
	Root->SetField(TEXT("perMin"), AIDANumber2(Plan.TargetPerMin));
	Root->SetNumberField(TEXT("totalMachines"), Plan.TotalMachines);
	Root->SetField(TEXT("totalPower_MW"), AIDANumber(Plan.TotalPowerMW));

	TArray<TSharedPtr<FJsonValue>> Steps;
	const int32 Shown = FMath::Min(Plan.Steps.Num(), MaxStepsInJson);
	for (int32 i = 0; i < Shown; ++i)
	{
		const FAIDAPlanStep& S = Plan.Steps[i];
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("item"), S.Item);
		O->SetField(TEXT("perMin"), AIDANumber2(S.RatePerMin));
		if (!S.Recipe.Equals(S.Item, ESearchCase::IgnoreCase)) { O->SetStringField(TEXT("recipe"), S.Recipe); }
		O->SetStringField(TEXT("building"), S.Building);
		O->SetNumberField(TEXT("machines"), S.Machines);
		O->SetField(TEXT("clock_pct"), AIDANumber2(S.Clock * 100.0));
		O->SetField(TEXT("power_MW"), AIDANumber(S.PowerMW));
		if (!S.Transport.IsEmpty()) { O->SetStringField(TEXT("transport"), S.Transport); }
		if (S.FootprintXM > 0.0 && S.FootprintYM > 0.0)
		{
			TArray<TSharedPtr<FJsonValue>> Fp;
			Fp.Add(AIDANumber(S.FootprintXM));
			Fp.Add(AIDANumber(S.FootprintYM));
			O->SetArrayField(TEXT("footprint_m"), Fp);
		}
		if (S.AlternateRecipes.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Alts;
			for (const FString& A : S.AlternateRecipes) { Alts.Add(MakeShared<FJsonValueString>(A)); }
			O->SetArrayField(TEXT("alternates"), Alts);
		}
		Steps.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("steps"), Steps);
	if (Plan.Steps.Num() > Shown) { Root->SetNumberField(TEXT("stepsOmitted"), Plan.Steps.Num() - Shown); }

	const auto ResourceArray = [](const TArray<FAIDAPlanResource>& List)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FAIDAPlanResource& R : List)
		{
			const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("item"), R.Item);
			O->SetField(TEXT("perMin"), AIDANumber2(R.RatePerMin));
			if (R.bFluid) { O->SetBoolField(TEXT("fluid"), true); }
			Arr.Add(MakeShared<FJsonValueObject>(O));
		}
		return Arr;
	};
	Root->SetArrayField(TEXT("rawInputs"), ResourceArray(Plan.RawInputs));
	if (Plan.Byproducts.Num() > 0) { Root->SetArrayField(TEXT("byproducts"), ResourceArray(Plan.Byproducts)); }

	if (Plan.Notes.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Notes;
		for (const FString& N : Plan.Notes) { Notes.Add(MakeShared<FJsonValueString>(N)); }
		Root->SetArrayField(TEXT("notes"), Notes);
	}
	return AIDAToCompactJson(Root);
}
