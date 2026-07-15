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
#include "Recipes/AIDARecipeService.h" // TTL-cached static recipe + building catalog
#include "Memory/AIDAMemory.h"          // Phase 3 persistence facade (in-save + sidecar)
#include "Actions/AIDAActionEngine.h"   // Phase 4 proposal pipeline (store + approve/execute coordinator)
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
	/** Handle an inbound player chat line for a conversation: record it, then stream an AIDA reply out. */
	void HandleChatRequest(const FAIDARequester& Requester, const FString& Text, const FGuid& ConversationId);

	/** Authoritative full body for one message (recovery). False if it aged out of the transcript. */
	bool GetMessageBody(const FGuid& Id, FAIDATranscriptEntry& OutEntry) const;

	/** Recent transcript for a late-joining client. */
	void GetRecentTranscript(TArray<FAIDATranscriptEntry>& OutEntries) const;

	/**
	 * Approve/reject a pending proposal on behalf of a player (docs/PHASE4.md §4b). Enforces BOTH
	 * gates server-side — the act tier and the actions.approvalPolicy — before touching the engine;
	 * denials post a System chat line and log the player id. The UI's buttons are cosmetic.
	 */
	void HandleProposalDecision(const FAIDARequester& Requester, const FGuid& ProposalId, bool bApprove);
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
	class AAIDAProposalRelay* GetProposalRelay();

	/** Push one proposal's current state to the replicated view (docs/PHASE4.md §4a). Server-only. */
	void PublishProposal(const FGuid& ProposalId);

	/** Post a System line to the shared default conversation (proposal announcements/outcomes). */
	void AnnounceSystem(const FString& Text);

	/** Kick off the streaming LLM reply (through the tool loop) for an accepted request in a conversation. */
	void StartAIDAReply(const FAIDARequester& Requester, const FGuid& ConversationId);

	/** Register the built-in tools exposed to the model (Phase 2 Slice 0: an echo verifier). */
	void RegisterTools();

	/** Register the Phase 4 proposal tools (propose_build/propose_dismantle/get_proposal_status). */
	void RegisterActionTools();

	/** Lazily expire pending proposals past their TTL (docs/PHASE4.md §3; timer piggyback lands in Slice 2). */
	void SweepProposals();

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

	/** Assemble the privacy-filtered chat context (one conversation's recent transcript) sent to the LLM. */
	void BuildChatContext(const FGuid& ConversationId, TArray<FAIDAChatMessage>& OutMessages) const;

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

	/** `AIDA.Recipes` — dump the recipe/building catalog for a filter (static-catalog check, no LLM). */
	void Recipes(const TArray<FString>& Args);

	/** `AIDA.Memory` — dump the memory session id + note/marker/journal + sidecar snapshot counts. */
	void MemoryStatus(const TArray<FString>& Args);

	/** `AIDA.Snapshot` — take a factory history snapshot now (Phase 3 check, no LLM). */
	void Snapshot(const TArray<FString>& Args);

	/**
	 * `AIDA.Propose <spec json>` — drive propose_build directly (no LLM): parse → dry-run → store,
	 * logging the report. The widget-free Phase 4 Slice 1 verifier, like AIDA.Say for chat.
	 */
	void Propose(const TArray<FString>& Args);

	/**
	 * `AIDA.Approve <proposalId>` / `AIDA.Reject <proposalId>` — stand-in for the Slice 3
	 * ProposalUI: approve (kicking off the time-sliced executor) or reject a pending proposal
	 * from the server/host console.
	 */
	void ApproveCmd(const TArray<FString>& Args);
	void RejectCmd(const TArray<FString>& Args);

	/** `AIDA.Undo [n]` — console mirror of `/aida undo [n]` (reverse the last n AI actions). */
	void UndoCmd(const TArray<FString>& Args);

	/** Start the 10 Hz executor timer if it isn't running (docs/PHASE4.md §3 time-slicer). */
	void StartActionTimer();

	/** Build a snapshot from the current aggregates and append it to the sidecar ring buffer. Server-only. */
	void TakeSnapshot(const FString& Label);

	/** Extract (TTL-cached) + aggregate the current factory. Server/authoritative worlds only. */
	FAIDAFactoryAggregates SnapshotAggregates();

	/** Max model<->tool round-trips before the loop gives up (guards against a tool-call cycle). */
	static constexpr int32 MaxToolRoundTrips = 5;

	FAIDAConfig Config;
	bool bConfigLoaded = false;

	TSharedPtr<FLLMClient> LLMClient;
	TUniquePtr<FAIDASessionManager> Session;
	TWeakObjectPtr<AAIDAChatRelay> Relay;
	TWeakObjectPtr<class AAIDAProposalRelay> ProposalRelay;
	FAIDARateLimiter RateLimiter;
	FAIDAPermissionService Permissions;
	FAIDAToolRegistry Tools;
	FAIDAFactoryIndex FactoryIndex;
	FAIDAMapService MapService;
	FAIDARecipeCatalog RecipeCatalog;
	FAIDAMemory Memory;
	FAIDAActionEngine Actions;
	IConsoleCommand* PingCommand = nullptr;
	IConsoleCommand* SayCommand = nullptr;
	IConsoleCommand* ToolPingCommand = nullptr;
	IConsoleCommand* IndexCommand = nullptr;
	IConsoleCommand* NodesCommand = nullptr;
	IConsoleCommand* RecipesCommand = nullptr;
	IConsoleCommand* MemoryCommand = nullptr;
	IConsoleCommand* SnapshotCommand = nullptr;
	IConsoleCommand* ProposeCommand = nullptr;
	IConsoleCommand* ApproveCommand = nullptr;
	IConsoleCommand* RejectCommand = nullptr;
	IConsoleCommand* UndoCommand = nullptr;

	//~ Periodic history snapshots (Phase 3). Defaults; config wiring lands in Slice 3.
	UFUNCTION()
	void OnSnapshotTimer();
	FTimerHandle SnapshotTimer;

	//~ Phase 4 executor time-slicer: 10 Hz, running only while a proposal executes.
	UFUNCTION()
	void OnActionTimer();
	FTimerHandle ActionTimer;
};
