#include "Core/AIDAOrchestrator.h"

#include "AIDA.h"
#include "Core/AIDAConfigLoader.h"
#include "Core/AIDAImageValidation.h"
#include "Adapters/LLMClient.h"
#include "Factory/AIDAFactoryAggregator.h"
#include "Tools/AIDAFactoryTools.h"
#include "Tools/AIDAMapTools.h"
#include "Tools/AIDAPromptPack.h"
#include "Tools/AIDARecipeTools.h"
#include "Recipes/AIDAFactoryPlanner.h"
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
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/Pawn.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"

namespace
{
	/** Does this reply text invite the player to approve a proposal? (Fabrication tripwire input.) */
	bool AIDATextInvitesApproval(const FString& Text)
	{
		return Text.Contains(TEXT("/aida approve"), ESearchCase::IgnoreCase)
			|| Text.Contains(TEXT("awaiting approval"), ESearchCase::IgnoreCase)
			|| Text.Contains(TEXT("approve with"), ESearchCase::IgnoreCase)
			|| Text.Contains(TEXT("awaiting your approval"), ESearchCase::IgnoreCase);
	}

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
		"Chat lines arrive as 'PlayerName: message' — the prefix before the colon is the SPEAKING PLAYER'S "
		"NAME (their gamertag, which can be any word, including item-like words). It is never a machine, "
		"item, or place; never search the factory or notes for it, and address the player by it naturally.\n\n"
		"You have tools that read the player's ACTUAL factory and world. Always call the relevant tool to "
		"answer a question about the factory instead of guessing or asking the player for data you can look "
		"up yourself:\n"
		"- get_factory_overview(): machine clusters, the biggest item deficits, and power per circuit.\n"
		"- get_item_balance(item?): net production vs consumption per item; deficits first.\n"
		"- inspect_cluster(id): one cluster's building census, net inputs/outputs, and efficiency.\n"
		"- find_bottleneck(item): why an item's production is limited (starved input, power, or backed up).\n"
		"- get_clock_advice(): machines worth underclocking — current vs suggested clock and the MW each "
		"change saves. Use for 'where am I wasting energy / what should I underclock'.\n"
		"- find_disconnected(): logistics breaks — dangling splitters/mergers, belts/pipes ending in "
		"nothing, machines with open ports. Use for 'is anything disconnected / nothing is arriving'.\n"
		"- find_belt_mismatch(): slow belts/pipes sandwiched in faster paths or undersized for their "
		"producer. Use for 'slow belts mixed in / throughput is mysteriously low'.\n"
		"- get_container_contents(item?, radius_m?): storage containers and what they hold (per-item "
		"counts, nearest first). Use for 'what's in these containers / where is my X stored'.\n"
		"- get_resource_nodes(resource?, untapped_only?): map resource nodes by purity and occupancy.\n"
		"- lookup_recipe(item?): static recipe reference — inputs/outputs (amount + per-minute), craft time, "
		"and which building makes it. Use for 'how is X made / what does X need', not live factory data.\n"
		"- lookup_building(name?): static building reference — a production building's power draw (and, for "
		"generators, power output).\n"
		"- plan_factory(item, rate_per_min): computed production plan — machines per step, exact clocks, "
		"belt/pipe mark, power, raw inputs. ALWAYS use this instead of doing recipe arithmetic yourself "
		"when the player asks what it takes to make N/min of something.\n"
		"- tag_node(resource, purity?, label?): drop a labeled marker on the map at the nearest untapped "
		"node of a resource to the player. This WRITES a shared map marker — only use it when the player "
		"asks you to mark/tag/find-and-mark a node.\n"
		"- mark_location(x, y, label, ping?): drop a labeled map marker at ANY coordinates (metres, as "
		"other tools report them) plus a 3D attention ping there (visible through walls ~10 s). Use when "
		"the player asks you to mark/show/point out clusters, problem spots, containers, or anything you "
		"just found — never tell them to navigate by raw coordinates; mark and ping instead.\n"
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
		"LOCATION RULE (applies to EVERY tool with an optional origin/center/position): an OMITTED "
		"location always resolves server-side to where the requesting player is AIMING (falling back to "
		"where they stand). When the player says 'here', 'there', 'in front of me', 'these machines', or "
		"gives NO location at all, call the tool immediately WITHOUT coordinates. NEVER ask the player "
		"for coordinates and NEVER refuse a request for lack of a location — omitting the field IS the "
		"correct way to say 'where I'm looking'.\n\n"
		"World-modifying proposal tools (only registered when the server enables them):\n"
		"- propose_build(spec): propose placing buildables on a snapped grid. NOTHING is built until a "
		"player with act permission approves the proposal. Only call it when the player explicitly asks "
		"you to build/place something. Omit the spec's 'origin' to build where the player is aiming ('here', "
		"'there', 'at my position', or no location given) — you never need to ask for coordinates. On success, "
		"relay the returned summary + cost and say it awaits approval; on a tool error (invalid placement, "
		"cost, cap), revise the spec or explain the reason. Never claim something was built until "
		"get_proposal_status says executed.\n"
		"- propose_dismantle(selector): same flow for removing buildables near a point.\n"
		"- propose_power(spec): wire EXISTING machines to electricity — poles beside them, power lines, "
		"and a tie to the nearest powered grid. Use when the player asks to power/wire up machines that "
		"are already built ('connect to power', 'wire it up'). Omit spec.center to wire what they're "
		"looking at; already-powered machines are skipped. NEVER tell the player machines can't be "
		"located — the tool finds them server-side.\n"
		"- propose_label_containers(spec): propose one sign per storage container, its text set to the "
		"container's main item. Use when the player asks to label containers/boxes. Omit spec.center to "
		"label the containers they're looking at; empty or already-labeled containers are skipped.\n"
		"- propose_manifold(spec): propose the belt-plumbing for a row of machines — one splitter (or "
		"merger) per machine plus the connecting belts, or the pipe-junction + pipe equivalent. Use it "
		"whenever the player asks to belt up / feed / connect machines; do NOT build splitters and belts "
		"one-by-one with propose_build, and do NOT detour through clusters or the factory overview — the "
		"tool selects machines by display name near a point, not by cluster. direction 'in' feeds machine "
		"inputs, 'out' collects outputs; 'both ends' or 'inputs and outputs' means TWO calls, one 'in' and "
		"one 'out'. Omit machines.center to use the machines the player is looking at ('these assemblers', "
		"'this row') — that is almost always right; only ask when you truly cannot tell which machine TYPE "
		"they mean. Machines already connected on that port are skipped automatically. Every port on a "
		"machine side gets its OWN row distance automatically (pipes hug the machines, belt rows sit "
		"further out, a second belt input gets the next row) — when machines need several manifolds "
		"(refineries: belt + pipe, both sides), just propose each one; any order works, rows won't "
		"collide.\n"
		"- get_proposal_status(proposalId?): check whether proposals were approved/executed/expired.\n"
		"You CANNOT undo actions yourself — when a player wants something reversed, tell them to type "
		"/aida undo (or /aida undo N) in this chat. Players decide proposals with the on-screen card or "
		"/aida approve, /aida reject — a pending proposal shows a live hologram ghost of the whole build, "
		"and players can move it with /aida nudge <north|south|east|west|up|down> [metres] and "
		"/aida rotate [degrees] before approving (manifold proposals are anchored to their machines "
		"and cannot be nudged).\n"
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
	if (RecipesCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(RecipesCommand);
		RecipesCommand = nullptr;
	}
	if (DumpPackCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(DumpPackCommand);
		DumpPackCommand = nullptr;
	}
	if (MemoryCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(MemoryCommand);
		MemoryCommand = nullptr;
	}
	if (SnapshotCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(SnapshotCommand);
		SnapshotCommand = nullptr;
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
	if (PostLoginHandle.IsValid())
	{
		FGameModeEvents::GameModePostLoginEvent.Remove(PostLoginHandle);
		PostLoginHandle.Reset();
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

		// P7 Slice 0 polish: a snapshot anchored at each player join, so "what changed since I last
		// logged in" compares against a baseline taken exactly then (labels sort it out from 'auto').
		if (!PostLoginHandle.IsValid())
		{
			PostLoginHandle = FGameModeEvents::GameModePostLoginEvent.AddUObject(this, &UAIDAOrchestrator::OnPlayerPostLogin);
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
	HandleChatRequest(Requester, Text, ConversationId, TArray<FGuid>());
}

void UAIDAOrchestrator::HandleChatRequest(const FAIDARequester& Requester, const FString& Text, const FGuid& ConversationId,
	const TArray<FGuid>& ImageIds)
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
	if (Trimmed.IsEmpty() && ImageIds.Num() == 0)
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

		// Approve/Reject/Nudge/Rotate act on a pending proposal (newest when no id is given).
		if (Command.Kind == FAIDAChatCommand::EKind::Approve || Command.Kind == FAIDAChatCommand::EKind::Reject ||
			Command.Kind == FAIDAChatCommand::EKind::Nudge || Command.Kind == FAIDAChatCommand::EKind::Rotate)
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

			if (Command.Kind == FAIDAChatCommand::EKind::Nudge || Command.Kind == FAIDAChatCommand::EKind::Rotate)
			{
				const FVector DeltaCm = Command.Kind == FAIDAChatCommand::EKind::Nudge
					? Command.NudgeDir * Command.NudgeDistM * 100.0 : FVector::ZeroVector;
				const int32 YawDelta = Command.Kind == FAIDAChatCommand::EKind::Rotate ? Command.RotateDeg : 0;
				HandleProposalAdjust(Requester, DeltaCm, YawDelta);
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

	// Phase 5: bind surviving attachments to this message. Ids must be live and owned by the
	// requester (MarkReferenced enforces both); referenced images outlast the upload TTL.
	TArray<FGuid> ValidImageIds;
	if (ImageIds.Num() > 0 && Config.Uploads.bEnabled)
	{
		for (const FGuid& Id : ImageIds)
		{
			if (ValidImageIds.Num() >= Config.Uploads.MaxImagesPerMessage)
			{
				break;
			}
			if (ImageStore.MarkReferenced(Id, Requester.PlayerId))
			{
				ValidImageIds.Add(Id);
			}
		}
		if (ValidImageIds.Num() < ImageIds.Num())
		{
			Session->PostSystemMessage(FString::Printf(TEXT("%d attachment(s) were dropped (expired or over the per-message limit)."),
				ImageIds.Num() - ValidImageIds.Num()), ConversationId);
		}
	}

	Session->PostPlayerMessage(Requester.Author, Trimmed, ConversationId, ValidImageIds);

	StartAIDAReply(Requester, ConversationId);
}

bool UAIDAOrchestrator::HandleImageUploadBegin(const FAIDARequester& Requester, const FString& MediaType,
	int32 TotalBytes, int32 ChunkCount, FString& OutError)
{
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		OutError = TEXT("not authority");
		return false;
	}
	if (!AreUploadsEnabled())
	{
		OutError = TEXT("image uploads are disabled on this server (uploads.enabled)");
		return false;
	}

	const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
	ImageUploads.Sweep(NowUtc); // lazy sweeps piggyback on upload traffic (proposal-store pattern)
	ImageStore.Sweep(NowUtc);

	return ImageUploads.Begin(Requester.PlayerId, MediaType, TotalBytes, ChunkCount,
		Config.Uploads.MaxImageBytes, NowUtc, OutError);
}

bool UAIDAOrchestrator::HandleImageUploadChunk(const FAIDARequester& Requester, int32 Seq,
	const TArray<uint8>& Data, FString& OutError)
{
	if (!AreUploadsEnabled())
	{
		OutError = TEXT("image uploads are disabled on this server (uploads.enabled)");
		return false;
	}
	return ImageUploads.AddChunk(Requester.PlayerId, Seq, Data, FDateTime::UtcNow().ToUnixTimestamp(), OutError);
}

bool UAIDAOrchestrator::HandleImageUploadCommit(const FAIDARequester& Requester, uint32 Crc32,
	FGuid& OutImageId, FString& OutError)
{
	if (!AreUploadsEnabled())
	{
		OutError = TEXT("image uploads are disabled on this server (uploads.enabled)");
		return false;
	}

	TArray<uint8> Bytes;
	FString ClaimedType;
	if (!ImageUploads.Commit(Requester.PlayerId, Crc32, Bytes, ClaimedType, OutError))
	{
		return false;
	}

	// Never trust the client's media type — the bytes must decode as a real, normalized image.
	FString MediaType;
	if (!AIDAValidateImageBytes(Bytes, Config.Uploads.MaxDimension, MediaType, OutError))
	{
		return false;
	}

	const FGuid Id = ImageStore.Add(MediaType, Bytes, Requester.PlayerId,
		FDateTime::UtcNow().ToUnixTimestamp(), OutError);
	if (!Id.IsValid())
	{
		return false;
	}

	UE_LOG(LogAIDA, Log, TEXT("[uploads] %s [%s] stored %d bytes (%s) as %s — store now %d image(s), %lld bytes."),
		*Requester.Author, *Requester.PlayerId, Bytes.Num(), *MediaType,
		*Id.ToString(EGuidFormats::DigitsWithHyphens), ImageStore.Num(), ImageStore.TotalBytes());
	OutImageId = Id;
	return true;
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

	// Player turns that carried attachments: (index into OutMessages, ids) for the second pass below.
	TArray<TPair<int32, TArray<FGuid>>> TurnsWithImages;

	for (int32 i = Start; i < Transcript.Num(); ++i)
	{
		const FAIDATranscriptEntry& Entry = Transcript[i];
		if (Entry.Body.IsEmpty() && Entry.ImageIds.Num() == 0)
		{
			continue; // skip an in-progress (empty) message; image-only player turns stay in
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
		const int32 Index = OutMessages.Add(MoveTemp(Msg));
		if (Entry.Header.Kind == EAIDAMsgKind::Player && Entry.ImageIds.Num() > 0)
		{
			TurnsWithImages.Emplace(Index, Entry.ImageIds);
		}
	}

	// Phase 5: attach stored images newest-first under the per-request budget; anything older,
	// expired, or evicted degrades to a text note so the model never hallucinates a lost image.
	int32 ImageBudget = FMath::Max(1, Config.Uploads.MaxImagesPerRequest);
	for (int32 t = TurnsWithImages.Num() - 1; t >= 0; --t)
	{
		FAIDAChatMessage& Msg = OutMessages[TurnsWithImages[t].Key];
		int32 Unavailable = 0;
		for (const FGuid& Id : TurnsWithImages[t].Value)
		{
			const FAIDAStoredImage* Img = ImageBudget > 0 ? ImageStore.Find(Id) : nullptr;
			if (Img)
			{
				FAIDAImagePart Part;
				Part.MediaType = Img->MediaType;
				Part.Base64Data = Img->Base64Data;
				Msg.Images.Add(MoveTemp(Part));
				--ImageBudget;
			}
			else
			{
				++Unavailable;
			}
		}
		if (Unavailable > 0)
		{
			Msg.Content += FString::Printf(TEXT(" [%d attached image(s) no longer available]"), Unavailable);
		}
	}
}

void UAIDAOrchestrator::StartAIDAReply(const FAIDARequester& Requester, const FGuid& ConversationId)
{
	if (!LLMClient.IsValid() || !LLMClient->IsReady())
	{
		Session->PostSystemMessage(TEXT("AIDA is not configured (no LLM provider). Ask an admin to set up Configs/AIDA/config.jsonc."), ConversationId);
		return;
	}

	// Assemble the system prompt most-stable-first (docs/PROMPT.md §5, prompt-cache friendliness):
	// persona/rules, then the generated game data pack (rebuilt at most every PackTtlSeconds), and
	// the volatile per-request state line LAST so it never invalidates the stable prefix.
	FString SystemPrompt(GAIDASystemPrompt);
	if (Config.Prompts.bPackEnabled)
	{
		SystemPrompt += TEXT("\n\n") + GetPromptPack();
	}

	// Phase 5: reference-image guidance (stable text — keep it BEFORE the volatile state line).
	// Reconstruction protocol (docs/PHASE5-RECONSTRUCTION.md): committing to numbers in text BEFORE
	// the tool call is what turns "vibes about massing" into buildable geometry.
	if (Config.Uploads.bEnabled)
	{
		SystemPrompt += TEXT("\n\nPlayers may attach reference images (photos, sketches, screenshots of buildings) to their"
			" messages. When a message includes an image, reconstruct it with this PROTOCOL, in order:"
			"\n1) SCALE: photos carry no scale bar — estimate the real building's size from human-scale cues"
			" (a door ~2 m, a person ~1.8 m, a deck chair ~1.5 m, one photographed storey ~3-4 m, a mature tree ~15 m)"
			" and state the overall bounding box as width x depth x height in metres."
			"\n2) PLAN IN TEXT FIRST: before any propose call, write a short dimensioned plan — storey count, then one"
			" line per part: buildable, size, at:{x,y,z} offset, and what it represents in the image (e.g. 'upper"
			" cantilevered terrace'). Only then translate that plan into the tool call."
			"\n3) SNAP to the game's modules: foundations are 8x8 m tiles (1/2/4 m thick), walls 8 m wide x 4 m tall"
			" (one storey = 4 m of z), so round every estimate to multiples of 1/2/4/8 m."
			"\n4) BUILD as ONE propose_build v2 composite. Placement conventions: a part's 'at' offset runs from the"
			" composite origin to the part's FIRST placement (grid row 0, col 0 — the actor pivot); grids expand +X"
			" then +Y and rotate with the composite yawDeg, so offsets and rows turn together; at.z is height above"
			" the origin — stacked storeys and cantilevered decks are separate parts at different z. On uneven ground,"
			" call probe_terrain first and set part z from the sampled heights. Use several proposals only when one"
			" composite can't express it, placing later ones relative to the resolved origin the tool result returns."
			"\n5) VERIFY: after execution, call get_proposal_status and compare each part's as-built position against"
			" your plan; if something is off, say so and propose a correcting composite."
			"\nIf the massing is ambiguous from one photo, ask the player for another angle (messages can carry several"
			" images) instead of guessing. Never claim to see an image unless one is actually attached to a message in"
			" this conversation.");
	}

	// Ground the model in the AUTHORITATIVE proposal state every request — it kept inventing queue
	// rules ("must wait for the previous proposal") and phantom proposals when it had to guess.
	if (Config.Actions.bEnabled)
	{
		SweepProposals();
		TArray<FString> Pending;
		for (const FAIDAProposal& Proposal : Actions.Store().All())
		{
			if (Proposal.State == EAIDAProposalState::Pending)
			{
				Pending.Add(FString::Printf(TEXT("%s (id %s)"), *Proposal.Summary,
					*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens)));
			}
		}
		FString StateLine = TEXT("\n\nLIVE PROPOSAL STATE (authoritative, refreshed each message): ");
		StateLine += Pending.Num() > 0
			? FString::Printf(TEXT("pending approval: %s."), *FString::Join(Pending, TEXT("; ")))
			: TEXT("no pending proposals.");
		if (Actions.IsExecuting())
		{
			StateLine += TEXT(" One approved proposal is executing right now.");
		}
		if (RecentProposalOutcomes.Num() > 0)
		{
			const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
			TArray<FString> Recent;
			for (int32 i = RecentProposalOutcomes.Num() - 1; i >= 0 && Recent.Num() < 5; --i)
			{
				const int64 AgeMin = FMath::Max<int64>(0, (NowUtc - RecentProposalOutcomes[i].Key) / 60);
				Recent.Add(FString::Printf(TEXT("%s (%lld min ago)"), *RecentProposalOutcomes[i].Value, AgeMin));
			}
			StateLine += FString::Printf(TEXT(" Recently resolved: %s."), *FString::Join(Recent, TEXT("; ")));
		}
		StateLine += TEXT(" THIS LIST IS THE ONLY TRUTH ABOUT THE QUEUE: anything not listed as pending here"
			" is NOT pending — never re-list earlier proposals from the conversation as a pending queue;"
			" they are in 'recently resolved' (or gone). Never claim you must wait for a previous proposal"
			" — just call the propose tool; if a limit applies, the tool error will say so.");
		SystemPrompt += StateLine;
	}
	LLMClient->SetSystemPrompt(SystemPrompt);

	// Build context BEFORE opening the AIDA message so the empty reply isn't included.
	TArray<FAIDAChatMessage> Context;
	BuildChatContext(ConversationId, Context);

	const FGuid MsgId = Session->BeginAIDAMessage(TEXT("AIDA"), ConversationId);
	const int64 ReplyStartUtc = FDateTime::UtcNow().ToUnixTimestamp();

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
		[Weak, MsgId, ConversationId, ReplyStartUtc](const FString& FullText)
		{
			if (UAIDAOrchestrator* O = Weak.Get(); O && O->Session.IsValid())
			{
				O->Session->CompleteMessage(MsgId);

				// Fabrication tripwire (live-verify: the model announced "approve with /aida approve"
				// after reading get_proposal_status, without ever calling propose_build). If the reply
				// invites approval but NOTHING is pending and nothing was proposed during this reply,
				// correct the record IMMEDIATELY — not when the player's approve bounces.
				if (O->Config.Actions.bEnabled && AIDATextInvitesApproval(FullText))
				{
					O->SweepProposals();
					bool bRealProposal = false;
					for (const FAIDAProposal& Proposal : O->Actions.Store().All())
					{
						if (Proposal.State == EAIDAProposalState::Pending || Proposal.ProposedUtc >= ReplyStartUtc)
						{
							bRealProposal = true;
							break;
						}
					}
					if (!bRealProposal)
					{
						UE_LOG(LogAIDA, Warning, TEXT("[actions] reply invited approval but created no proposal — posting correction."));
						O->Session->PostSystemMessage(
							TEXT("(Heads up: that reply mentions approving a proposal, but none was actually created — real proposals always get an 'AIDA proposes …' line. Tell AIDA to 'propose it' to get a real one.)"),
							ConversationId);
					}
				}
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

	// P7 Slice 1: logistics diagnostics over the Slice 0 graph.
	Tools.Register({
		TEXT("find_disconnected"),
		TEXT("Find logistics breaks: splitters/mergers with a whole side unconnected, belts/pipes whose far end attaches to nothing, and machines with open ports. Use for 'is anything disconnected / why is nothing arriving'. Locations are in metres."),
		TEXT(""),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& /*Args*/, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			const FAIDAFactorySnapshot& Snapshot = FactoryIndex.GetSnapshot(World, Now);
			return FAIDAToolResult::Ok(AIDAFactoryTools::BuildDisconnectedJson(FAIDAFactoryAggregator::FindDisconnected(Snapshot)));
		}
	});

	Tools.Register({
		TEXT("find_belt_mismatch"),
		TEXT("Find slow links: belts/pipes slower than both their neighbors on a path (they throttle it) or slower than the machine feeding them. Use for 'slow belts mixed with fast belts / why is throughput low'. Biggest choke first."),
		TEXT(""),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& /*Args*/, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			const FAIDAFactorySnapshot& Snapshot = FactoryIndex.GetSnapshot(World, Now);
			return FAIDAToolResult::Ok(AIDAFactoryTools::BuildBeltMismatchJson(FAIDAFactoryAggregator::FindBeltMismatch(Snapshot)));
		}
	});

	// P7 Slice 1: underclock advisor — pure math over the cached snapshot.
	Tools.Register({
		TEXT("get_clock_advice"),
		TEXT("Find machines worth underclocking: producers that idle enough that a lower clock saves power. Returns per-machine current vs suggested clock and the MW saved, biggest saving first. Use for 'where am I wasting energy / where should I underclock'."),
		TEXT(""),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& /*Args*/, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			const FAIDAFactorySnapshot& Snapshot = FactoryIndex.GetSnapshot(World, Now);
			return FAIDAToolResult::Ok(AIDAFactoryTools::BuildClockAdviceJson(FAIDAFactoryAggregator::BuildClockAdvice(Snapshot.Machines)));
		}
	});

	// P7 Slice 3 read side: storage container contents.
	Tools.Register({
		TEXT("get_container_contents"),
		TEXT("List storage containers and what they hold (per-item counts, slots used). Optional 'item' narrows to containers holding it; optional 'radius_m' narrows to containers within that distance of the player. Nearest containers first."),
		TEXT(R"({"type":"object","properties":{"item":{"type":"string","description":"Optional item name (substring) the container must hold (e.g. \"Iron Plate\")."},"radius_m":{"type":"number","description":"Optional radius in metres around the player."}}})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			FString Item;
			Args->TryGetStringField(TEXT("item"), Item);
			double RadiusM = 0.0;
			Args->TryGetNumberField(TEXT("radius_m"), RadiusM);

			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			const FAIDAFactorySnapshot& Snapshot = FactoryIndex.GetSnapshot(World, Now);
			return FAIDAToolResult::Ok(AIDAFactoryTools::BuildContainerContentsJson(
				Snapshot.Containers, Item, Ctx.Location, Ctx.bHasLocation, RadiusM));
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

	// P5 reconstruction aid (docs/PHASE5-RECONSTRUCTION.md): ground-height sampling so composite part
	// z offsets can follow the real terrain instead of assuming a flat plane.
	Tools.Register({
		TEXT("probe_terrain"),
		TEXT("Sample ground heights on a square grid around a point. Use BEFORE proposing multi-part builds on uneven ground (slopes, cliffs, river banks) so part z offsets can follow the terrain. Omit x/y to probe around the requesting player's aim (falling back to their position). Returns ground z in metres — row 0 is the grid's north edge (-Y), columns run west->east (+X) — plus min/max/spread."),
		TEXT(R"({"type":"object","properties":{"x":{"type":"number","description":"Optional center X in metres; omit to use the player's aim."},"y":{"type":"number","description":"Optional center Y in metres."},"radius_m":{"type":"number","description":"Half-width of the sampled square in metres (default 32, max 128)."},"step_m":{"type":"number","description":"Sample spacing in metres (default 8; coarsened automatically if the grid would exceed 33x33)."}}})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			double XM = 0.0, YM = 0.0;
			const bool bHasX = Args->TryGetNumberField(TEXT("x"), XM);
			const bool bHasY = Args->TryGetNumberField(TEXT("y"), YM);

			FVector CenterCm = FVector::ZeroVector;
			FVector AimCm = FVector::ZeroVector;
			if (bHasX && bHasY)
			{
				// Probe start height: the requester's z when known — ProbeGroundZ retries from +50 m anyway.
				CenterCm = FVector(XM * AIDAMetersToCm, YM * AIDAMetersToCm, Ctx.bHasLocation ? Ctx.Location.Z : 0.0);
			}
			else if (FAIDAActionSeam::ResolveAimPoint(this, Ctx.PlayerId, AimCm))
			{
				CenterCm = AimCm;
			}
			else if (Ctx.bHasLocation)
			{
				CenterCm = Ctx.Location;
			}
			else
			{
				return FAIDAToolResult::Error(TEXT("probe_terrain needs 'x' and 'y' — the requesting player's aim/position could not be resolved."));
			}

			double RadiusM = 32.0;
			Args->TryGetNumberField(TEXT("radius_m"), RadiusM);
			RadiusM = FMath::Clamp(RadiusM, 4.0, 128.0);
			double StepM = 8.0;
			Args->TryGetNumberField(TEXT("step_m"), StepM);
			StepM = FMath::Clamp(StepM, 1.0, RadiusM);

			// Cap the trace count: a finer step silently coarsens instead of firing thousands of traces.
			constexpr int32 MaxPerAxis = 33;
			int32 PerAxis = FMath::FloorToInt32(2.0 * RadiusM / StepM) + 1;
			if (PerAxis > MaxPerAxis)
			{
				PerAxis = MaxPerAxis;
				StepM = 2.0 * RadiusM / (MaxPerAxis - 1);
			}

			TArray<double> HeightsM;
			HeightsM.Reserve(PerAxis * PerAxis);
			for (int32 Row = 0; Row < PerAxis; ++Row)          // row 0 = north edge (-Y)
			{
				for (int32 Col = 0; Col < PerAxis; ++Col)      // west -> east (+X)
				{
					const FVector AtCm(CenterCm.X + (Col * StepM - RadiusM) * AIDAMetersToCm,
						CenterCm.Y + (Row * StepM - RadiusM) * AIDAMetersToCm, CenterCm.Z);
					double GroundZCm = 0.0;
					HeightsM.Add(FAIDAActionSeam::ProbeGroundZ(this, AtCm, GroundZCm)
						? GroundZCm / AIDAMetersToCm : AIDAMapTools::AIDATerrainNoHit);
				}
			}
			return FAIDAToolResult::Ok(AIDAMapTools::BuildTerrainProbeJson(HeightsM, PerAxis, PerAxis,
				CenterCm.X / AIDAMetersToCm, CenterCm.Y / AIDAMetersToCm, StepM));
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

	// P7 polish (live-verify feedback): a general map stamp, so "mark the clusters" works — tag_node
	// only marks resource nodes, and coordinates alone don't help a player NAVIGATE anywhere.
	Tools.Register({
		TEXT("mark_location"),
		TEXT("Place a labeled marker on the map at any coordinates (a saved, shared map stamp) AND a 3D attention ping there (the middle-click marker: visible through walls for ~10 s). Use to mark machine clusters, problem spots (disconnected splitters, slow belts, underclock candidates), containers — anything with a known [x, y] — so the player can actually navigate to it instead of reading coordinates. Pass 'x' and 'y' in metres (as returned by the other tools) and a short 'label'; 'ping': false skips the 3D ping (e.g. when marking many far-away spots)."),
		TEXT(R"({"type":"object","properties":{"x":{"type":"number","description":"X in metres (tool-result convention)."},"y":{"type":"number","description":"Y in metres."},"label":{"type":"string","description":"Short marker label (e.g. \"Cluster 1: Aluminum Scrap\")."},"ping":{"type":"boolean","description":"Also spawn the 3D attention ping (default true)."}},"required":["x","y","label"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			double X = 0.0, Y = 0.0;
			FString Label;
			const bool bHasX = Args->TryGetNumberField(TEXT("x"), X);
			const bool bHasY = Args->TryGetNumberField(TEXT("y"), Y);
			Args->TryGetStringField(TEXT("label"), Label);
			if (!bHasX || !bHasY) { return FAIDAToolResult::Error(TEXT("mark_location needs numeric 'x' and 'y' (metres).")); }
			if (Label.TrimStartAndEnd().IsEmpty()) { return FAIDAToolResult::Error(TEXT("mark_location needs a 'label'.")); }

			// The 2D map only cares about X/Y; borrow the requester's Z so the stamp isn't underground.
			const FVector LocationCm(X * 100.0, Y * 100.0, Ctx.bHasLocation ? Ctx.Location.Z : 0.0);
			if (!FAIDAMapService::PlaceMapMarker(GetWorld(), LocationCm, Label))
			{
				return FAIDAToolResult::Error(TEXT("Could not place the marker (map unavailable or marker limit reached)."));
			}

			bool bPing = true;
			Args->TryGetBoolField(TEXT("ping"), bPing);
			bool bPinged = false;
			if (bPing)
			{
				// The 3D ping wants to sit on/above the thing, not at the player's height across the
				// map — probe the ground at the spot (probe reach is local; the player-Z start is fine
				// for same-area marks, and a failed probe just keeps the borrowed height).
				FVector PingCm = LocationCm;
				double GroundZ = 0.0;
				if (FAIDAActionSeam::ProbeGroundZ(this, LocationCm, GroundZ))
				{
					PingCm.Z = GroundZ + 100.0;
				}
				bPinged = FAIDAMapService::SpawnAttentionPing(GetWorld(), Ctx.PlayerId, PingCm);
			}

			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetBoolField(TEXT("marked"), true);
			if (bPing) { Root->SetBoolField(TEXT("pinged"), bPinged); }
			Root->SetStringField(TEXT("label"), Label);
			TArray<TSharedPtr<FJsonValue>> At;
			At.Add(AIDANumber(X));
			At.Add(AIDANumber(Y));
			Root->SetArrayField(TEXT("location_m"), At);
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

	// P7 Slice 5 (math half): deterministic production planner over the unlocked recipe catalog.
	Tools.Register({
		TEXT("plan_factory"),
		TEXT("Plan production of an item at a target rate: per-step machine counts, exact clocks, belt/pipe mark per edge, power, and raw-resource needs, from the unlocked recipes. Use for 'how many machines/what do I need for N per minute of X' — the numbers are computed, not estimated. Then relay the plan; building it is a separate propose_build step."),
		TEXT(R"({"type":"object","properties":{"item":{"type":"string","description":"Item to produce (e.g. \"Heavy Modular Frame\")."},"rate_per_min":{"type":"number","description":"Target output rate in items per minute (fluids: m3 per minute)."}},"required":["item","rate_per_min"]})"),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			FString Item;
			Args->TryGetStringField(TEXT("item"), Item);
			double Rate = 0.0;
			Args->TryGetNumberField(TEXT("rate_per_min"), Rate);

			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			const FAIDAFactoryPlan Plan = FAIDAFactoryPlanner::Plan(Item, Rate,
				RecipeCatalog.GetRecipes(World, Now), RecipeCatalog.GetBuildings(World, Now));
			return FAIDAToolResult::Ok(FAIDAFactoryPlanner::BuildPlanJson(Plan));
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

	// Terminal transitions feed the prompt's LIVE PROPOSAL STATE "recently resolved" list — the
	// model must never re-report an executed/expired proposal as pending (live-verify: it recited
	// a stale 5-deep 'pending' queue from conversation memory after everything had built).
	if (Proposal->State == EAIDAProposalState::Executed || Proposal->State == EAIDAProposalState::Failed
		|| Proposal->State == EAIDAProposalState::Rejected || Proposal->State == EAIDAProposalState::Expired
		|| Proposal->State == EAIDAProposalState::Undone)
	{
		FString Outcome = FString::Printf(TEXT("%s: %s"), *AIDAActionSpec::StateToString(Proposal->State), *Proposal->Summary);
		// Executed builds carry their resolved origin so the model can verify/extend the structure
		// without guessing where it landed (per-part positions live in get_proposal_status's asBuilt).
		if (Proposal->State == EAIDAProposalState::Executed && !Proposal->bDismantle && !Proposal->bManifold
			&& !Proposal->bLabel && !Proposal->bPowerOnly && Proposal->Placements.Num() > 0)
		{
			const FVector OriginM = Proposal->Placements[0].GetLocation() / AIDAMetersToCm;
			Outcome += FString::Printf(TEXT(" [as-built origin (%.0f, %.0f, %.0f) m — get_proposal_status has per-part positions]"),
				OriginM.X, OriginM.Y, OriginM.Z);
		}
		RecentProposalOutcomes.Add({ FDateTime::UtcNow().ToUnixTimestamp(), MoveTemp(Outcome) });
		while (RecentProposalOutcomes.Num() > 8) { RecentProposalOutcomes.RemoveAt(0); }
	}

	FAIDAProposalView View;
	View.Id = Proposal->Id;
	View.Requester = Proposal->RequesterName;
	View.Summary = Proposal->Summary;
	View.CostSummary = AIDACostSummaryString(Proposal->Cost);
	View.State = AIDAActionSpec::StateToString(Proposal->State);
	View.ExpiresUtc = Proposal->State == EAIDAProposalState::Pending
		? Proposal->ProposedUtc + Config.Actions.TtlSeconds : 0;

	// Ghost-preview payload: pending build proposals ship their tiles so clients hologram them —
	// one part for v1 grids, one per part for spec-v2 composites (placements are grouped by part).
	if (Proposal->State == EAIDAProposalState::Pending && !Proposal->bDismantle)
	{
		const bool bComposite = Proposal->PlacementPartIndex.Num() == Proposal->Placements.Num()
			&& Proposal->PartRecipePaths.Num() > 0;
		FAIDAGhostPart* Current = nullptr;
		int32 CurrentPart = INDEX_NONE;
		for (int32 i = 0; i < Proposal->Placements.Num(); ++i)
		{
			const int32 Part = bComposite ? Proposal->PlacementPartIndex[i] : 0;
			if (!Current || Part != CurrentPart)
			{
				Current = &View.GhostParts.AddDefaulted_GetRef();
				Current->RecipeClassPath = bComposite && Proposal->PartRecipePaths.IsValidIndex(Part)
					? Proposal->PartRecipePaths[Part] : Proposal->RecipeClassPath;
				Current->YawDeg = static_cast<float>(Proposal->Placements[i].Rotator().Yaw);
				CurrentPart = Part;
			}
			Current->TileCenters.Add(Proposal->Placements[i].GetLocation());
		}
	}
	R->ServerUpsertProposal(View);
}

void UAIDAOrchestrator::AnnounceSystem(const FString& Text)
{
	if (Session.IsValid() && !Text.IsEmpty())
	{
		Session->PostSystemMessage(Text, AIDADefaultConversationId());
	}
}

void UAIDAOrchestrator::HandleProposalAdjust(const FAIDARequester& Requester, const FVector& DeltaCm, int32 YawDeltaDeg,
	bool bQuietSuccess)
{
	// Authority-only, like the other proposal paths.
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		return;
	}

	// Moving a proposed build is act-tier, like proposing/approving one.
	if (!Permissions.IsAllowed(EAIDATier::Act, Requester.PlayerId))
	{
		AnnounceSystem(FString::Printf(TEXT("%s isn't allowed to adjust AIDA proposals (act permission)."), *Requester.Author));
		return;
	}

	// The newest pending proposal is the adjustable one (matches the ghost the player is looking at).
	SweepProposals();
	FGuid Target;
	int64 NewestUtc = -1;
	for (const FAIDAProposal& Proposal : Actions.Store().All())
	{
		if (Proposal.State == EAIDAProposalState::Pending && Proposal.ProposedUtc > NewestUtc)
		{
			NewestUtc = Proposal.ProposedUtc;
			Target = Proposal.Id;
		}
	}
	if (!Target.IsValid())
	{
		AnnounceSystem(TEXT("No pending proposal to adjust."));
		return;
	}

	FString Message;
	const bool bAdjusted = Actions.AdjustPending(this, Config.Actions, Target, DeltaCm, YawDeltaDeg, Message);
	PublishProposal(Target); // moves every client's ghost
	if (!bAdjusted || !bQuietSuccess)
	{
		AnnounceSystem(Message);
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
		TEXT("PROPOSE placing buildables. Nothing is built until a player with act permission approves. Returns a dry-run report (count, cost, validity, the RESOLVED origin in metres) and a proposalId. TWO SPEC FORMS. v1 single grid: {version:1, buildable:'display name', origin?:{x,y,z metres}, yawDeg:0|90|180|270, grid:{countX,countY,stepX?,stepY?}, followTerrain?:bool}. v2 COMPOSITE — THE FORM FOR ANY MULTI-PART STRUCTURE (buildings, reference-image reconstructions): {version:2, origin?:{x,y,z}, yawDeg:0, parts:[{buildable:'display name', at:{x,y,z metres RELATIVE to the origin — z stacks upward}, yawDeg?:0, grid?:{countX,countY,stepX?,stepY?}}, ...]} — up to 32 parts place together, preview together, and get ONE approval; part offsets rotate with the composite yawDeg. OMIT origin to build where the requesting player is aiming (falls back to their position) — never ask the player for coordinates. OMIT stepX/stepY — they default to the buildable's real footprint (a 'Foundation (2 m)' tile is 8x8 m; the 2 m is THICKNESS — stack floors with at.z, e.g. next storey at z 4 on '4 m' walls). v1 grids are FLAT at the origin's height (followTerrain:true ONLY if the player asks to trace the ground). v1 machines that need power are wired AUTOMATICALLY (poles + lines + grid tie; power:false to skip; pole:'display name' to override) — v2 composites are NOT auto-wired: include poles as parts and wire later. Costs are paid from central storage (dimensional depot) FIRST and then the REQUESTING PLAYER'S INVENTORY — materials in the player's pockets count toward affordability; never tell a player to move items into storage first."),
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
			// Resolve every buildable up front — v1 = one resolution, v2 = one per part. Resolutions[0]
			// is the ANCHOR part: it drives aim snapping and stands in for the v1 single recipe.
			const bool bComposite = Spec.Parts.Num() > 0;
			TArray<FAIDARecipeResolution> Resolutions;
			if (bComposite)
			{
				for (int32 i = 0; i < Spec.Parts.Num(); ++i)
				{
					FAIDARecipeResolution PartRecipe;
					if (!FAIDAActionSeam::ResolveBuildRecipe(this, Spec.Parts[i].Buildable, PartRecipe))
					{
						FString Msg = FString::Printf(TEXT("parts[%d]: no unlocked buildable matches '%s'"), i, *Spec.Parts[i].Buildable);
						if (PartRecipe.Suggestions.Num() > 0)
						{
							Msg += FString::Printf(TEXT(" — closest: %s"), *FString::Join(PartRecipe.Suggestions, TEXT(", ")));
						}
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
					}
					Spec.Parts[i].Buildable = PartRecipe.DisplayName; // canonical name for the summary
					Resolutions.Add(MoveTemp(PartRecipe));
				}
			}
			else
			{
				FAIDARecipeResolution Single;
				if (!FAIDAActionSeam::ResolveBuildRecipe(this, Spec.Buildable, Single))
				{
					FString Msg = FString::Printf(TEXT("no unlocked buildable matches '%s'"), *Spec.Buildable);
					if (Single.Suggestions.Num() > 0)
					{
						Msg += FString::Printf(TEXT(" — closest: %s"), *FString::Join(Single.Suggestions, TEXT(", ")));
					}
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
				}
				Spec.Buildable = Single.DisplayName; // canonical name for the summary
				Resolutions.Add(MoveTemp(Single));
			}
			const FAIDARecipeResolution& Recipe = Resolutions[0];
			TArray<FString> PartRecipePaths;
			for (const FAIDARecipeResolution& R : Resolutions) { PartRecipePaths.Add(R.RecipeClassPath); }

			// No origin = "where the requesting player is aiming", SNAPPED like the build gun (extends
			// an aimed structure tile-perfectly / aligns to the world grid), falling back to their
			// position — players mean "build it THERE", and it keeps the grid off their feet.
			// Candidate origins, most-preferred first. Explicit origin = exactly one. Aim-derived
			// origins add the resolver's alternates (top-face aims: centered on the aim, then away
			// from the player) — a growth direction is a bet, and it can lose to whatever stands
			// that way while the aimed cell itself is fine (live-verify: tile 0 landed on the HUB,
			// then off the platform edge, and the model told the player the ground was "too uneven").
			// The dry-run, not the bet, picks the anchor.
			TArray<FVector> OriginCandidatesCm;
			const bool bAimDerivedOrigin = !Spec.bHasOrigin;
			if (bAimDerivedOrigin)
			{
				// Effective steps (the same footprint clamp ExpandGrid applies) drive the anchor math.
				// Composites snap by their ANCHOR part (parts[0]) — the rest ride its offsets.
				const FAIDAGridSpec& AnchorGrid = bComposite ? Spec.Parts[0].Grid : Spec.Grid;
				const double StepXCm = FMath::Max(AnchorGrid.StepXM, Recipe.FootprintXM) * 100.0;
				const double StepYCm = FMath::Max(AnchorGrid.StepYM, Recipe.FootprintYM) * 100.0;
				FVector AimCm;
				TArray<FVector> Alternates;
				if (FAIDAActionSeam::ResolveAimSnappedOrigin(this, Ctx.PlayerId, Recipe.RecipeClassPath, /*InOut*/ Spec.YawDeg,
					AnchorGrid.CountX, AnchorGrid.CountY, StepXCm, StepYCm, AimCm, &Alternates))
				{
					OriginCandidatesCm.Add(AimCm); // yaw now carries the snapped orientation
					OriginCandidatesCm.Append(Alternates);
				}
				else if (Ctx.bHasLocation)
				{
					OriginCandidatesCm.Add(Ctx.Location);
				}
				else
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("couldn't resolve the requesting player's aim or position — pass an explicit 'origin' {x,y in metres}"), {}));
				}
				Spec.bHasOrigin = true;
			}
			else
			{
				OriginCandidatesCm.Add(Spec.OriginM * AIDAMetersToCm);
			}

			// Grids are FLAT at the origin's height by default; followTerrain drops each tile to its
			// own ground (the "trace the ground" request) by adjusting placement Z up front — the
			// journal then records exactly what gets built.
			const auto ExpandAt = [&](const FVector& OriginCm, TArray<int32>& OutPartIndex)
			{
				Spec.OriginM = OriginCm / AIDAMetersToCm;
				OutPartIndex.Reset();
				if (bComposite)
				{
					TArray<FVector2D> Footprints;
					for (const FAIDARecipeResolution& R : Resolutions)
					{
						Footprints.Emplace(R.FootprintXM, R.FootprintYM);
					}
					return AIDAActionSpec::ExpandParts(Spec, Footprints, OutPartIndex);
				}
				TArray<FTransform> Expanded = AIDAActionSpec::ExpandGrid(Spec, Recipe.FootprintXM, Recipe.FootprintYM);
				if (Spec.bFollowTerrain)
				{
					for (FTransform& Placement : Expanded)
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
				return Expanded;
			};

			TArray<FTransform> Placements;
			TArray<int32> PlacementPartIndex;
			FAIDADryRunResult DryRun;
			TArray<FTransform> FallbackPlacements;   // first candidate that validates but clips
			TArray<int32> FallbackPartIndex;
			FAIDADryRunResult FallbackDryRun;
			FVector FallbackOriginCm = FVector::ZeroVector;
			FAIDADryRunResult FirstFailRun;          // the PREFERRED candidate's failures for the report
			int32 FirstFailTotal = 0;
			bool bPlaced = false;
			bool bHaveFallback = false;
			bool bHaveFirstFail = false;
			for (const FVector& OriginCm : OriginCandidatesCm)
			{
				TArray<int32> AttemptPartIndex;
				TArray<FTransform> Attempt = ExpandAt(OriginCm, AttemptPartIndex);
				const int32 AttemptTotal = Attempt.Num();
				FAIDADryRunResult AttemptRun;
				const bool bRan = bComposite
					? FAIDAActionSeam::DryRunBuildParts(this, PartRecipePaths, AttemptPartIndex, Attempt, AttemptRun, Ctx.PlayerId)
					: FAIDAActionSeam::DryRunBuild(this, Recipe.RecipeClassPath, Attempt, AttemptRun, Ctx.PlayerId);
				if (!bRan)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(AttemptRun.Error, {}));
				}
				if (AttemptRun.bOk && AttemptRun.ClippingCount == 0)
				{
					Placements = MoveTemp(Attempt);
					PlacementPartIndex = MoveTemp(AttemptPartIndex);
					DryRun = MoveTemp(AttemptRun);
					bPlaced = true; // Spec.OriginM holds this candidate (set in ExpandAt)
					break;
				}
				if (AttemptRun.bOk && !bHaveFallback)
				{
					FallbackPlacements = MoveTemp(Attempt);
					FallbackPartIndex = MoveTemp(AttemptPartIndex);
					FallbackDryRun = AttemptRun;
					FallbackOriginCm = OriginCm;
					bHaveFallback = true;
				}
				else if (!AttemptRun.bOk && !bHaveFirstFail)
				{
					FirstFailRun = MoveTemp(AttemptRun);
					FirstFailTotal = AttemptTotal;
					bHaveFirstFail = true;
				}
			}
			if (!bPlaced && bHaveFallback)
			{
				// Every candidate clips somewhere; the most-preferred valid one wins (clipping is
				// advisory — the player judges via the ghosts, or nudges).
				Placements = MoveTemp(FallbackPlacements);
				PlacementPartIndex = MoveTemp(FallbackPartIndex);
				DryRun = MoveTemp(FallbackDryRun);
				Spec.OriginM = FallbackOriginCm / AIDAMetersToCm;
				bPlaced = true;
			}
			if (!bPlaced)
			{
				const FString Msg = FString::Printf(TEXT("%d of %d placements invalid (every anchor tried)"),
					FirstFailRun.Failures.Num(), FirstFailTotal);
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, FirstFailRun.Failures));
			}

			// Write the CHOSEN origin back into the stored/journaled spec — the record (and any
			// later re-parse) must carry the concrete position, not "wherever the player was aiming".
			if (bAimDerivedOrigin)
			{
				const TSharedRef<FJsonObject> Origin = MakeShared<FJsonObject>();
				Origin->SetField(TEXT("x"), AIDANumber(Spec.OriginM.X));
				Origin->SetField(TEXT("y"), AIDANumber(Spec.OriginM.Y));
				Origin->SetField(TEXT("z"), AIDANumber(Spec.OriginM.Z));
				(*SpecObj)->SetObjectField(TEXT("origin"), Origin);
			}
			if (Config.Actions.CostMode == TEXT("central") && !DryRun.bAffordable)
			{
				FString Msg = TEXT("not affordable from central storage (dimensional depot) plus your inventory: needs ");
				for (int32 i = 0; i < DryRun.Cost.Num(); ++i)
				{
					Msg += FString::Printf(TEXT("%s%d %s"), i > 0 ? TEXT(", ") : TEXT(""), DryRun.Cost[i].Amount, *DryRun.Cost[i].Item);
				}
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}

			// Auto-power (docs/PHASE4-POWER.md): powered buildables get poles + power lines + a grid
			// tie by default. Failure to assemble the kit NEVER fails the build — it's reported and
			// the machines go up unwired.
			FAIDAActionSeam::FAIDAPowerInfo Power;
			FAIDAPowerPlan PowerPlan;
			FAIDADryRunResult PoleDryRun;
			FString PowerNote;
			bool bAutoPower = false;
			if (Spec.bPower && Placements.Num() > 0)
			{
				FString PowerError;
				if (FAIDAActionSeam::ResolveAutoPower(this, Recipe.RecipeClassPath, Spec.Pole, Power, PowerError))
				{
					const double EffStepXCm = FMath::Max(Spec.Grid.StepXM, Recipe.FootprintXM) * 100.0;
					const double EffStepYCm = FMath::Max(Spec.Grid.StepYM, Recipe.FootprintYM) * 100.0;
					PowerPlan = AIDAActionSpec::PlanPower(Spec.Grid.CountX, Spec.Grid.CountY,
						EffStepXCm, EffStepYCm, Spec.YawDeg, Placements[0].GetLocation(),
						FMath::Max(1, Power.PoleConnectionCap - 2));
					if (PowerPlan.Error.IsEmpty() && PowerPlan.Poles.Num() > 0)
					{
						for (FTransform& Pole : PowerPlan.Poles)
						{
							double GroundZ;
							if (FAIDAActionSeam::ProbeGroundZ(this, Pole.GetLocation(), GroundZ))
							{
								FVector Location = Pole.GetLocation();
								Location.Z = GroundZ;
								Pole.SetLocation(Location);
							}
						}
						// Pole dry-run: cost tally + validation. Hard-invalid poles are NOT fatal —
						// they get skipped at execute and their wires report loudly.
						FAIDAActionSeam::DryRunBuild(this, Power.PoleRecipePath, PowerPlan.Poles, PoleDryRun, Ctx.PlayerId);
						bAutoPower = true;
					}
				}
				else if (Power.bMachineNeedsPower)
				{
					PowerNote = FString::Printf(TEXT(" [no power kit: %s — machines left unwired]"), *PowerError);
				}
			}

			// Merged affordability (machines + poles) against central storage.
			TArray<FAIDACostItem> TotalCost = DryRun.Cost;
			if (bAutoPower)
			{
				for (const FAIDACostItem& Item : PoleDryRun.Cost)
				{
					bool bMerged = false;
					for (FAIDACostItem& Existing : TotalCost)
					{
						if (Existing.Item == Item.Item) { Existing.Amount += Item.Amount; bMerged = true; break; }
					}
					if (!bMerged) { TotalCost.Add(Item); }
				}
				if (Config.Actions.CostMode == TEXT("central") && !FAIDAActionSeam::CheckAffordable(this, TotalCost, Ctx.PlayerId))
				{
					FString Msg = TEXT("not affordable from central storage + your inventory with the power kit included: needs ");
					for (int32 i = 0; i < TotalCost.Num(); ++i)
					{
						Msg += FString::Printf(TEXT("%s%d %s"), i > 0 ? TEXT(", ") : TEXT(""), TotalCost[i].Amount, *TotalCost[i].Item);
					}
					Msg += TEXT(" — or retry with power:false");
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
				}
			}

			FAIDAProposal Proposal;
			Proposal.Id = FGuid::NewGuid(); // minted here so the report below carries the real id
			Proposal.SpecJson = AIDAToCompactJson(SpecObj->ToSharedRef());
			Proposal.RequesterId = Ctx.PlayerId;
			Proposal.RequesterName = Ctx.Author;
			Proposal.Placements = Placements;
			Proposal.RecipeClassPath = Recipe.RecipeClassPath;
			if (bComposite)
			{
				Proposal.PartRecipePaths = PartRecipePaths;
				Proposal.PlacementPartIndex = MoveTemp(PlacementPartIndex);
			}
			Proposal.Cost = MoveTemp(TotalCost);
			Proposal.Summary = AIDAActionSpec::SummarizeBuild(Spec);
			if (bAutoPower)
			{
				Proposal.bAutoPower = true;
				Proposal.PolePlacements = MoveTemp(PowerPlan.Poles);
				Proposal.PoleRecipePath = Power.PoleRecipePath;
				Proposal.PoleName = Power.PoleName;
				Proposal.WireRecipePath = Power.WireRecipePath;
				Proposal.MachineWires = MoveTemp(PowerPlan.MachineWires);
				Proposal.ChainWires = MoveTemp(PowerPlan.ChainWires);
				Proposal.Summary += FString::Printf(TEXT(" + %d x %s + wiring"),
					Proposal.PolePlacements.Num(), *Power.PoleName);
			}
			Proposal.Summary += PowerNote;

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

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds, DryRun.bAffordable, 0.0,
				&Spec.OriginM));
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

	// propose_manifold: resolve live machine ports -> pure row-fit plan -> attachment dry-run ->
	// store Pending (docs/PHASE4-MANIFOLDS.md). Runs (belts/pipes) build + charge at execute time.
	Tools.Register({
		TEXT("propose_manifold"),
		TEXT("PROPOSE the belt/pipe plumbing (a manifold) for a row of machines: one splitter or merger per machine on a straight trunk line in front of their ports, plus all connecting belt runs (or a pipe-junction + pipe equivalent). Nothing is built until a player with act permission approves. Spec: {version:1, kind:'belt'|'pipe', direction:'in' (feed inputs, splitters) | 'out' (collect outputs, mergers), transport:'belt or pipe display name', attachment?:'override display name', machines:{buildable:'machine display name', center?:{x,y in metres}, radiusM?:30, maxCount?:0=all}, standoffM?:4, port?:0}. OMIT machines.center to use the machines the requesting player is looking at. Machines whose matching port is already connected are skipped automatically. The machines must roughly face the same direction. Every port on a machine side gets its OWN row distance automatically (pipes hug the machines, belt rows further out, a second belt input the next row) — propose multiple manifolds in any order; the rows will not collide. Returns the attachment dry-run (cost, count) + run count; runs are charged as they build."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"The versioned manifold spec (see tool description)."}},"required":["spec"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			// Flushed stage markers: a hang inside this call must leave its last stage in the LOG
			// FILE, not a buffer (live-verify: the game froze during a propose with an empty log tail).
			const auto Stage = [](const FString& What)
			{
				UE_LOG(LogAIDA, Log, TEXT("[actions][mf] %s"), *What);
				GLog->Flush();
			};

			SweepProposals();

			const TSharedPtr<FJsonObject>* SpecObj = nullptr;
			Args->TryGetObjectField(TEXT("spec"), SpecObj);

			FAIDAManifoldSpec Spec;
			FString Error;
			if (!AIDAActionSpec::ParseManifoldSpec(SpecObj ? *SpecObj : nullptr, Config.Actions.MaxProposalItems, Spec, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			Stage(FString::Printf(TEXT("parsed kind=%s direction=%s transport=%s machines=%s"),
				Spec.bPipe ? TEXT("pipe") : TEXT("belt"), Spec.bOutput ? TEXT("out") : TEXT("in"),
				*Spec.Transport, *Spec.Machines.Buildable));

			// Transport (belt/pipe) and attachment (splitter/merger/junction) both resolve like any
			// buildable — unlocked recipes only, suggestions on a miss.
			FAIDARecipeResolution Transport;
			if (!FAIDAActionSeam::ResolveBuildRecipe(this, Spec.Transport, Transport))
			{
				FString Msg = FString::Printf(TEXT("no unlocked buildable matches transport '%s'"), *Spec.Transport);
				if (Transport.Suggestions.Num() > 0)
				{
					Msg += FString::Printf(TEXT(" — closest: %s"), *FString::Join(Transport.Suggestions, TEXT(", ")));
				}
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}
			// Default attachment names are TRIED IN ORDER — the game's display names drift ("Pipeline
			// Junction Cross" vs "Pipeline Junction"; live-verify: a miss with no suggestions read as
			// "you haven't unlocked junctions"). An explicit spec override is a single candidate.
			TArray<FString> AttachmentNames;
			if (!Spec.Attachment.IsEmpty())
			{
				AttachmentNames.Add(Spec.Attachment);
			}
			else if (Spec.bPipe)
			{
				AttachmentNames = { TEXT("Pipeline Junction Cross"), TEXT("Pipeline Junction"), TEXT("Junction") };
			}
			else
			{
				AttachmentNames.Add(Spec.bOutput ? TEXT("Conveyor Merger") : TEXT("Conveyor Splitter"));
			}
			FAIDARecipeResolution Attachment;
			FString AttachmentName;
			for (const FString& Candidate : AttachmentNames)
			{
				if (FAIDAActionSeam::ResolveBuildRecipe(this, Candidate, Attachment))
				{
					AttachmentName = Candidate;
					break;
				}
			}
			if (AttachmentName.IsEmpty())
			{
				FString Msg = FString::Printf(TEXT("no unlocked buildable matches attachment '%s' — the name may differ from the build menu, or it may be locked"),
					*FString::Join(AttachmentNames, TEXT("' / '")));
				if (Attachment.Suggestions.Num() > 0)
				{
					Msg += FString::Printf(TEXT("; closest: %s"), *FString::Join(Attachment.Suggestions, TEXT(", ")));
				}
				Msg += TEXT("; you can pass 'attachment' with the exact build-menu name");
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}

			// No center = "the machines the player is looking at", falling back to their position —
			// the same default as the other propose_* tools. Written back into the stored spec.
			if (!Spec.Machines.bHasCenter)
			{
				FVector AimCm;
				if (FAIDAActionSeam::ResolveAimPoint(this, Ctx.PlayerId, AimCm))
				{
					Spec.Machines.CenterM = AimCm / 100.0;
				}
				else if (Ctx.bHasLocation)
				{
					Spec.Machines.CenterM = Ctx.Location / 100.0;
				}
				else
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("couldn't resolve the requesting player's aim or position — pass an explicit 'machines.center' {x,y in metres}"), {}));
				}
				Spec.Machines.bHasCenter = true;

				const TSharedPtr<FJsonObject>* MachinesObj = nullptr;
				if ((*SpecObj)->TryGetObjectField(TEXT("machines"), MachinesObj))
				{
					const TSharedRef<FJsonObject> Center = MakeShared<FJsonObject>();
					Center->SetField(TEXT("x"), AIDANumber(Spec.Machines.CenterM.X));
					Center->SetField(TEXT("y"), AIDANumber(Spec.Machines.CenterM.Y));
					Center->SetField(TEXT("z"), AIDANumber(Spec.Machines.CenterM.Z));
					(*MachinesObj)->SetObjectField(TEXT("center"), Center);
				}
			}

			Stage(FString::Printf(TEXT("recipes+center resolved (attachment=%s center=%.0f,%.0f) — resolving ports"),
				*AttachmentName, Spec.Machines.CenterM.X, Spec.Machines.CenterM.Y));
			TArray<FAIDAManifoldPort> Ports;
			int32 SkippedConnected = 0;
			FString MachineName;
			FAIDAActionSeam::ResolveMachinePorts(this, Spec.Machines, Spec.bPipe, Spec.bOutput, Spec.PortIndex,
				Ports, SkippedConnected, MachineName);
			Stage(FString::Printf(TEXT("ports resolved: %d (skipped-connected %d) — planning"), Ports.Num(), SkippedConnected));
			if (Ports.Num() == 0)
			{
				const FString Msg = SkippedConnected > 0
					? FString::Printf(TEXT("all %d matching machine(s) already have that port connected — nothing to plumb"), SkippedConnected)
					: FString::Printf(TEXT("no '%s' with a free %s port within %.0f m of the given point"),
						*Spec.Machines.Buildable, Spec.bPipe ? TEXT("pipe") : (Spec.bOutput ? TEXT("output") : TEXT("input")),
						Spec.Machines.RadiusM);
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}
			if (!MachineName.IsEmpty()) { Spec.Machines.Buildable = MachineName; } // canonical for the summary

			// Pure row fit: sorted attachment transforms + the shared axis (docs/PHASE4-MANIFOLDS.md §3).
			TArray<FAIDAManifoldPortPoint> Points;
			Points.Reserve(Ports.Num());
			for (const FAIDAManifoldPort& Port : Ports)
			{
				Points.Add({ Port.PosCm, Port.NormalCm });
			}
			// DETERMINISTIC LANES (user rule: don't discover offsets by probing): every port on the
			// machine side owns a row — pipes hug the machines (inner lanes), belts sit outside, a
			// second belt input gets its own row further out. This manifold's base standoff is its
			// lane's distance; proposal ORDER between belt/pipe manifolds no longer matters.
			const double FootprintM = FMath::Max(Attachment.FootprintXM, Attachment.FootprintYM);
			int32 Lane = 0, RowsOnSide = 1;
			FAIDAActionSeam::ResolveManifoldLane(this, Spec.Machines, Spec.bPipe, Spec.bOutput, Spec.PortIndex, Lane, RowsOnSide);
			const double LaneWidthM = FMath::Max(4.0, FootprintM) + 1.0;
			const double BaseStandoffM = Spec.StandoffM + Lane * LaneWidthM;
			Stage(FString::Printf(TEXT("lane %d of %d on this side -> base standoff %.0f m"), Lane, RowsOnSide, BaseStandoffM));

			// Still step outward on trouble (safety net for hand-built clutter the lane math can't
			// know): a lane is dirty when placements hard-block, clip, or would bodily OVERLAP an
			// existing splitter/merger/junction (attachment clearance boxes are too small/soft for
			// the engine's clipping flags to catch that). Clipping stays ADVISORY (user rule) — when
			// no clean lane exists in range, the nearest valid non-overlapping lane wins (clips and
			// all), overlap only as the last resort, and the summary says so.
			FAIDAManifoldPlan Plan;
			FAIDADryRunResult DryRun;
			double UsedStandoffM = BaseStandoffM;
			FAIDAManifoldPlan NearestValidPlan;
			FAIDADryRunResult NearestValidDryRun;
			double NearestValidStandoffM = -1.0;
			bool bNearestValidOverlaps = false;
			bool bPlaced = false;
			bool bClips = false;
			bool bOverlaps = false;
			for (int32 Attempt = 0; Attempt < 4; ++Attempt)
			{
				UsedStandoffM = BaseStandoffM + Attempt * 3.0;
				Plan = AIDAActionSpec::PlanManifold(Points, Spec.bOutput, Spec.bPipe,
					UsedStandoffM, FootprintM, /*MaxRunM*/ 56.0);
				if (!Plan.Error.IsEmpty())
				{
					// Geometry errors (mixed facing, machines too close) don't improve with distance.
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Plan.Error, {}));
				}

				// Attachments stand on the floor under each trunk point; belts auto-route the height.
				for (FTransform& Placement : Plan.Attachments)
				{
					double GroundZ;
					if (FAIDAActionSeam::ProbeGroundZ(this, Placement.GetLocation(), GroundZ))
					{
						FVector Location = Placement.GetLocation();
						Location.Z = GroundZ;
						Placement.SetLocation(Location);
					}
				}
				Stage(FString::Printf(TEXT("planned %d attachment(s) at %.0f m (yaw=%d axis=%s) — dry-running"),
					Plan.Attachments.Num(), UsedStandoffM, Plan.YawDeg, *Plan.RowAxis.ToCompactString()));

				if (!FAIDAActionSeam::DryRunBuild(this, Attachment.RecipeClassPath, Plan.Attachments, DryRun, Ctx.PlayerId))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(DryRun.Error, {}));
				}
				const int32 Overlaps = FAIDAActionSeam::CountAttachmentOverlaps(this, Plan.Attachments, FootprintM * 100.0);
				if (DryRun.bOk && DryRun.ClippingCount == 0 && Overlaps == 0)
				{
					bPlaced = true;
					break;
				}
				// Fallback bookkeeping: a non-overlapping lane always beats an overlapping one.
				if (DryRun.bOk && (NearestValidStandoffM < 0.0 || (bNearestValidOverlaps && Overlaps == 0)))
				{
					NearestValidPlan = Plan;
					NearestValidDryRun = DryRun;
					NearestValidStandoffM = UsedStandoffM;
					bNearestValidOverlaps = Overlaps > 0;
				}
				Stage(FString::Printf(TEXT("%.0f m lane: %d blocked, %d clipping, %d overlapping — stepping the row out"),
					UsedStandoffM, DryRun.Failures.Num(), DryRun.ClippingCount, Overlaps));
			}
			if (!bPlaced && NearestValidStandoffM >= 0.0)
			{
				// No fully clean lane in range: the preferred fallback (recorded non-overlapping
				// when one existed) wins, clipping and all.
				Plan = MoveTemp(NearestValidPlan);
				DryRun = MoveTemp(NearestValidDryRun);
				UsedStandoffM = NearestValidStandoffM;
				bPlaced = true;
				bClips = true;
				bOverlaps = bNearestValidOverlaps;
			}
			if (!bPlaced)
			{
				const FString Msg = FString::Printf(TEXT("%d of %d attachment placement(s) invalid even %.0f m out from the ports"),
					DryRun.Failures.Num(), Plan.Attachments.Num(), UsedStandoffM);
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, DryRun.Failures));
			}
			// The stored/journaled spec carries the BASE standoff that actually validated (actual row
			// distance minus the lane offset) — a spec copied into a later propose re-derives the same
			// row through the lane math instead of stacking the lane offset twice.
			const double EffectiveBaseM = UsedStandoffM - Lane * LaneWidthM;
			if (EffectiveBaseM != Spec.StandoffM)
			{
				(*SpecObj)->SetNumberField(TEXT("standoffM"), EffectiveBaseM);
			}

			// Reorder the ports to the plan's sort so everything downstream is index-aligned.
			TArray<FAIDAManifoldPort> SortedPorts;
			SortedPorts.Reserve(Plan.PortOrder.Num());
			for (const int32 PortIdx : Plan.PortOrder)
			{
				SortedPorts.Add(Ports[PortIdx]);
			}
			if (Config.Actions.CostMode == TEXT("central") && !DryRun.bAffordable)
			{
				FString Msg = TEXT("attachments not affordable from central storage (dimensional depot) plus your inventory: needs ");
				for (int32 i = 0; i < DryRun.Cost.Num(); ++i)
				{
					Msg += FString::Printf(TEXT("%s%d %s"), i > 0 ? TEXT(", ") : TEXT(""), DryRun.Cost[i].Amount, *DryRun.Cost[i].Item);
				}
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}

			Stage(TEXT("dry-run done — storing + publishing"));
			const int32 RunCount = 2 * SortedPorts.Num() - 1;
			FAIDAProposal Proposal;
			Proposal.Id = FGuid::NewGuid();
			Proposal.SpecJson = AIDAToCompactJson(SpecObj->ToSharedRef());
			Proposal.RequesterId = Ctx.PlayerId;
			Proposal.RequesterName = Ctx.Author;
			Proposal.Placements = MoveTemp(Plan.Attachments);
			Proposal.RecipeClassPath = Attachment.RecipeClassPath;
			Proposal.Cost = DryRun.Cost;
			Proposal.bManifold = true;
			Proposal.bManifoldPipe = Spec.bPipe;
			Proposal.bManifoldOutput = Spec.bOutput;
			Proposal.TransportRecipePath = Transport.RecipeClassPath;
			Proposal.TransportName = Transport.DisplayName;
			Proposal.Ports = MoveTemp(SortedPorts);
			Proposal.RowAxis = Plan.RowAxis;
			Proposal.DropDir = Plan.DropDir;
			Proposal.Summary = AIDAActionSpec::SummarizeManifold(Spec, Attachment.DisplayName, Transport.DisplayName,
				Proposal.Ports.Num(), RunCount, AIDAActionSpec::CompassName(-Plan.RowAxis));
			if (SkippedConnected > 0)
			{
				Proposal.Summary += FString::Printf(TEXT(" [%d machine(s) already connected, skipped]"), SkippedConnected);
			}
			if (Lane > 0)
			{
				Proposal.Summary += FString::Printf(TEXT(" [lane %d: row at %.0f m]"), Lane + 1, UsedStandoffM);
			}
			else if (UsedStandoffM != Spec.StandoffM)
			{
				Proposal.Summary += FString::Printf(TEXT(" [row stepped out to %.0f m]"), UsedStandoffM);
			}
			if (bClips)
			{
				// Manifolds can't be nudged (anchored to ports) — the escape hatch is a re-propose.
				Proposal.Summary += TEXT(" [clips nearby structures — reject and ask for a different standoff if unwanted]");
			}
			if (bOverlaps)
			{
				Proposal.Summary += TEXT(" [OVERLAPS existing splitters/junctions — no clear lane in range; reject and ask for a larger standoffM unless intended]");
			}

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			if (!Actions.Store().Add(Proposal, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored: %s (by %s)"),
				*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal.Summary, *Ctx.Author);
			PublishProposal(Proposal.Id);
			AnnounceSystem(FString::Printf(TEXT("AIDA proposes (for %s): %s — attachments cost %s; runs charged as built. Awaiting approval."),
				*Ctx.Author, *Proposal.Summary, *AIDACostSummaryString(Proposal.Cost)));
			Stage(TEXT("published — done"));

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds, DryRun.bAffordable, 0.0));
		}
	});

	// propose_power (live-verify gap): wire EXISTING unpowered machines — poles + power lines + a
	// grid tie through the auto-power executor, phase 0 skipped (the machines are already there).
	Tools.Register({
		TEXT("propose_power"),
		TEXT("PROPOSE wiring EXISTING machines to electricity: power poles beside them, power lines from each machine to its pole, a pole chain, and a tie-in to the nearest powered grid. Use when the player asks to power/wire up machines that are already built. Spec: {version:1, buildable?:'machine display name (empty = any unpowered machine)', center?:{x,y in metres}, radiusM?:30, maxCount?:0=all, pole?:'pole display name override'}. OMIT center to wire the machines near where the requesting player is looking. Machines already on a circuit are skipped automatically. Costs poles upfront; wires charged as built."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"The versioned power spec (see tool description)."}},"required":["spec"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			SweepProposals();

			const TSharedPtr<FJsonObject>* SpecObj = nullptr;
			Args->TryGetObjectField(TEXT("spec"), SpecObj);

			FAIDAPowerSpec Spec;
			FString Error;
			if (!AIDAActionSpec::ParsePowerSpec(SpecObj ? *SpecObj : nullptr, Config.Actions.MaxProposalItems, Spec, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}

			// Pole + wire kit (empty machine path = "the machines exist; trust the resolver below").
			FAIDAActionSeam::FAIDAPowerInfo Power;
			if (!FAIDAActionSeam::ResolveAutoPower(this, FString(), Spec.Pole, Power, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}

			// No center = the machines the player is looking at, falling back to their position.
			if (!Spec.bHasCenter)
			{
				FVector AimCm;
				if (FAIDAActionSeam::ResolveAimPoint(this, Ctx.PlayerId, AimCm))
				{
					Spec.CenterM = AimCm / 100.0;
				}
				else if (Ctx.bHasLocation)
				{
					Spec.CenterM = Ctx.Location / 100.0;
				}
				else
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("couldn't resolve the requesting player's aim or position — pass an explicit 'center' {x,y in metres}"), {}));
				}
				Spec.bHasCenter = true;
				const TSharedRef<FJsonObject> Center = MakeShared<FJsonObject>();
				Center->SetField(TEXT("x"), AIDANumber(Spec.CenterM.X));
				Center->SetField(TEXT("y"), AIDANumber(Spec.CenterM.Y));
				Center->SetField(TEXT("z"), AIDANumber(Spec.CenterM.Z));
				(*SpecObj)->SetObjectField(TEXT("center"), Center);
			}

			TArray<FAIDAManifoldPort> Machines;
			int32 SkippedPowered = 0;
			FAIDAActionSeam::ResolveUnpoweredMachines(this, Spec.Buildable, Spec.CenterM * AIDAMetersToCm,
				Spec.RadiusM * AIDAMetersToCm, Spec.MaxCount, Machines, SkippedPowered);
			if (Machines.Num() == 0)
			{
				const FString Msg = SkippedPowered > 0
					? FString::Printf(TEXT("all %d matching machine(s) are already on a power circuit"), SkippedPowered)
					: FString::Printf(TEXT("no unpowered machine%s within %.0f m"),
						Spec.Buildable.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" matching '%s'"), *Spec.Buildable), Spec.RadiusM);
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}

			TArray<FVector> Positions;
			Positions.Reserve(Machines.Num());
			for (const FAIDAManifoldPort& Machine : Machines) { Positions.Add(Machine.PosCm); }

			// Pole lane candidates: either side of the row, stepping out — the dry-run picks, same
			// pattern as build anchors and manifold lanes.
			const double Offsets[] = { 300.0, -300.0, 600.0, -600.0 };
			FAIDAPowerPlan Plan;
			FAIDADryRunResult PoleDryRun;
			FAIDAPowerPlan FallbackPlan;
			FAIDADryRunResult FallbackDryRun;
			bool bPlaced = false;
			bool bHaveFallback = false;
			for (const double Offset : Offsets)
			{
				FAIDAPowerPlan AttemptPlan = AIDAActionSpec::PlanPowerForPoints(Positions,
					FMath::Max(1, Power.PoleConnectionCap - 2), Offset);
				if (!AttemptPlan.Error.IsEmpty())
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(AttemptPlan.Error, {}));
				}
				for (FTransform& Pole : AttemptPlan.Poles)
				{
					double GroundZ;
					if (FAIDAActionSeam::ProbeGroundZ(this, Pole.GetLocation(), GroundZ))
					{
						FVector Location = Pole.GetLocation();
						Location.Z = GroundZ;
						Pole.SetLocation(Location);
					}
				}
				FAIDADryRunResult AttemptRun;
				if (!FAIDAActionSeam::DryRunBuild(this, Power.PoleRecipePath, AttemptPlan.Poles, AttemptRun, Ctx.PlayerId))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(AttemptRun.Error, {}));
				}
				if (AttemptRun.bOk && AttemptRun.ClippingCount == 0)
				{
					Plan = MoveTemp(AttemptPlan);
					PoleDryRun = MoveTemp(AttemptRun);
					bPlaced = true;
					break;
				}
				if (AttemptRun.bOk && !bHaveFallback)
				{
					FallbackPlan = MoveTemp(AttemptPlan);
					FallbackDryRun = AttemptRun;
					bHaveFallback = true;
				}
			}
			if (!bPlaced && bHaveFallback)
			{
				Plan = MoveTemp(FallbackPlan);
				PoleDryRun = MoveTemp(FallbackDryRun);
				bPlaced = true;
			}
			if (!bPlaced)
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
					TEXT("no valid pole spot beside those machines (both sides blocked) — pass 'pole' or a different center"), {}));
			}
			if (Config.Actions.CostMode == TEXT("central") && !PoleDryRun.bAffordable)
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
					FString::Printf(TEXT("%d pole(s) not affordable from central storage (dimensional depot) plus your inventory"), Plan.Poles.Num()), {}));
			}

			FAIDAProposal Proposal;
			Proposal.Id = FGuid::NewGuid();
			Proposal.SpecJson = AIDAToCompactJson(SpecObj->ToSharedRef());
			Proposal.RequesterId = Ctx.PlayerId;
			Proposal.RequesterName = Ctx.Author;
			Proposal.Placements = Plan.Poles;              // ghost preview = the poles
			Proposal.RecipeClassPath = Power.PoleRecipePath;
			Proposal.Cost = PoleDryRun.Cost;
			Proposal.bAutoPower = true;
			Proposal.bPowerOnly = true;
			Proposal.Ports = MoveTemp(Machines);
			Proposal.PolePlacements = MoveTemp(Plan.Poles);
			Proposal.PoleRecipePath = Power.PoleRecipePath;
			Proposal.PoleName = Power.PoleName;
			Proposal.WireRecipePath = Power.WireRecipePath;
			Proposal.MachineWires = MoveTemp(Plan.MachineWires);
			Proposal.ChainWires = MoveTemp(Plan.ChainWires);
			Proposal.Summary = FString::Printf(TEXT("power up %d x %s: %d x %s + %d wire(s) + grid tie"),
				Proposal.Ports.Num(), *Proposal.Ports[0].MachineName, Proposal.PolePlacements.Num(),
				*Power.PoleName, Proposal.MachineWires.Num() + Proposal.ChainWires.Num());
			if (SkippedPowered > 0)
			{
				Proposal.Summary += FString::Printf(TEXT(" [%d machine(s) already powered, skipped]"), SkippedPowered);
			}

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			if (!Actions.Store().Add(Proposal, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored: %s (by %s)"),
				*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal.Summary, *Ctx.Author);
			PublishProposal(Proposal.Id);
			AnnounceSystem(FString::Printf(TEXT("AIDA proposes (for %s): %s — poles cost %s; wires charged as built. Awaiting approval."),
				*Ctx.Author, *Proposal.Summary, *AIDACostSummaryString(Proposal.Cost)));

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds, PoleDryRun.bAffordable, 0.0));
		}
	});

	// propose_label_containers (P7 Slice 3): containers resolve live -> sign spot + dominant-item
	// text each -> one proposal through the standard approve/execute/journal pipeline.
	Tools.Register({
		TEXT("propose_label_containers"),
		TEXT("PROPOSE labeling storage containers: one small sign per container, its text set to the container's main (dominant) item. Nothing is built until a player with act permission approves. Spec: {version:1, sign?:'sign display name (default: smallest unlocked sign)', center?:{x,y in metres}, radiusM?:30, maxCount?:20, item?:'only containers holding this item'}. OMIT center to label the containers near where the requesting player is looking. Empty containers and containers that already have a sign are skipped automatically. Costs the signs' materials."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"The versioned label spec (see tool description)."}},"required":["spec"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			SweepProposals();

			const TSharedPtr<FJsonObject>* SpecObj = nullptr;
			Args->TryGetObjectField(TEXT("spec"), SpecObj);

			FAIDALabelSpec Spec;
			FString Error;
			if (!AIDAActionSpec::ParseLabelSpec(SpecObj ? *SpecObj : nullptr, Config.Actions.MaxProposalItems, Spec, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}

			FAIDARecipeResolution Sign;
			if (!FAIDAActionSeam::ResolveSignRecipe(this, Spec.Sign, Sign, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}

			// No center = the containers the player is looking at, falling back to their position.
			if (!Spec.bHasCenter)
			{
				FVector AimCm;
				if (FAIDAActionSeam::ResolveAimPoint(this, Ctx.PlayerId, AimCm))
				{
					Spec.CenterM = AimCm / 100.0;
				}
				else if (Ctx.bHasLocation)
				{
					Spec.CenterM = Ctx.Location / 100.0;
				}
				else
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("couldn't resolve the requesting player's aim or position — pass an explicit 'center' {x,y in metres}"), {}));
				}
				Spec.bHasCenter = true;
				const TSharedRef<FJsonObject> Center = MakeShared<FJsonObject>();
				Center->SetField(TEXT("x"), AIDANumber(Spec.CenterM.X));
				Center->SetField(TEXT("y"), AIDANumber(Spec.CenterM.Y));
				Center->SetField(TEXT("z"), AIDANumber(Spec.CenterM.Z));
				(*SpecObj)->SetObjectField(TEXT("center"), Center);
			}

			// The signs face the requesting player — the person asking is looking at the boxes.
			TArray<FAIDALabelTarget> Targets;
			int32 SkippedEmpty = 0, SkippedLabeled = 0;
			FAIDAActionSeam::ResolveLabelTargets(this, Spec.CenterM * AIDAMetersToCm, Spec.RadiusM * AIDAMetersToCm,
				Spec.MaxCount, Spec.ItemFilter, Ctx.Location, Ctx.bHasLocation, Targets, SkippedEmpty, SkippedLabeled);
			if (Targets.Num() == 0)
			{
				FString Msg = FString::Printf(TEXT("no containers to label within %.0f m"), Spec.RadiusM);
				if (SkippedLabeled > 0) { Msg += FString::Printf(TEXT(" — %d already labeled"), SkippedLabeled); }
				if (SkippedEmpty > 0) { Msg += FString::Printf(TEXT(", %d empty (skipped)"), SkippedEmpty); }
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}

			TArray<FAIDACostItem> Cost;
			FAIDAActionSeam::TallyRecipeCost(this, Sign.RecipeClassPath, Targets.Num(), Cost);
			if (Config.Actions.CostMode == TEXT("central") && !FAIDAActionSeam::CheckAffordable(this, Cost, Ctx.PlayerId))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
					FString::Printf(TEXT("%d sign(s) not affordable from central storage (dimensional depot) plus your inventory"), Targets.Num()), {}));
			}

			// Sign spots as placements: the ghost preview, pending caps, and upfront cost reuse the
			// standard build paths for free. Yaw faces outward, like the built sign will.
			TArray<FTransform> Placements;
			Placements.Reserve(Targets.Num());
			for (const FAIDALabelTarget& Target : Targets)
			{
				Placements.Add(FTransform(Target.OutwardCm.Rotation(), Target.SignPosCm));
			}

			FAIDAProposal Proposal;
			Proposal.Id = FGuid::NewGuid();
			Proposal.SpecJson = AIDAToCompactJson(SpecObj->ToSharedRef());
			Proposal.RequesterId = Ctx.PlayerId;
			Proposal.RequesterName = Ctx.Author;
			Proposal.Placements = MoveTemp(Placements);
			Proposal.RecipeClassPath = Sign.RecipeClassPath;
			Proposal.Cost = Cost;
			Proposal.bLabel = true;
			Proposal.LabelTargets = MoveTemp(Targets);
			Proposal.Summary = AIDAActionSpec::SummarizeLabel(Spec, Sign.DisplayName, Proposal.LabelTargets.Num());
			if (SkippedLabeled > 0)
			{
				Proposal.Summary += FString::Printf(TEXT(" [%d already labeled, skipped]"), SkippedLabeled);
			}
			if (SkippedEmpty > 0)
			{
				Proposal.Summary += FString::Printf(TEXT(" [%d empty, skipped]"), SkippedEmpty);
			}

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			if (!Actions.Store().Add(Proposal, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored: %s (by %s)"),
				*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal.Summary, *Ctx.Author);
			PublishProposal(Proposal.Id);
			AnnounceSystem(FString::Printf(TEXT("AIDA proposes (for %s): %s — costs %s. Awaiting approval."),
				*Ctx.Author, *Proposal.Summary, *AIDACostSummaryString(Proposal.Cost)));

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds, true, 0.0));
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

			// A reply cut off by the output-token limit MID-TOOL-CALL arrives with truncated (usually
			// empty) arguments. Dispatching those yields nonsense errors ("missing spec object") the
			// model can't diagnose — live-verify showed it retrying the same oversized composite spec
			// ten times. Tell it exactly what happened instead, so it compacts or splits the call.
			if (Result.StopReason == TEXT("max_tokens") || Result.StopReason == TEXT("length"))
			{
				UE_LOG(LogAIDA, Warning, TEXT("[tools] reply hit the output-token limit mid-tool-call — %d call(s) NOT dispatched."),
					Result.ToolCalls.Num());
				FAIDAChatMessage TruncTurn;
				TruncTurn.Role = TEXT("user");
				for (const FAIDAToolCall& Call : Result.ToolCalls)
				{
					FAIDAToolResultPart Part;
					Part.ToolCallId = Call.Id;
					Part.bIsError = true;
					Part.Content = TEXT("NOT EXECUTED: your reply hit the provider's output-token limit before this tool call's"
						" arguments finished streaming. Make the call much more COMPACT and retry: use grid repeats"
						" (countX/countY) instead of listing similar parts one by one, drop optional fields, and if it is"
						" still large, split the build into two or three smaller proposals.");
					TruncTurn.ToolResults.Add(MoveTemp(Part));
				}
				Messages->Add(MoveTemp(TruncTurn));
				O->RunToolLoop(Messages, Requester, RoundsLeft - 1, OnDelta, OnDone, OnError);
				return;
			}

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

	DumpPackCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AIDA.DumpPack"),
		TEXT("Rebuild + log the generated game data pack appended to the system prompt (docs/PROMPT.md §2 eyeball check, no LLM). Run on server/host. Usage: AIDA.DumpPack"),
		FConsoleCommandWithArgsDelegate::CreateUObject(this, &UAIDAOrchestrator::DumpPack),
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

	// Manifold executions report their failed runs (skipped attachments, unsnappable/unaffordable
	// belts) so a half-plumbed row is loud, never a surprise.
	for (const FString& Line : Actions.TakeRunReport())
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

