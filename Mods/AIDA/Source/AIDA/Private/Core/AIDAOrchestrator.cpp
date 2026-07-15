#include "Core/AIDAOrchestrator.h"

#include "AIDA.h"
#include "Core/AIDAConfigLoader.h"
#include "Adapters/LLMClient.h"
#include "Factory/AIDAFactoryAggregator.h"
#include "Tools/AIDAFactoryTools.h"
#include "Tools/AIDAMapTools.h"
#include "Tools/AIDARecipeTools.h"
#include "Tools/AIDANotesTools.h"
#include "Tools/AIDASnapshotTools.h"
#include "Tools/AIDAToolJson.h"
#include "Actions/AIDAActionSpec.h"
#include "Actions/AIDAActionSeam.h"
#include "Actions/AIDAChatCommands.h"
#include "Memory/AIDAMemoryStore.h"
#include "Net/AIDAChatRelay.h"
#include "Net/AIDAProposalRelay.h"
#include "Subsystem/SubsystemActorManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/Pawn.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"

namespace
{
	/** Map a tool's declared tier onto the permission tier the orchestrator gates it with. */
	EAIDATier ToPermissionTier(EAIDAToolTier Tier)
	{
		switch (Tier)
		{
		case EAIDAToolTier::Chat: return EAIDATier::Chat;
		case EAIDAToolTier::Act:  return EAIDATier::Act;
		default:                  return EAIDATier::Query;
		}
	}

	/**
	 * Resolve a requesting player's pawn location by their net-id string (as set in the RCO).
	 * The listen-server host's FUniqueNetIdRepl resolves to null → an EMPTY PlayerId (same quirk
	 * the act allowlist handles), so an empty id matches the controller whose net id is also
	 * empty — i.e. the host. Remote clients always carry real ids, so they can't false-match.
	 */
	bool AIDAResolvePlayerLocation(UWorld* World, const FString& PlayerId, FVector& OutLocation)
	{
		if (!World || PlayerId == TEXT("debug")) { return false; }
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			APlayerState* PS = PC ? PC->PlayerState : nullptr;
			if (!PS) { continue; }
			const TSharedPtr<const FUniqueNetId> NetId = PS->GetUniqueId().GetUniqueNetId();
			const FString Id = NetId.IsValid() ? NetId->ToString() : FString();
			if (Id == PlayerId)
			{
				if (const APawn* Pawn = PC->GetPawn())
				{
					OutLocation = Pawn->GetActorLocation();
					return true;
				}
			}
		}
		return false;
	}

	/** AIDA's persona + tool guidance. Prepended to every chat completion so the model uses the tools. */
	const TCHAR* GAIDASystemPrompt = TEXT(
		"You are AIDA, an assistant embedded inside the factory-building game Satisfactory. You help the "
		"player understand and optimize THEIR live factory on the current save.\n\n"
		"You have tools that read the player's ACTUAL factory and world. Always call the relevant tool to "
		"answer a question about the factory instead of guessing or asking the player for data you can look "
		"up yourself:\n"
		"- get_factory_overview(): machine clusters, the biggest item deficits, and power per circuit.\n"
		"- get_item_balance(item?): net production vs consumption per item; deficits first.\n"
		"- inspect_cluster(id): one cluster's building census, net inputs/outputs, and efficiency.\n"
		"- find_bottleneck(item): why an item's production is limited (starved input, power, or backed up).\n"
		"- get_resource_nodes(resource?, untapped_only?): map resource nodes by purity and occupancy.\n"
		"- lookup_recipe(item?): static recipe reference — inputs/outputs (amount + per-minute), craft time, "
		"and which building makes it. Use for 'how is X made / what does X need', not live factory data.\n"
		"- lookup_building(name?): static building reference — a production building's power draw (and, for "
		"generators, power output).\n"
		"- tag_node(resource, purity?, label?): drop a labeled marker on the map at the nearest untapped "
		"node of a resource to the player. This WRITES a shared map marker — only use it when the player "
		"asks you to mark/tag/find-and-mark a node.\n"
		"- add_note(text, tags?): save a persistent note for the player (survives sessions), tagged with "
		"their location/region. Use when they ask you to remember/note something.\n"
		"- get_notes(keyword?, region?, near?): recall the player's saved notes. Check these when the "
		"player asks what they wanted to do, or refers to something they told you earlier. To list ALL "
		"notes (e.g. 'what were my notes?'), call get_notes with NO arguments — do not invent a keyword, "
		"and never use the player's name as a filter (notes match on content, not author).\n"
		"- take_snapshot(label?): capture the factory's production + power now for later comparison.\n"
		"- compare_to(timestamp?, item?): how the factory changed vs an earlier snapshot (per-item + power "
		"deltas). Use for 'how has my X changed', 'what changed since...'. Omit timestamp for the latest "
		"snapshot.\n\n"
		"World-modifying proposal tools (only registered when the server enables them):\n"
		"- propose_build(spec): propose placing buildables on a snapped grid. NOTHING is built until a "
		"player with act permission approves the proposal. Only call it when the player explicitly asks "
		"you to build/place something. Omit the spec's 'origin' to build where the player is aiming ('here', "
		"'there', 'at my position', or no location given) — you never need to ask for coordinates. On success, "
		"relay the returned summary + cost and say it awaits approval; on a tool error (invalid placement, "
		"cost, cap), revise the spec or explain the reason. Never claim something was built until "
		"get_proposal_status says executed.\n"
		"- propose_dismantle(selector): same flow for removing buildables near a point.\n"
		"- get_proposal_status(proposalId?): check whether proposals were approved/executed/expired.\n"
		"You CANNOT undo actions yourself — when a player wants something reversed, tell them to type "
		"/aida undo (or /aida undo N) in this chat. Players decide proposals with the on-screen card or "
		"/aida approve, /aida reject.\n"
		"CRITICAL: a proposal exists ONLY when a propose_* call in THIS turn returned a proposalId. Never "
		"announce a proposal, cost, or 'awaiting approval' without that tool result — if you did not call "
		"the tool, or it returned an error, say exactly that instead. Never invent costs or ids.\n\n"
		"Conventions: rates are items per minute (fluids in m3/min); coordinates are in metres; cluster ids "
		"come from get_factory_overview. When a question is about production, shortages, bottlenecks, or "
		"resources, call a tool first and answer from the real numbers it returns.\n\n"
		"Style: this is an in-game chat overlay, so keep answers short and concrete. Light Markdown is "
		"rendered — use **bold** for key items/numbers and short bullet lists ('- '); Markdown tables render "
		"aligned, so if you use one always include a header row and its |---| separator. Avoid big headers "
		"and long lists. You are NOT a general-purpose or coding assistant — stay focused on this "
		"Satisfactory factory.");
}

bool UAIDAOrchestrator::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Only real game worlds (dedicated/listen server, standalone, PIE) — not editor/preview worlds.
	if (const UWorld* World = Cast<UWorld>(Outer))
	{
		return World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE;
	}
	return false;
}

void UAIDAOrchestrator::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// The orchestrator is the only network-egress authority. Clients hold no config and no key —
	// but they MUST still register the relay subsystem class, or SML never records the replicated
	// relay actor on the client and the ChatWidget can never bind to it (client sees no stream).
	if (InWorld.GetNetMode() == NM_Client)
	{
		RegisterRelayClass();
		UE_LOG(LogAIDA, Verbose, TEXT("Orchestrator idle on client world (relay class registered for replication)."));
		return;
	}

	UE_LOG(LogAIDA, Log, TEXT("AIDA orchestrator starting (netmode=%d)."), static_cast<int32>(InWorld.GetNetMode()));
	LoadConfig();

	Session = MakeUnique<FAIDASessionManager>();
	RegisterRelay();
	RegisterTools();

	RegisterConsoleCommands();
}

void UAIDAOrchestrator::Deinitialize()
{
	if (PingCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(PingCommand);
		PingCommand = nullptr;
	}
	if (SayCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(SayCommand);
		SayCommand = nullptr;
	}
	if (ToolPingCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ToolPingCommand);
		ToolPingCommand = nullptr;
	}
	if (IndexCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(IndexCommand);
		IndexCommand = nullptr;
	}
	if (NodesCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(NodesCommand);
		NodesCommand = nullptr;
	}
	if (ProposeCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ProposeCommand);
		ProposeCommand = nullptr;
	}
	if (ApproveCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ApproveCommand);
		ApproveCommand = nullptr;
	}
	if (RejectCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(RejectCommand);
		RejectCommand = nullptr;
	}
	if (UndoCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(UndoCommand);
		UndoCommand = nullptr;
	}
	LLMClient.Reset();
	Session.Reset();

	Super::Deinitialize();
}

