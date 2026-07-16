#include "Net/AIDARemoteCallObject.h"

#include "AIDA.h"
#include "Core/AIDAOrchestrator.h"
#include "Net/AIDAChatRelay.h"
#include "Subsystem/SubsystemActorManager.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "FGGameMode.h"
#include "FGPlayerController.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerState.h"

namespace
{
	FDelegateHandle GGameModeInitHandle;

	void OnGameModeInitialized(AGameModeBase* GameMode)
	{
		// RCOs must be registered on the game mode before players join. This fires once per world.
		if (AFGGameMode* FactoryGameMode = Cast<AFGGameMode>(GameMode))
		{
			FactoryGameMode->RegisterRemoteCallObjectClass(UAIDARemoteCallObject::StaticClass());
			UE_LOG(LogAIDA, Log, TEXT("Registered AIDA RCO on game mode."));
		}
	}
}

void UAIDARemoteCallObject::RegisterHooks()
{
	if (!GGameModeInitHandle.IsValid())
	{
		GGameModeInitHandle = FGameModeEvents::GameModeInitializedEvent.AddStatic(&OnGameModeInitialized);
	}
}

void UAIDARemoteCallObject::UnregisterHooks()
{
	if (GGameModeInitHandle.IsValid())
	{
		FGameModeEvents::GameModeInitializedEvent.Remove(GGameModeInitHandle);
		GGameModeInitHandle.Reset();
	}
}

namespace
{
	// Hard upper bound on an inbound chat line (rate/permission are separate, in the orchestrator).
	constexpr int32 kMaxChatChars = 2000;

	AAIDAChatRelay* FindLocalRelay(const UWorld* World)
	{
		if (!World)
		{
			return nullptr;
		}
		if (USubsystemActorManager* Mgr = World->GetSubsystem<USubsystemActorManager>())
		{
			return Mgr->GetSubsystemActor<AAIDAChatRelay>();
		}
		return nullptr;
	}
}

void UAIDARemoteCallObject::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UAIDARemoteCallObject, DummyReplicatedField);
}

//~ ─────────────────────────── Client → server ───────────────────────────

bool UAIDARemoteCallObject::ServerSendChat_Validate(const FString& Text, const FGuid& ConversationId)
{
	// Cheap structural validation only; policy (rate/permission) is enforced server-side below.
	return Text.Len() <= kMaxChatChars;
}

void UAIDARemoteCallObject::ServerSendChat_Implementation(const FString& Text, const FGuid& ConversationId)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (UAIDAOrchestrator* Orchestrator = World->GetSubsystem<UAIDAOrchestrator>())
	{
		Orchestrator->HandleChatRequest(ResolveRequester(), Text, ConversationId);
	}
	else
	{
		UE_LOG(LogAIDA, Warning, TEXT("ServerSendChat: no orchestrator (AIDA idle)."));
	}
}

bool UAIDARemoteCallObject::ServerRequestMessageBody_Validate(const FGuid& Id)
{
	return Id.IsValid();
}

void UAIDARemoteCallObject::ServerRequestMessageBody_Implementation(const FGuid& Id)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (UAIDAOrchestrator* Orchestrator = World->GetSubsystem<UAIDAOrchestrator>())
	{
		FAIDATranscriptEntry Entry;
		if (Orchestrator->GetMessageBody(Id, Entry))
		{
			ClientReceiveMessageBody(Entry);
		}
	}
}

bool UAIDARemoteCallObject::ServerRequestRecentTranscript_Validate()
{
	return true;
}

void UAIDARemoteCallObject::ServerRequestRecentTranscript_Implementation()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (UAIDAOrchestrator* Orchestrator = World->GetSubsystem<UAIDAOrchestrator>())
	{
		TArray<FAIDATranscriptEntry> Entries;
		Orchestrator->GetRecentTranscript(Entries);
		if (Entries.Num() > 0)
		{
			ClientReceiveTranscript(Entries);
		}
	}
}

bool UAIDARemoteCallObject::ServerApproveProposal_Validate(const FGuid& ProposalId)
{
	return ProposalId.IsValid();
}

void UAIDARemoteCallObject::ServerApproveProposal_Implementation(const FGuid& ProposalId)
{
	UWorld* World = GetWorld();
	if (UAIDAOrchestrator* Orchestrator = World ? World->GetSubsystem<UAIDAOrchestrator>() : nullptr)
	{
		Orchestrator->HandleProposalDecision(ResolveRequester(), ProposalId, /*bApprove*/ true);
	}
}

bool UAIDARemoteCallObject::ServerRejectProposal_Validate(const FGuid& ProposalId)
{
	return ProposalId.IsValid();
}

void UAIDARemoteCallObject::ServerRejectProposal_Implementation(const FGuid& ProposalId)
{
	UWorld* World = GetWorld();
	if (UAIDAOrchestrator* Orchestrator = World ? World->GetSubsystem<UAIDAOrchestrator>() : nullptr)
	{
		Orchestrator->HandleProposalDecision(ResolveRequester(), ProposalId, /*bApprove*/ false);
	}
}

bool UAIDARemoteCallObject::ServerAdjustProposal_Validate(const FVector& DeltaCm, int32 YawDeltaDeg)
{
	// One keypress worth of adjustment: bounded delta, sane rotation.
	return DeltaCm.Size() <= 10000.0 && FMath::Abs(YawDeltaDeg) <= 180;
}

void UAIDARemoteCallObject::ServerAdjustProposal_Implementation(const FVector& DeltaCm, int32 YawDeltaDeg)
{
	UWorld* World = GetWorld();
	if (UAIDAOrchestrator* Orchestrator = World ? World->GetSubsystem<UAIDAOrchestrator>() : nullptr)
	{
		Orchestrator->HandleProposalAdjust(ResolveRequester(), DeltaCm, YawDeltaDeg, /*bQuietSuccess*/ true);
	}
}