const FString& UAIDAOrchestrator::GetPromptPack()
{
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (PromptPackBuiltAt < 0.0 || (Now - PromptPackBuiltAt) >= PackTtlSeconds)
	{
		PromptPackCache = AIDAPromptPack::Build(RecipeCatalog.GetRecipes(World, Now), RecipeCatalog.GetBuildings(World, Now));
		PromptPackBuiltAt = Now;
		UE_LOG(LogAIDA, Log, TEXT("[prompt] game data pack rebuilt: %d chars (~%d tokens)."),
			PromptPackCache.Len(), PromptPackCache.Len() / 4);
	}
	return PromptPackCache;
}

void UAIDAOrchestrator::DumpPack(const TArray<FString>& /*Args*/)
{
	// The pack reads the recipe manager (a world subsystem) — server/host only.
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client)
	{
		UE_LOG(LogAIDA, Warning, TEXT("AIDA.DumpPack runs only on the server/host (this is a client)."));
		return;
	}

	PromptPackBuiltAt = -1.0; // force a fresh build so the dump reflects current unlocks
	const FString& Pack = GetPromptPack();
	// UE_LOG truncates very long lines on some sinks — dump in chunks so the whole pack is readable.
	constexpr int32 ChunkChars = 4000;
	for (int32 Start = 0; Start < Pack.Len(); Start += ChunkChars)
	{
		UE_LOG(LogAIDA, Log, TEXT("AIDA.DumpPack [%d/%d]:\n%s"),
			Start / ChunkChars + 1, (Pack.Len() + ChunkChars - 1) / ChunkChars, *Pack.Mid(Start, ChunkChars));
	}
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

void UAIDAOrchestrator::OnPlayerPostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer)
{
	// The PostLogin event is global — only react to joins into our own world.
	if (!GameMode || GameMode->GetWorld() != GetWorld()) { return; }
	const FString Who = NewPlayer && NewPlayer->PlayerState ? NewPlayer->PlayerState->GetPlayerName() : FString();
	TakeSnapshot(Who.IsEmpty() ? TEXT("login") : FString::Printf(TEXT("login:%s"), *Who));
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
		ImageStore.Configure(Config.Uploads);
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
