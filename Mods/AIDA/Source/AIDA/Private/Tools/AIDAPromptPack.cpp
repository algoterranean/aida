#include "Tools/AIDAPromptPack.h"

#include "Recipes/AIDARecipeModel.h"

namespace
{
	/** Compact number: up to 2 decimals, trailing zeros stripped ("15", "7.14", "0.5"). */
	FString PackNum(double Value)
	{
		FString S = FString::Printf(TEXT("%.2f"), Value);
		while (S.EndsWith(TEXT("0"))) { S.LeftChopInline(1); }
		if (S.EndsWith(TEXT("."))) { S.LeftChopInline(1); }
		return S;
	}

	/** "10x15x11 m" from the clearance footprint; empty when unknown. */
	FString PackFootprint(const FAIDABuildingInfo& B)
	{
		if (B.FootprintXM <= 0.0 || B.FootprintYM <= 0.0) { return FString(); }
		FString S = PackNum(B.FootprintXM) + TEXT("x") + PackNum(B.FootprintYM);
		if (B.HeightM > 0.0) { S += TEXT("x") + PackNum(B.HeightM); }
		return S + TEXT(" m");
	}

	/** "Iron Ingot 30 + Water 20" with per-craft amounts converted to per-minute. */
	FString PackAmounts(const TArray<FAIDAItemAmount>& Items, double DurationSeconds)
	{
		TArray<FString> Parts;
		for (const FAIDAItemAmount& Item : Items)
		{
			const double PerMin = DurationSeconds > 0.0 ? Item.Amount * 60.0 / DurationSeconds : Item.Amount;
			Parts.Add(FString::Printf(TEXT("%s %s"), *Item.Item, *PackNum(PerMin)));
		}
		return FString::Join(Parts, TEXT(" + "));
	}

	void AppendSection(FString& Out, const TCHAR* Title, const TArray<FString>& Lines)
	{
		if (Lines.Num() == 0) { return; }
		Out += FString::Printf(TEXT("\n## %s\n"), Title);
		Out += FString::Join(Lines, TEXT("\n"));
		Out += TEXT("\n");
	}
}

