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
	else if (Config.CostMode == TEXT("central"))
	{
		// Pay the whole tally upfront; failure leaves the proposal pending (player can restock).
		if (!FAIDAActionSeam::DeductCost(WorldContext, Proposal->Cost))
		{
			OutMessage = TEXT("no longer affordable from central storage — restock and approve again");
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
	RunFailures.Reset();
	SkippedCount = RemovedCount = MissingCount = RunBuiltCount = RunFailCount = 0;
	Proposal->Cursor = 0;
	Proposal->Phase = 0;

	const int32 Total = Proposal->bDismantle ? DismantleQueue.Num() : Proposal->Placements.Num();
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
	if (Proposal->bDismantle)
	{
		OutMessage = TEXT("only build proposals can be nudged/rotated");
		return false;
	}
	if (Proposal->bManifold)
	{
		OutMessage = TEXT("manifolds are anchored to machine ports and can't be nudged — reject and re-propose instead");
		return false;
	}

	// Transform a copy; the original survives an invalid adjustment untouched.
	TArray<FTransform> Adjusted = Proposal->Placements;
	if (YawDeltaDeg != 0 && Adjusted.Num() > 0)
	{
		FVector Centroid = FVector::ZeroVector;
		for (const FTransform& Placement : Adjusted) { Centroid += Placement.GetLocation(); }
		Centroid /= Adjusted.Num();

		const FRotator Delta(0.0, static_cast<double>(YawDeltaDeg), 0.0);
		for (FTransform& Placement : Adjusted)
		{
			Placement.SetLocation(Centroid + Delta.RotateVector(Placement.GetLocation() - Centroid));
			FRotator Rotation = Placement.Rotator();
			Rotation.Yaw += YawDeltaDeg;
			Placement.SetRotation(FQuat(Rotation));
		}
	}
	if (!DeltaCm.IsNearlyZero())
	{
		for (FTransform& Placement : Adjusted)
		{
			Placement.SetLocation(Placement.GetLocation() + DeltaCm);
		}
	}

	// The new spot must validate like the original did (all-or-nothing, same rules).
	FAIDADryRunResult DryRun;
	if (!FAIDAActionSeam::DryRunBuild(WorldContext, Proposal->RecipeClassPath, Adjusted, DryRun) || !DryRun.bOk)
	{
		OutMessage = FString::Printf(TEXT("adjustment blocked — %s; the proposal is unchanged"),
			DryRun.Failures.Num() > 0
				? *FString::Printf(TEXT("%d placement(s) invalid there"), DryRun.Failures.Num())
				: (DryRun.Error.IsEmpty() ? TEXT("validation failed") : *DryRun.Error));
		return false;
	}

	Proposal->Placements = MoveTemp(Adjusted);
	OutMessage = YawDeltaDeg != 0
		? FString::Printf(TEXT("rotated %d° — %s"), YawDeltaDeg, *Proposal->Summary)
		: FString::Printf(TEXT("nudged %.1f m — %s"), DeltaCm.Size() / 100.0, *Proposal->Summary);
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

	if (Proposal->bManifold)
	{
		return TickManifold(WorldContext, Config, Memory, *Proposal);
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
		int32 Skipped = 0;
		Proposal->Cursor += FAIDAActionSeam::ExecuteBuildBatch(WorldContext, Proposal->RecipeClassPath,
			Proposal->Placements, Proposal->Cursor, FMath::Max(1, Config.BatchPerTick),
			Proposal->AffectedEntityIds, BuiltActors, Skipped);
		SkippedCount += Skipped;
		if (Proposal->Cursor < Proposal->Placements.Num()) { return true; }
	}

	FinishExecution(WorldContext, Config, Memory, *Proposal);
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
			Proposal.AffectedEntityIds, BuiltActors, Error))
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

	FinishExecution(WorldContext, Config, Memory, Proposal);
	return false;
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

	// The persistent record undo works from (docs/PHASE4.md §2d).
	const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
	FAIDAJournalEntry Entry;
	Entry.ProposalSpecJson = Proposal.SpecJson;
	Entry.RequesterId = Proposal.RequesterId;
	Entry.ApproverId = Proposal.ApproverId;
	Entry.ProposedUtc = Proposal.ProposedUtc;
	Entry.ExecutedUtc = NowUtc;
	Entry.AffectedEntityIds = Proposal.AffectedEntityIds;
	Entry.RefundJson = AIDAActionSpec::CostItemsToJson(Proposal.bDismantle ? AccruedRefund : Proposal.Cost);
	Entry.bDismantle = Proposal.bDismantle;
	const FGuid JournalId = Memory.AppendJournal(WorldContext, MoveTemp(Entry));

	if (JournalId.IsValid() && BuiltActors.Num() > 0)
	{
		SessionActors.Add(JournalId, BuiltActors);
	}

	ProposalStore.Transition(Proposal.Id, EAIDAProposalState::Executed, NowUtc);

	const int32 Affected = Proposal.AffectedEntityIds.Num();
	if (Proposal.bManifold)
	{
		UE_LOG(LogAIDA, Log, TEXT("[actions] %s DONE (manifold): %d attachment(s) (%d skipped), %d run(s) built, %d run(s) failed (journal %s)."),
			*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens),
			Proposal.Placements.Num() - SkippedCount, SkippedCount, RunBuiltCount, RunFailCount,
			*JournalId.ToString(EGuidFormats::DigitsWithHyphens));
		if (SkippedCount > 0 || RunFailCount > 0)
		{
			RunReport.Add(FString::Printf(TEXT("manifold: %d attachment(s) skipped, %d of %d run(s) failed to connect"),
				SkippedCount, RunFailCount, RunBuiltCount + RunFailCount));
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
	}

	ResetScratch();
}

void FAIDAActionEngine::ResetScratch()
{
	ExecutingId.Invalidate();
	DismantleQueue.Reset();
	BuiltActors.Reset();
	AccruedRefund.Reset();
	AttachmentActors.Reset();
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

	const TCHAR* Verb = Job.bDismantle ? TEXT("rebuilt") : TEXT("removed");
	const int32 Total = Job.Entities.Num();
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
			// Undo of a dismantle: collect the earlier refund back. Best-effort — the items may
			// have been spent; the rebuild still stands (report it, don't block).
			if (Scaled.Num() > 0 && !FAIDAActionSeam::DeductCost(WorldContext, Scaled))
			{
				Line += TEXT("; the earlier refund could not be re-collected from central storage");
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
