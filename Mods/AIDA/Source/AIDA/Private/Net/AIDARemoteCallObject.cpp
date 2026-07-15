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
		FAIDARequester Requester;
		if (const AFGPlayerController* PC = GetOwnerPlayerController())
		{
			if (const APlayerState* PS = PC->PlayerState)
			{
				Requester.Author = PS->GetPlayerName();
				// NOTE: FUniqueNetIdRepl::IsValid() can be true while the resolved id pointer is null
				// (e.g. the listen-server host), so dereferencing it with -> crashes. Check the shared
				// pointer itself before calling ToString().
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
		Orchestrator->HandleChatRequest(Requester, Text, ConversationId);
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
