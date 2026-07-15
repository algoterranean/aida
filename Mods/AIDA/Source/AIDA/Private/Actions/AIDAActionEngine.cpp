#include "Actions/AIDAActionEngine.h"

#include "AIDA.h"
#include "Actions/AIDAActionSeam.h"
#include "Actions/AIDAActionSpec.h"
#include "Core/AIDAConfig.h"
#include "Memory/AIDAMemory.h"
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
	if (IsExecuting())
	{
		OutMessage = TEXT("another proposal is executing — wait for it to finish");
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
			ProposalStore.Transition(Id, EAIDAProposalState::Rejected);
			OutMessage = FString::Printf(TEXT("stored selector no longer parses (%s) — proposal rejected"), *ParseError);
			return false;
		}

		TArray<FAIDADismantleHandle> Handles;
		FAIDAActionSeam::ResolveDismantleHandles(WorldContext, Selector, Handles);
		if (Handles.Num() == 0)
		{
			ProposalStore.Transition(Id, EAIDAProposalState::Rejected);
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
	ProposalStore.Transition(Id, EAIDAProposalState::Rejected);
	OutMessage = FString::Printf(TEXT("rejected — %s"), *Proposal->Summary);
	UE_LOG(LogAIDA, Log, TEXT("[actions] %s rejected by %s."), *Id.ToString(EGuidFormats::DigitsWithHyphens), *ApproverId);
	return true;
}

bool FAIDAActionEngine::Tick(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory)
{
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
	FAIDAJournalEntry Entry;
	Entry.ProposalSpecJson = Proposal.SpecJson;
	Entry.RequesterId = Proposal.RequesterId;
	Entry.ApproverId = Proposal.ApproverId;
	Entry.ProposedUtc = Proposal.ProposedUtc;
	Entry.ExecutedUtc = FDateTime::UtcNow().ToUnixTimestamp();
	Entry.AffectedEntityIds = Proposal.AffectedEntityIds;
	Entry.RefundJson = AIDAActionSpec::CostItemsToJson(Proposal.bDismantle ? AccruedRefund : Proposal.Cost);
	Entry.bDismantle = Proposal.bDismantle;
	const FGuid JournalId = Memory.AppendJournal(WorldContext, MoveTemp(Entry));

	if (JournalId.IsValid() && BuiltActors.Num() > 0)
	{
		SessionActors.Add(JournalId, BuiltActors);
	}

	ProposalStore.Transition(Proposal.Id, EAIDAProposalState::Executed);

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
