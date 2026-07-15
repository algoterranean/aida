#include "Recipes/AIDARecipeService.h"

#include "AIDA.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FGClearanceInterface.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "ItemAmount.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableGeneratorFuel.h"
#include "Buildables/FGBuildablePipeline.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Resources/FGItemDescriptor.h"

namespace
{
	/** Display name for an item/building descriptor, falling back to the class name if it has none. */
	FString ItemName(TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		if (!ItemClass) { return FString(); }
		const FString Name = UFGItemDescriptor::GetItemName(ItemClass).ToString();
		return Name.IsEmpty() ? GetNameSafe(ItemClass.Get()) : Name;
	}

	/** Per-craft amount, humanized: solids as-is, fluids/gases cm³ → m³ (matches the factory index). */
	double CraftAmount(const FItemAmount& Entry)
	{
		if (!Entry.ItemClass) { return 0.0; }
		double Amount = static_cast<double>(Entry.Amount);
		const EResourceForm Form = UFGItemDescriptor::GetForm(Entry.ItemClass);
		if (Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS) { Amount /= 1000.0; }
		return Amount;
	}

	bool IsBuildingDescriptor(TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		return ItemClass && ItemClass->IsChildOf(UFGBuildingDescriptor::StaticClass());
	}

	/** Read a protected float UPROPERTY off a CDO by name (no public getter). 0 when absent. */
	double ReflectedFloat(const UObject* CDO, const TCHAR* PropertyName)
	{
		if (!CDO) { return 0.0; }
		if (const FFloatProperty* Prop = FindFProperty<FFloatProperty>(CDO->GetClass(), PropertyName))
		{
			return static_cast<double>(Prop->GetPropertyValue_InContainer(CDO));
		}
		return 0.0;
	}

	bool ReflectedBool(const UObject* CDO, const TCHAR* PropertyName)
	{
		if (!CDO) { return false; }
		if (const FBoolProperty* Prop = FindFProperty<FBoolProperty>(CDO->GetClass(), PropertyName))
		{
			return Prop->GetPropertyValue_InContainer(CDO);
		}
		return false;
	}

	/**
	 * Fill the prompt-pack fields (docs/PROMPT.md §2) from the buildable CDO: clearance footprint,
	 * clock/power exponent, logistics throughputs, and generator fuel burn — everything the model
	 * needs for ratio/power/layout math without a lookup round-trip. Named uniquely (unity build
	 * merges anonymous namespaces across Private/*.cpp — ResolveFootprint already exists in Actions).
	 */
	void FillPackFields(const AFGBuildable* CDO, FAIDABuildingInfo& Info)
	{
		if (!CDO) { return; }

		// Footprint/height from the union of the CDO's clearance boxes (same source the build gun uses).
		TArray<FFGClearanceData> Clearances;
		IFGClearanceInterface::Execute_GetClearanceData(const_cast<AFGBuildable*>(CDO), Clearances);
		FBox Union(ForceInit);
		for (const FFGClearanceData& Data : Clearances)
		{
			if (Data.IsValid()) { Union += Data.GetTransformedClearanceBox(); }
		}
		if (Union.IsValid)
		{
			const FVector Size = Union.GetSize();
			if (Size.X > 1.0) { Info.FootprintXM = Size.X / 100.0; }
			if (Size.Y > 1.0) { Info.FootprintYM = Size.Y / 100.0; }
			if (Size.Z > 1.0) { Info.HeightM = Size.Z / 100.0; }
		}

		// Clock scaling: power = base × (clock/100)^exponent. The exponent is a protected UPROPERTY
		// on AFGBuildableFactory with no getter → reflection. Only meaningful for powered buildings.
		if (CDO->IsA<AFGBuildableFactory>() && (Info.PowerConsumptionMW > 0.0 || Info.bVariablePower))
		{
			Info.PowerExponent = ReflectedFloat(CDO, TEXT("mPowerConsumptionExponent"));
		}

		// Logistics tiers. Belt mSpeed is cm/s with 120 cm item spacing → items/min = speed/2.
		if (const AFGBuildableConveyorBase* Belt = Cast<AFGBuildableConveyorBase>(CDO))
		{
			Info.BeltItemsPerMin = Belt->GetSpeed() / 2.0;
		}
		else if (const AFGBuildablePipeline* Pipe = Cast<AFGBuildablePipeline>(CDO))
		{
			Info.PipeM3PerMin = Pipe->GetFlowLimit() * 60.0; // m³/s → m³/min
		}
		else if (const AFGBuildableResourceExtractor* Extractor = Cast<AFGBuildableResourceExtractor>(CDO))
		{
			const double CycleTime = Extractor->GetDefaultExtractCycleTime();
			if (CycleTime > 0.0)
			{
				double PerMin = Extractor->GetNumExtractedItemsPerCycle() * 60.0 / CycleTime;
				// Fluid extractors count cm³ per cycle (≥1000/cycle); solids extract single-digit items.
				if (Extractor->GetNumExtractedItemsPerCycle() >= 1000) { PerMin /= 1000.0; }
				Info.ExtractPerMinNormal = PerMin;
			}
		}
		else if (const AFGBuildableGeneratorFuel* Generator = Cast<AFGBuildableGeneratorFuel>(CDO))
		{
			const double MW = Info.PowerProductionMW;
			for (const TSoftClassPtr<UFGItemDescriptor>& FuelSoft : Generator->GetDefaultFuelClasses())
			{
				const TSubclassOf<UFGItemDescriptor> Fuel = FuelSoft.LoadSynchronous();
				if (!Fuel) { continue; }
				const double EnergyMJ = UFGItemDescriptor::GetEnergyValue(Fuel);
				if (EnergyMJ <= 0.0 || MW <= 0.0) { continue; }
				double BurnPerMin = MW * 60.0 / EnergyMJ; // MJ/min ÷ MJ/item
				const EResourceForm Form = UFGItemDescriptor::GetForm(Fuel);
				if (Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS)
				{
					BurnPerMin /= 1000.0; // fluid energy is per litre → m³/min
				}
				Info.Fuels.Add({ ItemName(Fuel), BurnPerMin });
			}
			// Supplemental (water) intake: ratio is litres per MJ produced (protected, no getter).
			if (ReflectedBool(CDO, TEXT("mRequiresSupplementalResource")))
			{
				const double Ratio = ReflectedFloat(CDO, TEXT("mSupplementalToPowerRatio"));
				Info.SupplementalM3PerMin = MW * 60.0 * Ratio / 1000.0;
			}
		}
	}
}

