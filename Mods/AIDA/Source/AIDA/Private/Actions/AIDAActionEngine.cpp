#include "Actions/AIDAActionEngine.h"

#include "AIDA.h"
#include "Actions/AIDAActionSeam.h"
#include "Actions/AIDAActionSpec.h"
#include "Core/AIDAConfig.h"
#include "Memory/AIDAMemory.h"
#include "Memory/AIDAMemoryStore.h"
#include "Memory/AIDAMemoryTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/** Merge one tally into another by item name (run costs accrue onto the proposal's tally so the
	 *  journaled refund covers the whole manifold). */
	void MergeCost(TArray<FAIDACostItem>& Into, const TArray<FAIDACostItem>& Add)
	{
		for (const FAIDACostItem& Item : Add)
		{
			bool bMerged = false;
			for (FAIDACostItem& Existing : Into)
			{
				if (Existing.Item == Item.Item)
				{
					Existing.Amount += Item.Amount;
					bMerged = true;
					break;
				}
			}
			if (!bMerged) { Into.Add(Item); }
		}
	}

	/** Subtract one tally from another by item name, clamping at zero (the journaled cost must be
	 *  the NET consumption once skipped placements refund — undo refunds from that record). */
	void SubtractCost(TArray<FAIDACostItem>& From, const TArray<FAIDACostItem>& Minus)
	{
		for (const FAIDACostItem& Item : Minus)
		{
			for (FAIDACostItem& Existing : From)
			{
				if (Existing.Item == Item.Item)
				{
					Existing.Amount = FMath::Max(0, Existing.Amount - Item.Amount);
					break;
				}
			}
		}
		From.RemoveAll([](const FAIDACostItem& Item) { return Item.Amount <= 0; });
	}
}

bool FAIDAActionEngine::Approve(UObject* WorldContext, const FAIDAActionsConfig& Config, const FGuid& Id, const FString& ApproverId, FString& OutMessage)
{
	ProposalStore.SweepExpired(FDateTime::UtcNow().ToUnixTimestamp(), Config.TtlSeconds);

	FAIDAProposal* Proposal = ProposalStore.Find(Id);
	if (!Proposal)
	{
		OutMessage = TEXT("unknown proposal id (it may have been retired)");
		return false;
	}
	if (Proposal->State != EAIDAProposalState::Pending)
	{
		OutMessage = FString::Printf(TEXT("proposal is %s, not pending"), *AIDAActionSpec::StateToString(Proposal->State));
		return false;
	}
	if (IsExecuting() || UndoQueue.Num() > 0)
	{
		OutMessage = TEXT("another action is executing — wait for it to finish");
		return false;
	}

	if (Proposal->bDismantle)
	{
		// Targets re-resolve NOW (docs/PHASE4.md §3) — the world moved on since the dry-run.
		FAIDADismantleSpec Selector;
		FString ParseError;
		TSharedPtr<FJsonObject> SpecObj;
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Proposal->SpecJson);
			FJsonSerializer::Deserialize(Reader, SpecObj);
		}
		if (!AIDAActionSpec::ParseDismantleSpec(SpecObj, Config.MaxProposalItems, Selector, ParseError) ||
			!Selector.bHasCenter) // a stored selector must carry its concrete center — never default to (0,0)
		{
			ProposalStore.Transition(Id, EAIDAProposalState::Rejected, FDateTime::UtcNow().ToUnixTimestamp());
			OutMessage = FString::Printf(TEXT("stored selector no longer parses (%s) — proposal rejected"), *ParseError);
			return false;
		}

		TArray<FAIDADismantleHandle> Handles;
		FAIDAActionSeam::ResolveDismantleHandles(WorldContext, Selector, Handles);
		if (Handles.Num() == 0)
		{
			ProposalStore.Transition(Id, EAIDAProposalState::Rejected, FDateTime::UtcNow().ToUnixTimestamp());
			OutMessage = TEXT("nothing left matching the selector — proposal rejected");
			return false;
		}
		if (Handles.Num() != Proposal->TargetCount)
		{
			UE_LOG(LogAIDA, Log, TEXT("[actions] dismantle target drift: %d at dry-run, %d at approval."),
				Proposal->TargetCount, Handles.Num());
		}
		DismantleQueue = MoveTemp(Handles);
	}
	else if (Proposal->MutationKind != EAIDAMutationKind::None)
	{
		// Mutation targets re-resolve NOW, like dismantles — the world moved on since the dry-run.
		// Advised clocks carry their own per-machine targets from propose (weak ptrs; dead = skip).
		if (Proposal->MutationAdvised.Num() > 0)
		{
			MutationQueue.Reset();
			for (const FAIDAMutationTarget& Target : Proposal->MutationAdvised)
			{
				if (Target.Actor.IsValid()) { MutationQueue.Add(Target); }
			}
		}
		else
		{
			const FString TargetRecipe = Proposal->MutationKind == EAIDAMutationKind::Recipe
				? Proposal->MutationRecipePath
				: (Proposal->MutationKind == EAIDAMutationKind::BeltMk ? Proposal->MutationBeltRecipePath : FString());
			FString Error;
			TArray<FAIDAMutationTarget> Targets;
			FAIDAActionSeam::ResolveMutationTargets(WorldContext, Proposal->MutationKind,
				Proposal->MutationSelector, TargetRecipe, Targets, Error);
			for (FAIDAMutationTarget& Target : Targets)
			{
				Target.ClockPct = Proposal->MutationClockPct;
			}
			MutationQueue = MoveTemp(Targets);
		}
		if (MutationQueue.Num() == 0)
		{
			ProposalStore.Transition(Id, EAIDAProposalState::Rejected, FDateTime::UtcNow().ToUnixTimestamp());
			OutMessage = TEXT("nothing left matching the selector — proposal rejected");
			return false;
		}
		if (MutationQueue.Num() != Proposal->TargetCount)
		{
			UE_LOG(LogAIDA, Log, TEXT("[actions] mutation target drift: %d at dry-run, %d at approval."),
				Proposal->TargetCount, MutationQueue.Num());
		}
		// No upfront cost: clocks/recipes are free, belt upgrades charge each belt as built.
	}
	else if (Config.CostMode == TEXT("central"))
	{
		// Pay the whole tally upfront (central storage first, then the REQUESTER's inventory —
		// they asked for the build); failure leaves the proposal pending (player can restock).
		if (!FAIDAActionSeam::DeductCost(WorldContext, Proposal->Cost, Proposal->RequesterId))
		{
			OutMessage = TEXT("no longer affordable from central storage + the requester's inventory — restock and approve again");
			return false;
		}
	}

	Proposal->ApproverId = ApproverId;
	ProposalStore.Transition(Id, EAIDAProposalState::Approved);
	ProposalStore.Transition(Id, EAIDAProposalState::Executing);
	ExecutingId = Id;
	BuiltActors.Reset();
	AccruedRefund.Reset();
	AttachmentActors.Reset();
	PoleActors.Reset();
	SetAttachmentActors.Reset();
	TapSourceActor = nullptr;
	ChainPrevActor = nullptr;
	RunFailures.Reset();
	MutationChanges.Reset();
	MutationCharged.Reset();
	SkippedCount = RemovedCount = MissingCount = RunBuiltCount = RunFailCount = 0;
	Proposal->Cursor = 0;
	Proposal->Phase = 0;

	const int32 Total = Proposal->bDismantle ? DismantleQueue.Num()
		: (Proposal->MutationKind != EAIDAMutationKind::None ? MutationQueue.Num() : Proposal->Placements.Num());
	OutMessage = FString::Printf(TEXT("approved — %s (%d item(s), batched)"), *Proposal->Summary, Total);
	UE_LOG(LogAIDA, Log, TEXT("[actions] %s: executing %s"), *Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal->Summary);
	return true;
}

bool FAIDAActionEngine::Reject(const FGuid& Id, const FString& ApproverId, FString& OutMessage)
{
	FAIDAProposal* Proposal = ProposalStore.Find(Id);
	if (!Proposal || Proposal->State != EAIDAProposalState::Pending)
	{
		OutMessage = TEXT("no pending proposal with that id");
		return false;
	}
	Proposal->ApproverId = ApproverId;
	ProposalStore.Transition(Id, EAIDAProposalState::Rejected, FDateTime::UtcNow().ToUnixTimestamp());
	OutMessage = FString::Printf(TEXT("rejected — %s"), *Proposal->Summary);
	UE_LOG(LogAIDA, Log, TEXT("[actions] %s rejected by %s."), *Id.ToString(EGuidFormats::DigitsWithHyphens), *ApproverId);
	return true;
}

