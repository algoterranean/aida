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
		if (!AIDAActionSpec::ParseDismantleSpec(SpecObj, Config.MaxProposalItems, Selector, ParseError))
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
	SkippedCount = RemovedCount = MissingCount = 0;
	Proposal->Cursor = 0;

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
	if (Proposal.bDismantle)
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
	SkippedCount = RemovedCount = MissingCount = 0;
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
