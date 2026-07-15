#include "Actions/AIDAActionSeam.h"

#include "AIDA.h"
#include "Actions/AIDAActionSpec.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"

#include "FGCharacterPlayer.h"

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

	void AddCost(TArray<FAIDACostItem>& Items, TSubclassOf<UFGItemDescriptor> ItemClass, int32 Amount)
	{
		const FString Name = DescriptorName(ItemClass);
		if (Name.IsEmpty() || Amount <= 0) { return; }
		for (FAIDACostItem& Existing : Items)
		{
			if (Existing.Item == Name)
			{
				Existing.Amount += Amount;
				return;
			}
		}
		Items.Add({ Name, Amount, ItemClass ? ItemClass->GetPathName() : FString() });
	}

	/** The dry-run/execute disqualifier filter: Unaffordable is ours to judge (central storage /
	 *  costMode), Initializing is a first-tick readiness flag, not a placement verdict. */
	bool IsBlockingDisqualifier(const TSubclassOf<UFGConstructDisqualifier>& Disqualifier)
	{
		return Disqualifier
			&& !Disqualifier->IsChildOf(UFGCDUnaffordable::StaticClass())
			&& !Disqualifier->IsChildOf(UFGCDInitializing::StaticClass());
	}

	/** Position + validate the hologram at one placement; true when no blocking disqualifier remains. */
	bool PlaceAndValidate(AFGHologram* Hologram, const FTransform& Placement, TSubclassOf<UFGConstructDisqualifier>* OutBlocking = nullptr)
	{
		const FVector Target = Placement.GetLocation();

		// Synthetic upward-normal hit at the target — the same shape the build gun feeds per frame.
		FHitResult Hit(ForceInit);
		Hit.Location = Target;
		Hit.ImpactPoint = Target;
		Hit.Normal = FVector::UpVector;
		Hit.ImpactNormal = FVector::UpVector;

		Hologram->SetScrollRotateValue(FMath::RoundToInt32(Placement.Rotator().Yaw));
		Hologram->PreHologramPlacement(Hit);
		Hologram->SetHologramLocationAndRotation(Hit);
		Hologram->PostHologramPlacement(Hit);

		// Public validation entry (CheckValidPlacement/CheckClearance are protected). Null inventory:
		// affordability is judged against central storage by the callers, so UFGCDUnaffordable is filtered.
		Hologram->ValidatePlacementAndCost(nullptr);

		TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
		Hologram->GetConstructDisqualifiers(Disqualifiers);
		for (const TSubclassOf<UFGConstructDisqualifier>& Disqualifier : Disqualifiers)
		{
			if (IsBlockingDisqualifier(Disqualifier))
			{
				if (OutBlocking) { *OutBlocking = Disqualifier; }
				return false;
			}
		}
		return true;
	}

	AFGHologram* SpawnValidationHologram(UWorld* World, UClass* RecipeClass, const FVector& At)
	{
		// Non-replicated (clients never see a ghost); destroyed by the caller before returning.
		return AFGHologram::SpawnHologramFromRecipe(RecipeClass, World->GetWorldSettings(), At,
			/*hologramInstigator*/ nullptr,
			[](AFGHologram* PreSpawn) { PreSpawn->SetReplicates(false); });
	}

	TSubclassOf<UFGItemDescriptor> LoadDescriptor(const FString& ClassPath)
	{
		return ClassPath.IsEmpty() ? nullptr : TSubclassOf<UFGItemDescriptor>(FSoftClassPath(ClassPath).TryLoadClass<UFGItemDescriptor>());
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

bool FAIDAActionSeam::DryRunBuild(UObject* WorldContext, const FString& RecipeClassPath,
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

	// ONE hologram walked across every placement, destroyed before this function returns —
	// validation writes nothing to the world.
	AFGHologram* Hologram = SpawnValidationHologram(World, RecipeClass, Placements[0].GetLocation());
	if (!Hologram)
	{
		Out.Error = TEXT("could not spawn a validation hologram for that recipe");
		return false;
	}

	for (int32 Index = 0; Index < Placements.Num(); ++Index)
	{
		TSubclassOf<UFGConstructDisqualifier> Blocking;
		if (!PlaceAndValidate(Hologram, Placements[Index], &Blocking))
		{
			FAIDAPlacementFailure Failure;
			Failure.Index = Index;
			Failure.AtM = Placements[Index].GetLocation() / AIDAMetersToCm;
			Failure.Reason = UFGConstructDisqualifier::GetDisqualifyingText(Blocking).ToString();
			Out.Failures.Add(MoveTemp(Failure));
		}
	}

	// Cost: per-construct cost × placement count (identical placements share one recipe).
	const TArray<FItemAmount> PerConstruct = Hologram->GetCost(/*includeChildren*/ true);
	for (const FItemAmount& Entry : PerConstruct)
	{
		AddCost(Out.Cost, Entry.ItemClass, Entry.Amount * Placements.Num());
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
				AddCost(Out.Refund, Stack.Item.GetItemClass(), Stack.NumItems);
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
						AddCost(Out.Refund, Ingredient.ItemClass, Ingredient.Amount);
					}
				}
				++Out.Count;
			}
		}
	}

	return true;
}