bool FAIDAActionEngine::AdjustPending(UObject* WorldContext, const FAIDAActionsConfig& Config, const FGuid& Id,
	const FVector& DeltaCm, int32 YawDeltaDeg, FString& OutMessage)
{
	FAIDAProposal* Proposal = ProposalStore.Find(Id);
	if (!Proposal || Proposal->State != EAIDAProposalState::Pending)
	{
		OutMessage = TEXT("no pending proposal to adjust");
		return false;
	}
	if (Proposal->bDismantle || Proposal->MutationKind != EAIDAMutationKind::None)
	{
		OutMessage = TEXT("only build proposals can be nudged/rotated");
		return false;
	}
	if (Proposal->bManifold)
	{
		OutMessage = TEXT("manifolds are anchored to machine ports and can't be nudged — reject and re-propose instead");
		return false;
	}
	if (Proposal->bLabel)
	{
		OutMessage = TEXT("labels are anchored to their containers and can't be nudged — reject and re-propose instead");
		return false;
	}

	// Transform a copy; the original survives an invalid adjustment untouched. Auto-power pole
	// placements ride along with the SAME transform (same rotation centroid — the machines') so a
	// nudged grid keeps its pole lanes; the wire pairs are index-based and move with them. A
	// connected build's manifold sets (attachments + planned ports + axes) ride along too.
	TArray<FTransform> Adjusted = Proposal->Placements;
	TArray<FTransform> AdjustedPoles = Proposal->PolePlacements;
	TArray<FAIDAManifoldSet> AdjustedSets = Proposal->ManifoldSets;
	if (YawDeltaDeg != 0 && Adjusted.Num() > 0)
	{
		FVector Centroid = FVector::ZeroVector;
		for (const FTransform& Placement : Adjusted) { Centroid += Placement.GetLocation(); }
		Centroid /= Adjusted.Num();

		const FRotator Delta(0.0, static_cast<double>(YawDeltaDeg), 0.0);
		const auto Rotate = [&](FTransform& Placement)
		{
			Placement.SetLocation(Centroid + Delta.RotateVector(Placement.GetLocation() - Centroid));
			FRotator Rotation = Placement.Rotator();
			Rotation.Yaw += YawDeltaDeg;
			Placement.SetRotation(FQuat(Rotation));
		};
		for (FTransform& Placement : Adjusted) { Rotate(Placement); }
		for (FTransform& Placement : AdjustedPoles) { Rotate(Placement); }
		for (FAIDAManifoldSet& Set : AdjustedSets)
		{
			for (FTransform& Placement : Set.Attachments) { Rotate(Placement); }
			for (FAIDAManifoldPort& Port : Set.Ports)
			{
				Port.PosCm = Centroid + Delta.RotateVector(Port.PosCm - Centroid);
				Port.NormalCm = Delta.RotateVector(Port.NormalCm);
			}
			Set.RowAxis = Delta.RotateVector(Set.RowAxis);
			Set.DropDir = Delta.RotateVector(Set.DropDir);
		}
	}
	if (!DeltaCm.IsNearlyZero())
	{
		for (FTransform& Placement : Adjusted)
		{
			Placement.SetLocation(Placement.GetLocation() + DeltaCm);
		}
		for (FTransform& Placement : AdjustedPoles)
		{
			Placement.SetLocation(Placement.GetLocation() + DeltaCm);
		}
		for (FAIDAManifoldSet& Set : AdjustedSets)
		{
			for (FTransform& Placement : Set.Attachments)
			{
				Placement.SetLocation(Placement.GetLocation() + DeltaCm);
			}
			for (FAIDAManifoldPort& Port : Set.Ports)
			{
				Port.PosCm += DeltaCm;
			}
		}
	}

	// Validity at the new spot is ADVISORY, never a rejection (user rule: a pending ghost is the
	// player's to move — they may be walking it OFF bad ground, so an invalid waypoint must not
	// freeze it there). The dry-run still runs so the reply doubles as live placement feedback,
	// and execute re-validates per tile anyway (skips + refunds what still fails). Composites
	// re-validate each part with its own recipe.
	FAIDADryRunResult DryRun;
	const bool bComposite = Proposal->PlacementPartIndex.Num() == Proposal->Placements.Num()
		&& Proposal->PartRecipePaths.Num() > 0;
	const bool bRan = bComposite
		? FAIDAActionSeam::DryRunBuildParts(WorldContext, Proposal->PartRecipePaths, Proposal->PlacementPartIndex, Adjusted, DryRun,
			Proposal->RequesterId)
		: FAIDAActionSeam::DryRunBuild(WorldContext, Proposal->RecipeClassPath, Adjusted, DryRun, Proposal->RequesterId);
	FString Validity;
	if (!bRan)
	{
		Validity = FString::Printf(TEXT(" [couldn't validate there: %s]"),
			DryRun.Error.IsEmpty() ? TEXT("validation failed") : *DryRun.Error);
	}
	else
	{
		Proposal->InvalidCount = DryRun.Failures.Num();
		Validity = DryRun.Failures.Num() > 0
			? FString::Printf(TEXT(" [%d placement(s) blocked here — keep nudging]"), DryRun.Failures.Num())
			: TEXT(" [all placements valid here]");
	}

	Proposal->Placements = MoveTemp(Adjusted);
	Proposal->PolePlacements = MoveTemp(AdjustedPoles);
	Proposal->ManifoldSets = MoveTemp(AdjustedSets);
	OutMessage = (YawDeltaDeg != 0
		? FString::Printf(TEXT("rotated %d° — %s"), YawDeltaDeg, *Proposal->Summary)
		: FString::Printf(TEXT("nudged %.1f m — %s"), DeltaCm.Size() / 100.0, *Proposal->Summary)) + Validity;
	UE_LOG(LogAIDA, Log, TEXT("[actions] %s adjusted (delta=%s yaw+=%d)."),
		*Id.ToString(EGuidFormats::DigitsWithHyphens), *DeltaCm.ToCompactString(), YawDeltaDeg);
	return true;
}

bool FAIDAActionEngine::Tick(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory)
{
	// Undo shares the 10 Hz slicer and is mutually exclusive with proposal execution.
	if (UndoQueue.Num() > 0)
	{
		return TickUndo(WorldContext, Config, Memory);
	}

	FAIDAProposal* Proposal = ProposalStore.Find(ExecutingId);
	if (!Proposal || Proposal->State != EAIDAProposalState::Executing)
	{
		ResetScratch();
		return false;
	}

	if (Proposal->MutationKind != EAIDAMutationKind::None)
	{
		return TickMutation(WorldContext, Config, Memory, *Proposal);
	}
	if (Proposal->bManifold)
	{
		return TickManifold(WorldContext, Config, Memory, *Proposal);
	}
	if (Proposal->bLabel)
	{
		return TickLabels(WorldContext, Config, Memory, *Proposal);
	}
	if (Proposal->bTap && !Proposal->bManifold && Proposal->ManifoldSets.Num() == 0)
	{
		// Standalone tap: the machines/manifold already EXIST — only the tap + feed chain build.
		return TickTapOnly(WorldContext, Config, Memory, *Proposal);
	}
	if (Proposal->ManifoldSets.Num() > 0 && !Proposal->bDismantle)
	{
		// Connected build (machines + manifold sets, revise-by-prompt) — runs its own power
		// phases when bAutoPower, so it must dispatch ahead of the plain powered path.
		return TickConnected(WorldContext, Config, Memory, *Proposal);
	}
	if (Proposal->bAutoPower && !Proposal->bDismantle)
	{
		return TickPowered(WorldContext, Config, Memory, *Proposal);
	}

	if (Proposal->bDismantle)
	{
		int32 Removed = 0, Missing = 0;
		Proposal->Cursor += FAIDAActionSeam::DismantleHandleBatch(WorldContext, DismantleQueue,
			Proposal->Cursor, FMath::Max(1, Config.BatchPerTick), AccruedRefund, Proposal->AffectedEntityIds, Removed, Missing);
		RemovedCount += Removed;
		MissingCount += Missing;
		if (Proposal->Cursor < DismantleQueue.Num()) { return true; }
	}
	else
	{
		// Spec-v2 composites: placements are grouped by part, so clamp each tick's batch to the
		// current part's contiguous run and construct with THAT part's recipe.
		FString Recipe = Proposal->RecipeClassPath;
		int32 Batch = FMath::Max(1, Config.BatchPerTick);
		if (Proposal->PlacementPartIndex.Num() == Proposal->Placements.Num()
			&& Proposal->PlacementPartIndex.IsValidIndex(Proposal->Cursor))
		{
			const int32 Part = Proposal->PlacementPartIndex[Proposal->Cursor];
			if (Proposal->PartRecipePaths.IsValidIndex(Part))
			{
				Recipe = Proposal->PartRecipePaths[Part];
			}
			int32 RunEnd = Proposal->Cursor;
			while (RunEnd < Proposal->PlacementPartIndex.Num() && Proposal->PlacementPartIndex[RunEnd] == Part)
			{
				++RunEnd;
			}
			Batch = FMath::Min(Batch, RunEnd - Proposal->Cursor);
		}

		int32 Skipped = 0;
		Proposal->Cursor += FAIDAActionSeam::ExecuteBuildBatch(WorldContext, Recipe,
			Proposal->Placements, Proposal->Cursor, Batch,
			Proposal->AffectedEntityIds, BuiltActors, Skipped);
		SkippedCount += Skipped;
		AccrueSkippedCost(WorldContext, Config, Recipe, Skipped);
		if (Proposal->Cursor < Proposal->Placements.Num()) { return true; }
	}

	FinishExecution(WorldContext, Config, Memory, *Proposal);
	return false;
}

