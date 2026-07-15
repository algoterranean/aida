#include "Recipes/AIDARecipeService.h"

#include "AIDA.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "ItemAmount.h"
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
		OutBuildings.Add(MoveTemp(Info));

		if (UClass* Buildable = UFGBuildingDescriptor::GetBuildableClass(Descriptor).Get())
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
