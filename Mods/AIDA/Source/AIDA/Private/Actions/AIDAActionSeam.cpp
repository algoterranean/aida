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

#include "AbstractInstanceManager.h"
#include "FGCharacterPlayer.h"

#include "FGBuildableSubsystem.h"
#include "FGCentralStorageSubsystem.h"
#include "FGFactoryConnectionComponent.h"
#include "FGPipeConnectionComponent.h"
#include "FGPowerConnectionComponent.h"
#include "Buildables/FGBuildablePowerPole.h"
#include "Buildables/FGBuildableWire.h"
#include "Hologram/FGBuildableHologram.h"
#include "Hologram/FGConveyorBeltHologram.h"
#include "Hologram/FGSplineHologram.h"
#include "FGClearanceInterface.h"
#include "FGConstructDisqualifier.h"
#include "FGDismantleInterface.h"
#include "FGInventoryComponent.h"
#include "FGLightweightBuildableSubsystem.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Buildables/FGBuildablePipelineAttachment.h"
#include "Buildables/FGBuildableStorage.h"
#include "Buildables/FGBuildableWidgetSign.h"
#include "FGSignInterface.h"
#include "FGSignTypes.h"
#include "Hologram/FGHologram.h"
#include "ItemAmount.h"
#include "Resources/FGBuildingDescriptor.h"
#include "Resources/FGItemDescriptor.h"

namespace
{
	constexpr int32 MaxSuggestions = 5;

	/**
	 * The game's build-gun trace channel. The exported extern (TC_BuildGun, FactoryGame.h) has no
	 * definition in the linkable header stubs, but the channel is pinned in the game's
	 * DefaultEngine.ini: ECC_GameTraceChannel5 = "BuildGun".
	 */
	constexpr ECollisionChannel AIDABuildGunChannel = ECC_GameTraceChannel5;

	UWorld* ResolveWorld(UObject* WorldContext)
	{
		return GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	}

	/**
	 * Ground trace that refuses to land on conveyors: belts/lifts are never "ground", and worse — a
	 * conveyor hit actor makes attachment holograms run their snap-to-belt spline math against our
	 * location-overridden hit (live-verify: proposing a manifold on a floor with belts running
	 * beneath it froze the game). Re-traces below each conveyor hit, bounded.
	 */
	bool TraceGroundIgnoringConveyors(UWorld* World, const FVector& Start, const FVector& End,
		const FCollisionQueryParams& InParams, FHitResult& OutHit)
	{
		FCollisionQueryParams Params(InParams);
		for (int32 Guard = 0; Guard < 4; ++Guard)
		{
			if (!World->LineTraceSingleByChannel(OutHit, Start, End, AIDABuildGunChannel, Params))
			{
				return false;
			}
			AActor* HitActor = OutHit.GetActor();
			if (!HitActor || !HitActor->IsA<AFGBuildableConveyorBase>())
			{
				return true;
			}
			Params.AddIgnoredActor(HitActor);
		}
		return false;
	}

	FString DescriptorName(TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		if (!ItemClass) { return FString(); }
		const FString Name = UFGItemDescriptor::GetItemName(ItemClass).ToString();
		return Name.IsEmpty() ? GetNameSafe(ItemClass.Get()) : Name;
	}

	/**
	 * Canonicalize a display name for matching: lowercase, alphanumerics only. Game names carry
	 * Unicode spaces (FText number formatting puts a no-break space in "Foundation (2 m)") that
	 * render identically to ASCII but fail Equals/Contains against what a model or player types —
	 * live-verify: the model echoed a suggestion character-perfectly and still missed. This also
	 * forgives "(2m)" vs "(2 m)" and stray punctuation.
	 */
	FString NormalizeName(const FString& Name)
	{
		FString Out;
		Out.Reserve(Name.Len());
		for (const TCHAR C : Name)
		{
			if (FChar::IsAlnum(C)) { Out.AppendChar(FChar::ToLower(C)); }
		}
		return Out;
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

	/** The user's placement philosophy: CLIPPING NEVER REJECTS A BUILD. If it can physically be
	 *  placed, propose it — the player picks the spot / nudges the ghost to their preferred clipping
	 *  level. This covers the whole encroaching family: hard + soft clearance, and transient bodies
	 *  (player/creature/vehicle standing in the spot). Reported as advisory, never as a failure. */
	bool IsClippingDisqualifier(const TSubclassOf<UFGConstructDisqualifier>& Disqualifier)
	{
		return Disqualifier
			&& (Disqualifier->IsChildOf(UFGCDEncroachingClearance::StaticClass())
				|| Disqualifier->IsChildOf(UFGCDEncroachingSoftClearance::StaticClass())
				|| Disqualifier->IsChildOf(UFGCDEncroachingPlayer::StaticClass())
				|| Disqualifier->IsChildOf(UFGCDEncroachingCreature::StaticClass())
				|| Disqualifier->IsChildOf(UFGCDEncroachingVehicle::StaticClass()));
	}

	/** The dry-run/execute disqualifier filter: Unaffordable is ours to judge (central storage /
	 *  costMode), Initializing is a first-tick readiness flag, and clipping (see above) is the
	 *  player's call, never a rejection. Everything else (floor, aim, snap rules, identical
	 *  overlapping duplicate, spline shape limits) genuinely blocks. */
	bool IsBlockingDisqualifier(const TSubclassOf<UFGConstructDisqualifier>& Disqualifier)
	{
		return Disqualifier
			&& !Disqualifier->IsChildOf(UFGCDUnaffordable::StaticClass())
			&& !Disqualifier->IsChildOf(UFGCDInitializing::StaticClass())
			&& !IsClippingDisqualifier(Disqualifier);
	}

	/**
	 * Any live player inventory, for ValidatePlacementAndCost: its CheckCanAfford hard-asserts on a
	 * null inventory in Shipping (live-verify crash, FGHologram.cpp:2187). WHOSE inventory doesn't
	 * matter — the Unaffordable disqualifier is filtered; affordability is judged vs central storage.
	 */
	UFGInventoryComponent* FindValidationInventory(UWorld* World)
	{
		if (!World) { return nullptr; }
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
			if (Character && Character->GetInventory()) { return Character->GetInventory(); }
		}
		return nullptr;
	}

	/**
	 * If the hit landed on a lightweight instance (foundations/walls — the hit actor is the
	 * AbstractInstanceManager, which hologram snapping/floor checks can't use), stand up a
	 * temporary buildable and retarget the hit at it, exactly like the build gun does. Returns the
	 * temp actor — the CALLER destroys it, and must keep it alive until after Construct() if it
	 * constructs (the hologram caches the snapped floor).
	 */
	AFGBuildable* ResolveInstanceHit(FHitResult& InOutHit)
	{
		AAbstractInstanceManager* Manager = Cast<AAbstractInstanceManager>(InOutHit.GetActor());
		if (!Manager) { return nullptr; }

		FInstanceHandle Handle;
		FLightweightBuildableInstanceRef InstanceRef;
		if (!Manager->ResolveHit(InOutHit, Handle) ||
			!AFGLightweightBuildableSubsystem::ResolveLightweightInstance(Handle, InstanceRef))
		{
			return nullptr;
		}
		AFGBuildable* TempBuildable = InstanceRef.SpawnTemporaryBuildable();
		if (TempBuildable)
		{
			InOutHit.HitObjectHandle = FActorInstanceHandle(TempBuildable);
			if (UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(TempBuildable->GetRootComponent()))
			{
				InOutHit.Component = Root;
			}
		}
		return TempBuildable;
	}

	/** Position + validate the hologram at one placement; true when no blocking disqualifier remains.
	 *  InOutTemplateHit carries the last REAL hit across a grid: tiles hanging over a drop (no ground
	 *  within trace range) borrow it — floating foundations are legal, and the hologram only needs a
	 *  believable hit actor. OutTempFloor receives the temporary buildable when the tile sits on a
	 *  lightweight instance (machines must see a real floor to snap to); the caller destroys it —
	 *  AFTER Construct() when constructing. bVerboseLog dumps the full state. */
	bool PlaceAndValidate(AFGHologram* Hologram, const FTransform& Placement, UFGInventoryComponent* Inventory,
		TSubclassOf<UFGConstructDisqualifier>* OutBlocking = nullptr, bool bVerboseLog = false,
		FHitResult* InOutTemplateHit = nullptr, AFGBuildable** OutTempFloor = nullptr,
		bool* OutClipping = nullptr)
	{
		const FVector Target = Placement.GetLocation();

		// The hologram wants a REAL hit — a zeroed FHitResult reads as "aiming at the sky" and
		// disqualifies every placement with InvalidAimLocation (live-verify). Trace straight down at
		// the target's X/Y on the game's OWN build-gun channel (a WorldStatic object trace missed the
		// terrain — live-verify round 2) and feed the actual ground/floor hit; that grounds Z, and
		// slopes behave like the build gun. Pawns are ignored so a player standing on a tile doesn't
		// become its "ground".
		FHitResult Hit(ForceInit);
		bool bHaveHit = false;
		if (UWorld* World = Hologram->GetWorld())
		{
			FCollisionQueryParams Params(SCENE_QUERY_STAT(AIDAPlacementTrace), /*bTraceComplex*/ false);
			Params.AddIgnoredActor(Hologram);
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				if (APawn* Pawn = It->Get() ? It->Get()->GetPawn() : nullptr) { Params.AddIgnoredActor(Pawn); }
			}

			// First from just above the intended plane (so a roof over the area doesn't win) …
			const FVector End = Target - FVector(0.0, 0.0, 50000.0);   // down to 500 m below (canyons)
			bHaveHit = TraceGroundIgnoringConveyors(World, Target + FVector(0.0, 0.0, 300.0), End, Params, Hit);
			if (!bHaveHit)
			{
				// … but on rising ground a far tile's terrain can sit ABOVE start+3 m — the ray then
				// begins underground and misses (live-verify: one uphill corner of an otherwise valid
				// grid). Retry from 50 m up before giving up.
				bHaveHit = TraceGroundIgnoringConveyors(World, Target + FVector(0.0, 0.0, 5000.0), End, Params, Hit);
			}
		}
		if (!bHaveHit && InOutTemplateHit && InOutTemplateHit->GetActor())
		{
			// Over a void (cliff edge inside the grid): borrow the last real hit — floating
			// foundations are legal, the hologram only rejects hits without a plausible actor.
			Hit = *InOutTemplateHit;
			bHaveHit = true;
		}
		else if (bHaveHit && InOutTemplateHit)
		{
			*InOutTemplateHit = Hit; // stored UNRESOLVED — each tile stands up its own temp floor
		}
		// Tiles on lightweight instances need a real floor actor for snap/floor validation
		// (machines on AIDA-built foundations failed as "invalid floor" without this). The temp
		// lives through validation; ownership passes to the caller when requested, else it's
		// destroyed before returning.
		AFGBuildable* TempFloor = bHaveHit ? ResolveInstanceHit(Hit) : nullptr;
		if (OutTempFloor) { *OutTempFloor = TempFloor; }
		if (bHaveHit)
		{
			// Keep the genuine hit actor (validity) but aim the hit at the placement's EXACT position:
			// grids are FLAT at the intended plane by default — per-tile terrain Z made sloped ground
			// produce stepped foundations (live-verify). Terrain-following specs pre-adjust placement
			// Z upstream instead.
			Hit.Location = Target;
			Hit.ImpactPoint = Target;
		}
		else
		{
			// Nothing below (void/water): fall back to the synthetic hit; the hologram's own
			// validation then reports the placement, one index at a time, instead of crashing out.
			Hit = FHitResult(ForceInit);
			Hit.bBlockingHit = true;
			Hit.Location = Target;
			Hit.ImpactPoint = Target;
			Hit.Normal = FVector::UpVector;
			Hit.ImpactNormal = FVector::UpVector;
		}

		if (bVerboseLog)
		{
			UE_LOG(LogAIDA, Log, TEXT("[actions][dbg] target=%s traceHit=%d hitActor=%s (%s) hitComp=%s hitLoc=%s normal=%s"),
				*Target.ToCompactString(), bHaveHit ? 1 : 0,
				*GetNameSafe(Hit.GetActor()), Hit.GetActor() ? *GetNameSafe(Hit.GetActor()->GetClass()) : TEXT("-"),
				*GetNameSafe(Hit.GetComponent()), *Hit.Location.ToCompactString(), *Hit.ImpactNormal.ToCompactString());
			UE_LOG(LogAIDA, Log, TEXT("[actions][dbg] hologram=%s IsValidHitResult=%d disabled=%d"),
				*GetNameSafe(Hologram->GetClass()), Hologram->IsValidHitResult(Hit) ? 1 : 0, Hologram->IsDisabled() ? 1 : 0);
		}