void UAIDAOrchestrator::RegisterRelayClass()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[diag] RegisterRelayClass: no world."));
		return;
	}

	USubsystemActorManager* Mgr = World->GetSubsystem<USubsystemActorManager>();
	if (!Mgr)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[diag] RegisterRelayClass: NO SubsystemActorManager for world (netmode=%d)."), static_cast<int32>(World->GetNetMode()));
		return;
	}

	// SpawnOnServer_Replicate: spawns immediately on the server, only registers the class on
	// clients (so the manager records the replicated actor when it lands — see header note).
	Mgr->RegisterSubsystemActor(AAIDAChatRelay::StaticClass());
	Mgr->RegisterSubsystemActor(AAIDAProposalRelay::StaticClass());

	// The in-save memory store (Phase 3). SpawnOnServer, not replicated — persists via IFGSaveInterface;
	// only the server touches it. Bind the memory facade to it (mints/reads the session GUID).
	if (World->GetNetMode() != NM_Client)
	{
		Mgr->RegisterSubsystemActor(AAIDAMemoryStore::StaticClass());
		Memory.Init(this);

		// Periodic history snapshots into the sidecar ring buffer (Phase 3). A looping world timer at the
		// configured interval; the first snapshot lands one interval in (take_snapshot can seed one sooner).
		if (!SnapshotTimer.IsValid())
		{
			const float IntervalSeconds = FMath::Max(1, Config.Snapshots.IntervalMinutes) * 60.f;
			World->GetTimerManager().SetTimer(SnapshotTimer, this, &UAIDAOrchestrator::OnSnapshotTimer,
				IntervalSeconds, /*bLoop=*/true);
		}
	}

	// Diagnostics: did the manager end up holding a findable actor, and does one actually exist in the world?
	const bool bFoundInMgr = Mgr->GetSubsystemActor<AAIDAChatRelay>() != nullptr;
	int32 ActorsInWorld = 0;
	for (TActorIterator<AAIDAChatRelay> It(World); It; ++It)
	{
		++ActorsInWorld;
	}
	UE_LOG(LogAIDA, Log, TEXT("[diag] RegisterRelayClass: netmode=%d mgrLookup=%s actorsInWorld=%d"),
		static_cast<int32>(World->GetNetMode()), bFoundInMgr ? TEXT("FOUND") : TEXT("null"), ActorsInWorld);
}

void UAIDAOrchestrator::RegisterRelay()
{
	RegisterRelayClass();
	AAIDAChatRelay* R = GetRelay(); // cache + hand it to the session manager (server: spawned above)
	UE_LOG(LogAIDA, Log, TEXT("Relay registered (server spawn %s)."), R ? TEXT("OK") : TEXT("PENDING"));
}

AAIDAProposalRelay* UAIDAOrchestrator::GetProposalRelay()
{
	if (ProposalRelay.IsValid())
	{
		return ProposalRelay.Get();
	}
	if (UWorld* World = GetWorld())
	{
		if (USubsystemActorManager* Mgr = World->GetSubsystem<USubsystemActorManager>())
		{
			if (AAIDAProposalRelay* R = Mgr->GetSubsystemActor<AAIDAProposalRelay>())
			{
				ProposalRelay = R;
				return R;
			}
		}
	}
	return nullptr;
}

AAIDAChatRelay* UAIDAOrchestrator::GetRelay()
{
	if (Relay.IsValid())
	{
		return Relay.Get();
	}
	if (UWorld* World = GetWorld())
	{
		if (USubsystemActorManager* Mgr = World->GetSubsystem<USubsystemActorManager>())
		{
			if (AAIDAChatRelay* R = Mgr->GetSubsystemActor<AAIDAChatRelay>())
			{
				Relay = R;
				if (Session.IsValid())
				{
					Session->SetRelay(R);
				}
				return R;
			}
		}
	}
	return nullptr;
}

void UAIDAOrchestrator::HandleChatRequest(const FAIDARequester& Requester, const FString& Text, const FGuid& ConversationId)
{
	// Authority-only: this path fans out network RPCs and calls the LLM (server egress).
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		return;
	}
	if (!Session.IsValid())
	{
		UE_LOG(LogAIDA, Warning, TEXT("HandleChatRequest before session init; dropping."));
		return;
	}

	const FString Trimmed = Text.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return;
	}

	// Make sure the relay is available (it may have finished spawning after OnWorldBeginPlay).
	GetRelay();

	UE_LOG(LogAIDA, Log, TEXT("Chat from %s [%s]: \"%s\""), *Requester.Author, *Requester.PlayerId, *Trimmed);

	// `/aida ...` commands short-circuit the LLM entirely (docs/PHASE4.md §4d) — undo in particular
	// must never depend on a model round-trip. The player's line still echoes into the transcript.
	FAIDAChatCommand Command;
	FString CommandError;
	if (AIDAChatCommands::TryParse(Trimmed, Command, CommandError))
	{
		Session->PostPlayerMessage(Requester.Author, Trimmed, ConversationId);

		if (Command.Kind == FAIDAChatCommand::EKind::None)
		{
			Session->PostSystemMessage(CommandError, ConversationId);
			return;
		}

		// Approve/Reject a pending proposal from chat (the same double-gated path as the UI buttons).
		if (Command.Kind == FAIDAChatCommand::EKind::Approve || Command.Kind == FAIDAChatCommand::EKind::Reject)
		{
			FGuid Target = Command.ProposalId;
			if (!Target.IsValid())
			{
				// No id = the newest pending proposal.
				SweepProposals();
				int64 NewestUtc = -1;
				for (const FAIDAProposal& Proposal : Actions.Store().All())
				{
					if (Proposal.State == EAIDAProposalState::Pending && Proposal.ProposedUtc > NewestUtc)
					{
						NewestUtc = Proposal.ProposedUtc;
						Target = Proposal.Id;
					}
				}
			}
			if (!Target.IsValid())
			{
				Session->PostSystemMessage(TEXT("No pending proposal to decide. (Real proposals always get an 'AIDA proposes …' system line — if AIDA claimed one without it, it was mistaken; ask it to propose again.)"), ConversationId);
				return;
			}
			HandleProposalDecision(Requester, Target, Command.Kind == FAIDAChatCommand::EKind::Approve);
			return;
		}

		// Undo reverses world actions — the same act tier the propose tools require.
		if (!Permissions.IsAllowed(EAIDATier::Act, Requester.PlayerId))
		{
			UE_LOG(LogAIDA, Warning, TEXT("Undo DENIED (act tier) for %s [%s]"), *Requester.Author, *Requester.PlayerId);
			Session->PostSystemMessage(TEXT("You don't have act permission to undo AIDA's actions."), ConversationId);
			return;
		}

		FString Message;
		if (Actions.StartUndo(this, Config.Actions, Memory, Command.Count, Requester.PlayerId, Message))
		{
			StartActionTimer();
		}
		Session->PostSystemMessage(Message, ConversationId);
		return;
	}

	// Permission (chat tier) — denials are logged with the player id and answered privately.
	if (!Permissions.IsAllowed(EAIDATier::Chat, Requester.PlayerId))
	{
		UE_LOG(LogAIDA, Warning, TEXT("Chat DENIED (permission) for %s [%s]"), *Requester.Author, *Requester.PlayerId);
		Session->PostSystemMessage(TEXT("You don't have permission to chat with AIDA on this server."), ConversationId);
		return;
	}

	// Rate limit (per-player + global). Denied requests never reach the LLM.
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	FString DenyReason;
	if (!RateLimiter.TryConsume(Requester.PlayerId, Now, DenyReason))
	{
		UE_LOG(LogAIDA, Warning, TEXT("Chat THROTTLED for %s [%s]: %s"), *Requester.Author, *Requester.PlayerId, *DenyReason);
		Session->PostSystemMessage(DenyReason, ConversationId);
		return;
	}

	Session->PostPlayerMessage(Requester.Author, Trimmed, ConversationId);

	StartAIDAReply(Requester, ConversationId);
}

void UAIDAOrchestrator::BuildChatContext(const FGuid& ConversationId, TArray<FAIDAChatMessage>& OutMessages) const
{
	OutMessages.Reset();
	if (!Session.IsValid())
	{
		return;
	}

	// Only this conversation's messages form the LLM context (each tab is an independent conversation).
	TArray<FAIDATranscriptEntry> All;
	Session->GetRecentTranscript(All);
	TArray<FAIDATranscriptEntry> Transcript;
	Transcript.Reserve(All.Num());
	for (const FAIDATranscriptEntry& E : All)
	{
		if (E.Header.ConversationId == ConversationId) { Transcript.Add(E); }
	}

	// Privacy: bound how much history leaves the server, and whether player names are included.
	const int32 Depth = FMath::Max(1, Config.Privacy.SendChatHistoryDepth);
	const bool bNames = Config.Privacy.bSendPlayerNames;
	const int32 Start = FMath::Max(0, Transcript.Num() - Depth);

	for (int32 i = Start; i < Transcript.Num(); ++i)
	{
		const FAIDATranscriptEntry& Entry = Transcript[i];
		if (Entry.Body.IsEmpty())
		{
			continue; // skip an in-progress (empty) message
		}

		FAIDAChatMessage Msg;
		switch (Entry.Header.Kind)
		{
		case EAIDAMsgKind::AIDA:
			Msg.Role = TEXT("assistant");
			Msg.Content = Entry.Body;
			break;
		case EAIDAMsgKind::Player:
			Msg.Role = TEXT("user");
			Msg.Content = bNames ? FString::Printf(TEXT("%s: %s"), *Entry.Header.Author, *Entry.Body) : Entry.Body;
			break;
		default:
			continue; // system notices are not part of the model context
		}
		OutMessages.Add(MoveTemp(Msg));
	}
}

