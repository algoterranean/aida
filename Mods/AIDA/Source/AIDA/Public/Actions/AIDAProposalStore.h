#pragma once

#include "CoreMinimal.h"
#include "Actions/AIDAActionTypes.h"

/**
 * Server-side proposal store (docs/PHASE4.md §3): the map, a validated state machine, TTL expiry, and
 * the pending-proposal cap. Deliberately in-memory only — a restart voids pending proposals (approval
 * must reference live server state); the journal is the only persisted artifact. The clock is injected
 * (NowUtc parameters) so TTL behavior unit-tests without the game.
 */
class FAIDAProposalStore
{
public:
	/**
	 * Store a new proposal (state forced to Pending, ProposedUtc stamped from NowUtc). Fails with a
	 * model-facing reason when the pending cap is already reached. Mints an Id if unset.
	 */
	bool Add(FAIDAProposal Proposal, int64 NowUtc, int32 MaxPending, FString& OutError);

	FAIDAProposal* Find(const FGuid& Id);
	const FAIDAProposal* Find(const FGuid& Id) const;

	/**
	 * Move a proposal to NewState if the transition is legal (docs/PHASE4.md §2c):
	 * Pending → Approved/Rejected/Expired; Approved → Executing; Executing → Executed/Failed;
	 * Executed → Undone. Anything else returns false and leaves the proposal untouched.
	 * NowUtc stamps ResolvedUtc when the new state is terminal (drives the UI linger/retire).
	 */
	bool Transition(const FGuid& Id, EAIDAProposalState NewState, int64 NowUtc = 0);

	static bool IsLegalTransition(EAIDAProposalState From, EAIDAProposalState To);

	/** True once a proposal can no longer change (terminal states linger for the UI, then retire). */
	static bool IsTerminal(EAIDAProposalState State);

	/** Expire Pending proposals whose TTL elapsed. Returns the ids that flipped to Expired. */
	TArray<FGuid> SweepExpired(int64 NowUtc, int32 TtlSeconds);

	/** Drop a proposal outright (terminal-state retirement after the UI linger). */
	void Remove(const FGuid& Id) { Proposals.Remove(Id); }

	int32 NumPending() const;

	/** Snapshot of all stored proposals (unordered map walk), e.g. for get_proposal_status. */
	TArray<FAIDAProposal> All() const;

private:
	TMap<FGuid, FAIDAProposal> Proposals;
};
