#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/AIDAConfig.h"
#include "Core/AIDASessionManager.h"
#include "Core/AIDARateLimiter.h"
#include "Core/AIDAPermissionService.h"
#include "Net/AIDANetTypes.h"
#include "AIDAOrchestrator.generated.h"

struct FAIDAChatMessage;

class FLLMClient;
class AAIDAChatRelay;
class AFGPlayerController;
struct IConsoleCommand;

/**
 * Server-authoritative orchestrator. Created with the world and safe with zero players
 * connected (never initialized per-player). The only network-egress authority.
 *
 * Phase 0: loads + validates config on world begin play and logs its status.
 * Later phases wire in SessionManager, LLMClient, ToolRegistry, etc. (docs/ARCHITECTURE.md §3.2).
 */
UCLASS()
class UAIDAOrchestrator : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ UWorldSubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	//~ End UWorldSubsystem

	const FAIDAConfig& GetConfig() const { return Config; }
	bool IsConfigLoaded() const { return bConfigLoaded; }

	//~ Server API — called from the per-player RCO (authority only). See Net/AIDARemoteCallObject.
	/** Handle an inbound player chat line: record it, then stream an AIDA reply out to all clients. */
	void HandleChatRequest(const FAIDARequester& Requester, const FString& Text);

	/** Authoritative full body for one message (recovery). False if it aged out of the transcript. */
	bool GetMessageBody(const FGuid& Id, FAIDATranscriptEntry& OutEntry) const;

	/** Recent transcript for a late-joining client. */
	void GetRecentTranscript(TArray<FAIDATranscriptEntry>& OutEntries) const;
	//~ End server API

private:
	void LoadConfig();

	/** Spawn + cache the replicated chat relay via SML's SubsystemActorManager (server only). */
	void RegisterRelay();
	AAIDAChatRelay* GetRelay();

	/** Kick off the streaming LLM reply for an accepted request. */
	void StartAIDAReply();

	/** Assemble the privacy-filtered chat context (recent transcript) sent to the LLM. */
	void BuildChatContext(TArray<FAIDAChatMessage>& OutMessages) const;

	/** Resolves the config path: server admin override first, then the mod's shipped example. */
	FString ResolveConfigPath(FString& OutSource) const;

	void RegisterConsoleCommands();

	/** `AIDA.Ping [prompt]` — one-shot completion against the configured LLM; logs the reply. */
	void Ping(const TArray<FString>& Args);

	FAIDAConfig Config;
	bool bConfigLoaded = false;

	TSharedPtr<FLLMClient> LLMClient;
	TUniquePtr<FAIDASessionManager> Session;
	TWeakObjectPtr<AAIDAChatRelay> Relay;
	FAIDARateLimiter RateLimiter;
	FAIDAPermissionService Permissions;
	IConsoleCommand* PingCommand = nullptr;
};