bool FAIDAActionSeam::DeductCost(UObject* WorldContext, const TArray<FAIDACostItem>& Cost)
{
	UWorld* World = ResolveWorld(WorldContext);
	AFGCentralStorageSubsystem* Central = World ? AFGCentralStorageSubsystem::Get(World) : nullptr;
	if (!Central) { return false; }

	// Verify the WHOLE tally first, then deduct — never a partial payment.
	TArray<TPair<TSubclassOf<UFGItemDescriptor>, int32>> Lines;
	for (const FAIDACostItem& Item : Cost)
	{
		const TSubclassOf<UFGItemDescriptor> Descriptor = LoadDescriptor(Item.ClassPath);
		if (!Descriptor || Central->GetNumItemsFromCentralStorage(Descriptor) < Item.Amount)
		{
			return false;
		}
		Lines.Emplace(Descriptor, Item.Amount);
	}
	for (const auto& Line : Lines)
	{
		const int32 Removed = Central->TryRemoveItemsFromCentralStorage(Line.Key, Line.Value);
		if (Removed < Line.Value)
		{
			// Should not happen after the verify pass; log loudly, keep going (items already left).
			UE_LOG(LogAIDA, Error, TEXT("[actions] cost deduction shortfall: %d/%d %s"),
				Removed, Line.Value, *GetNameSafe(Line.Key.Get()));
		}
	}
	return true;
}

void FAIDAActionSeam::RefundToPlayer(UObject* WorldContext, const FString& PlayerId, const TArray<FAIDACostItem>& Refund,
	int32& OutRefunded, int32& OutLost)
{
	OutRefunded = 0;
	OutLost = 0;

	int32 Total = 0;
	for (const FAIDACostItem& Item : Refund) { Total += Item.Amount; }
	if (Total == 0) { return; }

	UWorld* World = ResolveWorld(WorldContext);
	if (!World) { OutLost = Total; return; }

	// The refund destination: the named player's inventory, else the first player with one (covers
	// the console/host approver). Central storage exposes no server-side deposit API.
	UFGInventoryComponent* Inventory = nullptr;
	UFGInventoryComponent* Fallback = nullptr;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
		if (!Character || !Character->GetInventory()) { continue; }
		if (!Fallback) { Fallback = Character->GetInventory(); }

		APlayerState* PS = PC->PlayerState;
		const TSharedPtr<const FUniqueNetId> NetId = PS ? PS->GetUniqueId().GetUniqueNetId() : nullptr;
		if (NetId.IsValid() && NetId->ToString() == PlayerId)
		{
			Inventory = Character->GetInventory();
			break;
		}
	}
	if (!Inventory) { Inventory = Fallback; }
	if (!Inventory)
	{
		OutLost = Total;
		UE_LOG(LogAIDA, Warning, TEXT("[actions] refund of %d item(s) LOST — no player inventory reachable."), Total);
		return;
	}

	for (const FAIDACostItem& Item : Refund)
	{
		const TSubclassOf<UFGItemDescriptor> Descriptor = LoadDescriptor(Item.ClassPath);
		if (!Descriptor) { OutLost += Item.Amount; continue; }
		const int32 Added = Inventory->AddStack(FInventoryStack(Item.Amount, Descriptor), /*allowPartialAdd*/ true);
		OutRefunded += Added;
		OutLost += Item.Amount - Added;
	}
	if (OutLost > 0)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[actions] refund partially lost: %d item(s) did not fit the inventory."), OutLost);
	}
}