bool FAIDAActionEngine::TickMutation(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal)
{
	const auto EncodeActor = [](AActor* Actor)
	{
		FAIDAEntityId Entity;
		Entity.Type = TEXT("actor");
		Entity.ClassPath = Actor->GetClass()->GetPathName();
		Entity.Pos = Actor->GetActorLocation();
		Entity.YawDeg = FMath::RoundToInt32(Actor->GetActorRotation().Yaw);
		return AIDAActionSpec::EncodeEntityId(Entity);
	};
	const auto Fail = [this](const FString& Detail, const FString& Reason)
	{
		++RunFailCount;
		if (RunFailures.Num() < 5)
		{
			RunFailures.Add(FString::Printf(TEXT("%s: %s"), *Detail, *Reason));
		}
	};

	const int32 End = FMath::Min(Proposal.Cursor + FMath::Max(1, Config.BatchPerTick), MutationQueue.Num());
	for (int32 i = Proposal.Cursor; i < End; ++i)
	{
		const FAIDAMutationTarget& Target = MutationQueue[i];
		AActor* Actor = Target.Actor.Get();
		if (!Actor)
		{
			++MissingCount;
			continue;
		}

		FAIDAMutationChange Change;
		switch (Proposal.MutationKind)
		{
		case EAIDAMutationKind::Clock:
		{
			double BeforePct = 100.0;
			if (!FAIDAActionSeam::ApplyClockMutation(Actor, Target.ClockPct, BeforePct))
			{
				Fail(Target.Detail, TEXT("can't change this machine's clock"));
				continue;
			}
			Change.EncodedEntity = EncodeActor(Actor);
			Change.Before = FString::Printf(TEXT("%.4g"), BeforePct);
			Change.After = FString::Printf(TEXT("%.4g"), Target.ClockPct);
			break;
		}
		case EAIDAMutationKind::Recipe:
		{
			FString Before, Error;
			if (!FAIDAActionSeam::ApplyRecipeMutation(WorldContext, Actor, Proposal.MutationRecipePath,
				Proposal.bMutationForce, Before, Error))
			{
				Fail(Target.Detail, Error);
				continue;
			}
			Change.EncodedEntity = EncodeActor(Actor);
			Change.Before = Before;
			Change.After = Proposal.MutationRecipePath;
			break;
		}
		case EAIDAMutationKind::BeltMk:
		{
			// The upgrade REPLACES the belt actor — the journal carries the NEW actor's identity so
			// undo can find it and drive the same path back to the old recipe.
			TArray<FAIDACostItem> Charged;
			FString Before, NewId, Error;
			if (!FAIDAActionSeam::ApplyBeltUpgrade(WorldContext, Actor, Proposal.MutationBeltRecipePath,
				Config.CostMode == TEXT("central"), Proposal.RequesterId, Charged, Before, NewId, Error))
			{
				Fail(Target.Detail, Error);
				continue;
			}
			for (const FAIDACostItem& Item : Charged)
			{
				bool bMerged = false;
				for (FAIDACostItem& Existing : MutationCharged)
				{
					if (Existing.ClassPath == Item.ClassPath)
					{
						Existing.Amount += Item.Amount;
						bMerged = true;
						break;
					}
				}
				if (!bMerged) { MutationCharged.Add(Item); }
			}
			Change.EncodedEntity = NewId;
			Change.Before = Before;
			Change.After = Proposal.MutationBeltRecipePath;
			break;
		}
		default:
			continue;
		}

		Proposal.AffectedEntityIds.Add(Change.EncodedEntity);
		MutationChanges.Add(MoveTemp(Change));
		++RunBuiltCount;
	}
	Proposal.Cursor = End;
	if (Proposal.Cursor < MutationQueue.Num()) { return true; }

	FinishExecution(WorldContext, Config, Memory, Proposal);
	return false;
}

bool FAIDAActionEngine::TickManifold(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal)
{
	const int32 N = Proposal.Placements.Num();

	// Phase 0 — the attachment row, batched like any build, with per-index actor capture.
	if (Proposal.Phase == 0)
	{
		int32 Skipped = 0;
		Proposal.Cursor += FAIDAActionSeam::ExecuteBuildBatch(WorldContext, Proposal.RecipeClassPath,
			Proposal.Placements, Proposal.Cursor, FMath::Max(1, Config.BatchPerTick),
			Proposal.AffectedEntityIds, BuiltActors, Skipped, &AttachmentActors);
		SkippedCount += Skipped;
		AccrueSkippedCost(WorldContext, Config, Proposal.RecipeClassPath, Skipped);
		if (Proposal.Cursor < N) { return true; }
		Proposal.Phase = 1;
		Proposal.Cursor = 0;
		return true;
	}

	// Phases 1-2 — ONE run per tick (spawn spline hologram → two-step place → validate → construct).
	const auto RunOne = [&](AActor* From, const FVector& FromDir, AActor* To, const FVector& ToDir,
		const TCHAR* What, int32 Index)
	{
		if (!From || !To)
		{
			++RunFailCount;
			if (RunFailures.Num() < 5)
			{
				RunFailures.Add(FString::Printf(TEXT("%s run %d: endpoint missing (attachment skipped or machine gone)"), What, Index));
			}
			return;
		}
		TArray<FAIDACostItem> Cost;
		FString Error;
		if (FAIDAActionSeam::BuildConnectingRun(WorldContext, Proposal.TransportRecipePath, Proposal.bManifoldPipe,
			From, FromDir, To, ToDir, Config.CostMode == TEXT("central"), Cost,
			Proposal.AffectedEntityIds, BuiltActors, Error, Proposal.RequesterId))
		{
			MergeCost(Proposal.Cost, Cost); // the journaled refund covers the whole manifold
			++RunBuiltCount;
		}
		else
		{
			++RunFailCount;
			if (RunFailures.Num() < 5)
			{
				RunFailures.Add(FString::Printf(TEXT("%s run %d: %s"), What, Index, *Error));
			}
			UE_LOG(LogAIDA, Warning, TEXT("[actions] manifold %s run %d failed: %s"), What, Index, *Error);
		}
	};

	const auto AttachmentAt = [&](int32 Index) -> AActor*
	{
		return AttachmentActors.IsValidIndex(Index) ? AttachmentActors[Index].Get() : nullptr;
	};

	if (Proposal.Phase == 1)
	{
		if (Proposal.Cursor >= N - 1) // single machine: no trunk at all
		{
			Proposal.Phase = 2;
			Proposal.Cursor = 0;
			return true;
		}
		const int32 Index = Proposal.Cursor++;
		// Splitter trunks flow ascending (feed at index 0); merger trunks flow descending (collection
		// at index 0). Pipes are direction-agnostic — the splitter arrangement serves them fine.
		const bool bReverse = Proposal.bManifoldOutput && !Proposal.bManifoldPipe;
		AActor* From = AttachmentAt(bReverse ? Index + 1 : Index);
		AActor* To = AttachmentAt(bReverse ? Index : Index + 1);
		const FVector FromDir = bReverse ? -Proposal.RowAxis : Proposal.RowAxis;
		RunOne(From, FromDir, To, -FromDir, TEXT("trunk"), Index);
		return true;
	}

	if (Proposal.Phase == 2)
	{
		if (Proposal.Cursor < N)
		{
			const int32 Index = Proposal.Cursor++;
			AActor* Attachment = AttachmentAt(Index);
			AActor* Machine = Proposal.Ports.IsValidIndex(Index) ? Proposal.Ports[Index].Machine.Get() : nullptr;
			const FVector MachineDir = Proposal.Ports.IsValidIndex(Index)
				? Proposal.Ports[Index].NormalCm : -Proposal.DropDir;
			if (Proposal.bManifoldOutput) // machine output → attachment (merger/junction) input
			{
				RunOne(Machine, MachineDir, Attachment, Proposal.DropDir, TEXT("drop"), Index);
			}
			else                          // attachment (splitter/junction) output → machine input
			{
				RunOne(Attachment, Proposal.DropDir, Machine, MachineDir, TEXT("drop"), Index);
			}
			if (Proposal.Cursor < N) { return true; }
		}
		if (!Proposal.bTap)
		{
			FinishExecution(WorldContext, Config, Memory, Proposal);
			return false;
		}
		Proposal.Phase = 3;
		Proposal.Cursor = 0;
		return true;
	}

	// Phase 3 — tap the source belt: splice the splitter in (cut variant) or claim the free end.
	if (Proposal.Phase == 3)
	{
		if (!PrepareTapSource(WorldContext, Config, Proposal))
		{
			FinishExecution(WorldContext, Config, Memory, Proposal);
			return false;
		}
		Proposal.Phase = 4;
		Proposal.Cursor = 0;
		return true;
	}

	// Phase 4 — the feed: one hop per tick, tap → waypoints → the trunk's open end (index 0).
	if (TickFeedHop(WorldContext, Config, Proposal, Proposal.TransportRecipePath, AttachmentAt(0), -Proposal.RowAxis))
	{
		return true;
	}

	FinishExecution(WorldContext, Config, Memory, Proposal);
	return false;
}

