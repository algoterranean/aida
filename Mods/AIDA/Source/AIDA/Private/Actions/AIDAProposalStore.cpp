#include "Actions/AIDAProposalStore.h"

bool FAIDAProposalStore::Add(FAIDAProposal Proposal, int64 NowUtc, int32 MaxPending, FString& OutError)
{
	if (NumPending() >= MaxPending)
	{
		OutError = FString::Printf(TEXT("%d proposals are already awaiting approval — resolve or wait for them first"), MaxPending);
		return false;
	}

	if (!Proposal.Id.IsValid()) { Proposal.Id = FGuid::NewGuid(); }
	Proposal.State = EAIDAProposalState::Pending;
	Proposal.ProposedUtc = NowUtc;
	Proposals.Add(Proposal.Id, MoveTemp(Proposal));
	return true;
}

FAIDAProposal* FAIDAProposalStore::Find(const FGuid& Id)
{
	return Proposals.Find(Id);
}

const FAIDAProposal* FAIDAProposalStore::Find(const FGuid& Id) const
{
	return Proposals.Find(Id);
}

bool FAIDAProposalStore::IsLegalTransition(EAIDAProposalState From, EAIDAProposalState To)
{
	switch (From)
	{
	case EAIDAProposalState::Pending:
		return To == EAIDAProposalState::Approved || To == EAIDAProposalState::Rejected || To == EAIDAProposalState::Expired;
	case EAIDAProposalState::Approved:
		return To == EAIDAProposalState::Executing;
	case EAIDAProposalState::Executing:
		return To == EAIDAProposalState::Executed || To == EAIDAProposalState::Failed;
	case EAIDAProposalState::Executed:
		return To == EAIDAProposalState::Undone;
	default:
		return false; // terminal states (Failed/Rejected/Expired/Undone) never move again
	}
}

bool FAIDAProposalStore::IsTerminal(EAIDAProposalState State)
{
	return State == EAIDAProposalState::Executed || State == EAIDAProposalState::Failed
		|| State == EAIDAProposalState::Rejected || State == EAIDAProposalState::Expired
		|| State == EAIDAProposalState::Undone;
}

bool FAIDAProposalStore::Transition(const FGuid& Id, EAIDAProposalState NewState, int64 NowUtc)
{
	FAIDAProposal* Proposal = Proposals.Find(Id);
	if (!Proposal || !IsLegalTransition(Proposal->State, NewState)) { return false; }
	Proposal->State = NewState;
	if (IsTerminal(NewState)) { Proposal->ResolvedUtc = NowUtc; }
	return true;
}

TArray<FGuid> FAIDAProposalStore::SweepExpired(int64 NowUtc, int32 TtlSeconds)
{
	TArray<FGuid> Expired;
	for (auto& Pair : Proposals)
	{
		FAIDAProposal& P = Pair.Value;
		if (P.State == EAIDAProposalState::Pending && NowUtc >= P.ProposedUtc + TtlSeconds)
		{
			P.State = EAIDAProposalState::Expired;
			P.ResolvedUtc = NowUtc;
			Expired.Add(P.Id);
		}
	}
	return Expired;
}

int32 FAIDAProposalStore::NumPending() const
{
	int32 Count = 0;
	for (const auto& Pair : Proposals)
	{
		if (Pair.Value.State == EAIDAProposalState::Pending) { ++Count; }
	}
	return Count;
}

TArray<FAIDAProposal> FAIDAProposalStore::All() const
{
	TArray<FAIDAProposal> Out;
	Proposals.GenerateValueArray(Out);
	return Out;
}
