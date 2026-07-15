#include "Net/AIDAProposalRelay.h"

#include "AIDA.h"
#include "Net/AIDARemoteCallObject.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "FGPlayerController.h"
#include "Net/UnrealNetwork.h"

AAIDAProposalRelay::AAIDAProposalRelay()
{
	// Server-authoritative, replicated to all clients (base AFGSubsystem already sets
	// bReplicates + bAlwaysRelevant), same recipe as AAIDAChatRelay.
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;
}

void AAIDAProposalRelay::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAIDAProposalRelay, Proposals);
}

void AAIDAProposalRelay::ServerUpsertProposal(const FAIDAProposalView& View)
{
	if (!HasAuthority() || !View.Id.IsValid())
	{
		return;
	}
	bool bFound = false;
	for (FAIDAProposalView& Existing : Proposals)
	{
		if (Existing.Id == View.Id)
		{
			Existing = View;
			bFound = true;
			break;
		}
	}
	if (!bFound)
	{
		Proposals.Add(View);
	}
	// A replicated TArray member only OnReps on remote clients — the listen host's UI binds the
	// same delegate, so broadcast locally too.
	OnProposalsChanged.Broadcast();
}

void AAIDAProposalRelay::ServerRemoveProposal(const FGuid& Id)
{
	if (!HasAuthority())
	{
		return;
	}
	const int32 Removed = Proposals.RemoveAll([&Id](const FAIDAProposalView& View) { return View.Id == Id; });
	if (Removed > 0)
	{
		OnProposalsChanged.Broadcast();
	}
}

void AAIDAProposalRelay::OnRep_Proposals()
{
	OnProposalsChanged.Broadcast();
}

void AAIDAProposalRelay::Approve(const FGuid& ProposalId)
{
	if (UAIDARemoteCallObject* RCO = GetLocalRCO())
	{
		RCO->ServerApproveProposal(ProposalId);
	}
}

void AAIDAProposalRelay::Reject(const FGuid& ProposalId)
{
	if (UAIDARemoteCallObject* RCO = GetLocalRCO())
	{
		RCO->ServerRejectProposal(ProposalId);
	}
}

UAIDARemoteCallObject* AAIDAProposalRelay::GetLocalRCO() const
{
	// The local player's controller: null on a dedicated server (no local player).
	const UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (AFGPlayerController* FGPC = Cast<AFGPlayerController>(PC))
	{
		return FGPC->GetRemoteCallObjectOfClass<UAIDARemoteCallObject>();
	}
	return nullptr;
}