void UAIDAOrchestrator::StartAIDAReply(const FAIDARequester& Requester, const FGuid& ConversationId)
{
	if (!LLMClient.IsValid() || !LLMClient->IsReady())
	{
		Session->PostSystemMessage(TEXT("AIDA is not configured (no LLM provider). Ask an admin to set up Configs/AIDA/config.jsonc."), ConversationId);
		return;
	}

	// Build context BEFORE opening the AIDA message so the empty reply isn't included.
	TArray<FAIDAChatMessage> Context;
	BuildChatContext(ConversationId, Context);

	const FGuid MsgId = Session->BeginAIDAMessage(TEXT("AIDA"), ConversationId);

	// Route the visible chat through the tool loop so the model can inspect the factory before answering.
	// Shared so the message history survives the async tool rounds.
	const TSharedRef<TArray<FAIDAChatMessage>, ESPMode::ThreadSafe> Messages =
		MakeShared<TArray<FAIDAChatMessage>, ESPMode::ThreadSafe>(MoveTemp(Context));

	TWeakObjectPtr<UAIDAOrchestrator> Weak(this);
	RunToolLoop(Messages, Requester, MaxToolRoundTrips,
		[Weak, MsgId](const FString& Delta)
		{
			if (UAIDAOrchestrator* O = Weak.Get(); O && O->Session.IsValid())
			{
				O->Session->AppendDelta(MsgId, Delta);
			}
		},
		[Weak, MsgId](const FString& /*FullText*/)
		{
			if (UAIDAOrchestrator* O = Weak.Get(); O && O->Session.IsValid())
			{
				O->Session->CompleteMessage(MsgId);
			}
		},
		[Weak, MsgId](int32 Status, const FString& Message)
		{
			if (UAIDAOrchestrator* O = Weak.Get(); O && O->Session.IsValid())
			{
				UE_LOG(LogAIDA, Error, TEXT("AIDA reply failed (HTTP %d): %s"), Status, *Message);
				O->Session->AppendDelta(MsgId, TEXT("\n[response interrupted]"));
				O->Session->CompleteMessage(MsgId);
			}
		});
}

