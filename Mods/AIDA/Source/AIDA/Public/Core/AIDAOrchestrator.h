#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/AIDAConfig.h"
#include "Core/AIDASessionManager.h"
#include "Core/AIDARateLimiter.h"
#include "Core/AIDAPermissionService.h"
#include "Tools/AIDAToolRegistry.h"
#include "Factory/AIDAFactoryIndex.h" // TTL-cached factory extractor + FAIDAFactoryAggregates
#include "Map/AIDAMapService.h"       // TTL-cached resource-node scan
#include "Net/AIDANetTypes.h"
#include "Adapters/AIDALLMTypes.h" // FAIDAOnChunk/FAIDAOnError typedefs used by RunToolLoop
#include "AIDAOrchestrator.generated.h"

struct FAIDAChatMessage;
struct FAIDAToolDef;

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

	/**
	 * Register the relay subsystem CLASS with SML's SubsystemActorManager. Must run on EVERY world
	 * (client included): the manager only records a replicated subsystem actor when it arrives if its
	 * class was pre-registered, so without this a client's GetSubsystemActor<AAIDAChatRelay>() (and
	 * thus the ChatWidget binding) returns null forever. Idempotent; spawns nothing on clients.
	 */
	void RegisterRelayClass();

	/** Register the class, then (server) spawn + cache the relay and hand it to the session manager. */
	void RegisterRelay();
	AAIDAChatRelay* GetRelay();

	/** Kick off the streaming LLM reply for an accepted request. */
	void StartAIDAReply();

	/** Register the built-in tools exposed to the model (Phase 2 Slice 0: an echo verifier). */
	void RegisterTools();

	/** Snapshot the registry's specs as the wire tool definitions sent to the model. */
	void BuildToolDefs(TArray<FAIDAToolDef>& OutDefs) const;

	/**
	 * Drive one turn of the tool loop: complete against the tool set, and if the model returns
	 * tool_use, permission-gate + dispatch each call, append the tool_result turn, and recurse until
	 * the model answers with text or RoundsLeft hits zero. Messages is shared so history survives the
	 * async rounds. OnDelta streams visible text; OnDone fires once with the final text.
	 */
	void RunToolLoop(TSharedRef<TArray<FAIDAChatMessage>, ESPMode::ThreadSafe> Messages, FAIDARequester Requester,
		int32 RoundsLeft, FAIDAOnChunk OnDelta, TFunction<void(const FString&)> OnDone, FAIDAOnError OnError);

	/** Assemble the privacy-filtered chat context (recent transcript) sent to the LLM. */
	void BuildChatContext(TArray<FAIDAChatMessage>& OutMessages) const;

	/** Resolves the config path: server admin override first, then the mod's shipped example. */
	FString ResolveConfigPath(FString& OutSource) const;

	void RegisterConsoleCommands();

	/** `AIDA.Ping [prompt]` — one-shot completion against the configured LLM; logs the reply. */
	void Ping(const TArray<FString>& Args);

	/**
	 * `AIDA.Say [text]` — inject a chat request server-side (as a debug "player") through the full
	 * relay path: posts the player message and streams the AIDA reply, both fanning out to all
	 * clients. Run on the server/host to verify Slice 1 replication without a widget.
	 */
	void Say(const TArray<FString>& Args);

	/**
	 * `AIDA.ToolPing [prompt]` — run the tool loop against the echo tool and log the result. A
	 * server-side, widget-free way to verify the Phase 2 Slice 0 tool round-trip end to end.
	 */
	void ToolPing(const TArray<FString>& Args);

	/** `AIDA.Index` — dump the aggregated factory overview to the log (extraction check, no LLM). */
	void Index(const TArray<FString>& Args);

	/** `AIDA.Nodes` — dump the resource-node summary to the log (map-scan check, no LLM). */
	void Nodes(const TArray<FString>& Args);

	/** Extract (TTL-cached) + aggregate the current factory. Server/authoritative worlds only. */
	FAIDAFactoryAggregates SnapshotAggregates();

	/** Max model<->tool round-trips before the loop gives up (guards against a tool-call cycle). */
	static constexpr int32 MaxToolRoundTrips = 5;

	FAIDAConfig Config;
	bool bConfigLoaded = false;

	TSharedPtr<FLLMClient> LLMClient;
	TUniquePtr<FAIDASessionManager> Session;
	TWeakObjectPtr<AAIDAChatRelay> Relay;
	FAIDARateLimiter RateLimiter;
	FAIDAPermissionService Permissions;
	FAIDAToolRegistry Tools;
	FAIDAFactoryIndex FactoryIndex;
	FAIDAMapService MapService;
	IConsoleCommand* PingCommand = nullptr;
	IConsoleCommand* SayCommand = nullptr;
	IConsoleCommand* ToolPingCommand = nullptr;
	IConsoleCommand* IndexCommand = nullptr;
	IConsoleCommand* NodesCommand = nullptr;
};