bool FAIDAActionEngine::PrepareTapSource(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAProposal& Proposal)
{
	AActor* SourceBelt = Proposal.TapBelt.Get();
	FString Error;
	if (!SourceBelt)
	{
		Error = TEXT("the source belt no longer exists");
	}
	else if (Proposal.bTapDangling)
	{
		TapSourceActor = SourceBelt;
	}
	else if (!FAIDAActionSeam::ExecuteTapSplice(WorldContext, SourceBelt, Proposal.TapOffsetCm,
		Proposal.TapSplitterRecipePath, Proposal.AffectedEntityIds, BuiltActors, TapSourceActor, Error))
	{
		AccrueSkippedCost(WorldContext, Config, Proposal.TapSplitterRecipePath, 1); // refund the unbuilt splitter
	}
	if (!TapSourceActor.IsValid())
	{
		++RunFailCount;
		if (RunFailures.Num() < 5)
		{
			RunFailures.Add(FString::Printf(TEXT("tap: %s"), *Error));
		}
		UE_LOG(LogAIDA, Warning, TEXT("[actions] belt tap failed: %s"), *Error);
		return false;
	}
	ChainPrevActor = nullptr;
	return true;
}

bool FAIDAActionEngine::TickFeedHop(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAProposal& Proposal,
	const FString& TransportPath, AActor* DestActor, const FVector& DestWantDir)
{
	const int32 TotalHops = Proposal.TapChainPointsCm.Num() + 1;
	if (Proposal.Cursor >= TotalHops) { return false; }
	const int32 Hop = Proposal.Cursor++;
	const bool bLast = Hop == TotalHops - 1;
	AActor* From = Hop == 0 ? TapSourceActor.Get() : ChainPrevActor.Get();

	const auto Fail = [&](const FString& Why)
	{
		++RunFailCount;
		if (RunFailures.Num() < 5)
		{
			RunFailures.Add(FString::Printf(TEXT("feed hop %d: %s"), Hop, *Why));
		}
		UE_LOG(LogAIDA, Warning, TEXT("[actions] feed hop %d failed: %s"), Hop, *Why);
		Proposal.Cursor = TotalHops; // the remaining hops have no source — abort them
	};
	if (!From)
	{
		Fail(TEXT("previous segment missing"));
		return false;
	}

	const FVector NextPos = bLast
		? (DestActor ? DestActor->GetActorLocation() : From->GetActorLocation())
		: Proposal.TapChainPointsCm[Hop];
	FVector FromDir = NextPos - From->GetActorLocation();
	FromDir = FVector(FromDir.X, FromDir.Y, 0.0).GetSafeNormal(UE_SMALL_NUMBER, Proposal.TapDirCm);

	TArray<FAIDACostItem> Cost;
	FString Error;
	bool bOk = false;
	if (bLast)
	{
		if (!DestActor)
		{
			Fail(TEXT("destination missing (attachment skipped or machine gone)"));
			return false;
		}
		bOk = FAIDAActionSeam::BuildConnectingRun(WorldContext, TransportPath, /*bPipe*/ false,
			From, FromDir, DestActor, DestWantDir, Config.CostMode == TEXT("central"), Cost,
			Proposal.AffectedEntityIds, BuiltActors, Error, Proposal.RequesterId);
	}
	else
	{
		TWeakObjectPtr<AActor> NewBelt;
		bOk = FAIDAActionSeam::BuildChainSegment(WorldContext, TransportPath, From, FromDir,
			Proposal.TapChainPointsCm[Hop], Config.CostMode == TEXT("central"), Cost,
			Proposal.AffectedEntityIds, BuiltActors, NewBelt, Error, Proposal.RequesterId);
		if (bOk) { ChainPrevActor = NewBelt; }
	}
	if (!bOk)
	{
		Fail(Error);
		return false;
	}
	MergeCost(Proposal.Cost, Cost); // the journaled refund covers the whole feed
	++RunBuiltCount;
	return Proposal.Cursor < TotalHops;
}

bool FAIDAActionEngine::TickTapOnly(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal)
{
	// Phase 0 — splice/claim the source; then one feed hop per tick to the stored destination port.
	if (Proposal.Phase == 0)
	{
		if (!PrepareTapSource(WorldContext, Config, Proposal))
		{
			FinishExecution(WorldContext, Config, Memory, Proposal);
			return false;
		}
		Proposal.Phase = 1;
		Proposal.Cursor = 0;
		return true;
	}
	AActor* Dest = Proposal.Ports.Num() > 0 ? Proposal.Ports[0].Machine.Get() : nullptr;
	const FVector DestDir = Proposal.Ports.Num() > 0 ? Proposal.Ports[0].NormalCm : -Proposal.TapDirCm;
	if (TickFeedHop(WorldContext, Config, Proposal, Proposal.TransportRecipePath, Dest, DestDir))
	{
		return true;
	}
	FinishExecution(WorldContext, Config, Memory, Proposal);
	return false;
}

bool FAIDAActionEngine::TickLabels(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal)
{
	// One sign per tick: each is a trace + hologram walk + construct + sign-data write, and label
	// counts are small (a container row) — smoothness over raw throughput, like the run phases.
	if (Proposal.Cursor < Proposal.LabelTargets.Num())
	{
		const int32 Index = Proposal.Cursor++;
		const FAIDALabelTarget& Target = Proposal.LabelTargets[Index];
		FString Error;
		if (FAIDAActionSeam::BuildLabelSign(WorldContext, Proposal.RecipeClassPath, Target.Container.Get(),
			Target.SignPosCm, Target.OutwardCm, Target.Text,
			Proposal.AffectedEntityIds, BuiltActors, Error))
		{
			++RunBuiltCount;
		}
		else
		{
			++RunFailCount;
			AccrueSkippedCost(WorldContext, Config, Proposal.RecipeClassPath, 1); // the sign was paid upfront
			if (RunFailures.Num() < 5)
			{
				RunFailures.Add(FString::Printf(TEXT("sign %d (%s, \"%s\"): %s"),
					Index, *Target.ContainerClass, *Target.Text, *Error));
			}
			UE_LOG(LogAIDA, Warning, TEXT("[actions] label sign %d failed: %s"), Index, *Error);
		}
		if (Proposal.Cursor < Proposal.LabelTargets.Num()) { return true; }
	}

	FinishExecution(WorldContext, Config, Memory, Proposal);
	return false;
}

