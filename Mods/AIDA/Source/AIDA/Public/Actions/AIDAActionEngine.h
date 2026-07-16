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
	 * proposal's placements. The adjustment ALWAYS applies (user rule: validity never blocks a
	 * pending ghost — the player may be nudging it off bad ground); the dry-run still runs so
	 * OutMessage and the proposal's InvalidCount double as live placement feedback. The
	 * republished view moves the client ghosts.
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

	/** Human lines from the last manifold execution's FAILED runs (empty when all connected),
	 *  cleared on read — announced alongside the completion line. */
	TArray<FString> TakeRunReport();

private:
	void FinishExecution(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal);
	void ResetScratch();

	/**
	 * Skipped placements accrue their recipe cost for refund (validity is advisory at propose/nudge
	 * time — the upfront charge must not eat tiles that never re-validated at execute). Central
	 * cost mode only; FinishExecution pays AccruedRefund out and journals the NET cost.
	 */
	void AccrueSkippedCost(UObject* WorldContext, const FAIDAActionsConfig& Config, const FString& RecipeClassPath, int32 Skipped);

	/**
	 * Advance a manifold proposal (docs/PHASE4-MANIFOLDS.md §5): phase 0 batches the attachments
	 * (index-aligned capture), then ONE connecting run per tick — phase 1 trunk hops, phase 2 drops
	 * to the machines. Failed runs (missing endpoint, no snap, unaffordable) are counted + reported,
	 * never fatal. True while more work remains.
	 */
	bool TickManifold(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal);

	/**
	 * Advance a container-label proposal (P7 Slice 3): one sign per tick — trace onto the container
	 * face, hologram-place, construct, write the text. Failed signs (container gone, face blocked,
	 * placement invalid) are counted + reported like manifold runs, never fatal. True while more remain.
	 */
	bool TickLabels(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal);

	/**
	 * Advance an auto-powered build (docs/PHASE4-POWER.md): phase 0 machines, phase 1 poles (both
	 * index-captured), phase 2 power lines (batched per tick) + one grid tie-in. Wire failures use
	 * the same counted-and-announced report as manifold runs. True while more work remains.
	 */
	bool TickPowered(UObject* WorldContext, const FAIDAActionsConfig& Config, FAIDAMemory& Memory, FAIDAProposal& Proposal);

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

	//~ Manifold/powered-build scratch: phase-0 actors captured PER PLACEMENT INDEX (null = skipped/
	//  lost — dependent runs/wires then fail loudly instead of silently re-mapping to a neighbor).
	//  Manifolds: the attachments. Powered builds: the machines.
	TArray<TWeakObjectPtr<AActor>> AttachmentActors;
	//~ Powered-build scratch: phase-1 poles, per placement index.
	TArray<TWeakObjectPtr<AActor>> PoleActors;
	int32 RunBuiltCount = 0;
	int32 RunFailCount = 0;
	TArray<FString> RunFailures; // first few human-readable reasons for the outcome announcement
	TArray<FString> RunReport;   // published lines (survives scratch reset until read)

	/** Journal id → live actors built this session (undo's fast path; transform re-resolve after reload). */
	TMap<FGuid, TArray<TWeakObjectPtr<AActor>>> SessionActors;

	//~ Undo scratch (mutually exclusive with a proposal executing).
	TArray<FUndoJob> UndoQueue;
	FString UndoInstigatorId;
	TArray<FString> UndoReport;
};