		// The disqualifier list only ever ACCUMULATES — the build gun resets it every frame before
		// re-validating. Without this, the spawn-time defaults (Initializing + InvalidAimLocation)
		// stick forever and every placement reports "Invalid aim location!" (live-verify round 4).
		Hologram->ResetConstructDisqualifiers();

		// Drive the hologram's own placement pipeline (what the build gun calls per frame) — it wraps
		// Pre/SetLocationAndRotation/Post plus internal bookkeeping the raw calls skip.
		Hologram->SetScrollRotateValue(FMath::RoundToInt32(Placement.Rotator().Yaw));
		Hologram->UpdateHologramPlacement(Hit);

		// Public validation entry (CheckValidPlacement/CheckClearance are protected). The inventory is
		// only consumed by CheckCanAfford — which hard-asserts on null — and its Unaffordable verdict
		// is filtered below; affordability is judged against central storage by the callers.
		Hologram->ValidatePlacementAndCost(Inventory);

		TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
		Hologram->GetConstructDisqualifiers(Disqualifiers);

		if (bVerboseLog)
		{
			UE_LOG(LogAIDA, Log, TEXT("[actions][dbg] hologram now at %s (target %s); %d disqualifier(s):"),
				*Hologram->GetActorLocation().ToCompactString(), *Target.ToCompactString(), Disqualifiers.Num());
			for (const TSubclassOf<UFGConstructDisqualifier>& Disqualifier : Disqualifiers)
			{
				UE_LOG(LogAIDA, Log, TEXT("[actions][dbg]   - %s (%s)%s"),
					*GetNameSafe(Disqualifier.Get()),
					*UFGConstructDisqualifier::GetDisqualifyingText(Disqualifier).ToString(),
					IsBlockingDisqualifier(Disqualifier) ? TEXT(" [BLOCKING]")
						: (IsClippingDisqualifier(Disqualifier) ? TEXT(" [clipping — advisory]") : TEXT(" [filtered]")));
			}
		}

		bool bValid = true;
		for (const TSubclassOf<UFGConstructDisqualifier>& Disqualifier : Disqualifiers)
		{
			if (OutClipping && IsClippingDisqualifier(Disqualifier))
			{
				*OutClipping = true;
			}
			if (bValid && IsBlockingDisqualifier(Disqualifier))
			{
				if (OutBlocking) { *OutBlocking = Disqualifier; }
				bValid = false;
			}
		}
		if (!OutTempFloor && TempFloor)
		{
			TempFloor->Destroy(); // nobody claimed it — validation is done with it
		}
		return bValid;
	}

	AFGHologram* SpawnValidationHologram(UWorld* World, UClass* RecipeClass, const FVector& At)
	{
		// Non-replicated (clients never see a ghost); destroyed by the caller before returning.
		AFGHologram* Hologram = AFGHologram::SpawnHologramFromRecipe(RecipeClass, World->GetWorldSettings(), At,
			/*hologramInstigator*/ nullptr,
			[](AFGHologram* PreSpawn) { PreSpawn->SetReplicates(false); });
		if (Hologram)
		{
			// The build gun applies a build mode right after spawning; hologram placement logic
			// (foundations especially) expects one. Force the hologram's own default.
			Hologram->SetBuildModeOverride(Hologram->GetDefaultBuildGunMode());
		}
		return Hologram;
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

	const FString Wanted = NormalizeName(DisplayName);
	if (Wanted.IsEmpty()) { return false; }

	// Exact (normalized) match wins; otherwise a UNIQUE substring match — in EITHER direction: the
	// model often over-specifies ("Pipeline Junction Cross" for a building named "Pipeline
	// Junction"; live-verify: zero matches AND zero suggestions read as "not unlocked"). Reverse
	// candidates need some length so "Junction Deluxe" can't claim a query for "junk".
	const FBuildRecipeEntry* Exact = nullptr;
	TArray<const FBuildRecipeEntry*> Partial;
	TArray<const FBuildRecipeEntry*> Reverse;
	for (const FBuildRecipeEntry& Entry : Entries)
	{
		const FString Candidate = NormalizeName(Entry.Name);
		if (Candidate == Wanted) { Exact = &Entry; break; }
		if (Candidate.Contains(Wanted)) { Partial.Add(&Entry); }
		else if (Candidate.Len() >= 8 && Wanted.Contains(Candidate)) { Reverse.Add(&Entry); }
	}
	const FBuildRecipeEntry* Chosen = Exact ? Exact
		: (Partial.Num() == 1 ? Partial[0] : (Partial.Num() == 0 && Reverse.Num() == 1 ? Reverse[0] : nullptr));
	if (!Chosen)
	{
		for (const FBuildRecipeEntry* Candidate : Partial)
		{
			if (Out.Suggestions.Num() >= MaxSuggestions) { break; }
			Out.Suggestions.AddUnique(Candidate->Name);
		}
		for (const FBuildRecipeEntry* Candidate : Reverse)
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

namespace
{
	/** The requesting player's build-gun-channel aim hit (150 m reach). Empty-id = the listen host. */
	bool TraceAimHit(UWorld* World, const FString& PlayerId, FHitResult& OutHit, FVector* OutViewLocation = nullptr)
	{
		if (!World || PlayerId == TEXT("debug")) { return false; }

		// Same identity convention as the orchestrator's location resolver: the listen-server host's
		// net id resolves to null → an empty id string matches the empty-id controller.
		APlayerController* Requester = nullptr;
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			APlayerState* PS = PC ? PC->PlayerState : nullptr;
			if (!PS) { continue; }
			const TSharedPtr<const FUniqueNetId> NetId = PS->GetUniqueId().GetUniqueNetId();
			const FString Id = NetId.IsValid() ? NetId->ToString() : FString();
			if (Id == PlayerId)
			{
				Requester = PC;
				break;
			}
		}
		if (!Requester) { return false; }

		FVector ViewLocation;
		FRotator ViewRotation;
		Requester->GetPlayerViewPoint(ViewLocation, ViewRotation);

		FCollisionQueryParams Params(SCENE_QUERY_STAT(AIDAAimTrace), /*bTraceComplex*/ false);
		if (APawn* Pawn = Requester->GetPawn()) { Params.AddIgnoredActor(Pawn); }

		if (OutViewLocation) { *OutViewLocation = ViewLocation; }
		if (!World->LineTraceSingleByChannel(OutHit, ViewLocation, ViewLocation + ViewRotation.Vector() * 15000.0, AIDABuildGunChannel, Params))
		{
			UE_LOG(LogAIDA, Log, TEXT("[actions][dbg] aim trace MISSED (view=%s dir=%s)"),
				*ViewLocation.ToCompactString(), *ViewRotation.Vector().ToCompactString());
			return false;
		}
		UE_LOG(LogAIDA, Log, TEXT("[actions][dbg] aim hit %s (%s) at %s"),
			*GetNameSafe(OutHit.GetActor()), OutHit.GetActor() ? *GetNameSafe(OutHit.GetActor()->GetClass()) : TEXT("-"),
			*OutHit.ImpactPoint.ToCompactString());
		return true;
	}
}

bool FAIDAActionSeam::ResolveAimPoint(UObject* WorldContext, const FString& PlayerId, FVector& OutPointCm)
{
	FHitResult Hit;
	if (!TraceAimHit(ResolveWorld(WorldContext), PlayerId, Hit))
	{
		return false;
	}
	OutPointCm = Hit.ImpactPoint;
	return true;
}

bool FAIDAActionSeam::ResolveAimSnappedOrigin(UObject* WorldContext, const FString& PlayerId,
	const FString& RecipeClassPath, int32& InOutYawDeg,
	int32 CountX, int32 CountY, double StepXCm, double StepYCm, FVector& OutOriginCm,
	TArray<FVector>* OutAlternateOrigins)
{
	if (OutAlternateOrigins) { OutAlternateOrigins->Reset(); }
	UWorld* World = ResolveWorld(WorldContext);
	FHitResult Hit;
	FVector ViewLocation = FVector::ZeroVector;
	if (!TraceAimHit(World, PlayerId, Hit, &ViewLocation))
	{
		return false;
	}
	OutOriginCm = Hit.ImpactPoint;

	// Lightweight foundations/walls are instanced — the hit actor is the AbstractInstanceManager,
	// which hologram snapping can't snap to (live-verify: "snap" moved the origin ~1 cm). Resolve
	// the instance and stand up a temporary buildable actor, exactly like the build gun does, so
	// TrySnapToActor sees a real foundation.
	bool bAimedAtStructure = Cast<AFGBuildable>(Hit.GetActor()) != nullptr;
	AFGBuildable* TempBuildable = ResolveInstanceHit(Hit);
	bAimedAtStructure |= TempBuildable != nullptr;

	// Probe hologram at the REAL aim hit: UpdateHologramPlacement runs the game's snapping
	// (TrySnapToActor against the aimed structure, world-grid alignment), and where the hologram
	// lands — position AND rotation — is where the build gun would have put it. The snapped yaw
	// becomes the grid's yaw so every row shares the aimed structure's lattice.
	FVector Snapped = Hit.ImpactPoint;
	UClass* RecipeClass = FSoftClassPath(RecipeClassPath).TryLoadClass<UFGRecipe>();
	AFGHologram* Hologram = RecipeClass ? SpawnValidationHologram(World, RecipeClass, Hit.ImpactPoint) : nullptr;
	if (Hologram)
	{
		Hologram->SetScrollRotateValue(InOutYawDeg);
		Hologram->UpdateHologramPlacement(Hit);
		Snapped = Hologram->GetActorLocation();
		InOutYawDeg = FMath::RoundToInt32(Hologram->GetActorRotation().Yaw);
		Hologram->Destroy();
	}
	if (TempBuildable)
	{
		TempBuildable->Destroy();
	}

	// Anchor + growth (user-specified semantics): the grid grows OUT OF THE AIMED SURFACE, toward
	// the player — a side face's normal says exactly which way "out" is; a top face (normal ~up)
	// grows toward the player instead. Terrain aims just centre the grid on the aim (a stamp).
	const double YawRad = FMath::DegreesToRadians(static_cast<double>(InOutYawDeg));
	const FVector AxisX(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.0);
	const FVector AxisY(-FMath::Sin(YawRad), FMath::Cos(YawRad), 0.0);
	const FVector CenterX = AxisX * (((CountX - 1) / 2) * StepXCm);
	const FVector CenterY = AxisY * (((CountY - 1) / 2) * StepYCm);

	if (!bAimedAtStructure)
	{
		OutOriginCm = Snapped - CenterX - CenterY;
	}
	else
	{
		FVector GrowDir = Hit.ImpactNormal;
		GrowDir.Z = 0.0;
		const bool bSideAim = GrowDir.SizeSquared() >= 0.25; // horizontal-ish normal = a side face
		if (!bSideAim) // top/bottom face: rows run "coming toward me"
		{
			GrowDir = ViewLocation - Hit.ImpactPoint;
			GrowDir.Z = 0.0;
		}
		if (!GrowDir.Normalize()) { GrowDir = AxisX; }

		// Pick the lattice axis closest to the growth direction.
		const double DX = GrowDir | AxisX;
		const double DY = GrowDir | AxisY;
		const bool bAlongX = FMath::Abs(DX) >= FMath::Abs(DY);
		const double Sign = (bAlongX ? DX : DY) >= 0.0 ? 1.0 : -1.0;
		const FVector GrowthAxis = (bAlongX ? AxisX : AxisY) * Sign;
		const int32 CountAlong = bAlongX ? CountX : CountY;
		const double StepAlong = bAlongX ? StepXCm : StepYCm;

		// SIDE-face aims extend the structure: first tile = the cell adjacent to the aimed face —
		// unless the snap already stepped out (holograms aimed at a side face snap to the
		// neighbouring cell themselves). TOP-face aims mean "place it HERE on this surface"
		// (machines on a platform, stacking) — no step-out; the aimed cell is tile 0 and the row
		// runs toward the player (live-verify: assemblers got shoved 16 m off the platform).
		FVector Anchor = Snapped;
		if (bSideAim && ((Snapped - Hit.ImpactPoint) | GrowthAxis) < StepAlong * 0.25)
		{
			Anchor += GrowthAxis * StepAlong;
		}

		// Span the growth axis away from the face; centre the perpendicular axis on the anchor.
		OutOriginCm = Anchor;
		if (Sign < 0.0)
		{
			OutOriginCm += GrowthAxis * ((CountAlong - 1) * StepAlong); // +axis expansion must END at the anchor
		}
		OutOriginCm -= bAlongX ? CenterY : CenterX;

		// Fallback anchors for the caller to dry-run when the toward-player bet loses (top faces
		// only — see the header note). Same anchor cell, different spans: centered, then away.
		if (OutAlternateOrigins && !bSideAim && CountAlong > 1)
		{
			const FVector UnsignedAxis = bAlongX ? AxisX : AxisY;
			const FVector Perp = bAlongX ? CenterY : CenterX;
			const FVector Centered = Anchor - UnsignedAxis * (((CountAlong - 1) / 2) * StepAlong) - Perp;
			FVector Away = Anchor - Perp;
			if (Sign > 0.0)
			{
				Away -= UnsignedAxis * ((CountAlong - 1) * StepAlong); // mirror of the Sign<0 case above
			}
			OutAlternateOrigins->AddUnique(Centered);
			OutAlternateOrigins->AddUnique(Away);
			OutAlternateOrigins->Remove(OutOriginCm); // paranoia: never duplicate the primary
		}
	}

	UE_LOG(LogAIDA, Log, TEXT("[actions][dbg] aim origin snapped %s -> %s (anchored %s) yaw=%d structure=%d normal=%s"),
		*Hit.ImpactPoint.ToCompactString(), *Snapped.ToCompactString(), *OutOriginCm.ToCompactString(),
		InOutYawDeg, bAimedAtStructure ? 1 : 0, *Hit.ImpactNormal.ToCompactString());
	return true;
}

bool FAIDAActionSeam::ProbeGroundZ(UObject* WorldContext, const FVector& AtCm, double& OutGroundZCm)
{
	UWorld* World = ResolveWorld(WorldContext);
	if (!World) { return false; }

	FCollisionQueryParams Params(SCENE_QUERY_STAT(AIDAGroundProbe), /*bTraceComplex*/ false);
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		if (APawn* Pawn = It->Get() ? It->Get()->GetPawn() : nullptr) { Params.AddIgnoredActor(Pawn); }
	}

	// Low first (roofs over the area must not win), then from high on rising terrain. Conveyors are
	// never ground — a manifold's trunk points must land on the floor, not a belt running under it.
	const FVector End = AtCm - FVector(0.0, 0.0, 10000.0);
	FHitResult Hit;
	if (!TraceGroundIgnoringConveyors(World, AtCm + FVector(0.0, 0.0, 300.0), End, Params, Hit) &&
		!TraceGroundIgnoringConveyors(World, AtCm + FVector(0.0, 0.0, 5000.0), End, Params, Hit))
	{
		return false;
	}
	OutGroundZCm = Hit.Location.Z;
	return true;
}

