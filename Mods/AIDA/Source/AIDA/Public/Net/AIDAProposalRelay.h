#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "Net/AIDANetTypes.h"
#include "AIDAProposalRelay.generated.h"

/** Fired on clients (and the listen host) whenever the proposal list changes; re-read GetProposals(). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAIDAOnProposalsChangedDelegate);

/**
 * Server-authoritative, always-relevant replicated subsystem actor carrying the live proposal list
 * (docs/PHASE4.md §4a). DELIBERATE deviation from AAIDAChatRelay's multicast-event style: proposals
 * replicate as a **state array**, because pending proposals must survive late-join within their TTL —
 * state replication hands a late joiner the list for free, where fire-and-forget multicasts needed a
 * bulk-fetch RPC to patch that hole.
 *
 * Terminal-state entries (executed/rejected/expired/failed) linger briefly so approvers see the
 * outcome, then the orchestrator's sweep retires them. Business logic (both approval gates, the
 * executor) lives upstream; this actor is purely the replicated view + the client's approve/reject
 * passthrough to its RCO.
 */
UCLASS()
class AAIDAProposalRelay : public AModSubsystem
{
	GENERATED_BODY()

public:
	AAIDAProposalRelay();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	//~ Server API (authority only) — driven by the orchestrator.
	/** Insert or update one proposal's view; broadcasts the change locally too (listen host has no OnRep). */
	void ServerUpsertProposal(const FAIDAProposalView& View);
	/** Drop a retired proposal from the replicated list. */
	void ServerRemoveProposal(const FGuid& Id);
	//~ End server API

	//~ Client view.
	/** The current proposal list (pending first is NOT guaranteed — filter by State). */
	UFUNCTION(BlueprintCallable, Category = "AIDA")
	void GetProposals(TArray<FAIDAProposalView>& OutProposals) const { OutProposals = Proposals; }

	/** Client→server: approve a pending proposal via the local player's RCO. Server re-checks both gates. */
	UFUNCTION(BlueprintCallable, Category = "AIDA")
	void Approve(const FGuid& ProposalId);

	/** Client→server: reject a pending proposal via the local player's RCO. */
	UFUNCTION(BlueprintCallable, Category = "AIDA")
	void Reject(const FGuid& ProposalId);
	//~ End client view

	/** Fired whenever the replicated proposal list changes (bind the ProposalUI to this). */
	UPROPERTY(BlueprintAssignable, Category = "AIDA")
	FAIDAOnProposalsChangedDelegate OnProposalsChanged;

private:
	UFUNCTION()
	void OnRep_Proposals();

	UPROPERTY(ReplicatedUsing = OnRep_Proposals)
	TArray<FAIDAProposalView> Proposals;

	/** The local player's AIDA RCO, or null (e.g. on a dedicated server). */
	class UAIDARemoteCallObject* GetLocalRCO() const;
};