int32 FAIDAActionSeam::ExecuteBuildBatch(UObject* WorldContext, const FString& RecipeClassPath,
	const TArray<FTransform>& Placements, int32 Cursor, int32 BatchSize,
	TArray<FString>& OutEntityIds, TArray<TWeakObjectPtr<AActor>>& OutActors, int32& OutSkipped)
{
	OutSkipped = 0;

	UWorld* World = ResolveWorld(WorldContext);
	UClass* RecipeClass = World ? FSoftClassPath(RecipeClassPath).TryLoadClass<UFGRecipe>() : nullptr;
	if (!RecipeClass)
	{
		// Consume everything so the executor terminates instead of spinning on a dead recipe.
		OutSkipped = Placements.Num() - Cursor;
		return OutSkipped;
	}

	const int32 End = FMath::Min(Cursor + BatchSize, Placements.Num());
	for (int32 Index = Cursor; Index < End; ++Index)
	{
		// Fresh hologram per construct — Construct() consumes the hologram's configured state.
		AFGHologram* Hologram = SpawnValidationHologram(World, RecipeClass, Placements[Index].GetLocation());
		if (!Hologram) { ++OutSkipped; continue; }

		// Re-validate: the world may have changed since the dry-run (players build/walk around).
		if (!PlaceAndValidate(Hologram, Placements[Index]))
		{
			Hologram->Destroy();
			++OutSkipped;
			continue;
		}

		TArray<AActor*> Children;
		AActor* Built = Hologram->Construct(Children, FNetConstructionID());
		Hologram->Destroy();
		if (!Built) { ++OutSkipped; continue; }

		// Capture the undo handle (docs/PHASE4.md §2d). Lightweight-bound buildables are destroyed
		// and migrated to the instance subsystem right after Construct — journal class+transform
		// (index unknown here; undo re-resolves by transform). Full actors keep a weak ptr too.
		AFGBuildable* Buildable = Cast<AFGBuildable>(Built);
		FAIDAEntityId Entity;
		Entity.ClassPath = Built->GetClass()->GetPathName();
		Entity.Pos = Placements[Index].GetLocation();
		Entity.YawDeg = FMath::RoundToInt32(Placements[Index].Rotator().Yaw);
		if (Buildable && Buildable->ShouldConvertToLightweight())
		{
			Entity.Type = TEXT("lw");
			Entity.Index = INDEX_NONE;
		}
		else
		{
			Entity.Type = TEXT("actor");
			Entity.RecipePath = RecipeClassPath;
			OutActors.Add(Built);
		}
		OutEntityIds.Add(AIDAActionSpec::EncodeEntityId(Entity));

		// Auxiliary actors the hologram spawned alongside (poles, wires): journal them as actors.
		for (AActor* Child : Children)
		{
			if (!Child) { continue; }
			FAIDAEntityId ChildEntity;
			ChildEntity.Type = TEXT("actor");
			ChildEntity.ClassPath = Child->GetClass()->GetPathName();
			ChildEntity.Pos = Child->GetActorLocation();
			ChildEntity.YawDeg = FMath::RoundToInt32(Child->GetActorRotation().Yaw);
			OutEntityIds.Add(AIDAActionSpec::EncodeEntityId(ChildEntity));
			OutActors.Add(Child);
		}
	}
	return End - Cursor;
}