bool FAIDAActionSeam::DryRunBuild(UObject* WorldContext, const FString& RecipeClassPath,
	const TArray<FTransform>& Placements, FAIDADryRunResult& Out, const FString& PayerPlayerId)
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

	UFGInventoryComponent* Inventory = FindValidationInventory(World);
	if (!Inventory)
	{
		Out.Error = TEXT("no player inventory available to validate against (is anyone connected?)");
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

	FHitResult TemplateHit;
	for (int32 Index = 0; Index < Placements.Num(); ++Index)
	{
		TSubclassOf<UFGConstructDisqualifier> Blocking;
		bool bClipping = false;
		// Verbose diagnostics for the first placements only (a 200-tile grid must not spam the log).
		if (!PlaceAndValidate(Hologram, Placements[Index], Inventory, &Blocking, /*bVerboseLog*/ Index < 2,
			&TemplateHit, /*OutTempFloor*/ nullptr, &bClipping))
		{
			FAIDAPlacementFailure Failure;
			Failure.Index = Index;
			Failure.AtM = Placements[Index].GetLocation() / AIDAMetersToCm;
			Failure.Reason = UFGConstructDisqualifier::GetDisqualifyingText(Blocking).ToString();
			Out.Failures.Add(MoveTemp(Failure));
		}
		else if (bClipping)
		{
			++Out.ClippingCount; // advisory (never a failure): manifold rows prefer a clean lane
		}
	}

	// Cost: per-construct cost × placement count (identical placements share one recipe).
	const TArray<FItemAmount> PerConstruct = Hologram->GetCost(/*includeChildren*/ true);
	for (const FItemAmount& Entry : PerConstruct)
	{
		AddCost(Out.Cost, Entry.ItemClass, Entry.Amount * Placements.Num());
	}

	// Affordability vs central storage + (when known) the payer's pocket inventory.
	Out.bAffordable = CheckAffordable(WorldContext, Out.Cost, PayerPlayerId);

	Hologram->Destroy();
	Out.bOk = Out.Failures.Num() == 0;
	return true;
}