void UAIDAOrchestrator::RegisterTools()
{
	// Slice 0 verifier: a trivial echo tool. Proves the model->tool->model round-trip end to end
	// before any factory-inspection tools land (docs/PHASE2.md Slice 0).
	Tools.Register({
		TEXT("echo"),
		TEXT("Echo the 'text' argument back verbatim. Use this to verify that tool calling works."),
		TEXT(R"({"type":"object","properties":{"text":{"type":"string","description":"The text to echo back."}},"required":["text"]})"),
		EAIDAToolTier::Query,
		[](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			FString Text;
			Args->TryGetStringField(TEXT("text"), Text);
			return FAIDAToolResult::Ok(FString::Printf(TEXT("echo: %s"), *Text));
		}
	});

	// Slice 2 factory-sight tools (docs/ARCHITECTURE.md §4). Each aggregates a TTL-cached snapshot and
	// returns bounded JSON. Handlers capture `this`; the registry is a member, so it never outlives us.
	Tools.Register({
		TEXT("get_factory_overview"),
		TEXT("Summarize the whole factory: machine clusters (id, size, location, main output), the biggest item deficits, and power per circuit. Call this first for the big picture."),
		TEXT(""),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& /*Args*/, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			return FAIDAToolResult::Ok(AIDAFactoryTools::BuildOverviewJson(SnapshotAggregates()));
		}
	});

	Tools.Register({
		TEXT("get_item_balance"),
		TEXT("Net production vs consumption per item across the factory, deficits first. Pass 'item' to focus on one item; omit it for the whole balance."),
		TEXT(R"({"type":"object","properties":{"item":{"type":"string","description":"Optional item name to filter to (e.g. \"Iron Plate\")."}}})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			FString Item;
			Args->TryGetStringField(TEXT("item"), Item);
			return FAIDAToolResult::Ok(AIDAFactoryTools::BuildItemBalanceJson(SnapshotAggregates(), Item));
		}
	});

	Tools.Register({
		TEXT("inspect_cluster"),
		TEXT("Drill into one machine cluster by id (from get_factory_overview): its building census, net inputs/outputs, efficiency, and machine ids."),
		TEXT(R"({"type":"object","properties":{"id":{"type":"integer","description":"Cluster id from get_factory_overview."}},"required":["id"]})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			int32 Id = 0;
			Args->TryGetNumberField(TEXT("id"), Id);
			return FAIDAToolResult::Ok(AIDAFactoryTools::BuildClusterJson(SnapshotAggregates(), Id));
		}
	});

	Tools.Register({
		TEXT("find_bottleneck"),
		TEXT("Diagnose why an item's production is limited: starved by an upstream deficit, throttled by overloaded power, or output backing up. Pass the item name."),
		TEXT(R"({"type":"object","properties":{"item":{"type":"string","description":"Item to diagnose (e.g. \"Reinforced Iron Plate\")."}},"required":["item"]})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			FString Item;
			Args->TryGetStringField(TEXT("item"), Item);
			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			const FAIDAFactorySnapshot& Snapshot = FactoryIndex.GetSnapshot(World, Now);
			return FAIDAToolResult::Ok(AIDAFactoryTools::BuildBottleneckJson(FAIDAFactoryAggregator::FindBottleneck(Snapshot, Item)));
		}
	});

	Tools.Register({
		TEXT("get_resource_nodes"),
		TEXT("Resource nodes on the map, grouped by resource and purity (total vs free). Pass 'resource' to filter, or 'untapped_only' to see only unoccupied nodes (with grid coordinates)."),
		TEXT(R"({"type":"object","properties":{"resource":{"type":"string","description":"Optional resource name to filter to (e.g. \"Iron Ore\")."},"untapped_only":{"type":"boolean","description":"If true, only nodes with no extractor built on them."}}})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			FString Resource;
			Args->TryGetStringField(TEXT("resource"), Resource);
			bool bUntappedOnly = false;
			Args->TryGetBoolField(TEXT("untapped_only"), bUntappedOnly);

			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			return FAIDAToolResult::Ok(AIDAMapTools::BuildResourceNodesJson(MapService.GetNodes(World, Now), Resource, bUntappedOnly));
		}
	});

	// Slice 3b tag_node (Act tier — it writes a persistent, replicated map marker). Places a labeled
	// map stamp on the nearest untapped node of a resource to the requesting player.
	Tools.Register({
		TEXT("tag_node"),
		TEXT("Place a labeled marker on the map at the nearest untapped resource node of a given resource to the player (a saved, shared map stamp). Pass 'resource' (required), optional 'purity' (Impure/Normal/Pure), and an optional 'label'."),
		TEXT(R"({"type":"object","properties":{"resource":{"type":"string","description":"Resource to tag (e.g. \"Caterium Ore\")."},"purity":{"type":"string","description":"Optional purity: Impure, Normal, or Pure."},"label":{"type":"string","description":"Optional marker label; defaults to the resource name."}},"required":["resource"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			FString Resource, Purity, Label;
			Args->TryGetStringField(TEXT("resource"), Resource);
			Args->TryGetStringField(TEXT("purity"), Purity);
			Args->TryGetStringField(TEXT("label"), Label);
			if (Resource.IsEmpty()) { return FAIDAToolResult::Error(TEXT("tag_node needs a 'resource'.")); }

			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			const TArray<FAIDAResourceNode>& Nodes = MapService.GetNodes(World, Now);
			const FAIDAResourceNode* Node = AIDAMapTools::FindNearestUntapped(Nodes, Resource, Purity, Ctx.Location, Ctx.bHasLocation);
			if (!Node)
			{
				return FAIDAToolResult::Error(FString::Printf(TEXT("No untapped %s%s node found."),
					Purity.IsEmpty() ? TEXT("") : *(Purity + TEXT(" ")), *Resource));
			}

			const FString MarkerLabel = !Label.IsEmpty() ? Label
				: (Purity.IsEmpty() ? Node->Resource : FString::Printf(TEXT("%s (%s)"), *Node->Resource, *Node->Purity));
			if (!FAIDAMapService::PlaceMapMarker(World, Node->Location, MarkerLabel))
			{
				return FAIDAToolResult::Error(TEXT("Could not place the marker (map unavailable or marker limit reached)."));
			}

			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetBoolField(TEXT("tagged"), true);
			Root->SetStringField(TEXT("label"), MarkerLabel);
			Root->SetStringField(TEXT("resource"), Node->Resource);
			Root->SetStringField(TEXT("purity"), Node->Purity);
			if (!Node->Region.IsEmpty()) { Root->SetStringField(TEXT("region"), Node->Region); }
			return FAIDAToolResult::Ok(AIDAToCompactJson(Root));
		}
	});

	// Slice 3b static-reference tools (docs/PHASE2.md). Chat tier — pure game data, no live state read.
	Tools.Register({
		TEXT("lookup_recipe"),
		TEXT("Static recipe reference: how an item is made. Pass 'item' to find recipes that produce it (inputs/outputs with per-minute rates, craft time, and the building that makes it). Use for crafting questions, not the player's live factory."),
		TEXT(R"({"type":"object","properties":{"item":{"type":"string","description":"Item or recipe name to look up (e.g. \"Reinforced Iron Plate\")."}}})"),
		EAIDAToolTier::Chat,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			FString Item;
			Args->TryGetStringField(TEXT("item"), Item);
			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			return FAIDAToolResult::Ok(AIDARecipeTools::BuildRecipeJson(RecipeCatalog.GetRecipes(World, Now), Item));
		}
	});

	Tools.Register({
		TEXT("lookup_building"),
		TEXT("Static building reference: a production building's power draw (and, for generators, power output). Pass 'name' to filter (e.g. \"Assembler\", \"Refinery\")."),
		TEXT(R"({"type":"object","properties":{"name":{"type":"string","description":"Building name to look up (e.g. \"Constructor\")."}}})"),
		EAIDAToolTier::Chat,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			FString Name;
			Args->TryGetStringField(TEXT("name"), Name);
			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			return FAIDAToolResult::Ok(AIDARecipeTools::BuildBuildingJson(RecipeCatalog.GetBuildings(World, Now), Name));
		}
	});

	// Phase 3 note tools (docs/PHASE3.md) — persistent player annotations in the in-save memory store.
	Tools.Register({
		TEXT("add_note"),
		TEXT("Save a persistent note for the player (survives across sessions), tagged with their current location and region. Use when the player asks you to remember/note something. Pass 'text' and optional 'tags'."),
		TEXT(R"({"type":"object","properties":{"text":{"type":"string","description":"The note to remember."},"tags":{"type":"array","items":{"type":"string"},"description":"Optional short tags (e.g. \"power\", \"todo\")."}},"required":["text"]})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			FString Text;
			Args->TryGetStringField(TEXT("text"), Text);
			if (Text.TrimStartAndEnd().IsEmpty()) { return FAIDAToolResult::Error(TEXT("add_note needs non-empty 'text'.")); }

			AAIDAMemoryStore* Store = Memory.Store(this);
			if (!Store) { return FAIDAToolResult::Error(TEXT("Memory store unavailable (run on the server/host).")); }

			FAIDANote Note;
			Note.Text = Text;
			Note.Author = Ctx.Author;
			Note.AuthorId = Ctx.PlayerId;
			Note.CreatedUtc = FDateTime::UtcNow().ToUnixTimestamp();
			if (Ctx.bHasLocation)
			{
				Note.Location = Ctx.Location;
				Note.Region = FAIDAMapService::RegionNameForLocation(GetWorld(), Ctx.Location);
			}
			const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
			if (Args->TryGetArrayField(TEXT("tags"), Tags) && Tags)
			{
				for (const TSharedPtr<FJsonValue>& V : *Tags)
				{
					FString T;
					if (V.IsValid() && V->TryGetString(T) && !T.IsEmpty()) { Note.Tags.Add(T); }
				}
			}
			const FString Region = Note.Region;
			Store->AddNote(MoveTemp(Note));

			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetBoolField(TEXT("saved"), true);
			if (!Region.IsEmpty()) { Root->SetStringField(TEXT("region"), Region); }
			return FAIDAToolResult::Ok(AIDAToCompactJson(Root));
		}
	});

	Tools.Register({
		TEXT("get_notes"),
		TEXT("Recall the player's saved notes. To list everything (e.g. 'what were my notes?'), call with NO arguments. 'keyword' filters note CONTENT (text or a tag) — never pass the player's name or 'author' as a keyword. 'region' filters by map area; 'near' sorts by distance to the player."),
		TEXT(R"({"type":"object","properties":{"keyword":{"type":"string","description":"Case-insensitive text/tag filter."},"region":{"type":"string","description":"Map-area name filter."},"near":{"type":"boolean","description":"Sort by distance to the player instead of most-recent."}}})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			FString Keyword, Region;
			Args->TryGetStringField(TEXT("keyword"), Keyword);
			Args->TryGetStringField(TEXT("region"), Region);
			bool bNear = false;
			Args->TryGetBoolField(TEXT("near"), bNear);

			AAIDAMemoryStore* Store = Memory.Store(this);
			if (!Store) { return FAIDAToolResult::Error(TEXT("Memory store unavailable (run on the server/host).")); }

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			return FAIDAToolResult::Ok(AIDANotesTools::BuildNotesJson(
				Store->GetNotes(), Keyword, Region, Ctx.Location, bNear && Ctx.bHasLocation, Now));
		}
	});

	// Phase 3 history tools (docs/PHASE3.md) — timestamped factory snapshots + a diff over time.
	Tools.Register({
		TEXT("take_snapshot"),
		TEXT("Capture a timestamped snapshot of the factory's current production balance + power now, so it can be compared later. Snapshots are also taken automatically every 30 minutes. Optional 'label'."),
		TEXT(R"({"type":"object","properties":{"label":{"type":"string","description":"Optional short label (e.g. \"before boss\")."}}})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			FString Label;
			Args->TryGetStringField(TEXT("label"), Label);
			TakeSnapshot(Label);

			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetBoolField(TEXT("taken"), true);
			Root->SetNumberField(TEXT("totalSnapshots"), Memory.LoadSnapshots().Num());
			return FAIDAToolResult::Ok(AIDAToCompactJson(Root));
		}
	});

	Tools.Register({
		TEXT("compare_to"),
		TEXT("Compare the factory now to an earlier snapshot: per-item production/consumption changes and power delta. Omit 'timestamp' to compare to the most recent snapshot; pass a Unix timestamp to compare further back. Optional 'item' focuses on one item."),
		TEXT(R"({"type":"object","properties":{"timestamp":{"type":"integer","description":"Optional Unix time to compare back to; defaults to the latest snapshot."},"item":{"type":"string","description":"Optional item name to focus on."}},"required":[]})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			double TimestampD = 0.0;
			const bool bHasTs = Args->TryGetNumberField(TEXT("timestamp"), TimestampD);
			FString Item;
			Args->TryGetStringField(TEXT("item"), Item);

			const TArray<FAIDASnapshot> Snaps = Memory.LoadSnapshots();
			const FAIDASnapshot* Baseline = AIDASnapshotTools::PickBaseline(Snaps, static_cast<int64>(TimestampD), bHasTs);
			if (!Baseline)
			{
				return FAIDAToolResult::Error(TEXT("No snapshots yet to compare against. Use take_snapshot first, or wait for the automatic one."));
			}

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			const FAIDASnapshot Current = AIDASnapshotTools::MakeSnapshot(SnapshotAggregates(), Now, TEXT("now"));
			return FAIDAToolResult::Ok(AIDASnapshotTools::BuildCompareJson(*Baseline, Current, Item, Now));
		}
	});

	// Phase 4 proposal tools (docs/PHASE4.md). Gated by the actions.enabled kill-switch: when off,
	// the model never even sees propose_* in its tool list.
	if (Config.Actions.bEnabled)
	{
		RegisterActionTools();
	}

	UE_LOG(LogAIDA, Log, TEXT("[tools] registered %d tool(s)."), Tools.Num());
}

namespace
{
	/** How long a resolved (terminal) proposal lingers in the replicated view before retiring. */
	constexpr int64 kProposalLingerSeconds = 60;

	FString AIDACostSummaryString(const TArray<FAIDACostItem>& Items)
	{
		FString Out;
		for (const FAIDACostItem& Item : Items)
		{
			Out += FString::Printf(TEXT("%s%d %s"), Out.IsEmpty() ? TEXT("") : TEXT(", "), Item.Amount, *Item.Item);
		}
		return Out;
	}
}

void UAIDAOrchestrator::SweepProposals()
{
	const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
	for (const FGuid& Id : Actions.Store().SweepExpired(Now, Config.Actions.TtlSeconds))
	{
		UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s expired unapproved."), *Id.ToString(EGuidFormats::DigitsWithHyphens));
		PublishProposal(Id);
	}

	// Retire resolved proposals once their linger window closes: drop them from the store AND the view.
	for (const FAIDAProposal& Proposal : Actions.Store().All())
	{
		if (FAIDAProposalStore::IsTerminal(Proposal.State) &&
			Proposal.ResolvedUtc > 0 && Now - Proposal.ResolvedUtc > kProposalLingerSeconds)
		{
			Actions.Store().Remove(Proposal.Id);
			if (AAIDAProposalRelay* R = GetProposalRelay())
			{
				R->ServerRemoveProposal(Proposal.Id);
			}
		}
	}
}

