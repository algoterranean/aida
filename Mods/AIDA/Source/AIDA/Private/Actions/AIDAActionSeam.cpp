#include "Actions/AIDAActionSeam.h"

#include "AIDA.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

#include "FGBuildableSubsystem.h"
#include "FGCentralStorageSubsystem.h"
#include "FGClearanceInterface.h"
#include "FGConstructDisqualifier.h"
#include "FGDismantleInterface.h"
#include "FGInventoryComponent.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "Buildables/FGBuildable.h"
#include "Hologram/FGHologram.h"
#include "ItemAmount.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Resources/FGItemDescriptor.h"

namespace
{
	constexpr int32 MaxSuggestions = 5;

	UWorld* ResolveWorld(UObject* WorldContext)
	{
		return GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	}

	FString DescriptorName(TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		if (!ItemClass) { return FString(); }
		const FString Name = UFGItemDescriptor::GetItemName(ItemClass).ToString();
		return Name.IsEmpty() ? GetNameSafe(ItemClass.Get()) : Name;
	}

	/** One unlocked build-gun recipe: canonical name + classes (the resolve walk's working set). */
	struct FBuildRecipeEntry
	{
		TSubclassOf<UFGRecipe> Recipe;
		TSubclassOf<UFGBuildingDescriptor> Descriptor;
		FString Name;
	};

	void CollectBuildRecipes(UWorld* World, TArray<FBuildRecipeEntry>& Out)
	{
		AFGRecipeManager* Recipes = World ? AFGRecipeManager::Get(World) : nullptr;
		if (!Recipes) { return; }

		TArray<TSubclassOf<UFGRecipe>> Available;
		Recipes->GetAllAvailableRecipes(Available);
		for (const TSubclassOf<UFGRecipe>& Recipe : Available)
		{
			if (!Recipe) { continue; }
			const TArray<FItemAmount> Products = UFGRecipe::GetProducts(Recipe);
			if (Products.Num() == 0 || !Products[0].ItemClass ||
				!Products[0].ItemClass->IsChildOf(UFGBuildingDescriptor::StaticClass()))
			{
				continue;
			}
			Out.Add({ Recipe, TSubclassOf<UFGBuildingDescriptor>(Products[0].ItemClass.Get()), DescriptorName(Products[0].ItemClass) });
		}
	}

	/** Footprint (metres) from the buildable CDO's clearance boxes; falls back to a foundation's 8 m. */
	void ResolveFootprint(TSubclassOf<UFGBuildingDescriptor> Descriptor, double& OutXM, double& OutYM)
	{
		OutXM = 8.0;
		OutYM = 8.0;
		const TSubclassOf<AFGBuildable> BuildableClass = UFGBuildingDescriptor::GetBuildableClass(Descriptor);
		const AFGBuildable* CDO = BuildableClass ? BuildableClass->GetDefaultObject<AFGBuildable>() : nullptr;
		if (!CDO) { return; }

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
			if (Size.X > 1.0) { OutXM = Size.X / AIDAMetersToCm; }
			if (Size.Y > 1.0) { OutYM = Size.Y / AIDAMetersToCm; }
		}
	}

	void AddCost(TArray<FAIDACostItem>& Items, const FString& Name, int32 Amount)
	{
		if (Name.IsEmpty() || Amount <= 0) { return; }
		for (FAIDACostItem& Existing : Items)
		{
			if (Existing.Item == Name)
			{
				Existing.Amount += Amount;
				return;
			}
		}
		Items.Add({ Name, Amount });
	}
}