bool FAIDAActionSeam::DryRunBuildParts(UObject* WorldContext, const TArray<FString>& PartRecipePaths,
	const TArray<int32>& PlacementPartIndex, const TArray<FTransform>& Placements, FAIDADryRunResult& Out,
	const FString& PayerPlayerId)
{
	Out = FAIDADryRunResult();
	if (PlacementPartIndex.Num() != Placements.Num() || PartRecipePaths.Num() == 0)
	{
		Out.Error = TEXT("internal: part index map out of sync");
		return false;
	}

	// One DryRunBuild per part over its slice; failures/indices are remapped back to global.
	for (int32 PartIdx = 0; PartIdx < PartRecipePaths.Num(); ++PartIdx)
	{
		TArray<FTransform> Slice;
		TArray<int32> GlobalIndex;
		for (int32 i = 0; i < Placements.Num(); ++i)
		{
			if (PlacementPartIndex[i] == PartIdx)
			{
				Slice.Add(Placements[i]);
				GlobalIndex.Add(i);
			}
		}
		if (Slice.Num() == 0) { continue; }

		FAIDADryRunResult PartRun;
		if (!DryRunBuild(WorldContext, PartRecipePaths[PartIdx], Slice, PartRun))
		{
			Out.Error = PartRun.Error;
			return false;
		}
		for (FAIDAPlacementFailure& Failure : PartRun.Failures)
		{
			Failure.Index = GlobalIndex.IsValidIndex(Failure.Index) ? GlobalIndex[Failure.Index] : Failure.Index;
			Out.Failures.Add(MoveTemp(Failure));
		}
		Out.ClippingCount += PartRun.ClippingCount;
		for (const FAIDACostItem& Item : PartRun.Cost)
		{
			bool bMerged = false;
			for (FAIDACostItem& Existing : Out.Cost)
			{
				if (Existing.ClassPath == Item.ClassPath) { Existing.Amount += Item.Amount; bMerged = true; break; }
			}
			if (!bMerged) { Out.Cost.Add(Item); }
		}
	}

	// Per-part affordability checks can each pass while the SUM does not — re-check the merged tally.
	Out.bAffordable = CheckAffordable(WorldContext, Out.Cost, PayerPlayerId);
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

	const FString WantedName = NormalizeName(Selector.Buildable);
	const auto MatchesName = [&WantedName](const FString& Name)
	{
		return WantedName.IsEmpty() || NormalizeName(Name).Contains(WantedName);
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

UFGInventoryComponent* FAIDAActionSeam::ResolvePlayerInventory(UWorld* World, const FString& PlayerId,
	bool bFallbackToAny)
{
	if (!World) { return nullptr; }

	UFGInventoryComponent* Fallback = nullptr;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
		if (!Character || !Character->GetInventory()) { continue; }
		if (!Fallback) { Fallback = Character->GetInventory(); }

		// The LISTEN HOST's net id resolves null — an empty PlayerId matches it (allowlist convention).
		APlayerState* PS = PC->PlayerState;
		const TSharedPtr<const FUniqueNetId> NetId = PS ? PS->GetUniqueId().GetUniqueNetId() : nullptr;
		const FString ControllerId = NetId.IsValid() ? NetId->ToString() : FString();
		if (ControllerId == PlayerId)
		{
			return Character->GetInventory();
		}
	}
	return bFallbackToAny ? Fallback : nullptr;
}

bool FAIDAActionSeam::DeductCost(UObject* WorldContext, const TArray<FAIDACostItem>& Cost,
	const FString& PayerPlayerId)
{
	UWorld* World = ResolveWorld(WorldContext);
	AFGCentralStorageSubsystem* Central = World ? AFGCentralStorageSubsystem::Get(World) : nullptr;
	UFGInventoryComponent* Pockets = World ? ResolvePlayerInventory(World, PayerPlayerId) : nullptr;
	if (!Central && !Pockets) { return false; }

	// Verify the WHOLE tally first (central + payer pockets), then deduct — never a partial payment.
	TArray<TPair<TSubclassOf<UFGItemDescriptor>, int32>> Lines;
	for (const FAIDACostItem& Item : Cost)
	{
		const TSubclassOf<UFGItemDescriptor> Descriptor = LoadDescriptor(Item.ClassPath);
		if (!Descriptor) { return false; }
		const int32 Available = (Central ? Central->GetNumItemsFromCentralStorage(Descriptor) : 0)
			+ (Pockets ? Pockets->GetNumItems(Descriptor) : 0);
		if (Available < Item.Amount)
		{
			return false;
		}
		Lines.Emplace(Descriptor, Item.Amount);
	}
	for (const auto& Line : Lines)
	{
		// Central storage first (the depot exists to feed building); the payer's pockets cover the rest.
		int32 Remaining = Line.Value;
		if (Central)
		{
			const int32 FromCentral = FMath::Min(Remaining, Central->GetNumItemsFromCentralStorage(Line.Key));
			if (FromCentral > 0)
			{
				Remaining -= Central->TryRemoveItemsFromCentralStorage(Line.Key, FromCentral);
			}
		}
		if (Remaining > 0 && Pockets)
		{
			Pockets->Remove(Line.Key, Remaining);
			Remaining = 0;
		}
		if (Remaining > 0)
		{
			// Should not happen after the verify pass; log loudly, keep going (items already left).
			UE_LOG(LogAIDA, Error, TEXT("[actions] cost deduction shortfall: %d %s unpaid"),
				Remaining, *GetNameSafe(Line.Key.Get()));
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
	UFGInventoryComponent* Inventory = ResolvePlayerInventory(World, PlayerId, /*bFallbackToAny*/ true);
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
	TArray<FString>& OutEntityIds, TArray<TWeakObjectPtr<AActor>>& OutActors, int32& OutSkipped,
	TArray<TWeakObjectPtr<AActor>>* OutPerIndexActors)
{
	OutSkipped = 0;

	// Index-aligned callers (manifolds) get one slot per consumed index no matter how it ends.
	const auto RecordIndex = [OutPerIndexActors](AActor* Actor)
	{
		if (OutPerIndexActors) { OutPerIndexActors->Add(Actor); }
	};

	UWorld* World = ResolveWorld(WorldContext);
	UClass* RecipeClass = World ? FSoftClassPath(RecipeClassPath).TryLoadClass<UFGRecipe>() : nullptr;
	if (!RecipeClass)
	{
		// Consume everything so the executor terminates instead of spinning on a dead recipe.
		OutSkipped = Placements.Num() - Cursor;
		for (int32 i = 0; i < OutSkipped; ++i) { RecordIndex(nullptr); }
		return OutSkipped;
	}

	UFGInventoryComponent* Inventory = FindValidationInventory(World);
	if (!Inventory)
	{
		OutSkipped = Placements.Num() - Cursor; // nobody connected to validate against — terminate
		for (int32 i = 0; i < OutSkipped; ++i) { RecordIndex(nullptr); }
		return OutSkipped;
	}

	FHitResult TemplateHit;
	const int32 End = FMath::Min(Cursor + BatchSize, Placements.Num());
	for (int32 Index = Cursor; Index < End; ++Index)
	{
		// Fresh hologram per construct — Construct() consumes the hologram's configured state.
		AFGHologram* Hologram = SpawnValidationHologram(World, RecipeClass, Placements[Index].GetLocation());
		if (!Hologram) { ++OutSkipped; RecordIndex(nullptr); continue; }

		// Re-validate: the world may have changed since the dry-run (players build/walk around).
		// The temp floor (lightweight-instance tiles) must outlive Construct — the hologram caches
		// the snapped floor it validated against.
		AFGBuildable* TempFloor = nullptr;
		if (!PlaceAndValidate(Hologram, Placements[Index], Inventory, nullptr, false, &TemplateHit, &TempFloor))
		{
			Hologram->Destroy();
			if (TempFloor) { TempFloor->Destroy(); }
			++OutSkipped;
			RecordIndex(nullptr);
			continue;
		}

		TArray<AActor*> Children;
		AActor* Built = Hologram->Construct(Children, FNetConstructionID());
		Hologram->Destroy();
		if (TempFloor) { TempFloor->Destroy(); }
		if (!Built) { ++OutSkipped; RecordIndex(nullptr); continue; }

		// Capture the undo handle (docs/PHASE4.md §2d). Lightweight-bound buildables are destroyed
		// and migrated to the instance subsystem right after Construct — journal class+transform
		// (index unknown here; undo re-resolves by transform). Full actors keep a weak ptr too.
		// The journaled transform is the BUILT actor's, not the intended placement — the hologram
		// may snap/adjust, and undo's transform match must hit what actually exists (live-verify:
		// grounded tiles drifted metres from the plan and undo missed 8 of 9).
		AFGBuildable* Buildable = Cast<AFGBuildable>(Built);
		FAIDAEntityId Entity;
		Entity.ClassPath = Built->GetClass()->GetPathName();
		Entity.Pos = Built->GetActorLocation();
		Entity.YawDeg = FMath::RoundToInt32(Built->GetActorRotation().Yaw);
		if (Buildable && Buildable->ShouldConvertToLightweight())
		{
			Entity.Type = TEXT("lw");
			Entity.Index = INDEX_NONE;
			RecordIndex(nullptr); // the actor dies on conversion — index-aligned callers see a miss
		}
		else
		{
			Entity.Type = TEXT("actor");
			Entity.RecipePath = RecipeClassPath;
			OutActors.Add(Built);
			RecordIndex(Built);
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

	const FString WantedName = NormalizeName(Selector.Buildable);
	const auto MatchesName = [&WantedName](const FString& Name)
	{
		return WantedName.IsEmpty() || NormalizeName(Name).Contains(WantedName);
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

AActor* FAIDAActionSeam::SpawnGhostHologram(UObject* WorldContext, const FString& RecipeClassPath,
	const FVector& CenterCm, float YawDeg, AActor* Owner)
{
	UWorld* World = ResolveWorld(WorldContext);
	UClass* RecipeClass = World ? FSoftClassPath(RecipeClassPath).TryLoadClass<UFGRecipe>() : nullptr;
	if (!RecipeClass) { return nullptr; }

	AFGHologram* Hologram = AFGHologram::SpawnHologramFromRecipe(RecipeClass,
		Owner ? Owner : World->GetWorldSettings(), CenterCm, /*hologramInstigator*/ nullptr,
		[](AFGHologram* PreSpawn) { PreSpawn->SetReplicates(false); });
	if (!Hologram) { return nullptr; }

	Hologram->SetBuildModeOverride(Hologram->GetDefaultBuildGunMode());
	Hologram->SetScrollRotateValue(FMath::RoundToInt32(YawDeg));

	FHitResult Hit(ForceInit);
	Hit.bBlockingHit = true;
	Hit.Location = CenterCm;
	Hit.ImpactPoint = CenterCm;
	Hit.Normal = FVector::UpVector;
	Hit.ImpactNormal = FVector::UpVector;
	Hologram->UpdateHologramPlacement(Hit);
	Hologram->ResetConstructDisqualifiers(); // ghost is display-only; keep the valid-placement look

	// Display-only: kill ticking and collision on the hologram, its children, and every component.
	// Factory-building holograms run real per-tick work meant for one build-gun frame at a time —
	// left idling they hung the game (live-verify: assembler ghosts froze the session).
	const auto Quiesce = [](AActor* Actor)
	{
		Actor->SetActorEnableCollision(false);
		Actor->SetActorTickEnabled(false);
		Actor->ForEachComponent<UActorComponent>(false, [](UActorComponent* Component)
		{
			Component->SetComponentTickEnabled(false);
		});
	};
	Quiesce(Hologram);
	TArray<AActor*> Attached;
	Hologram->GetAttachedActors(Attached, /*bResetArray*/ true, /*bRecursivelyIncludeAttachedActors*/ true);
	for (AActor* Child : Attached)
	{
		Quiesce(Child);
	}
	return Hologram;
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

	UFGInventoryComponent* Inventory = FindValidationInventory(World);
	if (!Inventory) { return false; }

	AFGHologram* Hologram = SpawnValidationHologram(World, RecipeClass, Entity.Pos);
	if (!Hologram) { return false; }

	const FTransform Placement(FRotator(0.0, static_cast<double>(Entity.YawDeg), 0.0), Entity.Pos);
	if (!PlaceAndValidate(Hologram, Placement, Inventory))
	{
		Hologram->Destroy(); // something occupies the spot now — a reported partial-undo, not fatal
		return false;
	}
	TArray<AActor*> Children;
	AActor* Built = Hologram->Construct(Children, FNetConstructionID());
	Hologram->Destroy();
	return Built != nullptr;
}

namespace
{
	/** Buildable's canonical display name via its built-with recipe (the dismantle walk's convention). */
	FString BuildableDisplayName(AFGBuildable* Buildable)
	{
		const TSubclassOf<UFGRecipe> Recipe = Buildable->GetBuiltWithRecipe();
		const TArray<FItemAmount> Products = Recipe ? UFGRecipe::GetProducts(Recipe) : TArray<FItemAmount>();
		return Products.Num() > 0 ? DescriptorName(Products[0].ItemClass) : GetNameSafe(Buildable->GetClass());
	}

	/** Best unconnected factory (belt) port on a live actor: direction + best normal-dot with
	 *  WantDir, at least 60° aligned — a run must never leave from the far side of an attachment. */
	UFGFactoryConnectionComponent* FindFactoryPort(AActor* Actor, bool bWantOutput, const FVector& WantDir)
	{
		const FVector Wanted = FVector(WantDir.X, WantDir.Y, 0.0).GetSafeNormal();
		UFGFactoryConnectionComponent* Best = nullptr;
		double BestDot = 0.5;
		TInlineComponentArray<UFGFactoryConnectionComponent*> Connections;
		Actor->GetComponents(Connections);
		for (UFGFactoryConnectionComponent* Conn : Connections)
		{
			if (!Conn || Conn->IsConnected()) { continue; }
			const EFactoryConnectionDirection Dir = Conn->GetDirection();
			const bool bMatches = bWantOutput
				? (Dir == EFactoryConnectionDirection::FCD_OUTPUT || Dir == EFactoryConnectionDirection::FCD_ANY)
				: (Dir == EFactoryConnectionDirection::FCD_INPUT || Dir == EFactoryConnectionDirection::FCD_ANY);
			if (!bMatches) { continue; }
			const FVector Normal = Conn->GetConnectorNormal();
			const double Dot = FVector::DotProduct(FVector(Normal.X, Normal.Y, 0.0).GetSafeNormal(), Wanted);
			if (Dot > BestDot)
			{
				BestDot = Dot;
				Best = Conn;
			}
		}
		return Best;
	}

	/** Pipe twin of FindFactoryPort. Pipes are direction-agnostic (PCT_ANY junctions ↔ typed machine
	 *  ports); only snap-only ports are excluded. */
	UFGPipeConnectionComponent* FindPipePort(AActor* Actor, const FVector& WantDir)
	{
		const FVector Wanted = FVector(WantDir.X, WantDir.Y, 0.0).GetSafeNormal();
		UFGPipeConnectionComponent* Best = nullptr;
		double BestDot = 0.5;
		TInlineComponentArray<UFGPipeConnectionComponent*> Connections;
		Actor->GetComponents(Connections);
		for (UFGPipeConnectionComponent* Conn : Connections)
		{
			if (!Conn || Conn->IsConnected()) { continue; }
			if (Conn->GetPipeConnectionType() == EPipeConnectionType::PCT_SNAP_ONLY) { continue; }
			const FVector Normal = Conn->GetConnectorNormal();
			const double Dot = FVector::DotProduct(FVector(Normal.X, Normal.Y, 0.0).GetSafeNormal(), Wanted);
			if (Dot > BestDot)
			{
				BestDot = Dot;
				Best = Conn;
			}
		}
		return Best;
	}

	/** Synthetic build-gun hit AT a connector, shaped like a player aiming into the port face along
	 *  its outward normal, so a spline hologram's TrySnapToActor finds the port. */
	FHitResult MakePortHit(AActor* Actor, const FVector& ConnectorLoc, const FVector& ConnectorNormal)
	{
		const FVector Normal = FVector(ConnectorNormal.X, ConnectorNormal.Y, 0.0).GetSafeNormal(
			UE_SMALL_NUMBER, FVector::XAxisVector);
		FHitResult Hit(ForceInit);
		Hit.bBlockingHit = true;
		Hit.HitObjectHandle = FActorInstanceHandle(Actor);
		// A real primitive matters: built factory actors often root on a plain scene component, and
		// engine snap code reads the hit component (live-verify: no snap with a null component).
		UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
		if (!Prim) { Prim = Actor->FindComponentByClass<UPrimitiveComponent>(); }
		if (Prim) { Hit.Component = Prim; }
		Hit.Location = ConnectorLoc;
		Hit.ImpactPoint = ConnectorLoc;
		Hit.Normal = Normal;
		Hit.ImpactNormal = Normal;
		Hit.TraceStart = ConnectorLoc + Normal * 300.0 + FVector(0.0, 0.0, 150.0);
		Hit.TraceEnd = ConnectorLoc - Normal * 100.0;
		Hit.Distance = (Hit.TraceStart - ConnectorLoc).Size();
		return Hit;
	}

	/**
	 * Did the spline hologram actually snap its current end to the expected actor's port? Pipeline
	 * holograms answer via IsConnectionSnapped; CONVEYOR holograms don't override it (the base
	 * returns false even when snapped — live-verify: every belt run "failed" its start snap), so
	 * belts are asked through their own GetAnyConnectedBuildables. Logs the full state on failure.
	 */
	bool VerifyRunSnap(AFGSplineHologram* Spline, AActor* Expected, bool bLastConnection, const FVector& PortLoc)
	{
		bool bSnapped = false;
		if (AFGConveyorBeltHologram* Belt = Cast<AFGConveyorBeltHologram>(Spline))
		{
			TArray<AFGBuildable*> Connected = Belt->GetAnyConnectedBuildables();
			bSnapped = Connected.Contains(Cast<AFGBuildable>(Expected));
			if (!bSnapped)
			{
				FString Names;
				for (AFGBuildable* B : Connected) { Names += GetNameSafe(B) + TEXT(" "); }
				UE_LOG(LogAIDA, Warning, TEXT("[actions][dbg] belt snap check failed: want=%s got=[%s] step=%d holoLoc=%s port=%s"),
					*GetNameSafe(Expected), *Names, static_cast<int32>(Belt->GetCurrentBuildStep()),
					*Belt->GetActorLocation().ToCompactString(), *PortLoc.ToCompactString());
			}
		}
		else
		{
			bSnapped = Spline->IsConnectionSnapped(bLastConnection);
			if (!bSnapped)
			{
				UE_LOG(LogAIDA, Warning, TEXT("[actions][dbg] pipe snap check failed: want=%s step=%d holoLoc=%s port=%s"),
					*GetNameSafe(Expected), static_cast<int32>(Spline->GetCurrentBuildStep()),
					*Spline->GetActorLocation().ToCompactString(), *PortLoc.ToCompactString());
			}
		}
		return bSnapped;
	}
}

namespace
{
	/**
	 * Wire a constructed belt to its endpoint ports DIRECTLY — the same thing the game's blueprint
	 * paste does for open connections (FGBlueprintOpenFactoryConnectionManager). Engine hologram
	 * snapping never latched onto our synthetic hits (live-verify: hologram AT the port, zero
	 * snapped connections), so the connection layer is authored here: belt flow runs
	 * mConnection0 → mConnection1, so conn0 must marry the source (From) port and conn1 the
	 * destination — verified with CanConnectTo before touching anything. Idempotent per end (an end
	 * the hologram DID snap is already connected and is skipped). False = report + let the caller
	 * dismantle; never half-wires: a one-end failure disconnects what this call just connected.
	 */
	bool ConnectBeltEnds(AFGBuildableConveyorBase* Belt,
		UFGFactoryConnectionComponent* FromConn, UFGFactoryConnectionComponent* ToConn, FString& OutError)
	{
		UFGFactoryConnectionComponent* C0 = Belt->GetConnection0();
		UFGFactoryConnectionComponent* C1 = Belt->GetConnection1();
		if (!C0 || !C1)
		{
			OutError = TEXT("constructed belt has no connection components");
			return false;
		}

		// Orientation check: the belt's input (conn0) belongs at the SOURCE port. A reversed belt
		// would run backwards — refuse it (CanConnectTo also enforces direction compatibility).
		const double D0From = FVector::DistSquared(C0->GetConnectorLocation(), FromConn->GetConnectorLocation());
		const double D1From = FVector::DistSquared(C1->GetConnectorLocation(), FromConn->GetConnectorLocation());
		if (D1From < D0From)
		{
			OutError = TEXT("belt constructed reversed (output end at the source port)");
			return false;
		}

		bool bConnectedStart = false;
		if (!C0->IsConnected() && !FromConn->IsConnected())
		{
			if (!C0->CanConnectTo(FromConn))
			{
				OutError = TEXT("belt input end refuses the source port (direction mismatch)");
				return false;
			}
			C0->SetConnection(FromConn);
			bConnectedStart = true;
		}
		if (!C1->IsConnected() && !ToConn->IsConnected())
		{
			if (!C1->CanConnectTo(ToConn))
			{
				if (bConnectedStart) { C0->ClearConnection(); }
				OutError = TEXT("belt output end refuses the destination port (direction mismatch)");
				return false;
			}
			C1->SetConnection(ToConn);
		}

		if (!FromConn->IsConnected() || !ToConn->IsConnected())
		{
			// One port got taken between resolve and now (player built something) — undo our half.
			if (bConnectedStart) { C0->ClearConnection(); }
			OutError = TEXT("an endpoint port was already taken");
			return false;
		}
		return true;
	}
}

bool FAIDAActionSeam::ResolveMachinePorts(UObject* WorldContext, const FAIDADismantleSpec& Selector,
	bool bPipe, bool bOutput, int32 PortIndex,
	TArray<FAIDAManifoldPort>& OutPorts, int32& OutSkippedConnected, FString& OutMachineName)
{
	OutPorts.Reset();
	OutSkippedConnected = 0;
	OutMachineName.Reset();

	UWorld* World = ResolveWorld(WorldContext);
	AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
	const FString WantedName = NormalizeName(Selector.Buildable);
	if (!Subsystem || WantedName.IsEmpty()) { return false; }

	const FVector CenterCm = Selector.CenterM * AIDAMetersToCm;
	const double RadiusSq = FMath::Square(Selector.RadiusM * AIDAMetersToCm);
	const int32 MaxCount = Selector.MaxCount > 0 ? Selector.MaxCount : MAX_int32;

	for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
	{
		if (OutPorts.Num() >= MaxCount) { break; }
		if (!IsValid(Buildable)) { continue; }
		if (FVector::DistSquared(Buildable->GetActorLocation(), CenterCm) > RadiusSq) { continue; }

		const FString Name = BuildableDisplayName(Buildable);
		if (!NormalizeName(Name).Contains(WantedName)) { continue; }

		// The PortIndex-th UNCONNECTED port of the wanted kind/direction, in stable name order.
		// A machine with matching ports that are all taken counts as skipped-connected; a machine
		// with no ports of this kind at all (pipe manifold onto a smelter) is silently not a match.
		FAIDAManifoldPort Port;
		bool bFound = false;
		bool bAnyOfKind = false;
		int32 Unconnected = 0;
		if (!bPipe)
		{
			TInlineComponentArray<UFGFactoryConnectionComponent*> Connections;
			Buildable->GetComponents(Connections);
			UFGFactoryConnectionComponent::SortComponentList(Connections);
			for (UFGFactoryConnectionComponent* Conn : Connections)
			{
				if (!Conn) { continue; }
				const EFactoryConnectionDirection Dir = Conn->GetDirection();
				if (Dir != (bOutput ? EFactoryConnectionDirection::FCD_OUTPUT : EFactoryConnectionDirection::FCD_INPUT))
				{
					continue;
				}
				bAnyOfKind = true;
				if (Conn->IsConnected()) { continue; }
				if (Unconnected++ < PortIndex) { continue; }
				Port.PosCm = Conn->GetConnectorLocation();
				Port.NormalCm = Conn->GetConnectorNormal();
				bFound = true;
				break;
			}
		}
		else
		{
			TInlineComponentArray<UFGPipeConnectionComponent*> Connections;
			Buildable->GetComponents(Connections);
			Connections.Sort([](const UFGPipeConnectionComponent& A, const UFGPipeConnectionComponent& B)
			{
				return A.GetName() < B.GetName();
			});
			for (UFGPipeConnectionComponent* Conn : Connections)
			{
				if (!Conn) { continue; }
				const EPipeConnectionType Type = Conn->GetPipeConnectionType();
				if (Type != (bOutput ? EPipeConnectionType::PCT_PRODUCER : EPipeConnectionType::PCT_CONSUMER))
				{
					continue;
				}
				bAnyOfKind = true;
				if (Conn->IsConnected()) { continue; }
				if (Unconnected++ < PortIndex) { continue; }
				Port.PosCm = Conn->GetConnectorLocation();
				Port.NormalCm = Conn->GetConnectorNormal();
				bFound = true;
				break;
			}
		}

		if (!bAnyOfKind) { continue; }
		if (!bFound)
		{
			++OutSkippedConnected;
			continue;
		}
		if (OutMachineName.IsEmpty()) { OutMachineName = Name; }
		Port.Machine = Buildable;
		Port.MachineName = Name;
		OutPorts.Add(MoveTemp(Port));
	}
	return true;
}

bool FAIDAActionSeam::ResolvePlannedPorts(UObject* WorldContext, const FString& MachineRecipePath,
	const FString& MachineName, const TArray<FTransform>& Placements,
	bool bPipe, bool bOutput, int32 PortIndex, const TArray<FVector>& UsedPortPositions,
	TArray<FAIDAManifoldPort>& OutPorts, TArray<int32>& OutPortMachineIndex)
{
	OutPorts.Reset();
	OutPortMachineIndex.Reset();

	UWorld* World = ResolveWorld(WorldContext);
	UClass* RecipeClass = World ? FSoftClassPath(MachineRecipePath).TryLoadClass<UFGRecipe>() : nullptr;
	if (!RecipeClass || Placements.Num() == 0) { return false; }

	// One hologram walked across every placement, like the dry-run. Positioned DIRECTLY at the
	// stored transform (no UpdateHologramPlacement — the placements are final and a snap pass
	// could move them); the cached connection components ride the root, so their connector
	// locations/normals come out world-posed.
	AFGHologram* Hologram = SpawnValidationHologram(World, RecipeClass, Placements[0].GetLocation());
	AFGBuildableHologram* BuildableHologram = Cast<AFGBuildableHologram>(Hologram);
	if (!BuildableHologram)
	{
		if (Hologram) { Hologram->Destroy(); }
		return false;
	}

	constexpr double UsedPortToleranceSq = 30.0 * 30.0; // cm — same connector across sets
	const auto IsUsed = [&UsedPortPositions](const FVector& Pos)
	{
		for (const FVector& Used : UsedPortPositions)
		{
			if (FVector::DistSquared(Used, Pos) <= UsedPortToleranceSq) { return true; }
		}
		return false;
	};

	for (int32 Index = 0; Index < Placements.Num(); ++Index)
	{
		BuildableHologram->SetActorLocationAndRotation(Placements[Index].GetLocation(), Placements[Index].GetRotation());

		FAIDAManifoldPort Port;
		bool bFound = false;
		int32 Free = 0;
		if (!bPipe)
		{
			// Stable name order (SortComponentList wants a TInlineComponentArray; the name sort is
			// what it does for identically-priorities connections anyway).
			TArray<UFGFactoryConnectionComponent*> Connections = BuildableHologram->GetCachedFactoryConnectionComponents();
			Connections.Sort([](const UFGFactoryConnectionComponent& A, const UFGFactoryConnectionComponent& B)
			{
				return A.GetName() < B.GetName();
			});
			for (UFGFactoryConnectionComponent* Conn : Connections)
			{
				if (!Conn) { continue; }
				if (Conn->GetDirection() != (bOutput ? EFactoryConnectionDirection::FCD_OUTPUT : EFactoryConnectionDirection::FCD_INPUT))
				{
					continue;
				}
				if (IsUsed(Conn->GetConnectorLocation())) { continue; }
				if (Free++ < PortIndex) { continue; }
				Port.PosCm = Conn->GetConnectorLocation();
				Port.NormalCm = Conn->GetConnectorNormal();
				bFound = true;
				break;
			}
		}
		else
		{
			TArray<UFGPipeConnectionComponent*> Connections = BuildableHologram->GetCachedPipeConnectionComponents();
			Connections.Sort([](const UFGPipeConnectionComponent& A, const UFGPipeConnectionComponent& B)
			{
				return A.GetName() < B.GetName();
			});
			for (UFGPipeConnectionComponent* Conn : Connections)
			{
				if (!Conn) { continue; }
				const EPipeConnectionType Type = Conn->GetPipeConnectionType();
				if (Type != (bOutput ? EPipeConnectionType::PCT_PRODUCER : EPipeConnectionType::PCT_CONSUMER))
				{
					continue;
				}
				if (IsUsed(Conn->GetConnectorLocation())) { continue; }
				if (Free++ < PortIndex) { continue; }
				Port.PosCm = Conn->GetConnectorLocation();
				Port.NormalCm = Conn->GetConnectorNormal();
				bFound = true;
				break;
			}
		}

		if (!bFound) { continue; } // machine has no free port of this kind — silently not part of the row

		Port.Machine = nullptr; // rebound to the built actor at execute (phase 0 capture)
		Port.MachineName = MachineName;
		OutPorts.Add(MoveTemp(Port));
		OutPortMachineIndex.Add(Index);
	}

	BuildableHologram->Destroy();
	return true;
}

bool FAIDAActionSeam::BuildConnectingRun(UObject* WorldContext, const FString& TransportRecipePath, bool bPipe,
	AActor* FromActor, const FVector& FromWantDir, AActor* ToActor, const FVector& ToWantDir,
	bool bChargeCost, TArray<FAIDACostItem>& OutCost,
	TArray<FString>& OutEntityIds, TArray<TWeakObjectPtr<AActor>>& OutActors, FString& OutError,
	const FString& PayerPlayerId)
{
	OutCost.Reset();
	OutError.Reset();

	UWorld* World = ResolveWorld(WorldContext);
	UClass* RecipeClass = World ? FSoftClassPath(TransportRecipePath).TryLoadClass<UFGRecipe>() : nullptr;
	if (!RecipeClass) { OutError = TEXT("transport recipe no longer resolvable"); return false; }
	if (!IsValid(FromActor) || !IsValid(ToActor)) { OutError = TEXT("endpoint no longer exists"); return false; }

	UFGInventoryComponent* Inventory = FindValidationInventory(World);
	if (!Inventory) { OutError = TEXT("no player inventory available to validate against"); return false; }

	// Pick the endpoint ports on the LIVE actors (belts: from = output, to = input; pipes any↔any).
	// The factory (belt) connections stay in scope: belts get wired directly after Construct.
	FVector FromLoc, ToLoc, FromNormal, ToNormal;
	UFGFactoryConnectionComponent* FromFactoryConn = nullptr;
	UFGFactoryConnectionComponent* ToFactoryConn = nullptr;
	if (!bPipe)
	{
		FromFactoryConn = FindFactoryPort(FromActor, /*bWantOutput*/ true, FromWantDir);
		ToFactoryConn = FindFactoryPort(ToActor, /*bWantOutput*/ false, ToWantDir);
		if (!FromFactoryConn) { OutError = FString::Printf(TEXT("no free output port on %s facing the run"), *GetNameSafe(FromActor)); return false; }
		if (!ToFactoryConn) { OutError = FString::Printf(TEXT("no free input port on %s facing the run"), *GetNameSafe(ToActor)); return false; }
		FromLoc = FromFactoryConn->GetConnectorLocation();
		ToLoc = ToFactoryConn->GetConnectorLocation();
		FromNormal = FromFactoryConn->GetConnectorNormal();
		ToNormal = ToFactoryConn->GetConnectorNormal();
	}
	else
	{
		UFGPipeConnectionComponent* FromConn = FindPipePort(FromActor, FromWantDir);
		UFGPipeConnectionComponent* ToConn = FindPipePort(ToActor, ToWantDir);
		if (!FromConn) { OutError = FString::Printf(TEXT("no free pipe port on %s facing the run"), *GetNameSafe(FromActor)); return false; }
		if (!ToConn) { OutError = FString::Printf(TEXT("no free pipe port on %s facing the run"), *GetNameSafe(ToActor)); return false; }
		FromLoc = FromConn->GetConnectorLocation();
		ToLoc = ToConn->GetConnectorLocation();
		FromNormal = FromConn->GetConnectorNormal();
		ToNormal = ToConn->GetConnectorNormal();
	}

	// Flushed breadcrumb: this function drives engine hologram code with synthetic input — if it
	// ever hangs the game, the log must show which run and endpoints were in flight.
	UE_LOG(LogAIDA, Log, TEXT("[actions][mf] run %s(%s) -> %s(%s)"),
		*GetNameSafe(FromActor), *FromLoc.ToCompactString(), *GetNameSafe(ToActor), *ToLoc.ToCompactString());
	GLog->Flush();

	// Drive the spline hologram through the build gun's own two-step flow: place + snap the start,
	// advance the build step, place + snap the end. BOTH snaps are load-bearing — an unsnapped end
	// silently constructs a support pole instead of a connection (docs/PHASE4-MANIFOLDS.md §5).
	AFGHologram* Hologram = SpawnValidationHologram(World, RecipeClass, FromLoc);
	if (!Hologram) { OutError = TEXT("could not spawn a run hologram"); return false; }
	AFGSplineHologram* Spline = Cast<AFGSplineHologram>(Hologram);
	if (!Spline)
	{
		// A non-spline transport (the model resolved a wall as a "belt") must fail loudly here —
		// the two-step drive below would otherwise construct nonsense at the port.
		OutError = TEXT("the transport recipe is not a belt or pipe");
		Hologram->Destroy();
		return false;
	}

	// Snap is attempted (a snapped end is already perfect) but NOT required for belts — engine snap
	// never latched onto synthetic hits in live-verify; belts get their connections authored
	// directly after Construct instead. Pipes still require the snap (no manual fluid wiring yet).
	Hologram->ResetConstructDisqualifiers();
	Hologram->UpdateHologramPlacement(MakePortHit(FromActor, FromLoc, FromNormal));
	if (!VerifyRunSnap(Spline, FromActor, /*bLastConnection*/ false, FromLoc) && bPipe)
	{
		OutError = FString::Printf(TEXT("run start did not snap to %s's port"), *GetNameSafe(FromActor));
		Hologram->Destroy();
		return false;
	}
	Hologram->DoMultiStepPlacement(/*isInputFromARelease*/ false); // start placed; expect "more steps"

	Hologram->ResetConstructDisqualifiers();
	Hologram->UpdateHologramPlacement(MakePortHit(ToActor, ToLoc, ToNormal));
	if (!VerifyRunSnap(Spline, ToActor, /*bLastConnection*/ true, ToLoc) && bPipe)
	{
		OutError = FString::Printf(TEXT("run end did not snap to %s's port"), *GetNameSafe(ToActor));
		Hologram->Destroy();
		return false;
	}

	Hologram->ValidatePlacementAndCost(Inventory);
	TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
	Hologram->GetConstructDisqualifiers(Disqualifiers);
	for (const TSubclassOf<UFGConstructDisqualifier>& Disqualifier : Disqualifiers)
	{
		if (IsBlockingDisqualifier(Disqualifier))
		{
			OutError = UFGConstructDisqualifier::GetDisqualifyingText(Disqualifier).ToString();
			Hologram->Destroy();
			return false;
		}
	}

	// The run's ACTUAL length-scaled cost (docs/PHASE4-MANIFOLDS.md §4) — charged before Construct.
	for (const FItemAmount& Entry : Hologram->GetCost(/*includeChildren*/ true))
	{
		AddCost(OutCost, Entry.ItemClass, Entry.Amount);
	}
	if (bChargeCost && !DeductCost(WorldContext, OutCost, PayerPlayerId))
	{
		OutError = TEXT("central storage + the requester's inventory cannot afford this run");
		Hologram->Destroy();
		return false;
	}

	if (!Hologram->DoMultiStepPlacement(/*isInputFromARelease*/ false))
	{
		// Snapped ends should finish the multi-step flow; if the hologram still wants steps
		// (pole-adjust), log and construct anyway — both ends verified snapped above.
		UE_LOG(LogAIDA, Warning, TEXT("[actions] run hologram still multi-stepping after end snap (%s) — constructing anyway."),
			*GetNameSafe(Hologram->GetClass()));
	}

	TArray<AActor*> Children;
	AActor* Built = Hologram->Construct(Children, FNetConstructionID());
	Hologram->Destroy();
	if (!Built)
	{
		OutError = TEXT("run Construct() returned nothing");
		if (bChargeCost)
		{
			UE_LOG(LogAIDA, Error, TEXT("[actions] run failed AFTER cost deduction — items lost to the void."));
		}
		return false;
	}

	// Belts: author the endpoint connections directly (idempotent for ends the hologram DID snap).
	// A belt that can't be wired is torn down again — a manifold never leaves dead belts around.
	if (!bPipe)
	{
		AFGBuildableConveyorBase* Belt = Cast<AFGBuildableConveyorBase>(Built);
		FString WireError;
		if (!Belt || !ConnectBeltEnds(Belt, FromFactoryConn, ToFactoryConn, WireError))
		{
			OutError = FString::Printf(TEXT("belt could not be wired to its ports (%s)"),
				Belt ? *WireError : TEXT("constructed actor is not a conveyor"));
			for (AActor* Child : Children)
			{
				if (Child && Child->Implements<UFGDismantleInterface>()) { IFGDismantleInterface::Execute_Dismantle(Child); }
			}
			if (Built->Implements<UFGDismantleInterface>()) { IFGDismantleInterface::Execute_Dismantle(Built); }
			if (bChargeCost)
			{
				UE_LOG(LogAIDA, Warning, TEXT("[actions] run torn down after wiring failure — its cost stays spent (dismantle refunds go nowhere here)."));
			}
			return false;
		}

		// Unsnapped ends made the hologram construct support poles AT the ports — clipping into the
		// splitter/machine. The connections are authored now; the props go.
		for (AActor* Child : Children)
		{
			if (Child && Child != Built && !Cast<AFGBuildableConveyorBase>(Child) &&
				Child->Implements<UFGDismantleInterface>() &&
				IFGDismantleInterface::Execute_CanDismantle(Child))
			{
				IFGDismantleInterface::Execute_Dismantle(Child);
			}
		}
		Children.Reset(); // journal only the belt — the poles no longer exist
	}

	FAIDAEntityId Entity;
	Entity.Type = TEXT("actor");
	Entity.ClassPath = Built->GetClass()->GetPathName();
	Entity.RecipePath = TransportRecipePath;
	Entity.Pos = Built->GetActorLocation();
	Entity.YawDeg = FMath::RoundToInt32(Built->GetActorRotation().Yaw);
	OutEntityIds.Add(AIDAActionSpec::EncodeEntityId(Entity));
	OutActors.Add(Built);
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
	return true;
}

namespace
{
	/** Recipe class path -> the AFGBuildable class its first product builds. */
	TSubclassOf<AFGBuildable> BuildableClassFromRecipe(const FString& RecipeClassPath)
	{
		UClass* RecipeClass = FSoftClassPath(RecipeClassPath).TryLoadClass<UFGRecipe>();
		const TArray<FItemAmount> Products = RecipeClass ? UFGRecipe::GetProducts(RecipeClass) : TArray<FItemAmount>();
		if (Products.Num() == 0 || !Products[0].ItemClass ||
			!Products[0].ItemClass->IsChildOf(UFGBuildingDescriptor::StaticClass()))
		{
			return nullptr;
		}
		return UFGBuildingDescriptor::GetBuildableClass(TSubclassOf<UFGBuildingDescriptor>(Products[0].ItemClass.Get()));
	}

	/** First power connection on a live actor with a free wire slot (poles expose several). */
	UFGPowerConnectionComponent* FindFreePowerConn(AActor* Actor)
	{
		TInlineComponentArray<UFGPowerConnectionComponent*> Connections;
		Actor->GetComponents(Connections);
		for (UFGPowerConnectionComponent* Conn : Connections)
		{
			if (Conn && Conn->GetNumFreeConnections() > 0) { return Conn; }
		}
		return nullptr;
	}
}

bool FAIDAActionSeam::ResolveAutoPower(UObject* WorldContext, const FString& MachineRecipePath,
	const FString& PoleOverrideName, FAIDAPowerInfo& Out, FString& OutError)
{
	Out = FAIDAPowerInfo();
	OutError.Reset();

	// Does the machine even need power? The CDO carries the native power connection. An EMPTY
	// machine recipe = the power-only path (propose_power): the machines already exist and the
	// caller vetted their power connections — only the pole/wire kit needs resolving here.
	if (MachineRecipePath.IsEmpty())
	{
		Out.bMachineNeedsPower = true;
	}
	else
	{
		const TSubclassOf<AFGBuildable> MachineClass = BuildableClassFromRecipe(MachineRecipePath);
		const AFGBuildable* MachineCDO = MachineClass ? MachineClass->GetDefaultObject<AFGBuildable>() : nullptr;
		if (!MachineCDO || !MachineCDO->FindComponentByClass<UFGPowerConnectionComponent>())
		{
			return false; // no power connection = nothing to wire (bMachineNeedsPower stays false)
		}
		Out.bMachineNeedsPower = true;
	}

	// The pole: the override, or the lowest UNLOCKED mk.
	TArray<FString> PoleNames;
	if (!PoleOverrideName.IsEmpty())
	{
		PoleNames.Add(PoleOverrideName);
	}
	else
	{
		PoleNames = { TEXT("Power Pole Mk.1"), TEXT("Power Pole Mk.2"), TEXT("Power Pole Mk.3") };
	}
	FAIDARecipeResolution Pole;
	for (const FString& Candidate : PoleNames)
	{
		if (ResolveBuildRecipe(WorldContext, Candidate, Pole))
		{
			Out.PoleRecipePath = Pole.RecipeClassPath;
			Out.PoleName = Pole.DisplayName;
			break;
		}
	}
	if (Out.PoleRecipePath.IsEmpty())
	{
		OutError = FString::Printf(TEXT("no unlocked power pole matches '%s'"), *FString::Join(PoleNames, TEXT("' / '")));
		return false;
	}

	// Connection cap from the pole CDO (how many machines each pole can serve).
	const TSubclassOf<AFGBuildable> PoleClass = BuildableClassFromRecipe(Out.PoleRecipePath);
	const AFGBuildable* PoleCDO = PoleClass ? PoleClass->GetDefaultObject<AFGBuildable>() : nullptr;
	if (PoleCDO)
	{
		TInlineComponentArray<UFGPowerConnectionComponent*> Connections;
		PoleCDO->GetComponents(Connections);
		int32 Cap = 0;
		for (const UFGPowerConnectionComponent* Conn : Connections)
		{
			if (Conn) { Cap = FMath::Max(Cap, Conn->GetMaxNumConnections()); }
		}
		if (Cap > 0) { Out.PoleConnectionCap = Cap; }
	}

	FAIDARecipeResolution Wire;
	if (!ResolveBuildRecipe(WorldContext, TEXT("Power Line"), Wire))
	{
		OutError = TEXT("the Power Line recipe is not unlocked/resolvable");
		return false;
	}
	Out.WireRecipePath = Wire.RecipeClassPath;
	Out.WireName = Wire.DisplayName;
	return true;
}

bool FAIDAActionSeam::CheckAffordable(UObject* WorldContext, const TArray<FAIDACostItem>& Cost,
	const FString& PayerPlayerId)
{
	UWorld* World = ResolveWorld(WorldContext);
	AFGCentralStorageSubsystem* Central = World ? AFGCentralStorageSubsystem::Get(World) : nullptr;
	UFGInventoryComponent* Pockets = World ? ResolvePlayerInventory(World, PayerPlayerId) : nullptr;
	if (!Central && !Pockets) { return false; }
	for (const FAIDACostItem& Item : Cost)
	{
		const TSubclassOf<UFGItemDescriptor> Descriptor = LoadDescriptor(Item.ClassPath);
		if (!Descriptor) { return false; }
		const int32 Available = (Central ? Central->GetNumItemsFromCentralStorage(Descriptor) : 0)
			+ (Pockets ? Pockets->GetNumItems(Descriptor) : 0);
		if (Available < Item.Amount)
		{
			return false;
		}
	}
	return true;
}

bool FAIDAActionSeam::BuildWire(UObject* WorldContext, const FString& WireRecipePath,
	AActor* A, AActor* B, bool bChargeCost, TArray<FAIDACostItem>& OutCost,
	TArray<FString>& OutEntityIds, TArray<TWeakObjectPtr<AActor>>& OutActors, FString& OutError,
	const FString& PayerPlayerId)
{
	OutCost.Reset();
	OutError.Reset();

	UWorld* World = ResolveWorld(WorldContext);
	if (!World || !IsValid(A) || !IsValid(B)) { OutError = TEXT("wire endpoint no longer exists"); return false; }

	UClass* WireRecipeClass = FSoftClassPath(WireRecipePath).TryLoadClass<UFGRecipe>();
	const TSubclassOf<AFGBuildable> WireBuildableClass = BuildableClassFromRecipe(WireRecipePath);
	if (!WireRecipeClass || !WireBuildableClass || !WireBuildableClass->IsChildOf(AFGBuildableWire::StaticClass()))
	{
		OutError = TEXT("wire recipe no longer resolves to a power line");
		return false;
	}

	UFGPowerConnectionComponent* ConnA = FindFreePowerConn(A);
	UFGPowerConnectionComponent* ConnB = FindFreePowerConn(B);
	if (!ConnA) { OutError = FString::Printf(TEXT("no free power connection on %s"), *GetNameSafe(A)); return false; }
	if (!ConnB) { OutError = FString::Printf(TEXT("no free power connection on %s"), *GetNameSafe(B)); return false; }

	// Length-priced like belts/pipes (charged as built): the recipe's cost per ~10 m of line.
	const double LengthCm = FVector::Dist(ConnA->GetComponentLocation(), ConnB->GetComponentLocation());
	const int32 Multiplier = FMath::Max(1, FMath::CeilToInt32(LengthCm / 1000.0));
	for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(World, WireRecipeClass))
	{
		AddCost(OutCost, Ingredient.ItemClass, Ingredient.Amount * Multiplier);
	}
	if (bChargeCost && !DeductCost(WorldContext, OutCost, PayerPlayerId))
	{
		OutError = TEXT("central storage + the requester's inventory cannot afford this power line");
		return false;
	}

	const FVector Mid = (ConnA->GetComponentLocation() + ConnB->GetComponentLocation()) * 0.5;
	AFGBuildableWire* Wire = World->SpawnActor<AFGBuildableWire>(WireBuildableClass, FTransform(Mid));
	if (!Wire)
	{
		OutError = TEXT("wire actor failed to spawn");
		if (bChargeCost) { UE_LOG(LogAIDA, Error, TEXT("[actions] wire failed AFTER cost deduction — items lost.")); }
		return false;
	}
	Wire->SetBuiltWithRecipe(WireRecipeClass); // dismantle refunds + journal re-resolution
	if (!Wire->Connect(ConnA, ConnB))
	{
		OutError = TEXT("wire Connect() refused the endpoints");
		Wire->Destroy();
		if (bChargeCost) { UE_LOG(LogAIDA, Error, TEXT("[actions] wire failed AFTER cost deduction — items lost.")); }
		return false;
	}

	FAIDAEntityId Entity;
	Entity.Type = TEXT("actor");
	Entity.ClassPath = Wire->GetClass()->GetPathName();
	Entity.RecipePath = WireRecipePath;
	Entity.Pos = Wire->GetActorLocation();
	Entity.YawDeg = 0;
	OutEntityIds.Add(AIDAActionSpec::EncodeEntityId(Entity));
	OutActors.Add(Wire);
	return true;
}

bool FAIDAActionSeam::FindGridTie(UObject* WorldContext, const TArray<TWeakObjectPtr<AActor>>& OurPoles,
	double RangeCm, AActor*& OutExternal, int32& OutPoleIndex)
{
	OutExternal = nullptr;
	OutPoleIndex = INDEX_NONE;

	UWorld* World = ResolveWorld(WorldContext);
	AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
	if (!Subsystem) { return false; }

	TSet<AActor*> Ours;
	TArray<TPair<int32, FVector>> PolePoints;
	for (int32 i = 0; i < OurPoles.Num(); ++i)
	{
		if (AActor* Pole = OurPoles[i].Get())
		{
			Ours.Add(Pole);
			PolePoints.Emplace(i, Pole->GetActorLocation());
		}
	}
	if (PolePoints.Num() == 0) { return false; }

	// Nearest EXTERNAL connection that is on a live circuit and has a free slot. Existing power
	// poles are preferred over machines (that's where players expect the tie), wires excluded (they
	// hold no connection components of their own). Our freshly wired machines exclude themselves —
	// their single connection has no free slot left.
	double BestPoleDistSq = FMath::Square(RangeCm);
	double BestAnyDistSq = FMath::Square(RangeCm);
	AActor* BestPole = nullptr; int32 BestPoleFrom = INDEX_NONE;
	AActor* BestAny = nullptr; int32 BestAnyFrom = INDEX_NONE;
	for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
	{
		if (!IsValid(Buildable) || Ours.Contains(Buildable)) { continue; }
		UFGPowerConnectionComponent* Conn = FindFreePowerConn(Buildable);
		if (!Conn || !Conn->IsConnected()) { continue; } // must already be ON a circuit

		for (const TPair<int32, FVector>& PolePoint : PolePoints)
		{
			const double DistSq = FVector::DistSquared(Buildable->GetActorLocation(), PolePoint.Value);
			if (Buildable->IsA<AFGBuildablePowerPole>() && DistSq < BestPoleDistSq)
			{
				BestPoleDistSq = DistSq;
				BestPole = Buildable;
				BestPoleFrom = PolePoint.Key;
			}
			if (DistSq < BestAnyDistSq)
			{
				BestAnyDistSq = DistSq;
				BestAny = Buildable;
				BestAnyFrom = PolePoint.Key;
			}
		}
	}
	OutExternal = BestPole ? BestPole : BestAny;
	OutPoleIndex = BestPole ? BestPoleFrom : BestAnyFrom;
	return OutExternal != nullptr;
}

bool FAIDAActionSeam::ResolveSignRecipe(UObject* WorldContext, const FString& OverrideName, FAIDARecipeResolution& Out, FString& OutError)
{
	// An explicit override resolves like any buildable, then must actually be a widget sign.
	if (!OverrideName.IsEmpty())
	{
		if (!ResolveBuildRecipe(WorldContext, OverrideName, Out))
		{
			OutError = FString::Printf(TEXT("no unlocked buildable matches sign '%s'"), *OverrideName);
			if (Out.Suggestions.Num() > 0)
			{
				OutError += FString::Printf(TEXT(" — closest: %s"), *FString::Join(Out.Suggestions, TEXT(", ")));
			}
			return false;
		}
		UClass* RecipeClass = FSoftClassPath(Out.RecipeClassPath).TryLoadClass<UFGRecipe>();
		const TArray<FItemAmount> Products = RecipeClass ? UFGRecipe::GetProducts(RecipeClass) : TArray<FItemAmount>();
		const TSubclassOf<UFGBuildingDescriptor> Descriptor = Products.Num() > 0
			? TSubclassOf<UFGBuildingDescriptor>(Products[0].ItemClass.Get()) : nullptr;
		const TSubclassOf<AFGBuildable> BuildableClass = UFGBuildingDescriptor::GetBuildableClass(Descriptor);
		if (!BuildableClass || !BuildableClass->IsChildOf(AFGBuildableWidgetSign::StaticClass()))
		{
			OutError = FString::Printf(TEXT("'%s' is not a text sign — pick a label/display sign"), *Out.DisplayName);
			return false;
		}
		return true;
	}

	// No override: the SMALLEST unlocked widget sign, judged by clearance footprint — no name
	// guessing ("Label Sign 2m" vs "Small Label Sign" drift across game versions).
	UWorld* World = ResolveWorld(WorldContext);
	TArray<FBuildRecipeEntry> Entries;
	CollectBuildRecipes(World, Entries);

	const FBuildRecipeEntry* Best = nullptr;
	double BestArea = 0.0;
	double BestX = 8.0, BestY = 8.0;
	for (const FBuildRecipeEntry& Entry : Entries)
	{
		const TSubclassOf<AFGBuildable> BuildableClass = UFGBuildingDescriptor::GetBuildableClass(Entry.Descriptor);
		if (!BuildableClass || !BuildableClass->IsChildOf(AFGBuildableWidgetSign::StaticClass())) { continue; }
		double XM, YM;
		ResolveFootprint(Entry.Descriptor, XM, YM);
		const double Area = XM * YM;
		if (!Best || Area < BestArea)
		{
			Best = &Entry;
			BestArea = Area;
			BestX = XM;
			BestY = YM;
		}
	}
	if (!Best)
	{
		OutError = TEXT("no sign is unlocked yet (signs unlock with a later tier) — labels need one");
		return false;
	}
	Out = FAIDARecipeResolution();
	Out.RecipeClassPath = Best->Recipe->GetPathName();
	Out.DisplayName = Best->Name;
	Out.FootprintXM = BestX;
	Out.FootprintYM = BestY;
	return true;
}

int32 FAIDAActionSeam::ResolveLabelTargets(UObject* WorldContext, const FVector& CenterCm, double RadiusCm,
	int32 MaxCount, const FString& ItemFilter, const FVector& ViewerCm, bool bHasViewer,
	TArray<FAIDALabelTarget>& OutTargets, int32& OutSkippedEmpty, int32& OutSkippedLabeled)
{
	OutTargets.Reset();
	OutSkippedEmpty = 0;
	OutSkippedLabeled = 0;

	UWorld* World = ResolveWorld(WorldContext);
	if (!World) { return 0; }

	// Existing signs, so re-running "label these" does not stack a second sign on every container.
	TArray<FVector> SignLocations;
	for (TActorIterator<AFGBuildableWidgetSign> It(World); It; ++It)
	{
		SignLocations.Add(It->GetActorLocation());
	}

	const FString WantedItem = NormalizeName(ItemFilter);
	const double RadiusSq = FMath::Square(RadiusCm);

	AFGBuildableSubsystem* Subsystem = AFGBuildableSubsystem::Get(World);
	if (!Subsystem) { return 0; }
	for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
	{
		if (MaxCount > 0 && OutTargets.Num() >= MaxCount) { break; }
		AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(Buildable);
		if (!Storage || !IsValid(Storage)) { continue; }
		if (FVector::DistSquared(Storage->GetActorLocation(), CenterCm) > RadiusSq) { continue; }

		// Dominant inventory item -> label text; empty boxes would make noisy "Empty" signs, skip.
		UFGInventoryComponent* Inventory = Storage->GetStorageInventory();
		if (!Inventory) { continue; }
		TMap<FString, int32> Totals;
		bool bHoldsWanted = WantedItem.IsEmpty();
		for (int32 Idx = 0; Idx < Inventory->GetSizeLinear(); ++Idx)
		{
			FInventoryStack Stack;
			if (!Inventory->GetStackFromIndex(Idx, Stack) || !Stack.HasItems()) { continue; }
			const FString Name = DescriptorName(Stack.Item.GetItemClass());
			Totals.FindOrAdd(Name) += Stack.NumItems;
			bHoldsWanted |= NormalizeName(Name).Contains(WantedItem);
		}
		if (Totals.Num() == 0) { ++OutSkippedEmpty; continue; }
		if (!bHoldsWanted) { continue; }
		FString Dominant;
		int32 DominantCount = 0;
		for (const TPair<FString, int32>& Pair : Totals)
		{
			if (Pair.Value > DominantCount || (Pair.Value == DominantCount && Pair.Key < Dominant))
			{
				Dominant = Pair.Key;
				DominantCount = Pair.Value;
			}
		}

		FVector BoundsOrigin, BoundsExtent;
		Storage->GetActorBounds(/*bOnlyCollidingComponents*/ false, BoundsOrigin, BoundsExtent);

		// One sign per box: an existing sign within the container's reach means "already labeled".
		const double LabeledReach = BoundsExtent.GetMax() + 200.0;
		const bool bLabeled = SignLocations.ContainsByPredicate([&](const FVector& At)
		{
			return FVector::DistSquared(At, BoundsOrigin) <= FMath::Square(LabeledReach);
		});
		if (bLabeled) { ++OutSkippedLabeled; continue; }

		// Face the viewer when we know where they are; the container front otherwise. Snap to the
		// nearest of the actor's four horizontal axes so the sign sits flat on a face.
		FVector Toward = bHasViewer ? (ViewerCm - BoundsOrigin) : Storage->GetActorForwardVector();
		Toward.Z = 0.0;
		if (!Toward.Normalize()) { Toward = Storage->GetActorForwardVector(); }
		const FVector Axes[4] = {
			Storage->GetActorForwardVector(), -Storage->GetActorForwardVector(),
			Storage->GetActorRightVector(), -Storage->GetActorRightVector() };
		FVector Outward = Axes[0];
		double BestDot = -2.0;
		for (const FVector& Axis : Axes)
		{
			FVector Flat = Axis;
			Flat.Z = 0.0;
			if (!Flat.Normalize()) { continue; }
			const double Dot = Flat | Toward;
			if (Dot > BestDot) { BestDot = Dot; Outward = Flat; }
		}
		const double FaceOffset = FMath::Abs(Outward.X) * BoundsExtent.X + FMath::Abs(Outward.Y) * BoundsExtent.Y;

		FAIDALabelTarget Target;
		Target.Container = Storage;
		Target.ContainerClass = GetNameSafe(Storage->GetClass());
		Target.OutwardCm = Outward;
		Target.SignPosCm = BoundsOrigin + Outward * (FaceOffset + 60.0);
		Target.Text = Dominant;
		OutTargets.Add(MoveTemp(Target));
	}
	return OutTargets.Num();
}

bool FAIDAActionSeam::BuildLabelSign(UObject* WorldContext, const FString& SignRecipePath, AActor* Container,
	const FVector& SignPosCm, const FVector& OutwardCm, const FString& Text,
	TArray<FString>& OutEntityIds, TArray<TWeakObjectPtr<AActor>>& OutActors, FString& OutError)
{
	UWorld* World = ResolveWorld(WorldContext);
	UClass* RecipeClass = World ? FSoftClassPath(SignRecipePath).TryLoadClass<UFGRecipe>() : nullptr;
	if (!RecipeClass) { OutError = TEXT("sign recipe failed to load"); return false; }
	if (!IsValid(Container)) { OutError = TEXT("container is gone"); return false; }
	UFGInventoryComponent* Inventory = FindValidationInventory(World);
	if (!Inventory) { OutError = TEXT("no player available for validation"); return false; }

	// A real hit ON the container face — sign holograms snap to surfaces, not to thin air.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(AIDALabelTrace), /*bTraceComplex*/ false);
	FHitResult Hit;
	const FVector TraceEnd = SignPosCm - OutwardCm * 400.0;
	if (!World->LineTraceSingleByChannel(Hit, SignPosCm, TraceEnd, AIDABuildGunChannel, Params)
		|| Hit.GetActor() != Container)
	{
		// Something sits between the sign spot and the box (or the trace missed): aim at the center.
		if (!World->LineTraceSingleByChannel(Hit, SignPosCm, Container->GetActorLocation(), AIDABuildGunChannel, Params)
			|| Hit.GetActor() != Container)
		{
			OutError = TEXT("could not reach the container face (something is in the way)");
			return false;
		}
	}

	AFGHologram* Hologram = SpawnValidationHologram(World, RecipeClass, Hit.ImpactPoint);
	if (!Hologram) { OutError = TEXT("sign hologram failed to spawn"); return false; }

	Hologram->ResetConstructDisqualifiers();
	Hologram->UpdateHologramPlacement(Hit);
	Hologram->ValidatePlacementAndCost(Inventory);

	TArray<TSubclassOf<UFGConstructDisqualifier>> Disqualifiers;
	Hologram->GetConstructDisqualifiers(Disqualifiers);
	for (const TSubclassOf<UFGConstructDisqualifier>& Disqualifier : Disqualifiers)
	{
		if (IsBlockingDisqualifier(Disqualifier))
		{
			OutError = UFGConstructDisqualifier::GetDisqualifyingText(Disqualifier).ToString();
			Hologram->Destroy();
			return false;
		}
	}

	TArray<AActor*> Children;
	AActor* Built = Hologram->Construct(Children, FNetConstructionID());
	Hologram->Destroy();
	if (!Built) { OutError = TEXT("sign construction failed"); return false; }

	const auto Capture = [&](AActor* Actor)
	{
		if (!Actor) { return; }
		FAIDAEntityId Entity;
		Entity.Type = TEXT("actor");
		Entity.ClassPath = Actor->GetClass()->GetPathName();
		Entity.RecipePath = Actor == Built ? SignRecipePath : FString();
		Entity.Pos = Actor->GetActorLocation();
		Entity.YawDeg = FMath::RoundToInt32(Actor->GetActorRotation().Yaw);
		OutEntityIds.Add(AIDAActionSpec::EncodeEntityId(Entity));
		OutActors.Add(Actor);
	};
	Capture(Built);
	for (AActor* Child : Children) { Capture(Child); }

	// The label itself. A fresh sign may carry no prefab data at all — fall back to its type
	// descriptor for the element names, the first prefab layout, and the default colors.
	AFGBuildableWidgetSign* Sign = Cast<AFGBuildableWidgetSign>(Built);
	if (!Sign)
	{
		for (AActor* Child : Children)
		{
			Sign = Cast<AFGBuildableWidgetSign>(Child);
			if (Sign) { break; }
		}
	}
	if (!Sign)
	{
		OutError = TEXT("built, but the result is not a text sign — text not set");
		return false;
	}

	FPrefabSignData Data;
	Sign->GetSignPrefabData(Data);
	TSubclassOf<UFGSignTypeDescriptor> TypeDesc = Data.SignTypeDesc;
	if (!TypeDesc)
	{
		TypeDesc = IFGSignInterface::Execute_GetSignTypeDescriptor(Sign);
		Data.SignTypeDesc = TypeDesc;
	}
	if (const UFGSignTypeDescriptor* Descriptor = TypeDesc ? TypeDesc->GetDefaultObject<UFGSignTypeDescriptor>() : nullptr)
	{
		if (Data.TextElementData.Num() == 0)
		{
			TArray<FString> Keys;
			Descriptor->GetTextElementNameMap().GenerateKeyArray(Keys);
			for (const FString& Key : Keys) { Data.TextElementData.Add(Key, Text); }
		}
		if (Data.PrefabLayout.IsNull() && Descriptor->GetPrefabArray().Num() > 0)
		{
			Data.PrefabLayout = Descriptor->GetPrefabArray()[0];
		}
		if (Data.ForegroundColor.Equals(Data.BackgroundColor)) // black-on-black defaults = unreadable
		{
			Data.ForegroundColor = Descriptor->GetDefaultForegroundColor();
			Data.BackgroundColor = Descriptor->GetDefaultBackgroundColor();
			Data.AuxiliaryColor = Descriptor->GetDefaultAuxiliaryColor();
		}
	}
	for (TPair<FString, FString>& Element : Data.TextElementData)
	{
		Element.Value = Text;
	}
	Data.Emissive = FMath::Max(Data.Emissive, 1.0f);
	Sign->SetPrefabSignData(Data);
	return true;
}

bool FAIDAActionSeam::TallyRecipeCost(UObject* WorldContext, const FString& RecipeClassPath, int32 Count, TArray<FAIDACostItem>& Out)
{
	UClass* RecipeClass = FSoftClassPath(RecipeClassPath).TryLoadClass<UFGRecipe>();
	if (!RecipeClass || Count <= 0) { return false; }
	for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(WorldContext, RecipeClass))
	{
		AddCost(Out, Ingredient.ItemClass, Ingredient.Amount * Count);
	}
	return true;
}

int32 FAIDAActionSeam::CountAttachmentOverlaps(UObject* WorldContext, const TArray<FTransform>& Placements, double FootprintCm)
{
	UWorld* World = ResolveWorld(WorldContext);
	AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
	if (!Subsystem || Placements.Num() == 0) { return 0; }

	// Existing attachment bodies, once. Their half-width is ~2 m across the family (splitters,
	// mergers, junctions, pumps); ours comes from the caller's footprint. A small tolerance keeps
	// exactly-footprint-apart neighbours (a legal packed row) from counting as overlap.
	TArray<FVector> Existing;
	for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
	{
		if (!IsValid(Buildable)) { continue; }
		if (Buildable->IsA<AFGBuildableConveyorAttachment>() || Buildable->IsA<AFGBuildablePipelineAttachment>())
		{
			Existing.Add(Buildable->GetActorLocation());
		}
	}
	if (Existing.Num() == 0) { return 0; }

	constexpr double OtherHalfCm = 200.0;
	constexpr double ToleranceCm = 25.0;
	constexpr double SameFloorCm = 300.0;
	const double Threshold = FootprintCm * 0.5 + OtherHalfCm - ToleranceCm;
	const double ThresholdSq = Threshold * Threshold;

	int32 Overlaps = 0;
	for (const FTransform& Placement : Placements)
	{
		const FVector At = Placement.GetLocation();
		for (const FVector& Other : Existing)
		{
			if (FMath::Abs(Other.Z - At.Z) > SameFloorCm) { continue; }
			if (FVector::DistSquaredXY(At, Other) < ThresholdSq)
			{
				++Overlaps;
				break; // one hit marks this placement; move on
			}
		}
	}
	return Overlaps;
}

bool FAIDAActionSeam::ResolveManifoldLane(UObject* WorldContext, const FAIDADismantleSpec& Selector,
	bool bPipe, bool bOutput, int32 PortIndex, int32& OutLane, int32& OutRowsOnSide)
{
	OutLane = 0;
	OutRowsOnSide = 1;

	UWorld* World = ResolveWorld(WorldContext);
	AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
	const FString WantedName = NormalizeName(Selector.Buildable);
	if (!Subsystem || WantedName.IsEmpty()) { return false; }

	const FVector CenterCm = Selector.CenterM * AIDAMetersToCm;
	const double RadiusSq = FMath::Square(Selector.RadiusM * AIDAMetersToCm);

	for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
	{
		if (!IsValid(Buildable)) { continue; }
		if (FVector::DistSquared(Buildable->GetActorLocation(), CenterCm) > RadiusSq) { continue; }
		if (!NormalizeName(BuildableDisplayName(Buildable)).Contains(WantedName)) { continue; }

		// Census of the whole side — connected ports count too, so the lane never shifts between
		// runs as ports fill up.
		int32 PipePorts = 0;
		{
			TInlineComponentArray<UFGPipeConnectionComponent*> Connections;
			Buildable->GetComponents(Connections);
			for (const UFGPipeConnectionComponent* Conn : Connections)
			{
				if (Conn && Conn->GetPipeConnectionType() ==
					(bOutput ? EPipeConnectionType::PCT_PRODUCER : EPipeConnectionType::PCT_CONSUMER))
				{
					++PipePorts;
				}
			}
		}
		int32 BeltPorts = 0;
		{
			TInlineComponentArray<UFGFactoryConnectionComponent*> Connections;
			Buildable->GetComponents(Connections);
			for (const UFGFactoryConnectionComponent* Conn : Connections)
			{
				if (Conn && Conn->GetDirection() ==
					(bOutput ? EFactoryConnectionDirection::FCD_OUTPUT : EFactoryConnectionDirection::FCD_INPUT))
				{
					++BeltPorts;
				}
			}
		}

		OutRowsOnSide = FMath::Max(1, PipePorts + BeltPorts);
		OutLane = bPipe
			? FMath::Clamp(PortIndex, 0, FMath::Max(0, PipePorts - 1))
			: PipePorts + FMath::Clamp(PortIndex, 0, FMath::Max(0, BeltPorts - 1));
		return true;
	}
	return false;
}

int32 FAIDAActionSeam::ResolveUnpoweredMachines(UObject* WorldContext, const FString& Buildable,
	const FVector& CenterCm, double RadiusCm, int32 MaxCount,
	TArray<FAIDAManifoldPort>& OutMachines, int32& OutSkippedPowered)
{
	OutMachines.Reset();
	OutSkippedPowered = 0;

	UWorld* World = ResolveWorld(WorldContext);
	AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
	if (!Subsystem) { return 0; }

	const FString WantedName = NormalizeName(Buildable);
	const double RadiusSq = FMath::Square(RadiusCm);
	const int32 Cap = MaxCount > 0 ? MaxCount : MAX_int32;

	for (AFGBuildable* Candidate : Subsystem->GetAllBuildablesRef())
	{
		if (OutMachines.Num() >= Cap) { break; }
		if (!IsValid(Candidate)) { continue; }
		if (Candidate->IsA<AFGBuildablePowerPole>()) { continue; } // poles are infrastructure, not loads
		if (FVector::DistSquared(Candidate->GetActorLocation(), CenterCm) > RadiusSq) { continue; }

		const FString Name = BuildableDisplayName(Candidate);
		if (!WantedName.IsEmpty() && !NormalizeName(Name).Contains(WantedName)) { continue; }

		UFGPowerConnectionComponent* Power = Candidate->FindComponentByClass<UFGPowerConnectionComponent>();
		if (!Power) { continue; } // needs no power — not a match
		if (Power->IsConnected())
		{
			++OutSkippedPowered;
			continue;
		}

		FAIDAManifoldPort Machine;
		Machine.Machine = Candidate;
		Machine.MachineName = Name;
		Machine.PosCm = Candidate->GetActorLocation();
		OutMachines.Add(MoveTemp(Machine));
	}
	return OutMachines.Num();
}