bool FAIDAActionEngine::TickPowered(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal)
{
	const int32 Batch = FMath::Max(1, Config.BatchPerTick);

	// Phase 0 — the machines themselves, per-index captured for the wiring phase. Power-only
	// proposals (propose_power) wire EXISTING machines: the slots fill from the resolved actors
	// and nothing is built here.
	if (Proposal.Phase == 0)
	{
		if (Proposal.bPowerOnly)
		{
			AttachmentActors.Reset();
			for (const FAIDAManifoldPort& Machine : Proposal.Ports)
			{
				AttachmentActors.Add(Machine.Machine);
			}
			Proposal.Phase = 1;
			Proposal.Cursor = 0;
			return true;
		}
		int32 Skipped = 0;
		Proposal.Cursor += FAIDAActionSeam::ExecuteBuildBatch(WorldContext, Proposal.RecipeClassPath,
			Proposal.Placements, Proposal.Cursor, Batch,
			Proposal.AffectedEntityIds, BuiltActors, Skipped, &AttachmentActors);
		SkippedCount += Skipped;
		AccrueSkippedCost(WorldContext, Config, Proposal.RecipeClassPath, Skipped);
		if (Proposal.Cursor < Proposal.Placements.Num()) { return true; }
		Proposal.Phase = 1;
		Proposal.Cursor = 0;
		return true;
	}

	// Phase 1 — the pole lane(s). A pole that fails re-validation is skipped; its machines' wires
	// then fail loudly below instead of silently going nowhere.
	if (Proposal.Phase == 1)
	{
		int32 Skipped = 0;
		Proposal.Cursor += FAIDAActionSeam::ExecuteBuildBatch(WorldContext, Proposal.PoleRecipePath,
			Proposal.PolePlacements, Proposal.Cursor, Batch,
			Proposal.AffectedEntityIds, BuiltActors, Skipped, &PoleActors);
		SkippedCount += Skipped;
		AccrueSkippedCost(WorldContext, Config, Proposal.PoleRecipePath, Skipped);
		if (Proposal.Cursor < Proposal.PolePlacements.Num()) { return true; }
		Proposal.Phase = 2;
		Proposal.Cursor = 0;
		return true;
	}

	// Phase 2 — power lines (machine→pole, then the pole chain), batched; then ONE grid tie-in.
	const auto ActorAt = [](const TArray<TWeakObjectPtr<AActor>>& Arr, int32 Index) -> AActor*
	{
		return Arr.IsValidIndex(Index) ? Arr[Index].Get() : nullptr;
	};
	const auto WireOne = [&](AActor* A, AActor* B, const TCHAR* What, int32 Index) -> bool
	{
		if (!A || !B)
		{
			++RunFailCount;
			if (RunFailures.Num() < 5)
			{
				RunFailures.Add(FString::Printf(TEXT("%s wire %d: endpoint missing (machine or pole skipped)"), What, Index));
			}
			return false;
		}
		TArray<FAIDACostItem> Cost;
		FString Error;
		if (FAIDAActionSeam::BuildWire(WorldContext, Proposal.WireRecipePath, A, B,
			Config.CostMode == TEXT("central"), Cost, Proposal.AffectedEntityIds, BuiltActors, Error,
			Proposal.RequesterId))
		{
			MergeCost(Proposal.Cost, Cost);
			++RunBuiltCount;
			return true;
		}
		++RunFailCount;
		if (RunFailures.Num() < 5)
		{
			RunFailures.Add(FString::Printf(TEXT("%s wire %d: %s"), What, Index, *Error));
		}
		UE_LOG(LogAIDA, Warning, TEXT("[actions] %s wire %d failed: %s"), What, Index, *Error);
		return false;
	};

	const int32 TotalWires = Proposal.MachineWires.Num() + Proposal.ChainWires.Num();
	int32 DoneThisTick = 0;
	while (Proposal.Cursor < TotalWires && DoneThisTick < Batch)
	{
		const int32 Index = Proposal.Cursor++;
		++DoneThisTick;
		if (Index < Proposal.MachineWires.Num())
		{
			const FIntPoint& Pair = Proposal.MachineWires[Index];
			WireOne(ActorAt(AttachmentActors, Pair.X), ActorAt(PoleActors, Pair.Y), TEXT("machine"), Index);
		}
		else
		{
			const FIntPoint& Pair = Proposal.ChainWires[Index - Proposal.MachineWires.Num()];
			WireOne(ActorAt(PoleActors, Pair.X), ActorAt(PoleActors, Pair.Y), TEXT("chain"), Index);
		}
	}
	if (Proposal.Cursor < TotalWires) { return true; }

	// The grid tie: one line from an end pole to the nearest external circuit with a free slot.
	AActor* External = nullptr;
	int32 FromPole = INDEX_NONE;
	if (FAIDAActionSeam::FindGridTie(WorldContext, PoleActors, /*RangeCm*/ 10000.0, External, FromPole))
	{
		WireOne(ActorAt(PoleActors, FromPole), External, TEXT("grid tie"), 0);
	}
	else if (Proposal.PolePlacements.Num() > 0)
	{
		RunReport.Add(TEXT("no existing power grid within 100 m — connect the feed line manually"));
	}

	FinishExecution(WorldContext, Config, Memory, Proposal);
	return false;
}