void UAIDAOrchestrator::PublishProposal(const FGuid& ProposalId)
{
	AAIDAProposalRelay* R = GetProposalRelay();
	const FAIDAProposal* Proposal = Actions.Store().Find(ProposalId);
	if (!R || !Proposal)
	{
		return;
	}

	FAIDAProposalView View;
	View.Id = Proposal->Id;
	View.Requester = Proposal->RequesterName;
	View.Summary = Proposal->Summary;
	View.CostSummary = AIDACostSummaryString(Proposal->Cost);
	View.State = AIDAActionSpec::StateToString(Proposal->State);
	View.ExpiresUtc = Proposal->State == EAIDAProposalState::Pending
		? Proposal->ProposedUtc + Config.Actions.TtlSeconds : 0;
	R->ServerUpsertProposal(View);
}

void UAIDAOrchestrator::AnnounceSystem(const FString& Text)
{
	if (Session.IsValid() && !Text.IsEmpty())
	{
		Session->PostSystemMessage(Text, AIDADefaultConversationId());
	}
}

void UAIDAOrchestrator::HandleProposalDecision(const FAIDARequester& Requester, const FGuid& ProposalId, bool bApprove)
{
	// Authority-only, like HandleChatRequest — this mutates the world / replicated state.
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		return;
	}

	const TCHAR* Verb = bApprove ? TEXT("approve") : TEXT("reject");

	// Gate 1: the act tier ("may this player pull triggers at all").
	if (!Permissions.IsAllowed(EAIDATier::Act, Requester.PlayerId))
	{
		UE_LOG(LogAIDA, Warning, TEXT("[actions] %s (%s) may not %s proposals (act tier denied)."),
			*Requester.Author, *Requester.PlayerId, Verb);
		AnnounceSystem(FString::Printf(TEXT("%s isn't allowed to %s AIDA proposals (act permission)."), *Requester.Author, Verb));
		return;
	}

	// Gate 2: the approval policy ("who may decide THIS proposal").
	const FAIDAProposal* Proposal = Actions.Store().Find(ProposalId);
	const FString& Policy = Config.Actions.ApprovalPolicy;
	bool bPolicyOk = Policy == TEXT("any-act");
	if (Policy == TEXT("requester"))
	{
		bPolicyOk = Proposal && Proposal->RequesterId == Requester.PlayerId;
	}
	else if (Policy == TEXT("list"))
	{
		bPolicyOk = Config.Actions.ApprovalIds.Contains(Requester.PlayerId);
	}
	if (!bPolicyOk)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[actions] %s (%s) blocked by approvalPolicy '%s'."),
			*Requester.Author, *Requester.PlayerId, *Policy);
		AnnounceSystem(FString::Printf(TEXT("%s can't %s this proposal (approval policy: %s)."), *Requester.Author, Verb, *Policy));
		return;
	}

	FString Message;
	if (bApprove)
	{
		if (Actions.Approve(this, Config.Actions, ProposalId, Requester.PlayerId, Message))
		{
			StartActionTimer();
		}
	}
	else
	{
		Actions.Reject(ProposalId, Requester.PlayerId, Message);
	}
	PublishProposal(ProposalId);
	AnnounceSystem(FString::Printf(TEXT("%s: %s"), *Requester.Author, *Message));
	UE_LOG(LogAIDA, Log, TEXT("[actions] %s %s %s: %s"), *Requester.Author, Verb,
		*ProposalId.ToString(EGuidFormats::DigitsWithHyphens), *Message);
}

