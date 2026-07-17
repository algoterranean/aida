#include "Net/AIDAProposalRelay.h"

#include "AIDA.h"
#include "Actions/AIDAActionSeam.h"
#include "Net/AIDARemoteCallObject.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "FGPlayerController.h"
#include "Net/UnrealNetwork.h"

namespace
{
	/** Bound the local ghost count — a colossal proposal previews its first tiles, not the world. */
	constexpr int32 kMaxGhostTiles = 400;
	/** Runs are spline holograms (heavier than tiles) — cap them separately per refresh. */
	constexpr int32 kMaxGhostRuns = 64;
}

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

void AAIDAProposalRelay::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	for (auto& Pair : Ghosts)
	{
		for (const TWeakObjectPtr<AActor>& Ghost : Pair.Value)
		{
			if (AActor* Actor = Ghost.Get()) { Actor->Destroy(); }
		}
	}
	Ghosts.Reset();
	Super::EndPlay(EndPlayReason);
}

void AAIDAProposalRelay::ClearGhosts(const FGuid& Id)
{
	if (TArray<TWeakObjectPtr<AActor>>* Existing = Ghosts.Find(Id))
	{
		for (const TWeakObjectPtr<AActor>& Ghost : *Existing)
		{
			if (AActor* Actor = Ghost.Get()) { Actor->Destroy(); }
		}
		Ghosts.Remove(Id);
	}
}

void AAIDAProposalRelay::RefreshGhosts()
{
	// Rendering-side only — a dedicated server has no screen to ghost onto.
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// Drop ghosts whose proposal vanished or resolved; rebuild the rest (a nudge keeps the id but
	// moves every tile, so rebuilding on any change is the simple, correct thing at these counts).
	TArray<FGuid> Stale;
	for (const auto& Pair : Ghosts) { Stale.Add(Pair.Key); }
	for (const FGuid& Id : Stale) { ClearGhosts(Id); }

	for (const FAIDAProposalView& View : Proposals)
	{
		if (View.State != TEXT("pending") || (View.GhostParts.Num() == 0 && View.GhostRuns.Num() == 0))
		{
			continue;
		}
		TArray<TWeakObjectPtr<AActor>>& Spawned = Ghosts.Add(View.Id);
		int32 Budget = kMaxGhostTiles; // the tile cap spans all of a composite's parts
		int32 Total = 0;
		for (const FAIDAGhostPart& Part : View.GhostParts)
		{
			Total += Part.TileCenters.Num();
			if (Part.RecipeClassPath.IsEmpty()) { continue; }
			const int32 Count = FMath::Min(Part.TileCenters.Num(), Budget);
			for (int32 i = 0; i < Count; ++i)
			{
				AActor* Ghost = FAIDAActionSeam::SpawnGhostHologram(this, Part.RecipeClassPath, Part.TileCenters[i], Part.YawDeg, this);
				if (!Ghost) { break; }
				Spawned.Add(Ghost);
				--Budget;
			}
			if (Budget <= 0) { break; }
		}
		if (Total > Spawned.Num())
		{
			UE_LOG(LogAIDA, Log, TEXT("[actions] ghost preview capped at %d of %d tiles."), Spawned.Num(), Total);
		}

		// Belt/pipe runs of manifold / connected-build proposals — display-only spline holograms.
		// A failed spawn skips just that run (the tiles above already preview the row itself).
		int32 RunBudget = kMaxGhostRuns;
		int32 RunsSpawned = 0;
		for (const FAIDAGhostRun& Run : View.GhostRuns)
		{
			if (RunBudget <= 0)
			{
				UE_LOG(LogAIDA, Log, TEXT("[actions] ghost preview capped at %d of %d runs."), kMaxGhostRuns, View.GhostRuns.Num());
				break;
			}
			if (Run.RecipeClassPath.IsEmpty()) { continue; }
			AActor* Ghost = FAIDAActionSeam::SpawnGhostRunHologram(this, Run.RecipeClassPath,
				Run.FromCm, Run.FromNormal, Run.ToCm, Run.ToNormal, this);
			if (!Ghost) { continue; }
			Spawned.Add(Ghost);
			++RunsSpawned;
			--RunBudget;
		}
		if (View.GhostRuns.Num() > 0)
		{
			UE_LOG(LogAIDA, Log, TEXT("[actions] ghost preview: %d run ghost(s) of %d."), RunsSpawned, View.GhostRuns.Num());
		}
	}
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
	// A replicated TArray member only OnReps on remote clients — the listen host's UI (and ghosts)
	// bind the same paths, so refresh locally too.
	RefreshGhosts();
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
		RefreshGhosts();
		OnProposalsChanged.Broadcast();
	}
}

void AAIDAProposalRelay::OnRep_Proposals()
{
	RefreshGhosts();
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

void AAIDAProposalRelay::Adjust(const FVector& DeltaCm, int32 YawDeltaDeg)
{
	if (UAIDARemoteCallObject* RCO = GetLocalRCO())
	{
		RCO->ServerAdjustProposal(DeltaCm, YawDeltaDeg);
	}
}

bool AAIDAProposalRelay::HasPendingProposal() const
{
	for (const FAIDAProposalView& View : Proposals)
	{
		if (View.State == TEXT("pending")) { return true; }
	}
	return false;
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