bool FAIDAActionEngine::TickConnected(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal)
{
	const int32 Batch = FMath::Max(1, Config.BatchPerTick);
	const int32 Sets = Proposal.ManifoldSets.Num();
	// Phase map: 0 = machines; per set s: 1+3s attachments, 2+3s trunk runs, 3+3s drops; then
	// poles = 1+3*Sets and wires = 2+3*Sets when the revised build carried an auto-power kit.
	const int32 PolePhase = 1 + 3 * Sets;
	const int32 WirePhase = PolePhase + 1;

	// Phase 0 — the machines, index-captured so the sets' planned ports can rebind to real actors.
	if (Proposal.Phase == 0)
	{
		// Spec-v2 composites: placements are grouped by part, so clamp each tick's batch to the
		// current part's contiguous run and construct with THAT part's recipe (same shape as the
		// plain build path).
		FString Recipe = Proposal.RecipeClassPath;
		int32 MachineBatch = Batch;
		if (Proposal.PlacementPartIndex.Num() == Proposal.Placements.Num()
			&& Proposal.PlacementPartIndex.IsValidIndex(Proposal.Cursor))
		{
			const int32 Part = Proposal.PlacementPartIndex[Proposal.Cursor];
			if (Proposal.PartRecipePaths.IsValidIndex(Part))
			{
				Recipe = Proposal.PartRecipePaths[Part];
			}
			int32 RunEnd = Proposal.Cursor;
			while (RunEnd < Proposal.PlacementPartIndex.Num() && Proposal.PlacementPartIndex[RunEnd] == Part)
			{
				++RunEnd;
			}
			MachineBatch = FMath::Min(MachineBatch, RunEnd - Proposal.Cursor);
		}

		int32 Skipped = 0;
		Proposal.Cursor += FAIDAActionSeam::ExecuteBuildBatch(WorldContext, Recipe,
			Proposal.Placements, Proposal.Cursor, MachineBatch,
			Proposal.AffectedEntityIds, BuiltActors, Skipped, &AttachmentActors);
		SkippedCount += Skipped;
		AccrueSkippedCost(WorldContext, Config, Recipe, Skipped);
		if (Proposal.Cursor < Proposal.Placements.Num()) { return true; }

		// Rebind: planned ports (Machine was null since propose) now point at the BUILT actors.
		// A skipped machine leaves a null and its runs fail loudly instead of re-mapping.
		for (FAIDAManifoldSet& Set : Proposal.ManifoldSets)
		{
			for (int32 i = 0; i < Set.Ports.Num(); ++i)
			{
				const int32 MachineIdx = Set.PortMachineIndex.IsValidIndex(i) ? Set.PortMachineIndex[i] : INDEX_NONE;
				Set.Ports[i].Machine = AttachmentActors.IsValidIndex(MachineIdx) ? AttachmentActors[MachineIdx] : nullptr;
			}
		}
		SetAttachmentActors.SetNum(Sets);
		Proposal.Phase = 1;
		Proposal.Cursor = 0;
		return true;
	}

	// One connecting run per tick — TickManifold's RunOne, parameterized by the owning set.
	const auto RunOne = [&](const FAIDAManifoldSet& Set, AActor* From, const FVector& FromDir,
		AActor* To, const FVector& ToDir, const TCHAR* What, int32 Index)
	{
		if (!From || !To)
		{
			++RunFailCount;
			if (RunFailures.Num() < 5)
			{
				RunFailures.Add(FString::Printf(TEXT("%s run %d: endpoint missing (attachment or machine skipped)"), What, Index));
			}
			return;
		}
		TArray<FAIDACostItem> Cost;
		FString Error;
		if (FAIDAActionSeam::BuildConnectingRun(WorldContext, Set.TransportRecipePath, Set.bPipe,
			From, FromDir, To, ToDir, Config.CostMode == TEXT("central"), Cost,
			Proposal.AffectedEntityIds, BuiltActors, Error, Proposal.RequesterId))
		{
			MergeCost(Proposal.Cost, Cost); // the journaled refund covers the whole group
			++RunBuiltCount;
		}
		else
		{
			++RunFailCount;
			if (RunFailures.Num() < 5)
			{
				RunFailures.Add(FString::Printf(TEXT("%s run %d: %s"), What, Index, *Error));
			}
			UE_LOG(LogAIDA, Warning, TEXT("[actions] connected %s run %d failed: %s"), What, Index, *Error);
		}
	};

	if (Proposal.Phase < PolePhase)
	{
		const int32 SetIdx = (Proposal.Phase - 1) / 3;
		const int32 Sub = (Proposal.Phase - 1) % 3; // 0 = attachments, 1 = trunk, 2 = drops
		if (!Proposal.ManifoldSets.IsValidIndex(SetIdx))
		{
			Proposal.Phase = PolePhase;
			Proposal.Cursor = 0;
			return true;
		}
		FAIDAManifoldSet& Set = Proposal.ManifoldSets[SetIdx];
		const int32 N = Set.Attachments.Num();
		if (SetAttachmentActors.Num() < Sets) { SetAttachmentActors.SetNum(Sets); }
		TArray<TWeakObjectPtr<AActor>>& Attachments = SetAttachmentActors[SetIdx];
		const auto AttachmentAt = [&Attachments](int32 Index) -> AActor*
		{
			return Attachments.IsValidIndex(Index) ? Attachments[Index].Get() : nullptr;
		};

		if (Sub == 0) // the set's attachment row, batched with per-index capture
		{
			int32 Skipped = 0;
			Proposal.Cursor += FAIDAActionSeam::ExecuteBuildBatch(WorldContext, Set.AttachmentRecipePath,
				Set.Attachments, Proposal.Cursor, Batch,
				Proposal.AffectedEntityIds, BuiltActors, Skipped, &Attachments);
			SkippedCount += Skipped;
			AccrueSkippedCost(WorldContext, Config, Set.AttachmentRecipePath, Skipped);
			if (Proposal.Cursor < N) { return true; }
			++Proposal.Phase;
			Proposal.Cursor = 0;
			return true;
		}
		if (Sub == 1) // trunk hops between consecutive attachments
		{
			if (Proposal.Cursor >= N - 1) // single machine: no trunk at all
			{
				++Proposal.Phase;
				Proposal.Cursor = 0;
				return true;
			}
			const int32 Index = Proposal.Cursor++;
			// Splitter trunks flow ascending (feed at index 0); merger trunks flow descending.
			const bool bReverse = Set.bOutput && !Set.bPipe;
			AActor* From = AttachmentAt(bReverse ? Index + 1 : Index);
			AActor* To = AttachmentAt(bReverse ? Index : Index + 1);
			const FVector FromDir = bReverse ? -Set.RowAxis : Set.RowAxis;
			RunOne(Set, From, FromDir, To, -FromDir, TEXT("trunk"), Index);
			return true;
		}
		// Sub == 2 — drops between each attachment and its machine's port.
		if (Proposal.Cursor < N)
		{
			const int32 Index = Proposal.Cursor++;
			AActor* Attachment = AttachmentAt(Index);
			AActor* Machine = Set.Ports.IsValidIndex(Index) ? Set.Ports[Index].Machine.Get() : nullptr;
			const FVector MachineDir = Set.Ports.IsValidIndex(Index) ? Set.Ports[Index].NormalCm : -Set.DropDir;
			if (Set.bOutput) // machine output → attachment (merger/junction) input
			{
				RunOne(Set, Machine, MachineDir, Attachment, Set.DropDir, TEXT("drop"), Index);
			}
			else             // attachment (splitter/junction) output → machine input
			{
				RunOne(Set, Attachment, Set.DropDir, Machine, MachineDir, TEXT("drop"), Index);
			}
			if (Proposal.Cursor < N) { return true; }
		}
		++Proposal.Phase;
		Proposal.Cursor = 0;
		return true;
	}

	// Auto-power kit carried over from the revised build: pole lane, then wires + one grid tie —
	// the same shape as TickPowered, against the machine slots captured in phase 0.
	if (Proposal.bAutoPower && Proposal.Phase == PolePhase)
	{
		if (Proposal.Cursor < Proposal.PolePlacements.Num())
		{
			int32 Skipped = 0;
			Proposal.Cursor += FAIDAActionSeam::ExecuteBuildBatch(WorldContext, Proposal.PoleRecipePath,
				Proposal.PolePlacements, Proposal.Cursor, Batch,
				Proposal.AffectedEntityIds, BuiltActors, Skipped, &PoleActors);
			SkippedCount += Skipped;
			AccrueSkippedCost(WorldContext, Config, Proposal.PoleRecipePath, Skipped);
			if (Proposal.Cursor < Proposal.PolePlacements.Num()) { return true; }
		}
		Proposal.Phase = WirePhase;
		Proposal.Cursor = 0;
		return true;
	}
	if (Proposal.bAutoPower && Proposal.Phase == WirePhase)
	{
		const auto ActorAt = [](const TArray<TWeakObjectPtr<AActor>>& Arr, int32 Index) -> AActor*
		{
			return Arr.IsValidIndex(Index) ? Arr[Index].Get() : nullptr;
		};
		const auto WireOne = [&](AActor* A, AActor* B, const TCHAR* What, int32 Index) -> bool
		{
			if (!A || !B)
			{
				++RunFailCount;
				if (RunFailures.Num() < 5)
				{
					RunFailures.Add(FString::Printf(TEXT("%s wire %d: endpoint missing (machine or pole skipped)"), What, Index));
				}
				return false;
			}
			TArray<FAIDACostItem> Cost;
			FString Error;
			if (FAIDAActionSeam::BuildWire(WorldContext, Proposal.WireRecipePath, A, B,
				Config.CostMode == TEXT("central"), Cost, Proposal.AffectedEntityIds, BuiltActors, Error,
				Proposal.RequesterId))
			{
				MergeCost(Proposal.Cost, Cost);
				++RunBuiltCount;
				return true;
			}
			++RunFailCount;
			if (RunFailures.Num() < 5)
			{
				RunFailures.Add(FString::Printf(TEXT("%s wire %d: %s"), What, Index, *Error));
			}
			UE_LOG(LogAIDA, Warning, TEXT("[actions] %s wire %d failed: %s"), What, Index, *Error);
			return false;
		};

		const int32 TotalWires = Proposal.MachineWires.Num() + Proposal.ChainWires.Num();
		int32 DoneThisTick = 0;
		while (Proposal.Cursor < TotalWires && DoneThisTick < Batch)
		{
			const int32 Index = Proposal.Cursor++;
			++DoneThisTick;
			if (Index < Proposal.MachineWires.Num())
			{
				const FIntPoint& Pair = Proposal.MachineWires[Index];
				WireOne(ActorAt(AttachmentActors, Pair.X), ActorAt(PoleActors, Pair.Y), TEXT("machine"), Index);
			}
			else
			{
				const FIntPoint& Pair = Proposal.ChainWires[Index - Proposal.MachineWires.Num()];
				WireOne(ActorAt(PoleActors, Pair.X), ActorAt(PoleActors, Pair.Y), TEXT("chain"), Index);
			}
		}
		if (Proposal.Cursor < TotalWires) { return true; }

		AActor* External = nullptr;
		int32 FromPole = INDEX_NONE;
		if (FAIDAActionSeam::FindGridTie(WorldContext, PoleActors, /*RangeCm*/ 10000.0, External, FromPole))
		{
			WireOne(ActorAt(PoleActors, FromPole), External, TEXT("grid tie"), 0);
		}
		else if (Proposal.PolePlacements.Num() > 0)
		{
			RunReport.Add(TEXT("no existing power grid within 100 m — connect the feed line manually"));
		}
	}

	// Tap phases (belt-tap combined proposals) — last, so the feed run's open-end attachment
	// exists. Plain proposals arrive here at PolePhase; powered ones fall through the wire block.
	const int32 TapPhase = WirePhase + 1;
	if (Proposal.bTap)
	{
		if (Proposal.Phase < TapPhase)
		{
			Proposal.Phase = TapPhase;
			Proposal.Cursor = 0;
			return true;
		}
		if (Proposal.Phase == TapPhase)
		{
			if (!PrepareTapSource(WorldContext, Config, Proposal))
			{
				FinishExecution(WorldContext, Config, Memory, Proposal);
				return false;
			}
			Proposal.Phase = TapPhase + 1;
			Proposal.Cursor = 0;
			return true;
		}
		// The feed: one hop per tick, tap → waypoints → the belt-in set's trunk open end (index 0).
		const int32 SetIdx = Proposal.TapSetIndex;
		if (Proposal.ManifoldSets.IsValidIndex(SetIdx))
		{
			const FAIDAManifoldSet& FeedSet = Proposal.ManifoldSets[SetIdx];
			AActor* OpenEnd = (SetAttachmentActors.IsValidIndex(SetIdx) && SetAttachmentActors[SetIdx].Num() > 0)
				? SetAttachmentActors[SetIdx][0].Get() : nullptr;
			if (TickFeedHop(WorldContext, Config, Proposal, FeedSet.TransportRecipePath, OpenEnd, -FeedSet.RowAxis))
			{
				return true;
			}
		}
	}

	FinishExecution(WorldContext, Config, Memory, Proposal);
	return false;
}