void UAIDAOrchestrator::RegisterActionTools()
{
	// propose_build: parse spec -> resolve recipe -> expand grid -> hologram dry-run -> store Pending.
	// NEVER executes (docs/PHASE4.md §1); execution needs a player approval (Slice 2+3).
	Tools.Register({
		TEXT("propose_build"),
		TEXT("PROPOSE placing buildables on a snapped grid (one buildable type, N x M repeat). Nothing is built until a player with act permission approves. Returns a dry-run report (count, cost, validity) and a proposalId. Spec: {version:1, buildable:'display name', origin?:{x,y,z in metres}, yawDeg:0|90|180|270, grid:{countX,countY,stepX?,stepY?}, followTerrain?:bool}. Grids are FLAT at the origin's height by default; set followTerrain:true ONLY if the player asks to follow/trace the ground. OMIT origin to build where the requesting player is aiming (falls back to their position) — never ask the player for coordinates. OMIT stepX/stepY — they default to the buildable's real footprint (a 'Foundation (2 m)' tile is 8x8 m; the 2 m is thickness); only set steps for deliberate gaps."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"The versioned build spec (see tool description)."}},"required":["spec"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			SweepProposals();

			const TSharedPtr<FJsonObject>* SpecObj = nullptr;
			Args->TryGetObjectField(TEXT("spec"), SpecObj);

			FAIDABuildSpec Spec;
			FString Error;
			if (!AIDAActionSpec::ParseBuildSpec(SpecObj ? *SpecObj : nullptr, Config.Actions.MaxProposalItems, Spec, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			FAIDARecipeResolution Recipe;
			if (!FAIDAActionSeam::ResolveBuildRecipe(this, Spec.Buildable, Recipe))
			{
				FString Msg = FString::Printf(TEXT("no unlocked buildable matches '%s'"), *Spec.Buildable);
				if (Recipe.Suggestions.Num() > 0)
				{
					Msg += FString::Printf(TEXT(" — closest: %s"), *FString::Join(Recipe.Suggestions, TEXT(", ")));
				}
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}
			Spec.Buildable = Recipe.DisplayName; // canonical name for the summary

			// No origin = "where the requesting player is aiming", SNAPPED like the build gun (extends
			// an aimed structure tile-perfectly / aligns to the world grid), falling back to their
			// position — players mean "build it THERE", and it keeps the grid off their feet.
			if (!Spec.bHasOrigin)
			{
				FVector AimCm;
				if (FAIDAActionSeam::ResolveAimSnappedOrigin(this, Ctx.PlayerId, Recipe.RecipeClassPath, /*InOut*/ Spec.YawDeg, AimCm))
				{
					Spec.OriginM = AimCm / 100.0; // world cm -> spec metres (yaw now carries the snapped orientation)
				}
				else if (Ctx.bHasLocation)
				{
					Spec.OriginM = Ctx.Location / 100.0;
				}
				else
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("couldn't resolve the requesting player's aim or position — pass an explicit 'origin' {x,y in metres}"), {}));
				}
				Spec.bHasOrigin = true;

				// Write the resolved origin back into the stored/journaled spec — the record (and any
				// later re-parse) must carry the concrete position, not "wherever the player is".
				const TSharedRef<FJsonObject> Origin = MakeShared<FJsonObject>();
				Origin->SetField(TEXT("x"), AIDANumber(Spec.OriginM.X));
				Origin->SetField(TEXT("y"), AIDANumber(Spec.OriginM.Y));
				Origin->SetField(TEXT("z"), AIDANumber(Spec.OriginM.Z));
				(*SpecObj)->SetObjectField(TEXT("origin"), Origin);
			}

			TArray<FTransform> Placements = AIDAActionSpec::ExpandGrid(Spec, Recipe.FootprintXM, Recipe.FootprintYM);

			// Grids are FLAT at the origin's height by default; followTerrain drops each tile to its
			// own ground (the "trace the ground" request) by adjusting placement Z here, up front —
			// the journal then records exactly what gets built.
			if (Spec.bFollowTerrain)
			{
				for (FTransform& Placement : Placements)
				{
					double GroundZ;
					if (FAIDAActionSeam::ProbeGroundZ(this, Placement.GetLocation(), GroundZ))
					{
						FVector Location = Placement.GetLocation();
						Location.Z = GroundZ;
						Placement.SetLocation(Location);
					}
				}
			}

			FAIDADryRunResult DryRun;
			if (!FAIDAActionSeam::DryRunBuild(this, Recipe.RecipeClassPath, Placements, DryRun))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(DryRun.Error, {}));
			}
			if (!DryRun.bOk)
			{
				const FString Msg = FString::Printf(TEXT("%d of %d placements invalid"), DryRun.Failures.Num(), Placements.Num());
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, DryRun.Failures));
			}
			if (Config.Actions.CostMode == TEXT("central") && !DryRun.bAffordable)
			{
				FString Msg = TEXT("not affordable from central storage (dimensional depot): needs ");
				for (int32 i = 0; i < DryRun.Cost.Num(); ++i)
				{
					Msg += FString::Printf(TEXT("%s%d %s"), i > 0 ? TEXT(", ") : TEXT(""), DryRun.Cost[i].Amount, *DryRun.Cost[i].Item);
				}
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}

			FAIDAProposal Proposal;
			Proposal.Id = FGuid::NewGuid(); // minted here so the report below carries the real id
			Proposal.SpecJson = AIDAToCompactJson(SpecObj->ToSharedRef());
			Proposal.RequesterId = Ctx.PlayerId;
			Proposal.RequesterName = Ctx.Author;
			Proposal.Placements = Placements;
			Proposal.RecipeClassPath = Recipe.RecipeClassPath;
			Proposal.Cost = DryRun.Cost;
			Proposal.Summary = AIDAActionSpec::SummarizeBuild(Spec);

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			if (!Actions.Store().Add(Proposal, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored: %s (by %s)"),
				*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal.Summary, *Ctx.Author);
			PublishProposal(Proposal.Id);
			AnnounceSystem(FString::Printf(TEXT("AIDA proposes (for %s): %s — cost %s. Awaiting approval."),
				*Ctx.Author, *Proposal.Summary, *AIDACostSummaryString(Proposal.Cost)));

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds, DryRun.bAffordable, 0.0));
		}
	});

	// propose_dismantle: resolve live targets (count + refund) and store Pending. Targets are
	// re-resolved at execute time (Slice 2) — never trusted from this dry-run.
	Tools.Register({
		TEXT("propose_dismantle"),
		TEXT("PROPOSE removing buildables near a point. Nothing is dismantled until a player with act permission approves. Returns the matched count + refund tally and a proposalId. Selector: {version:1, buildable:'display name or empty for any', center?:{x,y,z in metres}, radiusM, maxCount?}. OMIT center to search around the requesting player — never ask the player for coordinates."),
		TEXT(R"({"type":"object","properties":{"selector":{"type":"object","description":"The versioned dismantle selector (see tool description)."}},"required":["selector"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			SweepProposals();

			const TSharedPtr<FJsonObject>* SelectorObj = nullptr;
			Args->TryGetObjectField(TEXT("selector"), SelectorObj);

			FAIDADismantleSpec Selector;
			FString Error;
			if (!AIDAActionSpec::ParseDismantleSpec(SelectorObj ? *SelectorObj : nullptr, Config.Actions.MaxProposalItems, Selector, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			// No center = "around where the requesting player is aiming", falling back to their
			// position — mirroring propose_build's origin default.
			if (!Selector.bHasCenter)
			{
				FVector AimCm;
				if (FAIDAActionSeam::ResolveAimPoint(this, Ctx.PlayerId, AimCm))
				{
					Selector.CenterM = AimCm / 100.0; // world cm -> spec metres
				}
				else if (Ctx.bHasLocation)
				{
					Selector.CenterM = Ctx.Location / 100.0;
				}
				else
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("couldn't resolve the requesting player's aim or position — pass an explicit 'center' {x,y in metres}"), {}));
				}
				Selector.bHasCenter = true;

				// The engine RE-PARSES this stored selector at approval to re-resolve targets — it has
				// no player context there, so the concrete center must live in the stored JSON.
				const TSharedRef<FJsonObject> Center = MakeShared<FJsonObject>();
				Center->SetField(TEXT("x"), AIDANumber(Selector.CenterM.X));
				Center->SetField(TEXT("y"), AIDANumber(Selector.CenterM.Y));
				Center->SetField(TEXT("z"), AIDANumber(Selector.CenterM.Z));
				(*SelectorObj)->SetObjectField(TEXT("center"), Center);
			}

			FAIDADismantleResolution Targets;
			if (!FAIDAActionSeam::ResolveDismantleTargets(this, Selector, Targets))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(TEXT("dismantle target scan unavailable"), {}));
			}
			if (Targets.Count == 0)
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
					FString::Printf(TEXT("no%s buildables within %.0f m of the given point"),
						Selector.Buildable.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" '%s'"), *Selector.Buildable),
						Selector.RadiusM), {}));
			}

			FAIDAProposal Proposal;
			Proposal.Id = FGuid::NewGuid(); // minted here so the report below carries the real id
			Proposal.bDismantle = true;
			Proposal.SpecJson = AIDAToCompactJson(SelectorObj->ToSharedRef());
			Proposal.RequesterId = Ctx.PlayerId;
			Proposal.RequesterName = Ctx.Author;
			Proposal.TargetCount = Targets.Count;
			Proposal.Cost = Targets.Refund;
			Proposal.Summary = AIDAActionSpec::SummarizeDismantle(Selector);

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			if (!Actions.Store().Add(Proposal, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored: %s (by %s)"),
				*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal.Summary, *Ctx.Author);
			PublishProposal(Proposal.Id);
			AnnounceSystem(FString::Printf(TEXT("AIDA proposes (for %s): %s — refunds %s. Awaiting approval."),
				*Ctx.Author, *Proposal.Summary, *AIDACostSummaryString(Proposal.Cost)));

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds, /*bAffordable*/ true, 0.0));
		}
	});

	// get_proposal_status: read-only view over the live store, so the model never guesses outcomes.
	Tools.Register({
		TEXT("get_proposal_status"),
		TEXT("Status of AIDA's build/dismantle proposals (pending, approved, executed, rejected, expired). Pass 'proposalId' for one, or omit it to list all current proposals."),
		TEXT(R"({"type":"object","properties":{"proposalId":{"type":"string","description":"Optional proposal id to check."}}})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			SweepProposals();

			FString IdText;
			Args->TryGetStringField(TEXT("proposalId"), IdText);
			FGuid Filter;
			FGuid::Parse(IdText, Filter); // invalid text leaves the filter unset => list all

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			return FAIDAToolResult::Ok(AIDAActionSpec::BuildStatusJson(Actions.Store().All(), Filter, Now, Config.Actions.TtlSeconds));
		}
	});
}

void UAIDAOrchestrator::BuildToolDefs(TArray<FAIDAToolDef>& OutDefs) const
{
	TArray<const FAIDAToolSpec*> Specs;
	Tools.GetSpecs(Specs);
	OutDefs.Reset(Specs.Num());
	for (const FAIDAToolSpec* Spec : Specs)
	{
		OutDefs.Add({ Spec->Name, Spec->Description, Spec->ParametersJsonSchema });
	}
}

void UAIDAOrchestrator::RunToolLoop(TSharedRef<TArray<FAIDAChatMessage>, ESPMode::ThreadSafe> Messages, FAIDARequester Requester,
	int32 RoundsLeft, FAIDAOnChunk OnDelta, TFunction<void(const FString&)> OnDone, FAIDAOnError OnError)
{
	if (!LLMClient.IsValid() || !LLMClient->IsReady())
	{
		if (OnError) { OnError(0, TEXT("LLM client not ready")); }
		return;
	}

	TArray<FAIDAToolDef> ToolDefs;
	BuildToolDefs(ToolDefs);

	TWeakObjectPtr<UAIDAOrchestrator> Weak(this);
	LLMClient->CompleteChat(*Messages, ToolDefs, OnDelta,
		[Weak, Messages, Requester, RoundsLeft, OnDelta, OnDone, OnError](const FAIDACompletionResult& Result)
		{
			UAIDAOrchestrator* O = Weak.Get();
			if (!O)
			{
				return;
			}

			if (!Result.HasToolCalls() || RoundsLeft <= 0)
			{
				if (Result.HasToolCalls())
				{
					UE_LOG(LogAIDA, Warning, TEXT("[tools] round-trip cap reached with %d unhandled tool call(s); returning text."), Result.ToolCalls.Num());
				}
				if (OnDone) { OnDone(Result.Text); }
				return;
			}

			// Echo the model's assistant turn (text + its tool_use calls) back into the history.
			FAIDAChatMessage Assistant;
			Assistant.Role = TEXT("assistant");
			Assistant.Content = Result.Text;
			Assistant.ToolCalls = Result.ToolCalls;
			Messages->Add(Assistant);

			// Dispatch each call (permission-gated by the tool's declared tier), collecting one
			// tool_result per call into a single follow-up user turn.
			FAIDAChatMessage ToolTurn;
			ToolTurn.Role = TEXT("user");

			FAIDAToolContext Ctx;
			Ctx.Author = Requester.Author;
			Ctx.PlayerId = Requester.PlayerId;
			Ctx.bHasLocation = AIDAResolvePlayerLocation(O->GetWorld(), Requester.PlayerId, Ctx.Location);

			for (const FAIDAToolCall& Call : Result.ToolCalls)
			{
				FAIDAToolResultPart Part;
				Part.ToolCallId = Call.Id;

				const FAIDAToolSpec* Spec = O->Tools.Find(Call.Name);
				if (Spec && !O->Permissions.IsAllowed(ToPermissionTier(Spec->Tier), Requester.PlayerId))
				{
					Part.bIsError = true;
					Part.Content = FString::Printf(TEXT("Permission denied: you may not use the '%s' tool."), *Call.Name);
				}
				else
				{
					const FAIDAToolResult R = O->Tools.Dispatch(Call.Name, Call.ArgsJson, Ctx);
					Part.bIsError = R.bIsError;
					Part.Content = R.Content;
				}

				UE_LOG(LogAIDA, Log, TEXT("[tools] %s(%s) -> %s%s"),
					*Call.Name, *Call.ArgsJson, Part.bIsError ? TEXT("ERROR: ") : TEXT(""), *Part.Content);
				ToolTurn.ToolResults.Add(MoveTemp(Part));
			}
			Messages->Add(MoveTemp(ToolTurn));

			O->RunToolLoop(Messages, Requester, RoundsLeft - 1, OnDelta, OnDone, OnError);
		},
		OnError);
}

