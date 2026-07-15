#pragma once

#include "CoreMinimal.h"
#include "Actions/AIDAActionTypes.h"
#include "Actions/AIDAProposalStore.h"

struct FAIDAActionsConfig;
class FAIDAMemory;

/**
 * The Phase 4 coordinator (docs/PHASE4.md §3): owns the proposal store, the approve/reject
 * bookkeeping, the batched executor's progress, and the in-session weak-ptr cache that undo
 * (Slice 4) re-resolves against. A plain orchestrator member like FAIDAMemory — the orchestrator
 * supplies the world, the 10 Hz timer, and the permission/policy gates upstream.
 *
 * One proposal executes at a time; approving another while one runs is refused (v1 simplicity —
 * matches the "time-sliced batches, no hitching" contract).
 */
class FAIDAActionEngine
{
public:
	FAIDAProposalStore& Store() { return ProposalStore; }
	const FAIDAProposalStore& Store() const { return ProposalStore; }

	bool IsExecuting() const { return ExecutingId.IsValid(); }

	/** The proposal currently executing (invalid when idle) — capture before Tick to publish outcomes. */
	const FGuid& CurrentExecutingId() const { return ExecutingId; }

	/**
	 * Approve a pending proposal and start execution: dismantles re-resolve their targets fresh
	 * (never trusted from the dry-run); builds pay their whole cost upfront from central storage
	 * under costMode "central" (one failure point — a mid-run change can't strand a half-built
	 * grid). OutMessage is human-facing either way. True = execution started (start the timer).
	 */
	bool Approve(UObject* WorldContext, const FAIDAActionsConfig& Config, const FGuid& Id, const FString& ApproverId, FString& OutMessage);

	/** Reject a pending proposal. */
	bool Reject(const FGuid& Id, const FString& ApproverId, FString& OutMessage);

	/**
	 * Move (DeltaCm) and/or rotate (YawDeltaDeg, around the grid centroid) a PENDING build
	 * proposal's placements, re-running the dry-run against the new spot — an invalid adjustment
	 * reports and leaves the proposal unchanged. The republished view moves the client ghosts.
	 */
	bool AdjustPending(UObject* WorldContext, const FAIDAActionsConfig& Config, const FGuid& Id,
		const FVector& DeltaCm, int32 YawDeltaDeg, FString& OutMessage);

	/**
	 * One executor tick: advance the executing proposal (or the undo queue) by batchPerTick items.
	 * On completion, journals the action (in-save), pays out dismantle refunds, and caches built
	 * actors for undo. Returns true while more work remains (keep the timer running).
	 */
	bool Tick(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory);

	/**
	 * Queue the last Count non-undone journal entries (within actions.undoWindow) for reversal
	 * (docs/PHASE4.md §4d): built entities get dismantled (cost refunded to the instigator),
	 * dismantled entities get rebuilt (refund re-deducted, best-effort). Time-sliced through the
	 * same Tick. True = work queued (start the timer). Refused while anything else is executing.
	 */
	bool StartUndo(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory,
		int32 Count, const FString& InstigatorId, FString& OutMessage);

	/**
	 * Per-entry human report lines from the last completed undo run ("undid 97 of 100 …"),
	 * cleared on read — the orchestrator announces them when Tick runs dry.
	 */
	TArray<FString> TakeUndoReport();

private:
	void FinishExecution(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal);
	void ResetScratch();

	/** One journal entry queued for reversal. */
	struct FUndoJob
	{
		FGuid JournalId;
		bool bDismantle = false;                       // the ORIGINAL action's direction
		TArray<FAIDAEntityId> Entities;
		TArray<TWeakObjectPtr<AActor>> CachedActors;   // in-session fast path (builds only)
		TArray<FAIDACostItem> Refund;                  // journaled RefundJson (cost to refund / re-deduct)
		int32 Cursor = 0;
		int32 Done = 0;
		int32 Missing = 0;
	};

	/** Advance the undo queue by one batch; true while jobs remain. */
	bool TickUndo(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory);
	void FinishUndoJob(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FUndoJob& Job);

	FAIDAProposalStore ProposalStore;

	//~ The executing proposal's scratch state (one at a time).
	FGuid ExecutingId;
	TArray<FAIDADismantleHandle> DismantleQueue; // resolved at approval, consumed by Cursor
	TArray<TWeakObjectPtr<AActor>> BuiltActors;
	TArray<FAIDACostItem> AccruedRefund;
	int32 SkippedCount = 0;
	int32 RemovedCount = 0;
	int32 MissingCount = 0;

	/** Journal id → live actors built this session (undo's fast path; transform re-resolve after reload). */
	TMap<FGuid, TArray<TWeakObjectPtr<AActor>>> SessionActors;

	//~ Undo scratch (mutually exclusive with a proposal executing).
	TArray<FUndoJob> UndoQueue;
	FString UndoInstigatorId;
	TArray<FString> UndoReport;
};