void FAIDAActionEngine::AccrueSkippedCost(UObject* WorldContext, const FAIDAActionsConfig& Config, const FString& RecipeClassPath, int32 Skipped)
{
	if (Skipped <= 0 || Config.CostMode != TEXT("central")) { return; }

	TArray<FAIDACostItem> Items;
	if (FAIDAActionSeam::TallyRecipeCost(WorldContext, RecipeClassPath, Skipped, Items))
	{
		MergeCost(AccruedRefund, Items);
	}
}

void FAIDAActionEngine::FinishExecution(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal)
{
	// Dismantle refunds pay out to the approver's inventory (central storage has no deposit API).
	if (Proposal.bDismantle && Config.CostMode == TEXT("central") && AccruedRefund.Num() > 0)
	{
		int32 Refunded = 0, Lost = 0;
		FAIDAActionSeam::RefundToPlayer(WorldContext, Proposal.ApproverId, AccruedRefund, Refunded, Lost);
		UE_LOG(LogAIDA, Log, TEXT("[actions] dismantle refund: %d item(s) to the approver, %d lost."), Refunded, Lost);
	}
	else if (!Proposal.bDismantle && Config.CostMode == TEXT("central") && AccruedRefund.Num() > 0)
	{
		// Build skips (placements that failed re-validation — a partially-blocked ghost approved
		// as-is, or a world that changed underneath) give back what the upfront charge covered but
		// never built. The REQUESTER paid, so they get the refund (pockets — no central deposit
		// API); the journal below records the NET cost so undo can't refund the skips twice.
		SubtractCost(Proposal.Cost, AccruedRefund);
		int32 Refunded = 0, Lost = 0;
		FAIDAActionSeam::RefundToPlayer(WorldContext, Proposal.RequesterId, AccruedRefund, Refunded, Lost);
		UE_LOG(LogAIDA, Log, TEXT("[actions] skipped-placement refund: %d item(s) to the requester, %d lost."), Refunded, Lost);
		if (Refunded > 0)
		{
			RunReport.Add(FString::Printf(TEXT("refunded %d item(s) for placements that no longer validated"), Refunded));
		}
	}

	// The persistent record undo works from (docs/PHASE4.md §2d).
	const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
	FAIDAJournalEntry Entry;
	Entry.ProposalSpecJson = Proposal.SpecJson;
	Entry.RequesterId = Proposal.RequesterId;
	Entry.ApproverId = Proposal.ApproverId;
	Entry.ProposedUtc = Proposal.ProposedUtc;
	Entry.ExecutedUtc = NowUtc;
	Entry.AffectedEntityIds = Proposal.AffectedEntityIds;
	Entry.RefundJson = AIDAActionSpec::CostItemsToJson(
		Proposal.MutationKind != EAIDAMutationKind::None ? MutationCharged
		: (Proposal.bDismantle ? AccruedRefund : Proposal.Cost));
	Entry.bDismantle = Proposal.bDismantle;
	if (Proposal.MutationKind != EAIDAMutationKind::None)
	{
		Entry.MutationJson = AIDAActionSpec::BuildMutationJson(Proposal.MutationKind, MutationChanges);
	}
	const FGuid JournalId = Memory.AppendJournal(WorldContext, MoveTemp(Entry));

	if (JournalId.IsValid() && BuiltActors.Num() > 0)
	{
		SessionActors.Add(JournalId, BuiltActors);
	}

	ProposalStore.Transition(Proposal.Id, EAIDAProposalState::Executed, NowUtc);

	const int32 Affected = Proposal.AffectedEntityIds.Num();
	if (Proposal.MutationKind != EAIDAMutationKind::None)
	{
		UE_LOG(LogAIDA, Log, TEXT("[actions] %s DONE (mutation): %d change(s), %d failed, %d missing (journal %s)."),
			*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), RunBuiltCount, RunFailCount, MissingCount,
			*JournalId.ToString(EGuidFormats::DigitsWithHyphens));
		if (RunFailCount > 0 || MissingCount > 0)
		{
			RunReport.Add(FString::Printf(TEXT("mutation: %d change(s) applied, %d skipped/failed"),
				RunBuiltCount, RunFailCount + MissingCount));
			RunReport.Append(RunFailures);
		}
	}
	else if (Proposal.bManifold || Proposal.bAutoPower || Proposal.bLabel || Proposal.ManifoldSets.Num() > 0)
	{
		const TCHAR* Kind = Proposal.ManifoldSets.Num() > 0 ? TEXT("connected")
			: (Proposal.bManifold ? TEXT("manifold") : (Proposal.bLabel ? TEXT("labels") : TEXT("power")));
		const TCHAR* Piece = (Proposal.bManifold || Proposal.ManifoldSets.Num() > 0) ? TEXT("run")
			: (Proposal.bLabel ? TEXT("sign") : TEXT("wire"));
		int32 PlacementTotal = Proposal.bPowerOnly
			? Proposal.PolePlacements.Num() // Placements mirror the poles for ghosts — don't double count
			: Proposal.Placements.Num() + Proposal.PolePlacements.Num();
		for (const FAIDAManifoldSet& Set : Proposal.ManifoldSets)
		{
			PlacementTotal += Set.Attachments.Num();
		}
		UE_LOG(LogAIDA, Log, TEXT("[actions] %s DONE (%s): %d placement(s) (%d skipped), %d %s(s) built, %d %s(s) failed (journal %s)."),
			*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), Kind,
			PlacementTotal - SkippedCount, SkippedCount,
			RunBuiltCount, Piece, RunFailCount, Piece,
			*JournalId.ToString(EGuidFormats::DigitsWithHyphens));
		if (SkippedCount > 0 || RunFailCount > 0)
		{
			RunReport.Add(FString::Printf(TEXT("%s: %d placement(s) skipped, %d of %d %s(s) failed to connect"),
				Kind, SkippedCount, RunFailCount, RunBuiltCount + RunFailCount, Piece));
			RunReport.Append(RunFailures);
		}
	}
	else if (Proposal.bDismantle)
	{
		UE_LOG(LogAIDA, Log, TEXT("[actions] %s DONE: removed %d, missing %d (journal %s)."),
			*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), RemovedCount, MissingCount,
			*JournalId.ToString(EGuidFormats::DigitsWithHyphens));
	}
	else
	{
		UE_LOG(LogAIDA, Log, TEXT("[actions] %s DONE: built %d, skipped %d (journal %s)."),
			*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), Affected, SkippedCount,
			*JournalId.ToString(EGuidFormats::DigitsWithHyphens));
		if (!Proposal.bDismantle && SkippedCount > 0)
		{
			RunReport.Add(FString::Printf(TEXT("%d placement(s) skipped — still blocked at execute (uneven ground or obstruction)"),
				SkippedCount));
		}
	}

	ResetScratch();
}

void FAIDAActionEngine::ResetScratch()
{
	ExecutingId.Invalidate();
	DismantleQueue.Reset();
	MutationQueue.Reset();
	MutationChanges.Reset();
	MutationCharged.Reset();
	BuiltActors.Reset();
	AccruedRefund.Reset();
	AttachmentActors.Reset();
	PoleActors.Reset();
	SetAttachmentActors.Reset();
	TapSourceActor = nullptr;
	ChainPrevActor = nullptr;
	RunFailures.Reset();
	SkippedCount = RemovedCount = MissingCount = RunBuiltCount = RunFailCount = 0;
}