bool UAIDAOrchestrator::GetMessageBody(const FGuid& Id, FAIDATranscriptEntry& OutEntry) const
{
	return Session.IsValid() && Session->GetMessageBody(Id, OutEntry);
}

void UAIDAOrchestrator::GetRecentTranscript(TArray<FAIDATranscriptEntry>& OutEntries) const
{
	if (Session.IsValid())
	{
		Session->GetRecentTranscript(OutEntries);
	}
}

void UAIDAOrchestrator::RegisterConsoleCommands()
{
	if (PingCommand)
	{
		return; // already registered (OnWorldBeginPlay can fire per-world)
	}

	PingCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Ping"),
		TEXT("Send a one-shot test prompt to the configured LLM and log the reply. Usage: AIDA.Ping [prompt...]"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::Ping),
		ECVF_Default);

	SayCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Say"),
		TEXT("Inject a chat request through the full relay path (run on server/host). Usage: AIDA.Say [text...]"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::Say),
		ECVF_Default);

	ToolPingCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.ToolPing"),
		TEXT("Run the tool loop against the echo tool and log the result (verifies Phase 2 Slice 0). Usage: AIDA.ToolPing [prompt...]"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::ToolPing),
		ECVF_Default);

	IndexCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Index"),
		TEXT("Extract + aggregate the factory and log the overview JSON (checks Phase 2 extraction without the LLM). Run on server/host. Usage: AIDA.Index"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::Index),
		ECVF_Default);

	NodesCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Nodes"),
		TEXT("Scan resource nodes and log the grouped summary JSON (checks Phase 2 map scan without the LLM). Run on server/host. Usage: AIDA.Nodes"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::Nodes),
		ECVF_Default);

	RecipesCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Recipes"),
		TEXT("Log the static recipe + building catalog for a filter (checks Slice 3b lookup tools without the LLM). Run on server/host. Usage: AIDA.Recipes [item/name filter...]"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::Recipes),
		ECVF_Default);

	MemoryCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Memory"),
		TEXT("Log the memory session id + note/marker/journal counts + sidecar snapshot count (checks Phase 3 persistence). Run on server/host. Usage: AIDA.Memory"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::MemoryStatus),
		ECVF_Default);

	SnapshotCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Snapshot"),
		TEXT("Take a factory history snapshot now (checks Phase 3 snapshots without the LLM). Run on server/host. Usage: AIDA.Snapshot [label...]"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::Snapshot),
		ECVF_Default);

	ProposeCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Propose"),
		TEXT("Drive propose_build directly with a spec JSON (checks the Phase 4 dry-run without the LLM). Run on server/host. Usage: AIDA.Propose {\"version\":1,\"buildable\":\"Foundation\",...}"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::Propose),
		ECVF_Default);

	ApproveCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Approve"),
		TEXT("Approve a pending proposal and run the time-sliced executor (Slice 3's ProposalUI stand-in). Run on server/host. Usage: AIDA.Approve <proposalId>"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::ApproveCmd),
		ECVF_Default);

	RejectCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Reject"),
		TEXT("Reject a pending proposal. Run on server/host. Usage: AIDA.Reject <proposalId>"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::RejectCmd),
		ECVF_Default);

	UndoCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.Undo"),
		TEXT("Reverse the last n executed AIDA actions (console mirror of /aida undo). Run on server/host. Usage: AIDA.Undo [n]"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::UndoCmd),
		ECVF_Default);
}

void UAIDAOrchestrator::UndoCmd(const TArray<FString>& Args)
{
	int32 Count = 1;
	if (Args.Num() >= 1)
	{
		Count = FMath::Max(1, FCString::Atoi(*Args[0]));
	}

	FString Message;
	if (Actions.StartUndo(this, Config.Actions, Memory, Count, TEXT("console"), Message))
	{
		StartActionTimer();
	}
	AnnounceSystem(Message);
	UE_LOG(LogAIDA, Log, TEXT("AIDA.Undo: %s"), *Message);
}

void UAIDAOrchestrator::ApproveCmd(const TArray<FString>& Args)
{
	FGuid Id;
	if (Args.Num() < 1 || !FGuid::Parse(Args[0], Id))
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Approve: pass a proposal id (from AIDA.Propose / get_proposal_status)."));
		return;
	}

	// Console = host-side diagnostic; the RCO path (HandleProposalDecision) adds both approval gates.
	FString Message;
	if (Actions.Approve(this, Config.Actions, Id, TEXT("console"), Message))
	{
		StartActionTimer();
	}
	PublishProposal(Id);
	AnnounceSystem(FString::Printf(TEXT("console: %s"), *Message));
	UE_LOG(LogAIDA, Log, TEXT("AIDA.Approve: %s"), *Message);
}

void UAIDAOrchestrator::RejectCmd(const TArray<FString>& Args)
{
	FGuid Id;
	if (Args.Num() < 1 || !FGuid::Parse(Args[0], Id))
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Reject: pass a proposal id."));
		return;
	}
	FString Message;
	Actions.Reject(Id, TEXT("console"), Message);
	PublishProposal(Id);
	AnnounceSystem(FString::Printf(TEXT("console: %s"), *Message));
	UE_LOG(LogAIDA, Log, TEXT("AIDA.Reject: %s"), *Message);
}

void UAIDAOrchestrator::StartActionTimer()
{
	UWorld* World = GetWorld();
	if (!World || ActionTimer.IsValid())
	{
		return;
	}
	// 10 Hz while executing; OnActionTimer clears the handle when the engine runs dry.
	World->GetTimerManager().SetTimer(ActionTimer, this, &UAIDAOrchestrator::OnActionTimer, 0.1f, /*bLoop=*/true);
}

void UAIDAOrchestrator::OnActionTimer()
{
	const FGuid Executing = Actions.CurrentExecutingId(); // captured before Tick clears it on finish
	if (Actions.Tick(this, Config.Actions, Memory))
	{
		return; // more batches to go
	}

	// Execution finished (or died): stop slicing and drop the read caches so the next factory
	// query sees the new world immediately instead of a stale TTL snapshot.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ActionTimer);
	}
	ActionTimer.Invalidate();
	FactoryIndex.Invalidate();
	MapService.Invalidate(); // dismantles can free resource nodes (extractor occupancy)

	// Publish the outcome (executed/failed) + a System line so every client sees it land.
	if (const FAIDAProposal* Proposal = Actions.Store().Find(Executing))
	{
		PublishProposal(Executing);
		AnnounceSystem(FString::Printf(TEXT("AIDA %s: %s (%d entit%s affected)."),
			Proposal->State == EAIDAProposalState::Executed ? TEXT("finished") : TEXT("stopped"),
			*Proposal->Summary, Proposal->AffectedEntityIds.Num(),
			Proposal->AffectedEntityIds.Num() == 1 ? TEXT("y") : TEXT("ies")));
	}

	// A finished undo run reports per-entry results ("undid 97 of 100 …") the same way.
	for (const FString& Line : Actions.TakeUndoReport())
	{
		AnnounceSystem(Line);
	}
}

void UAIDAOrchestrator::Propose(const TArray<FString>& Args)
{
	if (!Config.Actions.bEnabled || !Tools.Contains(TEXT("propose_build")))
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Propose: actions are disabled (actions.enabled=false)."));
		return;
	}

	// The console splits on spaces; rejoin to recover the JSON spec.
	const FString SpecJson = FString::Join(Args, TEXT(" ")).TrimStartAndEnd();
	if (SpecJson.IsEmpty())
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Propose: pass a build spec, e.g. AIDA.Propose {\"version\":1,\"buildable\":\"Foundation\",\"origin\":{\"x\":0,\"y\":0},\"grid\":{\"countX\":3,\"countY\":3}}"));
		return;
	}

	FAIDAToolContext Ctx;
	Ctx.Author = TEXT("console");
	Ctx.PlayerId = TEXT("debug");

	// Dispatch through the registry exactly like the tool loop would (minus the permission gate —
	// this is a host-side diagnostic, like AIDA.Say).
	const FString ArgsJson = FString::Printf(TEXT("{\"spec\":%s}"), *SpecJson);
	const FAIDAToolResult Result = Tools.Dispatch(TEXT("propose_build"), ArgsJson, Ctx);
	UE_LOG(LogAIDA, Log, TEXT("AIDA.Propose %s: %s"), Result.bIsError ? TEXT("ERROR") : TEXT("ok"), *Result.Content);
}

