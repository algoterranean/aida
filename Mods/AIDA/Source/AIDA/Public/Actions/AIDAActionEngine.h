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
	 * One executor tick: advance the executing proposal by batchPerTick items. On completion,
	 * journals the action (in-save), pays out dismantle refunds, and caches built actors for undo.
	 * Returns true while more work remains (keep the timer running).
	 */
	bool Tick(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory);

private:
	void FinishExecution(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal);
	void ResetScratch();

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
};