bool FAIDAActionSeam::ResolveBuildRecipe(UObject* WorldContext, const FString& DisplayName, FAIDARecipeResolution& Out)
{
	Out = FAIDARecipeResolution();

	UWorld* World = ResolveWorld(WorldContext);
	TArray<FBuildRecipeEntry> Entries;
	CollectBuildRecipes(World, Entries);
	if (Entries.Num() == 0)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[actions] no build recipes available (no recipe manager or nothing unlocked)."));
		return false;
	}

	const FString Wanted = DisplayName.TrimStartAndEnd();

	// Exact (case-insensitive) match wins; otherwise a UNIQUE substring match; otherwise suggestions.
	const FBuildRecipeEntry* Exact = nullptr;
	TArray<const FBuildRecipeEntry*> Partial;
	for (const FBuildRecipeEntry& Entry : Entries)
	{
		if (Entry.Name.Equals(Wanted, ESearchCase::IgnoreCase)) { Exact = &Entry; break; }
		if (Entry.Name.Contains(Wanted, ESearchCase::IgnoreCase)) { Partial.Add(&Entry); }
	}
	const FBuildRecipeEntry* Chosen = Exact ? Exact : (Partial.Num() == 1 ? Partial[0] : nullptr);
	if (!Chosen)
	{
		for (const FBuildRecipeEntry* Candidate : Partial)
		{
			if (Out.Suggestions.Num() >= MaxSuggestions) { break; }
			Out.Suggestions.AddUnique(Candidate->Name);
		}
		return false;
	}

	Out.RecipeClassPath = Chosen->Recipe->GetPathName();
	Out.DisplayName = Chosen->Name;
	ResolveFootprint(Chosen->Descriptor, Out.FootprintXM, Out.FootprintYM);
	return true;
}

bool FAIDAActionSeam::DryRunBuild(UObject* WorldContext, const FString& RecipeClassPath, int32 YawDeg,
	const TArray<FTransform>& Placements, FAIDADryRunResult& Out)
{
	Out = FAIDADryRunResult();

	UWorld* World = ResolveWorld(WorldContext);
	if (!World || Placements.Num() == 0)
	{
		Out.Error = TEXT("no world or no placements");
		return false;
	}

	UClass* RecipeClass = FSoftClassPath(RecipeClassPath).TryLoadClass<UFGRecipe>();
	if (!RecipeClass)
	{
		Out.Error = FString::Printf(TEXT("build recipe no longer resolvable (%s)"), *RecipeClassPath);
		return false;
	}

	// ONE hologram walked across every placement. Non-replicated (clients never see a ghost) and
	// destroyed before this function returns — validation writes nothing to the world.
	AFGHologram* Hologram = AFGHologram::SpawnHologramFromRecipe(RecipeClass, World->GetWorldSettings(),
		Placements[0].GetLocation(), /*hologramInstigator*/ nullptr,
		[](AFGHologram* PreSpawn) { PreSpawn->SetReplicates(false); });
	if (!Hologram)
	{
		Out.Error = TEXT("could not spawn a validation hologram for that recipe");
		return false;
	}

	Hologram->SetScrollRotateValue(YawDeg);

	for (int32 Index = 0; Index < Placements.Num(); ++Index)
	{
		const FVector Target = Placements[Index].GetLocation();

		// Synthetic upward-normal hit at the target — the same shape the build gun feeds per frame.
		FHitResult Hit(ForceInit);
		Hit.Location = Target;
		Hit.ImpactPoint = Target;
		Hit.Normal = FVector::UpVector;
		Hit.ImpactNormal = FVector::UpVector;

		Hologram->PreHologramPlacement(Hit);
		Hologram->SetHologramLocationAndRotation(Hit);
		Hologram->PostHologramPlacement(Hit);

		// Public validation entry (CheckValidPlacement/CheckClearance are protected). Null inventory:
		// affordability is checked against central storage below, so UFGCDUnaffordable is filtered.
		Hologram->ValidatePlacementAndCost(nullptr);

		TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
		Hologram->GetConstructDisqualifiers(Disqualifiers);
		for (const TSubclassOf<UFGConstructDisqualifier>& Disqualifier : Disqualifiers)
		{
			if (!Disqualifier) { continue; }
			// Unaffordable: ours to judge (central storage / costMode). Initializing: a first-tick
			// readiness flag, not a placement verdict — the placement checks above have already run.
			if (Disqualifier->IsChildOf(UFGCDUnaffordable::StaticClass()) ||
				Disqualifier->IsChildOf(UFGCDInitializing::StaticClass()))
			{
				continue;
			}
			FAIDAPlacementFailure Failure;
			Failure.Index = Index;
			Failure.AtM = Target / AIDAMetersToCm;
			Failure.Reason = UFGConstructDisqualifier::GetDisqualifyingText(Disqualifier).ToString();
			Out.Failures.Add(MoveTemp(Failure));
			break; // one reason per placement keeps the report bounded
		}
	}

	// Cost: per-construct cost × placement count (identical placements share one recipe).
	const TArray<FItemAmount> PerConstruct = Hologram->GetCost(/*includeChildren*/ true);
	for (const FItemAmount& Entry : PerConstruct)
	{
		AddCost(Out.Cost, DescriptorName(Entry.ItemClass), Entry.Amount * Placements.Num());
	}

	// Affordability vs central storage. Name→descriptor is resolved from the same cost entries.
	Out.bAffordable = true;
	if (AFGCentralStorageSubsystem* Central = AFGCentralStorageSubsystem::Get(World))
	{
		for (const FItemAmount& Entry : PerConstruct)
		{
			if (!Entry.ItemClass) { continue; }
			const int32 Needed = Entry.Amount * Placements.Num();
			if (Central->GetNumItemsFromCentralStorage(Entry.ItemClass) < Needed)
			{
				Out.bAffordable = false;
				break;
			}
		}
	}
	else
	{
		Out.bAffordable = false; // no central storage built yet — "central" costMode can't pay
	}

	Hologram->Destroy();
	Out.bOk = Out.Failures.Num() == 0;
	return true;
}