void FAIDARecipeCatalog::ExtractInto(UObject* WorldContext, TArray<FAIDARecipeInfo>& OutRecipes, TArray<FAIDABuildingInfo>& OutBuildings)
{
	OutRecipes.Reset();
	OutBuildings.Reset();

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	AFGRecipeManager* Recipes = World ? AFGRecipeManager::Get(World) : nullptr;
	if (!Recipes)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[recipes] no recipe manager for catalog extraction."));
		return;
	}

	TArray<TSubclassOf<UFGRecipe>> Available;
	Recipes->GetAllAvailableRecipes(Available);

	// Pass A: build recipes (products are building descriptors) → the building catalog + a
	// buildable-class → name map so manufacturing recipes can name the machines they run in.
	TMap<UClass*, FString> BuildableToName;
	for (const TSubclassOf<UFGRecipe>& Recipe : Available)
	{
		if (!Recipe) { continue; }
		const TArray<FItemAmount> Products = UFGRecipe::GetProducts(Recipe);
		if (Products.Num() == 0 || !IsBuildingDescriptor(Products[0].ItemClass)) { continue; }

		const TSubclassOf<UFGBuildingDescriptor> Descriptor = Products[0].ItemClass.Get();
		FAIDABuildingInfo Info;
		Info.Name = ItemName(Products[0].ItemClass);
		Info.bVariablePower = UFGBuildingDescriptor::HasVariablePowerConsumption(Descriptor);
		Info.PowerConsumptionMW = Info.bVariablePower ? 0.0 : UFGBuildingDescriptor::GetPowerConsumption(Descriptor);
		Info.MinPowerMW = UFGBuildingDescriptor::GetMinimumPowerConsumption(Descriptor);
		Info.MaxPowerMW = UFGBuildingDescriptor::GetMaximumPowerConsumption(Descriptor);
		Info.PowerProductionMW = UFGBuildingDescriptor::GetPowerProduction(Descriptor);

		UClass* Buildable = UFGBuildingDescriptor::GetBuildableClass(Descriptor).Get();
		FillPackFields(Buildable ? Buildable->GetDefaultObject<AFGBuildable>() : nullptr, Info);
		OutBuildings.Add(MoveTemp(Info));

		if (Buildable)
		{
			BuildableToName.Add(Buildable, OutBuildings.Last().Name);
		}
	}

	// Pass B: manufacturing recipes (everything that isn't a build recipe) → the recipe catalog.
	for (const TSubclassOf<UFGRecipe>& Recipe : Available)
	{
		if (!Recipe) { continue; }
		const TArray<FItemAmount> Products = UFGRecipe::GetProducts(Recipe);
		if (Products.Num() > 0 && IsBuildingDescriptor(Products[0].ItemClass)) { continue; } // build recipe → skipped

		FAIDARecipeInfo Info;
		Info.RecipeName = UFGRecipe::GetRecipeName(Recipe).ToString();
		Info.DurationSeconds = UFGRecipe::GetManufacturingDuration(Recipe);

		for (const FItemAmount& Product : Products)
		{
			if (Product.ItemClass) { Info.Products.Add({ ItemName(Product.ItemClass), CraftAmount(Product) }); }
		}
		for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(WorldContext, Recipe))
		{
			if (Ingredient.ItemClass) { Info.Ingredients.Add({ ItemName(Ingredient.ItemClass), CraftAmount(Ingredient) }); }
		}
		for (const TSubclassOf<UObject>& Producer : UFGRecipe::GetProducedIn(Recipe))
		{
			if (UClass* Cls = Producer.Get())
			{
				if (const FString* Named = BuildableToName.Find(Cls)) { Info.ProducedIn.AddUnique(*Named); }
			}
		}

		if (Info.Products.Num() > 0) { OutRecipes.Add(MoveTemp(Info)); }
	}

	UE_LOG(LogAIDA, Log, TEXT("[recipes] catalog: %d recipes, %d buildings."), OutRecipes.Num(), OutBuildings.Num());
}

void FAIDARecipeCatalog::EnsureFresh(UObject* WorldContext, double NowSeconds, double TtlSeconds)
{
	if (!bValid || (NowSeconds - LastExtractSeconds) >= TtlSeconds)
	{
		ExtractInto(WorldContext, CachedRecipes, CachedBuildings);
		LastExtractSeconds = NowSeconds;
		bValid = true;
	}
}

const TArray<FAIDARecipeInfo>& FAIDARecipeCatalog::GetRecipes(UObject* WorldContext, double NowSeconds, double TtlSeconds)
{
	EnsureFresh(WorldContext, NowSeconds, TtlSeconds);
	return CachedRecipes;
}

const TArray<FAIDABuildingInfo>& FAIDARecipeCatalog::GetBuildings(UObject* WorldContext, double NowSeconds, double TtlSeconds)
{
	EnsureFresh(WorldContext, NowSeconds, TtlSeconds);
	return CachedBuildings;
}