void UAIDAOrchestrator::ToolPing(const TArray<FString>& Args)
{
	if (!LLMClient.IsValid() || !LLMClient->IsReady())
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.ToolPing: LLM client not ready (config not loaded, or provider not implemented)."));
		return;
	}

	const FString Prompt = Args.Num() > 0
		? FString::Join(Args, TEXT(" "))
		: TEXT("Call the echo tool with text \"pong\", then tell me exactly what it returned.");
	UE_LOG(LogAIDA, Log, TEXT("AIDA.ToolPing -> \"%s\""), *Prompt);

	const TSharedRef<TArray<FAIDAChatMessage>, ESPMode::ThreadSafe> Messages = MakeShared<TArray<FAIDAChatMessage>, ESPMode::ThreadSafe>();
	Messages->Add({ TEXT("user"), Prompt });

	FAIDARequester Requester;
	Requester.Author = TEXT("DebugPlayer");
	Requester.PlayerId = TEXT("debug");

	RunToolLoop(Messages, Requester, MaxToolRoundTrips,
		[](const FString& Delta)
		{
			UE_LOG(LogAIDA, Log, TEXT("AIDA delta: %s"), *Delta);
		},
		[](const FString& Text)
		{
			UE_LOG(LogAIDA, Log, TEXT("AIDA.ToolPing reply (complete): %s"), *Text);
		},
		[](int32 Status, const FString& Message)
		{
			UE_LOG(LogAIDA, Error, TEXT("AIDA.ToolPing failed (HTTP %d): %s"), Status, *Message);
		});
}

FAIDAFactoryAggregates UAIDAOrchestrator::SnapshotAggregates()
{
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const FAIDAFactorySnapshot& Snapshot = FactoryIndex.GetSnapshot(World, Now);
	return FAIDAFactoryAggregator::Aggregate(Snapshot);
}

void UAIDAOrchestrator::Index(const TArray<FString>& /*Args*/)
{
	// Extraction reads the authoritative buildable subsystem — server/host only.
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Index runs only on the server/host (this is a client)."));
		return;
	}

	const FString Overview = AIDAFactoryTools::BuildOverviewJson(SnapshotAggregates());
	UE_LOG(LogAIDA, Log, TEXT("AIDA.Index overview: %s"), *Overview);
}

void UAIDAOrchestrator::Nodes(const TArray<FString>& /*Args*/)
{
	// Node scan iterates world actors — server/host only.
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Nodes runs only on the server/host (this is a client)."));
		return;
	}

	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const FString Json = AIDAMapTools::BuildResourceNodesJson(MapService.GetNodes(World, Now), FString(), /*bUntappedOnly=*/false);
	UE_LOG(LogAIDA, Log, TEXT("AIDA.Nodes summary: %s"), *Json);
}

void UAIDAOrchestrator::Recipes(const TArray<FString>& Args)
{
	// Catalog reads the recipe manager (a world subsystem) — server/host only.
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Recipes runs only on the server/host (this is a client)."));
		return;
	}

	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	const FString Filter = FString::Join(Args, TEXT(" "));
	UE_LOG(LogAIDA, Log, TEXT("AIDA.Recipes recipes: %s"), *AIDARecipeTools::BuildRecipeJson(RecipeCatalog.GetRecipes(World, Now), Filter));
	UE_LOG(LogAIDA, Log, TEXT("AIDA.Recipes buildings: %s"), *AIDARecipeTools::BuildBuildingJson(RecipeCatalog.GetBuildings(World, Now), Filter));
}

void UAIDAOrchestrator::MemoryStatus(const TArray<FString>& /*Args*/)
{
	// The in-save store is server-authoritative.
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Memory runs only on the server/host (this is a client)."));
		return;
	}

	Memory.Init(this); // resolve the store if it wasn't ready at RegisterRelayClass time
	const AAIDAMemoryStore* MemStore = Memory.Store(this);
	if (!MemStore)
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Memory: in-save store not spawned yet."));
		return;
	}

	UE_LOG(LogAIDA, Log, TEXT("AIDA.Memory: session=%s notes=%d markers=%d journal=%d snapshots=%d"),
		*Memory.GetSessionId().ToString(EGuidFormats::DigitsWithHyphens),
		MemStore->Notes.Num(), MemStore->Markers.Num(), MemStore->Journal.Num(), Memory.LoadSnapshots().Num());
}

void UAIDAOrchestrator::TakeSnapshot(const FString& Label)
{
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client) { return; }
	Memory.Init(this); // ensure the sidecar is bound (no-op once ready)

	const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
	const FAIDASnapshot Snap = AIDASnapshotTools::MakeSnapshot(SnapshotAggregates(), Now, Label);
	Memory.AppendSnapshot(Snap, Config.Snapshots.Keep);
	UE_LOG(LogAIDA, Log, TEXT("[memory] snapshot taken (label='%s', %d items); %d total."),
		*Label, Snap.ItemBalance.Num(), Memory.LoadSnapshots().Num());
}

void UAIDAOrchestrator::OnSnapshotTimer()
{
	TakeSnapshot(TEXT("auto"));
}

void UAIDAOrchestrator::Snapshot(const TArray<FString>& Args)
{
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Snapshot runs only on the server/host (this is a client)."));
		return;
	}
	TakeSnapshot(Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : TEXT("manual"));
}

void UAIDAOrchestrator::Say(const TArray<FString>& Args)
{
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Say runs only on the server/host (this is a client)."));
		return;
	}

	FAIDARequester Requester;
	Requester.Author = TEXT("DebugPlayer");
	Requester.PlayerId = TEXT("debug");

	const FString Text = Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : TEXT("Hello from AIDA.Say");
	HandleChatRequest(Requester, Text, AIDADefaultConversationId());
}

void UAIDAOrchestrator::Ping(const TArray<FString>& Args)
{
	if (!LLMClient.IsValid() || !LLMClient->IsReady())
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.Ping: LLM client not ready (config not loaded, or provider not implemented)."));
		return;
	}

	const FString Prompt = Args.Num() > 0 ? FString::Join(Args, TEXT(" ")) : TEXT("Reply with a short, friendly hello.");
	UE_LOG(LogAIDA, Log, TEXT("AIDA.Ping -> \"%s\""), *Prompt);

	LLMClient->Complete(Prompt,
		[](const FString& Delta)
		{
			// One line per streamed delta so the incremental arrival is visible in the Output Log.
			UE_LOG(LogAIDA, Log, TEXT("AIDA delta: %s"), *Delta);
		},
		[](const FString& Text)
		{
			UE_LOG(LogAIDA, Log, TEXT("AIDA reply (complete): %s"), *Text);
		},
		[](int32 Status, const FString& Message)
		{
			UE_LOG(LogAIDA, Error, TEXT("AIDA.Ping failed (HTTP %d): %s"), Status, *Message);
		});
}

FString UAIDAOrchestrator::ResolveConfigPath(FString& OutSource) const
{
	// 1) Server admin override: <ProjectDir>/Configs/AIDA/config.jsonc
	const FString Override = FPaths::Combine(FPaths::ProjectDir(), TEXT("Configs"), TEXT("AIDA"), TEXT("config.jsonc"));
	if (FPaths::FileExists(Override))
	{
		OutSource = TEXT("admin override");
		return Override;
	}

	// 2) Fall back to the mod's shipped example: <PluginDir>/Configs/config.example.jsonc
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AIDA")))
	{
		OutSource = TEXT("shipped example");
		return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Configs"), TEXT("config.example.jsonc"));
	}

	OutSource = TEXT("unresolved");
	return Override;
}

void UAIDAOrchestrator::LoadConfig()
{
	FString Source;
	const FString Path = ResolveConfigPath(Source);

	FString Error;
	if (FAIDAConfigLoader::LoadFromFile(Path, Config, Error))
	{
		bConfigLoaded = true;
		// NEVER log the API key — only whether one is present.
		UE_LOG(LogAIDA, Log,
			TEXT("Config loaded (%s): provider=%s baseUrl=%s model=%s apiKey=%s"),
			*Source,
			*Config.Provider.Type, *Config.Provider.BaseUrl, *Config.Provider.Model,
			Config.Provider.ApiKey.IsEmpty() ? TEXT("<none>") : TEXT("<set:redacted>"));

		LLMClient = MakeShared<FLLMClient>(Config);
		LLMClient->SetSystemPrompt(GAIDASystemPrompt);
		RateLimiter.Configure(Config.Limits);
		Permissions.Configure(Config.Permissions);
		UE_LOG(LogAIDA, Log, TEXT("LLM client %s. Run console command 'AIDA.Ping' to test a round-trip."),
			LLMClient->IsReady() ? TEXT("ready") : TEXT("NOT ready (provider unimplemented)"));
	}
	else
	{
		bConfigLoaded = false;
		UE_LOG(LogAIDA, Warning,
			TEXT("Config not loaded (%s). AIDA will stay idle until configured. [tried: %s]"),
			*Error, *Path);
	}
}