bool FAIDAActionSeam::ResolveDismantleTargets(UObject* WorldContext, const FAIDADismantleSpec& Selector, FAIDADismantleResolution& Out)
{
	Out = FAIDADismantleResolution();

	UWorld* World = ResolveWorld(WorldContext);
	if (!World) { return false; }

	const FVector CenterCm = Selector.CenterM * AIDAMetersToCm;
	const double RadiusCm = Selector.RadiusM * AIDAMetersToCm;
	const double RadiusSq = RadiusCm * RadiusCm;

	const auto MatchesName = [&Selector](const FString& Name)
	{
		return Selector.Buildable.IsEmpty() || Name.Contains(Selector.Buildable, ESearchCase::IgnoreCase);
	};

	// Full-actor buildables — live walk, never a cached FactoryIndex snapshot.
	if (AFGBuildableSubsystem* Subsystem = AFGBuildableSubsystem::Get(World))
	{
		for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
		{
			if (Out.Count >= Selector.MaxCount) { break; }
			if (!IsValid(Buildable)) { continue; }
			if (FVector::DistSquared(Buildable->GetActorLocation(), CenterCm) > RadiusSq) { continue; }

			const TSubclassOf<UFGRecipe> Recipe = Buildable->GetBuiltWithRecipe();
			const TArray<FItemAmount> Products = Recipe ? UFGRecipe::GetProducts(Recipe) : TArray<FItemAmount>();
			const FString Name = Products.Num() > 0 ? DescriptorName(Products[0].ItemClass) : GetNameSafe(Buildable->GetClass());
			if (!MatchesName(Name)) { continue; }
			if (!IFGDismantleInterface::Execute_CanDismantle(Buildable)) { continue; }

			TArray<FInventoryStack> Refund;
			IFGDismantleInterface::Execute_GetDismantleRefund(Buildable, Refund, /*noBuildCostEnabled*/ false);
			for (const FInventoryStack& Stack : Refund)
			{
				AddCost(Out.Refund, DescriptorName(Stack.Item.GetItemClass()), Stack.NumItems);
			}
			++Out.Count;
		}
	}

	// Lightweight instances (foundations/walls) — a separate subsystem the buildable walk misses.
	if (AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World))
	{
		for (const auto& Pair : Lightweights->GetAllLightweightBuildableInstances())
		{
			if (Out.Count >= Selector.MaxCount) { break; }

			const TSubclassOf<UFGRecipe> Recipe = Lightweights->GetBuiltWithRecipeForBuildableClass(Pair.Key);
			const TArray<FItemAmount> Products = Recipe ? UFGRecipe::GetProducts(Recipe) : TArray<FItemAmount>();
			const FString Name = Products.Num() > 0 ? DescriptorName(Products[0].ItemClass) : GetNameSafe(Pair.Key.Get());
			if (!MatchesName(Name)) { continue; }

			for (const FRuntimeBuildableInstanceData& Instance : Pair.Value)
			{
				if (Out.Count >= Selector.MaxCount) { break; }
				if (!Instance.IsValid()) { continue; } // removed slots linger in the array
				if (FVector::DistSquared(Instance.Transform.GetLocation(), CenterCm) > RadiusSq) { continue; }

				// Refund = build cost (the game refunds ingredients in full).
				if (Recipe)
				{
					for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(World, Recipe))
					{
						AddCost(Out.Refund, DescriptorName(Ingredient.ItemClass), Ingredient.Amount);
					}
				}
				++Out.Count;
			}
		}
	}

	return true;
}