bool FAIDAActionSeam::ResolveDismantleHandles(UObject* WorldContext, const FAIDADismantleSpec& Selector, TArray<FAIDADismantleHandle>& OutHandles)
{
	OutHandles.Reset();

	UWorld* World = ResolveWorld(WorldContext);
	if (!World) { return false; }

	const FVector CenterCm = Selector.CenterM * AIDAMetersToCm;
	const double RadiusSq = FMath::Square(Selector.RadiusM * AIDAMetersToCm);

	const auto MatchesName = [&Selector](const FString& Name)
	{
		return Selector.Buildable.IsEmpty() || Name.Contains(Selector.Buildable, ESearchCase::IgnoreCase);
	};

	if (AFGBuildableSubsystem* Subsystem = AFGBuildableSubsystem::Get(World))
	{
		for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
		{
			if (OutHandles.Num() >= Selector.MaxCount) { break; }
			if (!IsValid(Buildable)) { continue; }
			if (FVector::DistSquared(Buildable->GetActorLocation(), CenterCm) > RadiusSq) { continue; }

			const TSubclassOf<UFGRecipe> Recipe = Buildable->GetBuiltWithRecipe();
			const TArray<FItemAmount> Products = Recipe ? UFGRecipe::GetProducts(Recipe) : TArray<FItemAmount>();
			const FString Name = Products.Num() > 0 ? DescriptorName(Products[0].ItemClass) : GetNameSafe(Buildable->GetClass());
			if (!MatchesName(Name)) { continue; }
			if (!IFGDismantleInterface::Execute_CanDismantle(Buildable)) { continue; }

			FAIDAEntityId Entity;
			Entity.Type = TEXT("actor");
			Entity.ClassPath = Buildable->GetClass()->GetPathName();
			Entity.RecipePath = Recipe ? Recipe->GetPathName() : FString();
			Entity.Pos = Buildable->GetActorLocation();
			Entity.YawDeg = FMath::RoundToInt32(Buildable->GetActorRotation().Yaw);

			FAIDADismantleHandle Handle;
			Handle.Actor = Buildable;
			Handle.EncodedId = AIDAActionSpec::EncodeEntityId(Entity);
			OutHandles.Add(MoveTemp(Handle));
		}
	}

	if (AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World))
	{
		for (const auto& Pair : Lightweights->GetAllLightweightBuildableInstances())
		{
			if (OutHandles.Num() >= Selector.MaxCount) { break; }

			const TSubclassOf<UFGRecipe> Recipe = Lightweights->GetBuiltWithRecipeForBuildableClass(Pair.Key);
			const TArray<FItemAmount> Products = Recipe ? UFGRecipe::GetProducts(Recipe) : TArray<FItemAmount>();
			const FString Name = Products.Num() > 0 ? DescriptorName(Products[0].ItemClass) : GetNameSafe(Pair.Key.Get());
			if (!MatchesName(Name)) { continue; }

			for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
			{
				if (OutHandles.Num() >= Selector.MaxCount) { break; }
				const FRuntimeBuildableInstanceData& Instance = Pair.Value[Index];
				if (!Instance.IsValid()) { continue; }
				if (FVector::DistSquared(Instance.Transform.GetLocation(), CenterCm) > RadiusSq) { continue; }

				FAIDAEntityId Entity;
				Entity.Type = TEXT("lw");
				Entity.ClassPath = Pair.Key->GetPathName();
				Entity.Index = Index;
				Entity.Pos = Instance.Transform.GetLocation();
				Entity.YawDeg = FMath::RoundToInt32(Instance.Transform.Rotator().Yaw);

				FAIDADismantleHandle Handle;
				Handle.EncodedId = AIDAActionSpec::EncodeEntityId(Entity);
				OutHandles.Add(MoveTemp(Handle));
			}
		}
	}

	return true;
}