FString AIDAPromptPack::Build(const TArray<FAIDARecipeInfo>& Recipes, const TArray<FAIDABuildingInfo>& Buildings)
{
	TArray<FAIDABuildingInfo> SortedBuildings = Buildings;
	SortedBuildings.Sort([](const FAIDABuildingInfo& A, const FAIDABuildingInfo& B) { return A.Name < B.Name; });
	TArray<FAIDARecipeInfo> SortedRecipes = Recipes;
	SortedRecipes.Sort([](const FAIDARecipeInfo& A, const FAIDARecipeInfo& B) { return A.RecipeName < B.RecipeName; });

	TArray<FString> Producers, Generators, Logistics, Extractors, Structures, RecipeLines;

	for (const FAIDABuildingInfo& B : SortedBuildings)
	{
		if (B.Name.IsEmpty()) { continue; }
		const FString Footprint = PackFootprint(B);

		if (B.PowerProductionMW > 0.0)
		{
			FString Line = FString::Printf(TEXT("%s: %s MW out"), *B.Name, *PackNum(B.PowerProductionMW));
			if (!Footprint.IsEmpty()) { Line += TEXT(", ") + Footprint; }
			if (B.Fuels.Num() > 0)
			{
				TArray<FString> FuelParts;
				for (const FAIDAFuelInfo& Fuel : B.Fuels)
				{
					FuelParts.Add(FString::Printf(TEXT("%s %s"), *Fuel.Item, *PackNum(Fuel.BurnPerMin)));
				}
				Line += TEXT("; burns/min: ") + FString::Join(FuelParts, TEXT(" or "));
			}
			if (B.SupplementalM3PerMin > 0.0)
			{
				Line += FString::Printf(TEXT("; water %s m3/min"), *PackNum(B.SupplementalM3PerMin));
			}
			Generators.Add(MoveTemp(Line));
		}
		else if (B.BeltItemsPerMin > 0.0)
		{
			Logistics.Add(FString::Printf(TEXT("%s: %s items/min"), *B.Name, *PackNum(B.BeltItemsPerMin)));
		}
		else if (B.PipeM3PerMin > 0.0)
		{
			Logistics.Add(FString::Printf(TEXT("%s: %s m3/min"), *B.Name, *PackNum(B.PipeM3PerMin)));
		}
		else if (B.ExtractPerMinNormal > 0.0)
		{
			FString Line = FString::Printf(TEXT("%s: %s/min on a normal node"), *B.Name, *PackNum(B.ExtractPerMinNormal));
			if (B.PowerConsumptionMW > 0.0) { Line += FString::Printf(TEXT(", %s MW"), *PackNum(B.PowerConsumptionMW)); }
			if (!Footprint.IsEmpty()) { Line += TEXT(", ") + Footprint; }
			Extractors.Add(MoveTemp(Line));
		}
		else if (B.PowerConsumptionMW > 0.0 || B.bVariablePower)
		{
			FString Line = B.bVariablePower
				? FString::Printf(TEXT("%s: %s-%s MW (variable)"), *B.Name, *PackNum(B.MinPowerMW), *PackNum(B.MaxPowerMW))
				: FString::Printf(TEXT("%s: %s MW"), *B.Name, *PackNum(B.PowerConsumptionMW));
			if (B.PowerExponent > 0.0) { Line += FString::Printf(TEXT(", exp %s"), *PackNum(B.PowerExponent)); }
			if (!Footprint.IsEmpty()) { Line += TEXT(", ") + Footprint; }
			Producers.Add(MoveTemp(Line));
		}
		else if (!Footprint.IsEmpty())
		{
			Structures.Add(FString::Printf(TEXT("%s: %s"), *B.Name, *Footprint));
		}
	}

	for (const FAIDARecipeInfo& Recipe : SortedRecipes)
	{
		if (Recipe.RecipeName.IsEmpty() || Recipe.Products.Num() == 0) { continue; }
		const FString MadeIn = Recipe.ProducedIn.Num() > 0 ? FString::Join(Recipe.ProducedIn, TEXT("/")) : TEXT("manual");
		RecipeLines.Add(FString::Printf(TEXT("%s [%s, %ss]: %s -> %s"),
			*Recipe.RecipeName, *MadeIn, *PackNum(Recipe.DurationSeconds),
			Recipe.Ingredients.Num() > 0 ? *PackAmounts(Recipe.Ingredients, Recipe.DurationSeconds) : TEXT("nothing"),
			*PackAmounts(Recipe.Products, Recipe.DurationSeconds)));
	}

	FString Out;
	Out += TEXT("# GAME DATA PACK (generated from THIS save — unlocked content only; authoritative over anything you remember about Satisfactory)\n");
	Out += TEXT("All rates are per minute at 100% clock. Fluids are m3. Footprints are width x depth x height in metres — build spacing comes from THESE numbers, never from the name (a 'Foundation (2 m)' tile is 8x8 m; 2 m is its thickness). Recipes named 'Alternate: ...' are unlocked alternates.\n");

	AppendSection(Out, TEXT("Production buildings (power draw, clock exponent, footprint)"), Producers);
	AppendSection(Out, TEXT("Power generators"), Generators);
	AppendSection(Out, TEXT("Belts, lifts & pipes (max throughput)"), Logistics);
	AppendSection(Out, TEXT("Miners & extractors (multiply by 0.5 on impure nodes, 2 on pure)"), Extractors);
	AppendSection(Out, TEXT("Structures (footprints for layout math)"), Structures);

	Out += TEXT("\n## Clock speed rules\n");
	Out += TEXT("Production building power = base MW x (clock/100)^exp (exp listed above; underclocking saves power superlinearly, e.g. exp 1.32 at 50% clock -> 40% power). Generator fuel burn scales linearly with clock. Clocking above 100% requires power shards; below 100% is always free.\n");

	AppendSection(Out, TEXT("Recipes (inputs -> outputs, per minute at 100% clock)"), RecipeLines);
	return Out;
}