bool FAIDAActionEngine::StartUndo(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory,
	int32 Count, const FString& InstigatorId, FString& OutMessage)
{
	if (IsExecuting() || UndoQueue.Num() > 0)
	{
		OutMessage = TEXT("another action is executing — wait for it to finish");
		return false;
	}
	AAIDAMemoryStore* Store = Memory.Store(WorldContext);
	if (!Store)
	{
		OutMessage = TEXT("the action journal is unavailable (server only)");
		return false;
	}

	// Only the last undoWindow journal entries are reachable; skip already-undone; newest first.
	const TArray<FAIDAJournalEntry>& Journal = Store->GetJournal();
	const int32 Oldest = FMath::Max(0, Journal.Num() - FMath::Max(1, Config.UndoWindow));
	int32 TotalEntities = 0;
	for (int32 i = Journal.Num() - 1; i >= Oldest && UndoQueue.Num() < Count; --i)
	{
		const FAIDAJournalEntry& Entry = Journal[i];
		if (Entry.bUndone) { continue; }

		FUndoJob Job;
		Job.JournalId = Entry.Id;
		Job.bDismantle = Entry.bDismantle;
		// Mutation entries restore before-values — their entity list is inside MutationJson, and
		// the AffectedEntityIds walk must not ALSO dismantle the touched machines.
		if (!Entry.MutationJson.IsEmpty()
			&& AIDAActionSpec::ParseMutationJson(Entry.MutationJson, Job.MutationKind, Job.MutationRestores))
		{
			Job.Refund = AIDAActionSpec::ParseCostItems(Entry.RefundJson);
			TotalEntities += Job.MutationRestores.Num();
			UndoQueue.Add(MoveTemp(Job));
			continue;
		}
		for (const FString& Encoded : Entry.AffectedEntityIds)
		{
			FAIDAEntityId Entity;
			if (AIDAActionSpec::DecodeEntityId(Encoded, Entity)) { Job.Entities.Add(MoveTemp(Entity)); }
		}
		if (const TArray<TWeakObjectPtr<AActor>>* Cached = SessionActors.Find(Entry.Id))
		{
			Job.CachedActors = *Cached;
		}
		Job.Refund = AIDAActionSpec::ParseCostItems(Entry.RefundJson);
		TotalEntities += Job.Entities.Num();
		UndoQueue.Add(MoveTemp(Job));
	}
	if (UndoQueue.Num() == 0)
	{
		OutMessage = FString::Printf(TEXT("nothing to undo — no reversible AIDA action in the last %d journal entries"), Config.UndoWindow);
		return false;
	}

	UndoInstigatorId = InstigatorId;
	OutMessage = FString::Printf(TEXT("undoing %d action(s), %d entit%s…"),
		UndoQueue.Num(), TotalEntities, TotalEntities == 1 ? TEXT("y") : TEXT("ies"));
	UE_LOG(LogAIDA, Log, TEXT("[actions] %s"), *OutMessage);
	return true;
}

bool FAIDAActionEngine::TickUndo(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory)
{
	if (UndoQueue.Num() == 0) { return false; }

	FUndoJob& Job = UndoQueue[0];
	if (Job.MutationKind != EAIDAMutationKind::None)
	{
		// Restore each change's before value on the re-resolved actor (belts drive the same
		// upgrade path back to the old recipe, free of charge — the refund flows at finish).
		const int32 MutEnd = FMath::Min(Job.Cursor + FMath::Max(1, Config.BatchPerTick), Job.MutationRestores.Num());
		for (int32 i = Job.Cursor; i < MutEnd; ++i)
		{
			const FAIDAMutationChange& Change = Job.MutationRestores[i];
			FAIDAEntityId Entity;
			AActor* Actor = AIDAActionSpec::DecodeEntityId(Change.EncodedEntity, Entity)
				? FAIDAActionSeam::ResolveMutationActor(WorldContext, Entity) : nullptr;
			bool bOk = false;
			if (Actor)
			{
				switch (Job.MutationKind)
				{
				case EAIDAMutationKind::Clock:
				{
					double Dummy = 0.0;
					bOk = FAIDAActionSeam::ApplyClockMutation(Actor, FCString::Atod(*Change.Before), Dummy);
					break;
				}
				case EAIDAMutationKind::Recipe:
				{
					FString DummyBefore, Error;
					bOk = FAIDAActionSeam::ApplyRecipeMutation(WorldContext, Actor, Change.Before,
						/*bForce*/ true, DummyBefore, Error);
					break;
				}
				case EAIDAMutationKind::BeltMk:
				{
					TArray<FAIDACostItem> DummyCharged;
					FString DummyBefore, DummyNewId, Error;
					bOk = FAIDAActionSeam::ApplyBeltUpgrade(WorldContext, Actor, Change.Before,
						/*bChargeCost*/ false, FString(), DummyCharged, DummyBefore, DummyNewId, Error);
					break;
				}
				default: break;
				}
			}
			bOk ? ++Job.Done : ++Job.Missing;
		}
		Job.Cursor = MutEnd;
		if (Job.Cursor >= Job.MutationRestores.Num())
		{
			FinishUndoJob(WorldContext, Config, Memory, Job);
			UndoQueue.RemoveAt(0);
		}
		return UndoQueue.Num() > 0;
	}

	const int32 End = FMath::Min(Job.Cursor + FMath::Max(1, Config.BatchPerTick), Job.Entities.Num());
	for (int32 i = Job.Cursor; i < End; ++i)
	{
		const FAIDAEntityId& Entity = Job.Entities[i];
		bool bOk = false;
		if (Job.bDismantle)
		{
			bOk = FAIDAActionSeam::UndoRebuildEntity(WorldContext, Entity);
		}
		else
		{
			// The in-session fast path: a still-live cached actor at this entity's position.
			AActor* Cached = nullptr;
			for (const TWeakObjectPtr<AActor>& Weak : Job.CachedActors)
			{
				AActor* Actor = Weak.Get();
				if (Actor && Actor->GetActorLocation().Equals(Entity.Pos, 5.0))
				{
					Cached = Actor;
					break;
				}
			}
			bOk = FAIDAActionSeam::UndoRemoveEntity(WorldContext, Entity, Cached);
		}
		bOk ? ++Job.Done : ++Job.Missing;
	}
	Job.Cursor = End;

	if (Job.Cursor >= Job.Entities.Num())
	{
		FinishUndoJob(WorldContext, Config, Memory, Job);
		UndoQueue.RemoveAt(0);
	}
	return UndoQueue.Num() > 0;
}

void FAIDAActionEngine::FinishUndoJob(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FUndoJob& Job)
{
	// Consume the entry even on a partial undo — retrying the same entry would double-refund.
	Memory.MarkUndone(WorldContext, Job.JournalId);
	SessionActors.Remove(Job.JournalId);

	const TCHAR* Verb = Job.MutationKind != EAIDAMutationKind::None ? TEXT("restored")
		: (Job.bDismantle ? TEXT("rebuilt") : TEXT("removed"));
	const int32 Total = Job.MutationKind != EAIDAMutationKind::None ? Job.MutationRestores.Num() : Job.Entities.Num();
	FString Line = FString::Printf(TEXT("undo: %s %d of %d entit%s"), Verb, Job.Done, Total, Total == 1 ? TEXT("y") : TEXT("ies"));
	if (Job.Missing > 0)
	{
		Line += FString::Printf(TEXT(" (%d already gone or blocked)"), Job.Missing);
	}

	// Money flows, scaled to what actually reversed (a player who manually dismantled AIDA's
	// buildables already pocketed those refunds — never pay them twice).
	if (Config.CostMode == TEXT("central") && Job.Refund.Num() > 0 && Job.Done > 0 && Total > 0)
	{
		TArray<FAIDACostItem> Scaled = Job.Refund;
		for (FAIDACostItem& Item : Scaled)
		{
			Item.Amount = static_cast<int32>(static_cast<int64>(Item.Amount) * Job.Done / Total);
		}
		Scaled.RemoveAll([](const FAIDACostItem& Item) { return Item.Amount <= 0; });

		if (Job.bDismantle)
		{
			// Undo of a dismantle: collect the earlier refund back (central first, then the undo
			// instigator's pockets). Best-effort — the items may have been spent; the rebuild
			// still stands (report it, don't block).
			if (Scaled.Num() > 0 && !FAIDAActionSeam::DeductCost(WorldContext, Scaled, UndoInstigatorId))
			{
				Line += TEXT("; the earlier refund could not be re-collected");
			}
		}
		else
		{
			int32 Refunded = 0, Lost = 0;
			FAIDAActionSeam::RefundToPlayer(WorldContext, UndoInstigatorId, Scaled, Refunded, Lost);
			if (Refunded > 0) { Line += FString::Printf(TEXT("; refunded %d item(s)"), Refunded); }
			if (Lost > 0) { Line += FString::Printf(TEXT(" (%d didn't fit)"), Lost); }
		}
	}

	UE_LOG(LogAIDA, Log, TEXT("[actions] %s (journal %s)"), *Line, *Job.JournalId.ToString(EGuidFormats::DigitsWithHyphens));
	UndoReport.Add(MoveTemp(Line));
}

TArray<FString> FAIDAActionEngine::TakeUndoReport()
{
	TArray<FString> Out = MoveTemp(UndoReport);
	UndoReport.Reset();
	return Out;
}

TArray<FString> FAIDAActionEngine::TakeRunReport()
{
	TArray<FString> Out = MoveTemp(RunReport);
	RunReport.Reset();
	return Out;
}