int32 FAIDAActionSeam::DismantleHandleBatch(UObject* WorldContext, const TArray<FAIDADismantleHandle>& Handles,
	int32 Cursor, int32 BatchSize, TArray<FAIDACostItem>& InOutRefund, TArray<FString>& OutRemovedIds,
	int32& OutRemoved, int32& OutMissing)
{
	OutRemoved = 0;
	OutMissing = 0;

	UWorld* World = ResolveWorld(WorldContext);
	if (!World)
	{
		OutMissing = Handles.Num() - Cursor;
		return OutMissing;
	}
	AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);

	const int32 End = FMath::Min(Cursor + BatchSize, Handles.Num());
	for (int32 Index = Cursor; Index < End; ++Index)
	{
		const FAIDADismantleHandle& Handle = Handles[Index];

		// Full actor: still alive and dismantlable?
		if (AActor* Actor = Handle.Actor.Get())
		{
			if (!IFGDismantleInterface::Execute_CanDismantle(Actor)) { ++OutMissing; continue; }
			TArray<FInventoryStack> Refund;
			IFGDismantleInterface::Execute_GetDismantleRefund(Actor, Refund, /*noBuildCostEnabled*/ false);
			for (const FInventoryStack& Stack : Refund)
			{
				AddCost(InOutRefund, Stack.Item.GetItemClass(), Stack.NumItems);
			}
			IFGDismantleInterface::Execute_Dismantle(Actor);
			OutRemovedIds.Add(Handle.EncodedId);
			++OutRemoved;
			continue;
		}

		// Lightweight (or an actor that died since approval): re-resolve from the encoded id.
		FAIDAEntityId Entity;
		if (!AIDAActionSpec::DecodeEntityId(Handle.EncodedId, Entity) || Entity.Type != TEXT("lw") || !Lightweights)
		{
			++OutMissing;
			continue;
		}
		const TSubclassOf<AFGBuildable> BuildableClass(FSoftClassPath(Entity.ClassPath).TryLoadClass<AFGBuildable>());
		const FRuntimeBuildableInstanceData* Instance = BuildableClass
			? Lightweights->GetRuntimeDataForBuildableClassAndIndex(BuildableClass, Entity.Index) : nullptr;
		// Indices are recycled — the cached transform is the identity check (docs/PHASE4.md §2d).
		if (!Instance || !Instance->IsValid() ||
			!Instance->Transform.GetLocation().Equals(Entity.Pos, /*tolerance*/ 5.0))
		{
			++OutMissing;
			continue;
		}

		if (const TSubclassOf<UFGRecipe> Recipe = Lightweights->GetBuiltWithRecipeForBuildableClass(BuildableClass))
		{
			for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(World, Recipe))
			{
				AddCost(InOutRefund, Ingredient.ItemClass, Ingredient.Amount);
			}
		}
		FLightweightBuildableInstanceRef Ref;
		Ref.Initialize(Lightweights, BuildableClass, Entity.Index);
		if (Ref.Remove())
		{
			OutRemovedIds.Add(Handle.EncodedId);
			++OutRemoved;
		}
		else
		{
			++OutMissing;
		}
	}
	return End - Cursor;
}

