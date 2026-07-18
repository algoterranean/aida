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
#include "Core/AIDAImageStore.h"        // Phase 5 reference-image store + chunked-upload assembler
#include "Actions/AIDAActionEngine.h"   // Phase 4 proposal pipeline (store + approve/execute coordinator)
#include "Testing/AIDASelfTest.h"       // packaged-game scenario harness (complete type: TUniquePtr member)
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

	/**
	 * Phase 5 multimodal chat: Text plus reference-image ids the requester uploaded earlier this
	 * session. Ids are validated against the store (owner + liveness); the survivors ride the player
	 * turn into the LLM as image content blocks (docs/PHASE5.md §1).
	 */
	void HandleChatRequest(const FAIDARequester& Requester, const FString& Text, const FGuid& ConversationId,
		const TArray<FGuid>& ImageIds);

	//~ Phase 5 chunked image upload (docs/PHASE5.md §3). Called synchronously from the RCO; a false
	//~ return + OutError means the RCO should notify the owning client and consider the upload dead.
	bool HandleImageUploadBegin(const FAIDARequester& Requester, const FString& MediaType,
		int32 TotalBytes, int32 ChunkCount, FString& OutError);
	bool HandleImageUploadChunk(const FAIDARequester& Requester, int32 Seq, const TArray<uint8>& Data,
		FString& OutError);
	bool HandleImageUploadCommit(const FAIDARequester& Requester, uint32 Crc32, FGuid& OutImageId,
		FString& OutError);
	bool AreUploadsEnabled() const { return bConfigLoaded && Config.Uploads.bEnabled; }

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

	/**
	 * Nudge/rotate the newest pending proposal (ghost adjust — chat commands and keybinds both land
	 * here). Act-gated; the engine re-validates the moved placements and keeps the original on
	 * failure; the republished view moves every client's ghost.
	 */
	void HandleProposalAdjust(const FAIDARequester& Requester, const FVector& DeltaCm, int32 YawDeltaDeg,
		bool bQuietSuccess = false); // keybind taps: the moving ghost IS the feedback, don't spam chat
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

	/** Retire a pending proposal QUIETLY because a revision replaces it (revise-by-prompt): removed
	 *  from the store and the relay with no terminal announce/linger — the new proposal's ghost
	 *  takes over in the same publish cycle. */
	void SupersedeProposal(const FGuid& ProposalId);

	/** Post a System line to the shared default conversation (proposal announcements/outcomes). */
	void AnnounceSystem(const FString& Text);

	/** Proposal-created announcements only — OFF by default (the replicated proposal card + ghost
	 *  already show everything; the chat lines were clutter). actions.announceProposals re-enables. */
	void AnnounceProposal(const FString& Text);

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
		int32 RoundsLeft, FAIDAOnChunk OnDelta, TFunction<void(const FString&)> OnDone, FAIDAOnError OnError,
		bool bReadOnly = false);

	/** Assemble the privacy-filtered chat context (one conversation's recent transcript) sent to the LLM. */
	void BuildChatContext(const FGuid& ConversationId, TArray<FAIDAChatMessage>& OutMessages) const;

	/**
	 * Terminal step of a chat reply, with the fabrication SELF-REPAIR loop: when the finished text
	 * invites approval of a proposal but NO proposal was created during this request, the reply is
	 * not accepted — the fabricated turn goes back into the history with a corrective user turn and
	 * the tool loop re-runs (RetriesLeft bounds it) so the model actually calls the propose_* tool.
	 * Only when repair is exhausted does the old heads-up note post. Streaming continues into the
	 * same widget message either way.
	 */
	void FinishChatReply(const FGuid& MsgId, const FGuid& ConversationId, int64 ReplyStartUtc,
		TSharedRef<TArray<FAIDAChatMessage>, ESPMode::ThreadSafe> Messages, FAIDARequester Requester,
		int32 RetriesLeft, const FString& FinalText);

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

	/** `AIDA.CleanGhosts` — destroy stray hologram actors (leaked ghost previews) in the world. */
	void CleanGhosts(const TArray<FString>& Args);

	/** `AIDA.Recipes` — dump the recipe/building catalog for a filter (static-catalog check, no LLM). */
	void Recipes(const TArray<FString>& Args);

	/** `AIDA.DumpPack` — rebuild + log the generated game data pack (docs/PROMPT.md §2 eyeball check, no LLM). */
	void DumpPack(const TArray<FString>& Args);

	/**
	 * The generated game data pack appended to the system prompt (docs/PROMPT.md §2). Built lazily
	 * from the recipe catalog and cached for PackTtlSeconds — the bytes must stay stable across
	 * requests (prompt-cache friendliness), so it is NOT rebuilt per message. Server-only.
	 */
	const FString& GetPromptPack();

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
	FString PromptPackCache;
	double PromptPackBuiltAt = -1.0;
	static constexpr double PackTtlSeconds = 300.0;
	FAIDAMemory Memory;
	FAIDAActionEngine Actions;
	//~ Phase 5: uploaded reference images (server RAM only) + per-player upload reassembly.
	FAIDAImageStore ImageStore;
	FAIDAImageUploadAssembler ImageUploads;
	IConsoleCommand* PingCommand = nullptr;
	IConsoleCommand* SayCommand = nullptr;
	IConsoleCommand* ToolPingCommand = nullptr;
	IConsoleCommand* IndexCommand = nullptr;
	IConsoleCommand* NodesCommand = nullptr;
	IConsoleCommand* RecipesCommand = nullptr;
	IConsoleCommand* DumpPackCommand = nullptr;
	IConsoleCommand* MemoryCommand = nullptr;
	IConsoleCommand* SnapshotCommand = nullptr;
	IConsoleCommand* ProposeCommand = nullptr;
	IConsoleCommand* ApproveCommand = nullptr;
	IConsoleCommand* RejectCommand = nullptr;
	IConsoleCommand* UndoCommand = nullptr;
	IConsoleCommand* CleanGhostsCommand = nullptr;

	//~ Periodic history snapshots (Phase 3). Defaults; config wiring lands in Slice 3.
	UFUNCTION()
	void OnSnapshotTimer();
	FTimerHandle SnapshotTimer;

	/** Terminal proposal outcomes (utc, "state: summary") feeding the prompt's LIVE PROPOSAL STATE
	 *  "recently resolved" list — the model must never re-report a resolved proposal as pending. */
	TArray<TPair<int64, FString>> RecentProposalOutcomes;

	//~ Login-anchored snapshot (P7 Slice 0 polish): one per player join, so "since I last logged in"
	//~ has a clean baseline. Bound server-side to the global PostLogin event; unbound in Deinitialize.
	void OnPlayerPostLogin(class AGameModeBase* GameMode, class APlayerController* NewPlayer);
	FDelegateHandle PostLoginHandle;

	//~ Phase 4 executor time-slicer: 10 Hz, running only while a proposal executes.
	UFUNCTION()
	void OnActionTimer();
	FTimerHandle ActionTimer;

	//~ P8 Slice 5 standing tasks: a slow poll (60 s) that runs at most ONE due task at a time
	//~ through the tool loop with Query-tier tools only. Human-created via /aida task commands.
	void HandleTaskCommand(const FAIDARequester& Requester, const struct FAIDAChatCommand& Command, const FGuid& ConversationId);
	UFUNCTION()
	void OnTaskTimer();
	FTimerHandle TaskTimer;
	bool bTaskRunning = false;
	int32 TaskRunsToday = 0;
	int64 TaskDayStamp = 0; // UTC day number the counter belongs to

	//~ Packaged-game scenario harness (docs/SELFTEST.md): the runner drives Tools/decisions through
	//~ the same seams the model uses, so it gets friend access instead of a widened public surface.
	friend class FAIDASelfTestRunner;
	TUniquePtr<FAIDASelfTestRunner> SelfTest;
};