bool UAIDARemoteCallObject::ServerBeginImageUpload_Validate(const FString& MediaType, int32 TotalBytes, int32 ChunkCount)
{
	// Structural sanity only; real caps (config, budgets) are enforced in the orchestrator.
	return MediaType.Len() <= 64 && TotalBytes > 0 && ChunkCount > 0
		&& ChunkCount <= FAIDAImageUploadAssembler::kMaxChunkCount;
}

void UAIDARemoteCallObject::ServerBeginImageUpload_Implementation(const FString& MediaType, int32 TotalBytes, int32 ChunkCount)
{
	UWorld* World = GetWorld();
	UAIDAOrchestrator* Orchestrator = World ? World->GetSubsystem<UAIDAOrchestrator>() : nullptr;
	FString Error = TEXT("AIDA is not running on this server");
	if (Orchestrator && Orchestrator->HandleImageUploadBegin(ResolveRequester(), MediaType, TotalBytes, ChunkCount, Error))
	{
		ClientImageUploadAck(-1); // session open — send the first window
		return;
	}
	ClientImageUploadResult(false, FGuid(), Error);
}

bool UAIDARemoteCallObject::ServerImageUploadChunk_Validate(int32 Seq, const TArray<uint8>& Data)
{
	return Seq >= 0 && Seq < FAIDAImageUploadAssembler::kMaxChunkCount
		&& Data.Num() > 0 && Data.Num() <= FAIDAImageUploadAssembler::kChunkBytes;
}

void UAIDARemoteCallObject::ServerImageUploadChunk_Implementation(int32 Seq, const TArray<uint8>& Data)
{
	UWorld* World = GetWorld();
	UAIDAOrchestrator* Orchestrator = World ? World->GetSubsystem<UAIDAOrchestrator>() : nullptr;
	FString Error = TEXT("AIDA is not running on this server");
	if (Orchestrator && Orchestrator->HandleImageUploadChunk(ResolveRequester(), Seq, Data, Error))
	{
		ClientImageUploadAck(Seq);
		return;
	}
	ClientImageUploadResult(false, FGuid(), Error);
}

bool UAIDARemoteCallObject::ServerCommitImageUpload_Validate(uint32 Crc32)
{
	return true;
}

void UAIDARemoteCallObject::ServerCommitImageUpload_Implementation(uint32 Crc32)
{
	UWorld* World = GetWorld();
	UAIDAOrchestrator* Orchestrator = World ? World->GetSubsystem<UAIDAOrchestrator>() : nullptr;
	FString Error = TEXT("AIDA is not running on this server");
	FGuid ImageId;
	const bool bOk = Orchestrator && Orchestrator->HandleImageUploadCommit(ResolveRequester(), Crc32, ImageId, Error);
	ClientImageUploadResult(bOk, ImageId, bOk ? FString() : Error);
}

bool UAIDARemoteCallObject::ServerSendChatWithImages_Validate(const FString& Text, const FGuid& ConversationId, const TArray<FGuid>& ImageIds)
{
	return Text.Len() <= kMaxChatChars && ImageIds.Num() <= 8;
}

void UAIDARemoteCallObject::ServerSendChatWithImages_Implementation(const FString& Text, const FGuid& ConversationId, const TArray<FGuid>& ImageIds)
{
	UWorld* World = GetWorld();
	if (UAIDAOrchestrator* Orchestrator = World ? World->GetSubsystem<UAIDAOrchestrator>() : nullptr)
	{
		Orchestrator->HandleChatRequest(ResolveRequester(), Text, ConversationId, ImageIds);
	}
}

FAIDARequester UAIDARemoteCallObject::ResolveRequester() const
{
	FAIDARequester Requester;
	if (const AFGPlayerController* PC = GetOwnerPlayerController())
	{
		if (const APlayerState* PS = PC->PlayerState)
		{
			Requester.Author = PS->GetPlayerName();
			// NOTE: FUniqueNetIdRepl::IsValid() can be true while the resolved id pointer is null
			// (e.g. the listen-server host) — check the shared pointer before ToString().
			if (const TSharedPtr<const FUniqueNetId> NetId = PS->GetUniqueId().GetUniqueNetId())
			{
				Requester.PlayerId = NetId->ToString();
			}
		}
	}
	if (Requester.Author.IsEmpty())
	{
		Requester.Author = TEXT("Player");
	}
	return Requester;
}

//~ ─────────────────────────── Server → client ───────────────────────────

void UAIDARemoteCallObject::ClientReceiveMessageBody_Implementation(FAIDATranscriptEntry Entry)
{
	if (AAIDAChatRelay* Relay = FindLocalRelay(GetWorld()))
	{
		Relay->ClientApplyBody(Entry);
	}
}

void UAIDARemoteCallObject::ClientReceiveTranscript_Implementation(const TArray<FAIDATranscriptEntry>& Entries)
{
	if (AAIDAChatRelay* Relay = FindLocalRelay(GetWorld()))
	{
		Relay->ClientApplyTranscript(Entries);
	}
}

void UAIDARemoteCallObject::ClientImageUploadAck_Implementation(int32 UpToSeq)
{
	if (AAIDAChatRelay* Relay = FindLocalRelay(GetWorld()))
	{
		Relay->OnUploadAck.Broadcast(UpToSeq);
	}
}

void UAIDARemoteCallObject::ClientImageUploadResult_Implementation(bool bOk, const FGuid& ImageId, const FString& Error)
{
	if (AAIDAChatRelay* Relay = FindLocalRelay(GetWorld()))
	{
		Relay->OnUploadResult.Broadcast(bOk, ImageId, Error);
	}
}