bool FAIDAActionSeam::UndoRemoveEntity(UObject* WorldContext, const FAIDAEntityId& Entity, AActor* CachedActor)
{
	UWorld* World = ResolveWorld(WorldContext);
	if (!World) { return false; }

	// In-session fast path: the actor we built is still the actor we hold.
	if (IsValid(CachedActor))
	{
		if (IFGDismantleInterface::Execute_CanDismantle(CachedActor))
		{
			IFGDismantleInterface::Execute_Dismantle(CachedActor);
			return true;
		}
		return false;
	}

	if (Entity.Type == TEXT("lw"))
	{
		AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
		const TSubclassOf<AFGBuildable> BuildableClass(FSoftClassPath(Entity.ClassPath).TryLoadClass<AFGBuildable>());
		if (!Lightweights || !BuildableClass) { return false; }

		// Try the journaled index first (transform-verified — indices are recycled), else scan the
		// class's instances by transform (the post-reload / index-unknown path, docs/PHASE4.md §2d).
		int32 Index = Entity.Index;
		const FRuntimeBuildableInstanceData* Instance = Index != INDEX_NONE
			? Lightweights->GetRuntimeDataForBuildableClassAndIndex(BuildableClass, Index) : nullptr;
		if (!Instance || !Instance->IsValid() || !Instance->Transform.GetLocation().Equals(Entity.Pos, 5.0))
		{
			Index = INDEX_NONE;
			if (const TArray<FRuntimeBuildableInstanceData>* Instances =
				Lightweights->GetAllLightweightBuildableInstances().Find(BuildableClass))
			{
				for (int32 i = 0; i < Instances->Num(); ++i)
				{
					if ((*Instances)[i].IsValid() && (*Instances)[i].Transform.GetLocation().Equals(Entity.Pos, 5.0))
					{
						Index = i;
						break;
					}
				}
			}
		}
		if (Index == INDEX_NONE) { return false; }

		FLightweightBuildableInstanceRef Ref;
		Ref.Initialize(Lightweights, BuildableClass, Index);
		return Ref.Remove();
	}

	// Full actor: re-resolve by class + transform epsilon (the save gives us no persistent id).
	UClass* ActorClass = FSoftClassPath(Entity.ClassPath).TryLoadClass<AActor>();
	if (!ActorClass) { return false; }
	for (TActorIterator<AActor> It(World, ActorClass); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor) || !Actor->GetActorLocation().Equals(Entity.Pos, 5.0)) { continue; }
		if (Actor->Implements<UFGDismantleInterface>() &&
			IFGDismantleInterface::Execute_CanDismantle(Actor))
		{
			IFGDismantleInterface::Execute_Dismantle(Actor);
			return true;
		}
	}
	return false;
}

bool FAIDAActionSeam::UndoRebuildEntity(UObject* WorldContext, const FAIDAEntityId& Entity)
{
	UWorld* World = ResolveWorld(WorldContext);
	if (!World) { return false; }

	// The recipe: journaled for actors; recovered from the class for lightweights.
	FString RecipePath = Entity.RecipePath;
	if (RecipePath.IsEmpty() && Entity.Type == TEXT("lw"))
	{
		AFGLightweightBuildableSubsystem* Lightweights = AFGLightweightBuildableSubsystem::Get(World);
		const TSubclassOf<AFGBuildable> BuildableClass(FSoftClassPath(Entity.ClassPath).TryLoadClass<AFGBuildable>());
		const TSubclassOf<UFGRecipe> Recipe = (Lightweights && BuildableClass)
			? Lightweights->GetBuiltWithRecipeForBuildableClass(BuildableClass) : nullptr;
		if (Recipe) { RecipePath = Recipe->GetPathName(); }
	}
	UClass* RecipeClass = RecipePath.IsEmpty() ? nullptr : FSoftClassPath(RecipePath).TryLoadClass<UFGRecipe>();
	if (!RecipeClass) { return false; }

	AFGHologram* Hologram = SpawnValidationHologram(World, RecipeClass, Entity.Pos);
	if (!Hologram) { return false; }

	const FTransform Placement(FRotator(0.0, static_cast<double>(Entity.YawDeg), 0.0), Entity.Pos);
	if (!PlaceAndValidate(Hologram, Placement))
	{
		Hologram->Destroy(); // something occupies the spot now — a reported partial-undo, not fatal
		return false;
	}
	TArray<AActor*> Children;
	AActor* Built = Hologram->Construct(Children, FNetConstructionID());
	Hologram->Destroy();
	return Built != nullptr;
}
