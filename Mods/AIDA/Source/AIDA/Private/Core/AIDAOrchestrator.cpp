#include "Core/AIDAOrchestrator.h"

#include "Testing/AIDASelfTest.h"

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
// P8 Slice 1 (reset_fuse / get_milestone_status): live circuit + schematic reads.
#include "FGBuildableSubsystem.h"
#include "Buildables/FGBuildableFactory.h"
#include "FGPowerInfoComponent.h"
#include "FGPowerCircuit.h"
#include "FGSchematicManager.h"
#include "FGSchematic.h"
#include "FGRecipe.h"
#include "ItemAmount.h"
#include "Resources/FGItemDescriptor.h"
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
			|| Text.Contains(TEXT("awaiting your approval"), ESearchCase::IgnoreCase)
			// The fabricated form seen live omits approve wording sometimes — the announcement
			// itself is the tell. False positives are harmless: the check only acts when NO real
			// proposal exists.
			|| Text.Contains(TEXT("proposal ready"), ESearchCase::IgnoreCase)
			|| Text.Contains(TEXT("proposal is ready"), ESearchCase::IgnoreCase)
			|| Text.Contains(TEXT("proposal created"), ESearchCase::IgnoreCase);
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
		"- get_alerts(): ONE consolidated health sweep — tripped fuses, out-of-fuel generators, stopped "
		"machines split by cause (input starved vs output full), paused machines, dangling belts/pipes. "
		"Call this FIRST for 'is something wrong / why did everything stop / factory status'.\n"
		"- get_milestone_status(): the active milestone's remaining cost plus the next milestones "
		"available to pick. Use for 'what do we still owe / what should we unlock next'.\n"
		"- reset_fuse(): reset ALL tripped power fuses immediately (no approval needed). Use when 'the "
		"power is out / everything went dark'; afterwards WARN the player if load still exceeds capacity "
		"(the fuse will trip again — suggest pausing machines or adding generators).\n"
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
		"FABRICATION RULE (absolute): NEVER write 'proposal ready', a cost list, or 'awaiting approval' "
		"unless a propose_* TOOL RESULT in THIS very reply returned a proposalId. Describing a proposal "
		"without calling the tool creates NOTHING: there is no ghost, /aida approve fails, and the server "
		"posts a public correction under your reply — it is the most damaging mistake you can make. When "
		"the player asks you to build/place/change something, your reply MUST START with the propose_* "
		"tool call — never with prose announcing what you 'will' do or 'have' proposed. The server "
		"announces every real proposal itself with an 'AIDA proposes …' system line; if that line has "
		"not appeared, no proposal exists, whatever the conversation says.\n"
		"- propose_build(spec): propose placing buildables on a snapped grid. NOTHING is built until a "
		"player with act permission approves the proposal. Only call it when the player explicitly asks "
		"you to build/place something. Omit the spec's 'origin' to build where the player is aiming ('here', "
		"'there', 'at my position', or no location given) — you never need to ask for coordinates. On success, "
		"relay the returned summary + cost and say it awaits approval; on a tool error (unknown buildable, "
		"cost, cap), revise the spec or explain the reason. Blocked/uneven ground is NEVER an error: the "
		"proposal still goes up with its ghost and the result carries invalidCount — relay it and tell the "
		"player to nudge the ghost onto clear ground before approving (approving as-is builds only the valid "
		"placements and refunds the rest). Never claim something was built until "
		"get_proposal_status says executed.\n"
		"- propose_extend_foundations(spec): extend the foundation slab the player stands on or aims "
		"at by spec.count tiles, across the slab's WHOLE width — the server finds the slab, its "
		"foundation types and lattice, and the direction (their look direction when standing on it; "
		"the aimed edge's outward face when aimed from beside it; spec.direction only when they name "
		"one — NEVER ask for a direction or a location). ALWAYS use this when the player asks to "
		"extend/continue/grow existing foundations, a floor, or a platform — never propose_build.\n"
		"- propose_dismantle(selector): same flow for removing buildables near a point.\n"
		"- propose_set_clocks(selector?, percent? | advised:true): change machine clock speeds in place "
		"(approval-gated, undoable). 'underclock what you recommended' = one call with advised:true; a "
		"specific percent (1-100) applies to every machine the selector matches. Above 100% is not "
		"supported yet (power shards).\n"
		"- propose_set_recipe(selector?, recipe, force?): retask manufacturers to a different production "
		"recipe. Machines with items inside are skipped unless force:true, which DESTROYS their contents "
		"— warn the player before proposing force. Use when the player says to switch/change what "
		"machines make.\n"
		"- propose_upgrade_belts(selector?, targetMk): upgrade (or downgrade) conveyor belts to a mark, "
		"preserving connections and items. Use for 'upgrade these belts / all belts within X m to Mk.N'. "
		"Each new belt's cost is charged as built.\n"
		"- propose_pipe_tap(spec, forProposalId?): feed a machine from an EXISTING pipeline — splice a "
		"junction in (or use a free pipe end) + one pipe run to the destination (~56 m max, no chaining "
		"yet). Use for 'hook this up to the water line / tap the oil pipe'. Same two modes as "
		"propose_belt_tap: forProposalId feeds a pending pipe-in manifold; omit it to feed the nearest "
		"free pipe input where the player is aiming.\n"
		"- propose_factory(spec): a WHOLE production line from the plan_factory math in ONE proposal — "
		"foundations, a machine row per step with recipes + exact clocks assigned, manifold rows on every "
		"step's inputs/outputs, runs linking step outputs into the next step's feed, and power. Use when "
		"the player asks to BUILD N/min of something ('build me 30 iron plates a minute right here'); "
		"afterwards offer belt/pipe taps (forProposalId) for the raw inputs the result lists. For numbers "
		"only use plan_factory; for one machine group use propose_build.\n"
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
		"- propose_belt_tap(spec, forProposalId?): feed machines from an EXISTING belt — the server "
		"finds the nearest belt carrying spec.item ('Coal'; omit = any belt), taps it (splices a "
		"splitter into it mid-belt, or uses a free belt end), and routes a feed belt to the "
		"destination, chaining runs automatically for long distances (up to ~2 km). WITH "
		"forProposalId = a PENDING belt-in manifold proposal: everything merges into ONE proposal/"
		"approval. WITHOUT forProposalId: the destination is the nearest FREE belt input near the "
		"player's aim on machines or a BUILT manifold row — use this whenever the manifold already "
		"executed ('connect it' after the fact). NEVER hand-build feeds with propose_build; never "
		"tell the player to run belts manually — call this tool (raise spec.maxDistanceM, default "
		"250, when the source is far). The item match reads what is riding each belt RIGHT NOW.\n"
		"- get_proposal_status(proposalId?): check whether proposals were approved/executed/expired. "
		"Pending proposals include their full stored 'spec' — the input for revisions.\n"
		"REVISING A PENDING PROPOSAL (very common): when the player asks to change or extend a proposed "
		"build while its ghost is up ('add an input manifold', 'make it 20', 'use mk.2 belts', 'add a row'), "
		"NEVER reject it or tell them to approve first. Instead: (a) for spec changes, read the pending "
		"proposal's spec from get_proposal_status, merge the change, and call propose_build with "
		"replaceProposalId = the old id; (b) to add a manifold to UNBUILT proposed machines (v1 grids "
		"and v2 composites alike), call "
		"propose_manifold with forProposalId = the pending proposal's id (omit spec.machines) — the "
		"machines do not need to exist yet. Either way the old proposal is replaced atomically, the ghosts "
		"update to preview the WHOLE revised group, and ONE approval builds everything. Add more manifolds "
		"by calling propose_manifold again with the NEW proposal id ('both sides' = two calls).\n"
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

	// P8 Slice 5: standing tasks poll slowly while enabled — the poll itself is nearly free; only
	// a DUE task spends an LLM run (budgeted in OnTaskTimer).
	if (Config.Tasks.bEnabled)
	{
		InWorld.GetTimerManager().SetTimer(TaskTimer, this, &UAIDAOrchestrator::OnTaskTimer, 60.0f, /*bLoop=*/true);
		UE_LOG(LogAIDA, Log, TEXT("[tasks] standing tasks enabled (min interval %d min, %d runs/day)."),
			Config.Tasks.MinIntervalMinutes, Config.Tasks.MaxPerDay);
	}

	// Packaged-game scenario harness (docs/SELFTEST.md): armed only by -AIDASelfTest=<file> on the
	// command line; drives the tool registry against the loaded save and writes a results file.
	FString SelfTestScript;
	if (FAIDASelfTestRunner::ShouldRun(SelfTestScript))
	{
		SelfTest = MakeUnique<FAIDASelfTestRunner>();
		SelfTest->Start(this, SelfTestScript);
	}
}

void UAIDAOrchestrator::Deinitialize()
{
	if (SelfTest)
	{
		SelfTest->Shutdown();
		SelfTest.Reset();
	}
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

		// Standing tasks grant recurring token spend — act tier, humans only (never an LLM tool).
		if (Command.Kind == FAIDAChatCommand::EKind::Task)
		{
			if (!Permissions.IsAllowed(EAIDATier::Act, Requester.PlayerId))
			{
				Session->PostSystemMessage(TEXT("You don't have act permission to manage standing tasks."), ConversationId);
				return;
			}
			HandleTaskCommand(Requester, Command, ConversationId);
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
		[Weak, MsgId, ConversationId, ReplyStartUtc, Messages, Requester](const FString& FullText)
		{
			if (UAIDAOrchestrator* O = Weak.Get(); O && O->Session.IsValid())
			{
				// Terminal step runs the fabrication SELF-REPAIR loop (one retry) before accepting.
				O->FinishChatReply(MsgId, ConversationId, ReplyStartUtc, Messages, Requester,
					/*RetriesLeft*/ 1, FullText);
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

void UAIDAOrchestrator::FinishChatReply(const FGuid& MsgId, const FGuid& ConversationId, int64 ReplyStartUtc,
	TSharedRef<TArray<FAIDAChatMessage>, ESPMode::ThreadSafe> Messages, FAIDARequester Requester,
	int32 RetriesLeft, const FString& FinalText)
{
	if (!Session.IsValid())
	{
		return;
	}

	// Fabrication check (live-verify, repeatedly: the model announces "Proposal ready … /aida
	// approve" without ever calling a propose_* tool — it pattern-completes the announcement shape
	// it sees all over the transcript). A reply that invites approval while NOTHING was proposed
	// during this request is a false statement about server state.
	bool bFabricated = false;
	if (Config.Actions.bEnabled && AIDATextInvitesApproval(FinalText))
	{
		SweepProposals();
		bFabricated = true;
		for (const FAIDAProposal& Proposal : Actions.Store().All())
		{
			if (Proposal.State == EAIDAProposalState::Pending || Proposal.ProposedUtc >= ReplyStartUtc)
			{
				bFabricated = false;
				break;
			}
		}
	}

	if (bFabricated && RetriesLeft > 0)
	{
		// ROOT-CAUSE REPAIR: don't accept the reply — put the fabricated turn on the record, tell
		// the model exactly what is wrong, and re-run the loop so it makes the REAL tool call. The
		// already-streamed text stays visible; the repair continues in the same message, and a
		// successful retry ends with a genuine proposal + the server's own announce line.
		UE_LOG(LogAIDA, Warning, TEXT("[actions] reply invited approval but created no proposal — running self-repair round."));
		FAIDAChatMessage Fabricated;
		Fabricated.Role = TEXT("assistant");
		Fabricated.Content = FinalText;
		Messages->Add(MoveTemp(Fabricated));
		FAIDAChatMessage Correction;
		Correction.Role = TEXT("user");
		Correction.Content = TEXT(
			"SERVER CHECK FAILED: that reply describes a proposal and invites approval, but you never called a"
			" propose_* tool this turn — NOTHING exists for the player to approve, and your text cannot be"
			" retracted. Fix it NOW: call the appropriate propose_* tool with exactly what you described"
			" (omit origin/center to use the player's aim). After the tool result, reply with ONE short line"
			" relaying the real summary. Do not apologize at length, and never announce a proposal without a"
			" tool result again.");
		Messages->Add(MoveTemp(Correction));
		Session->AppendDelta(MsgId, TEXT("\n"));

		TWeakObjectPtr<UAIDAOrchestrator> Weak(this);
		RunToolLoop(Messages, Requester, MaxToolRoundTrips,
			[Weak, MsgId](const FString& Delta)
			{
				if (UAIDAOrchestrator* O = Weak.Get(); O && O->Session.IsValid())
				{
					O->Session->AppendDelta(MsgId, Delta);
				}
			},
			[Weak, MsgId, ConversationId, ReplyStartUtc, Messages, Requester, RetriesLeft](const FString& RetryText)
			{
				if (UAIDAOrchestrator* O = Weak.Get(); O && O->Session.IsValid())
				{
					O->FinishChatReply(MsgId, ConversationId, ReplyStartUtc, Messages, Requester,
						RetriesLeft - 1, RetryText);
				}
			},
			[Weak, MsgId](int32 Status, const FString& Message)
			{
				if (UAIDAOrchestrator* O = Weak.Get(); O && O->Session.IsValid())
				{
					UE_LOG(LogAIDA, Error, TEXT("AIDA self-repair round failed (HTTP %d): %s"), Status, *Message);
					O->Session->AppendDelta(MsgId, TEXT("\n[response interrupted]"));
					O->Session->CompleteMessage(MsgId);
				}
			});
		return;
	}

	Session->CompleteMessage(MsgId);
	if (bFabricated)
	{
		// Repair exhausted (or disabled) — at least correct the record immediately, not when the
		// player's approve bounces.
		UE_LOG(LogAIDA, Warning, TEXT("[actions] reply STILL invited approval without a proposal after self-repair — posting correction."));
		Session->PostSystemMessage(
			TEXT("(Heads up: that reply mentions approving a proposal, but none was actually created — real proposals always get an 'AIDA proposes …' line. Tell AIDA to 'propose it' to get a real one.)"),
			ConversationId);
	}
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

	// P8 Slice 1: one consolidated health sweep — the model's first call for "is something wrong".
	Tools.Register({
		TEXT("get_alerts"),
		TEXT("ONE consolidated factory health sweep: tripped fuses (located at the dark machines' centroid), fuel generators out of fuel, stopped machines split by cause (input starved vs output full vs unknown), player-paused machines, and belts/pipes ending in nothing. ALWAYS call this first for 'is something wrong / why did production stop / factory status'. Machines dark because of a tripped fuse are folded into that fuse alert, not listed one by one. Locations in metres."),
		TEXT(""),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& /*Args*/, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			UWorld* World = GetWorld();
			const double Now = World ? World->GetTimeSeconds() : 0.0;
			const FAIDAFactorySnapshot& Snapshot = FactoryIndex.GetSnapshot(World, Now);
			return FAIDAToolResult::Ok(AIDAFactoryTools::BuildAlertsJson(FAIDAFactoryAggregator::BuildAlerts(Snapshot)));
		}
	});

	// P8 Slice 1: milestone progress — live schematic-manager read (not snapshot data).
	Tools.Register({
		TEXT("get_milestone_status"),
		TEXT("The active HUB milestone: name, tier, and the REMAINING cost to pay off, plus the next few milestones available to select. Use for 'what do we still owe / how far along is the milestone / what should we unlock next' — and as the target for planning ('plan production for the remaining milestone cost')."),
		TEXT(""),
		EAIDAToolTier::Query,
		[this](const TSharedRef<FJsonObject>& /*Args*/, const FAIDAToolContext& /*Ctx*/) -> FAIDAToolResult
		{
			AFGSchematicManager* Schematics = GetWorld() ? AFGSchematicManager::Get(GetWorld()) : nullptr;
			if (!Schematics)
			{
				return FAIDAToolResult::Error(TEXT("no schematic manager in this world — is a save loaded?"));
			}
			const auto CostToJson = [](const TArray<FItemAmount>& Cost)
			{
				TArray<TSharedPtr<FJsonValue>> Arr;
				for (const FItemAmount& Entry : Cost)
				{
					if (!Entry.ItemClass) { continue; }
					const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
					O->SetStringField(TEXT("item"), UFGItemDescriptor::GetItemName(Entry.ItemClass).ToString());
					O->SetNumberField(TEXT("amount"), Entry.Amount);
					Arr.Add(MakeShared<FJsonValueObject>(O));
				}
				return Arr;
			};

			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			const TSubclassOf<UFGSchematic> Active = Schematics->GetActiveSchematic();
			if (Active)
			{
				const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("name"), UFGSchematic::GetSchematicDisplayName(Active).ToString());
				O->SetNumberField(TEXT("tier"), UFGSchematic::GetTechTier(Active));
				O->SetArrayField(TEXT("remainingCost"), CostToJson(Schematics->GetRemainingCostFor(Active)));
				Root->SetObjectField(TEXT("active"), O);
			}
			else
			{
				Root->SetStringField(TEXT("active"), TEXT("none — no milestone selected at the HUB"));
			}

			// The next choices: available-but-unpurchased milestones, a handful is plenty.
			TArray<TSubclassOf<UFGSchematic>> Available;
			Schematics->GetAvailableSchematicsOfTypes({ ESchematicType::EST_Milestone }, Available);
			TArray<TSharedPtr<FJsonValue>> Arr;
			constexpr int32 MaxAvailableMilestones = 6;
			for (int32 i = 0; i < Available.Num() && Arr.Num() < MaxAvailableMilestones; ++i)
			{
				if (!Available[i] || Available[i] == Active) { continue; }
				const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("name"), UFGSchematic::GetSchematicDisplayName(Available[i]).ToString());
				O->SetNumberField(TEXT("tier"), UFGSchematic::GetTechTier(Available[i]));
				O->SetArrayField(TEXT("cost"), CostToJson(UFGSchematic::GetCost(Available[i])));
				Arr.Add(MakeShared<FJsonValueObject>(O));
			}
			Root->SetArrayField(TEXT("availableMilestones"), Arr);
			if (Available.Num() > MaxAvailableMilestones)
			{
				Root->SetNumberField(TEXT("availableOmitted"), Available.Num() - MaxAvailableMilestones);
			}
			return FAIDAToolResult::Ok(AIDAToCompactJson(Root));
		}
	});

	// P8 Slice 1: fuse reset — Act tier but DIRECT execute (docs/PHASE8.md: trivially reversible,
	// players ask in a panic; precedent: tag_node writes world state without the proposal flow).
	Tools.Register({
		TEXT("reset_fuse"),
		TEXT("Reset every tripped power fuse NOW (executes immediately — no proposal/approval; it is trivially reversible). Returns each reset circuit's load vs generation capacity AFTER the reset: when load exceeds capacity, WARN the player it will trip again and suggest pausing machines or adding generators. Use when 'the power is out / everything went dark / reset the breaker'. Says so if no fuse was tripped."),
		TEXT(""),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& /*Args*/, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			if (!Config.Actions.bAllowDirectFuseReset)
			{
				return FAIDAToolResult::Error(TEXT("direct fuse reset is disabled on this server (actions.allowDirectFuseReset) — a player must reset the fuse at a power pole or switch."));
			}
			// Live circuit walk (the snapshot may be stale): every factory's power info leads to its
			// circuit; the subsystem has no public enumeration.
			AFGBuildableSubsystem* Subsystem = GetWorld() ? AFGBuildableSubsystem::Get(GetWorld()) : nullptr;
			if (!Subsystem)
			{
				return FAIDAToolResult::Error(TEXT("no buildable subsystem in this world — is a save loaded?"));
			}
			TSet<UFGPowerCircuit*> Circuits;
			for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
			{
				const AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Buildable);
				UFGPowerInfoComponent* PowerInfo = Factory ? Factory->GetPowerInfo() : nullptr;
				if (UFGPowerCircuit* Circuit = PowerInfo ? PowerInfo->GetPowerCircuit() : nullptr)
				{
					Circuits.Add(Circuit);
				}
			}

			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Arr;
			int32 ResetCount = 0;
			int32 StillOverloaded = 0;
			for (UFGPowerCircuit* Circuit : Circuits)
			{
				if (!Circuit || !Circuit->IsFuseTriggered()) { continue; }
				Circuit->ResetFuse();
				++ResetCount;

				FPowerCircuitStats Stats;
				Circuit->GetStats(Stats);
				const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetNumberField(TEXT("circuitId"), Circuit->GetCircuitID());
				O->SetField(TEXT("load_MW"), AIDANumber(Stats.PowerConsumed));
				O->SetField(TEXT("capacity_MW"), AIDANumber(Stats.PowerProductionCapacity));
				const bool bOverloaded = Stats.PowerConsumed > Stats.PowerProductionCapacity + 1e-3;
				O->SetBoolField(TEXT("willTripAgain"), bOverloaded);
				if (bOverloaded) { ++StillOverloaded; }
				Arr.Add(MakeShared<FJsonValueObject>(O));
			}
			Root->SetNumberField(TEXT("fusesReset"), ResetCount);
			Root->SetArrayField(TEXT("circuits"), Arr);
			if (ResetCount == 0)
			{
				Root->SetStringField(TEXT("note"), TEXT("no tripped fuses found — the power problem is something else (check get_alerts)"));
			}
			else
			{
				UE_LOG(LogAIDA, Log, TEXT("[actions] reset %d tripped fuse(s) for %s (%d still overloaded)."),
					ResetCount, *Ctx.Author, StillOverloaded);
				AnnounceSystem(FString::Printf(TEXT("AIDA reset %d tripped fuse(s) for %s.%s"), ResetCount, *Ctx.Author,
					StillOverloaded > 0 ? TEXT(" WARNING: load still exceeds capacity — it will trip again.") : TEXT("")));
			}
			return FAIDAToolResult::Ok(AIDAToCompactJson(Root));
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
	// Live validity advisory (never a rejection — user rule): recomputed on every nudge, so the
	// tag disappears the moment the player walks the ghost onto clear ground. Manifolds bake
	// their own (they can't be nudged).
	if (Proposal->State == EAIDAProposalState::Pending && Proposal->InvalidCount > 0 && !Proposal->bManifold)
	{
		View.Summary += FString::Printf(TEXT(" [%d placement(s) blocked here — nudge the ghost before approving]"),
			Proposal->InvalidCount);
	}
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
			// A yaw change also breaks the run — a ghost part carries ONE yaw, and perimeter walls
			// mix all four facings under a single part index.
			const float Yaw = static_cast<float>(Proposal->Placements[i].Rotator().Yaw);
			if (!Current || Part != CurrentPart || !FMath::IsNearlyEqual(Current->YawDeg, Yaw, 0.1f))
			{
				Current = &View.GhostParts.AddDefaulted_GetRef();
				Current->RecipeClassPath = bComposite && Proposal->PartRecipePaths.IsValidIndex(Part)
					? Proposal->PartRecipePaths[Part] : Proposal->RecipeClassPath;
				Current->YawDeg = Yaw;
				CurrentPart = Part;
			}
			Current->TileCenters.Add(Proposal->Placements[i].GetLocation());
		}
		// Connected builds: every manifold set's attachment row ghosts alongside the machines, so
		// a revised proposal previews the WHOLE group before approval.
		for (const FAIDAManifoldSet& Set : Proposal->ManifoldSets)
		{
			if (Set.Attachments.Num() == 0) { continue; }
			FAIDAGhostPart& Part = View.GhostParts.AddDefaulted_GetRef();
			Part.RecipeClassPath = Set.AttachmentRecipePath;
			Part.YawDeg = static_cast<float>(Set.Attachments[0].Rotator().Yaw);
			for (const FTransform& Attachment : Set.Attachments)
			{
				Part.TileCenters.Add(Attachment.GetLocation());
			}
		}

		// The runs (belts/pipes) preview too — live-verify: an approved manifold surprised the
		// player with belts the ghost never showed. Endpoints and directions mirror the executor's
		// RunOne exactly (trunk hops in flow order, then drops), so the ghost belts curve the way
		// the built ones will.
		const auto AddGhostRuns = [&View](const FString& TransportPath, bool bPipe, bool bOutput,
			const TArray<FTransform>& Attachments, const TArray<FAIDAManifoldPort>& Ports,
			const FVector& RowAxis, const FVector& DropDir)
		{
			const bool bReverse = bOutput && !bPipe; // merger trunks flow descending
			for (int32 i = 0; i + 1 < Attachments.Num(); ++i)
			{
				FAIDAGhostRun& Run = View.GhostRuns.AddDefaulted_GetRef();
				Run.RecipeClassPath = TransportPath;
				Run.FromCm = Attachments[bReverse ? i + 1 : i].GetLocation();
				Run.ToCm = Attachments[bReverse ? i : i + 1].GetLocation();
				Run.FromNormal = bReverse ? -RowAxis : RowAxis;
				Run.ToNormal = bReverse ? RowAxis : -RowAxis;
			}
			for (int32 i = 0; i < Attachments.Num(); ++i)
			{
				if (!Ports.IsValidIndex(i)) { continue; }
				FAIDAGhostRun& Run = View.GhostRuns.AddDefaulted_GetRef();
				Run.RecipeClassPath = TransportPath;
				if (bOutput) // machine output → attachment (merger/junction)
				{
					Run.FromCm = Ports[i].PosCm;
					Run.FromNormal = Ports[i].NormalCm;
					Run.ToCm = Attachments[i].GetLocation();
					Run.ToNormal = DropDir;
				}
				else         // attachment (splitter/junction) → machine input
				{
					Run.FromCm = Attachments[i].GetLocation();
					Run.FromNormal = DropDir;
					Run.ToCm = Ports[i].PosCm;
					Run.ToNormal = Ports[i].NormalCm;
				}
			}
		};
		for (const FAIDAManifoldSet& Set : Proposal->ManifoldSets)
		{
			AddGhostRuns(Set.TransportRecipePath, Set.bPipe, Set.bOutput,
				Set.Attachments, Set.Ports, Set.RowAxis, Set.DropDir);
		}
		if (Proposal->bManifold)
		{
			AddGhostRuns(Proposal->TransportRecipePath, Proposal->bManifoldPipe, Proposal->bManifoldOutput,
				Proposal->Placements, Proposal->Ports, Proposal->RowAxis, Proposal->DropDir);
		}

		// Belt taps preview their splitter (cut variant) and the feed run to the trunk's open end.
		if (Proposal->bTap)
		{
			if (!Proposal->bTapDangling && !Proposal->TapSplitterRecipePath.IsEmpty())
			{
				FAIDAGhostPart& Part = View.GhostParts.AddDefaulted_GetRef();
				Part.RecipeClassPath = Proposal->TapSplitterRecipePath;
				Part.YawDeg = static_cast<float>(FMath::RadiansToDegrees(
					FMath::Atan2(Proposal->TapDirCm.Y, Proposal->TapDirCm.X)));
				Part.TileCenters.Add(Proposal->TapPointCm);
			}
			// Destination + belt: a pending row's open end, or (standalone) the stored built port.
			const bool bSet = Proposal->ManifoldSets.IsValidIndex(Proposal->TapSetIndex);
			const bool bStandalone = !Proposal->bManifold && Proposal->ManifoldSets.Num() == 0;
			FVector DestCm = FVector::ZeroVector;
			FVector DestNormal = -Proposal->RowAxis;
			FString FeedTransport = Proposal->TransportRecipePath;
			bool bHaveDest = false;
			if (bSet)
			{
				const FAIDAManifoldSet& Set = Proposal->ManifoldSets[Proposal->TapSetIndex];
				if (Set.Attachments.Num() > 0)
				{
					DestCm = Set.Attachments[0].GetLocation();
					DestNormal = -Set.RowAxis;
					FeedTransport = Set.TransportRecipePath;
					bHaveDest = true;
				}
			}
			else if (bStandalone && Proposal->Ports.Num() > 0)
			{
				DestCm = Proposal->Ports[0].PosCm;
				DestNormal = Proposal->Ports[0].NormalCm;
				bHaveDest = true;
			}
			else if (Proposal->Placements.Num() > 0)
			{
				DestCm = Proposal->Placements[0].GetLocation();
				bHaveDest = true;
			}
			if (bHaveDest && !FeedTransport.IsEmpty())
			{
				// One ghost run per feed hop: tap → waypoints → the destination port.
				TArray<FVector> Points;
				Points.Add(Proposal->TapPointCm);
				Points.Append(Proposal->TapChainPointsCm);
				Points.Add(DestCm);
				for (int32 i = 0; i + 1 < Points.Num(); ++i)
				{
					FAIDAGhostRun& Run = View.GhostRuns.AddDefaulted_GetRef();
					Run.RecipeClassPath = FeedTransport;
					Run.FromCm = Points[i];
					Run.FromNormal = (Points[i + 1] - Points[i]).GetSafeNormal(UE_SMALL_NUMBER, Proposal->TapDirCm);
					Run.ToCm = Points[i + 1];
					Run.ToNormal = (i + 2 == Points.Num()) ? DestNormal : -Run.FromNormal;
				}
			}
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

void UAIDAOrchestrator::SupersedeProposal(const FGuid& ProposalId)
{
	Actions.Store().Remove(ProposalId);
	if (AAIDAProposalRelay* R = GetProposalRelay())
	{
		R->ServerRemoveProposal(ProposalId);
	}
	UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s superseded by a revision."),
		*ProposalId.ToString(EGuidFormats::DigitsWithHyphens));
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
		TEXT("PROPOSE placing buildables. Nothing is built until a player with act permission approves. Returns a dry-run report (count, cost, validity, the RESOLVED origin in metres) and a proposalId. TWO SPEC FORMS. v1 single grid: {version:1, buildable:'display name', origin?:{x,y,z metres}, yawDeg:0|90|180|270, grid:{countX,countY,stepX?,stepY?}, followTerrain?:bool} — ALWAYS the form for a row/line/grid of IDENTICAL buildables ('three refineries in a line' = v1 with grid countX 3), never a composite. v2 COMPOSITE — the form for MULTI-PART structures with DIFFERENT parts (buildings, reference-image reconstructions): {version:2, origin?:{x,y,z}, yawDeg:0, parts:[{buildable:'display name', at:{x,y,z metres RELATIVE to the origin — z stacks upward}, yawDeg?:0, grid?:{countX,countY,stepX?,stepY?}}, ...]} — up to 32 parts place together, preview together, and get ONE approval; part offsets rotate with the composite yawDeg. OMIT origin to build where the requesting player is aiming (falls back to their position) — never ask the player for coordinates. OMIT stepX/stepY — they default to the buildable's real footprint (a 'Foundation (2 m)' tile is 8x8 m; the 2 m is THICKNESS — stack floors with at.z, e.g. next storey at z 4 on '4 m' walls). v1 grids are FLAT at the origin's height (followTerrain:true ONLY if the player asks to trace the ground). v1 machines that need power are wired AUTOMATICALLY (poles + lines + grid tie; power:false to skip; pole:'display name' to override) — v2 composites are NOT auto-wired: include poles as parts and wire later. Costs are paid from central storage (dimensional depot) FIRST and then the REQUESTING PLAYER'S INVENTORY — materials in the player's pockets count toward affordability; never tell a player to move items into storage first. BLOCKED GROUND NEVER FAILS A PROPOSAL: uneven terrain/obstructions produce a proposal anyway (invalidCount + firstFailures in the result) with the ghost preview up — tell the player to NUDGE the ghost onto clear ground before approving (approving as-is builds only the valid placements and refunds the rest); never report blocked ground as 'can't build there'. REVISING: when the player asks to CHANGE a pending proposal ('make it 20', 'use mk.2', 'actually 2 rows'), read its spec from get_proposal_status, merge the change, and call this tool again with replaceProposalId set to the old id — the ghost swaps to the revision, no reject needed. To ADD A MANIFOLD to a pending build use propose_manifold with forProposalId instead."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"The versioned build spec (see tool description)."},"replaceProposalId":{"type":"string","description":"Optional: a PENDING proposal this one revises — it is retired and its ghost swaps to this proposal atomically. Use when the player asks to change a proposed build (get its spec from get_proposal_status, merge the change, re-propose here)."}},"required":["spec"]})"),
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

			// Revision (revise-by-prompt): the new proposal replaces a pending one on SUCCESS —
			// validated up front so a typo'd id fails before any dry-run work, resolved again
			// (supersede) only after everything else succeeded.
			FGuid ReplaceId;
			bool bReplacedHadManifolds = false;
			{
				FString ReplaceIdStr;
				Args->TryGetStringField(TEXT("replaceProposalId"), ReplaceIdStr);
				if (!ReplaceIdStr.TrimStartAndEnd().IsEmpty())
				{
					const FAIDAProposal* Replaced = FGuid::Parse(ReplaceIdStr.TrimStartAndEnd(), ReplaceId)
						? Actions.Store().Find(ReplaceId) : nullptr;
					if (!Replaced || Replaced->State != EAIDAProposalState::Pending)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("replaceProposalId doesn't match a pending proposal (it may have been decided or expired) — call get_proposal_status"), {}));
					}
					bReplacedHadManifolds = Replaced->ManifoldSets.Num() > 0;
				}
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
			FAIDADryRunResult FirstFailRun;          // the PREFERRED failing candidate (last resort — still ghosts)
			TArray<FTransform> FirstFailPlacements;
			TArray<int32> FirstFailPartIndex;
			FVector FirstFailOriginCm = FVector::ZeroVector;
			bool bPlaced = false;
			bool bHaveFallback = false;
			bool bHaveFirstFail = false;
			for (const FVector& OriginCm : OriginCandidatesCm)
			{
				TArray<int32> AttemptPartIndex;
				TArray<FTransform> Attempt = ExpandAt(OriginCm, AttemptPartIndex);
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
					FirstFailPlacements = MoveTemp(Attempt);
					FirstFailPartIndex = MoveTemp(AttemptPartIndex);
					FirstFailOriginCm = OriginCm;
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
			if (!bPlaced && bHaveFirstFail)
			{
				// Every candidate has hard-blocked tiles (uneven terrain, water, a cliff edge…).
				// The ghost STILL goes up (user rule: validation trouble never prohibits a proposal)
				// — the failures ride along as an advisory and the player nudges it somewhere valid;
				// execute re-validates per tile, skips what still fails, and refunds the skips.
				Placements = MoveTemp(FirstFailPlacements);
				PlacementPartIndex = MoveTemp(FirstFailPartIndex);
				DryRun = MoveTemp(FirstFailRun);
				Spec.OriginM = FirstFailOriginCm / AIDAMetersToCm;
				bPlaced = true;
			}
			if (!bPlaced)
			{
				// Unreachable in practice (every candidate lands in one of the buckets above) —
				// defensive against an empty candidate list.
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
					TEXT("no placement candidate could be evaluated"), {}));
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
			Proposal.InvalidCount = DryRun.Failures.Num(); // advisory — never blocks the ghost

			// Everything validated: the revision may now retire the proposal it replaces (freeing
			// its pending-cap slot) — the new ghost takes over in the same publish cycle.
			if (ReplaceId.IsValid())
			{
				SupersedeProposal(ReplaceId);
			}

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			if (!Actions.Store().Add(Proposal, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored: %s (by %s, %d blocked placement(s))"),
				*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal.Summary, *Ctx.Author, Proposal.InvalidCount);
			PublishProposal(Proposal.Id);
			FString Announce = FString::Printf(TEXT("AIDA proposes (for %s%s): %s — cost %s. Awaiting approval."),
				*Ctx.Author, ReplaceId.IsValid() ? TEXT(", revised") : TEXT(""),
				*Proposal.Summary, *AIDACostSummaryString(Proposal.Cost));
			if (Proposal.InvalidCount > 0)
			{
				Announce += FString::Printf(TEXT(" NOTE: %d of %d placement(s) are blocked at this spot — nudge the ghost onto clear ground before approving (approving as-is builds only the valid ones and refunds the rest)."),
					Proposal.InvalidCount, Proposal.Placements.Num());
			}
			if (bReplacedHadManifolds)
			{
				Announce += TEXT(" NOTE: the previous revision's manifolds were dropped — re-add them with propose_manifold (forProposalId).");
			}
			AnnounceSystem(Announce);

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds, DryRun.bAffordable, 0.0,
				&Spec.OriginM, DryRun.Failures.Num() > 0 ? &DryRun.Failures : nullptr));
		}
	});

	// propose_extend_foundations: census the slab underfoot/at-aim -> pure per-lane extension ->
	// composite-style proposal (per-class placement runs) through the standard approval flow.
	Tools.Register({
		TEXT("propose_extend_foundations"),
		TEXT("PROPOSE extending the foundation slab the player is standing on or aiming at. The server finds the CONTIGUOUS slab (its foundation types, lattice, and height steps), then extends EVERY lane of the slab's full width by spec.count tiles from that lane's own edge — same foundation class, height and rotation as the edge it grows from, so ragged edges, mixed types and terrain-following steps all extend correctly. Direction: spec.direction ('north'|'south'|'east'|'west') ONLY when the player names one; otherwise the server uses their LOOK direction when standing on the slab, or the aimed edge's outward face when they aim at the slab from beside it — never ask for a direction. Spec: {version:1, count: tiles to extend (default 1), direction?}. ALWAYS use this when the player says to extend/continue/grow existing foundations, a floor, or a platform — NEVER assemble an extension with propose_build (it cannot find the existing slab's lattice). Returns the dry-run (count, cost, validity) + proposalId; same approval flow as propose_build (ghost preview, nudge, /aida approve). REVISING: pass replaceProposalId to swap a pending extension ('make it 20' = call again with count 20)."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"{version:1, count?: tiles to extend (default 1), direction?: 'north'|'south'|'east'|'west' (omit = the player's stance/aim decides)}"},"replaceProposalId":{"type":"string","description":"Optional: a PENDING proposal this one revises — it is retired and its ghost swaps to this proposal atomically."}},"required":["spec"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			SweepProposals();

			const TSharedPtr<FJsonObject>* SpecObj = nullptr;
			Args->TryGetObjectField(TEXT("spec"), SpecObj);
			int32 Count = 1;
			FString Direction;
			if (SpecObj && SpecObj->IsValid())
			{
				double CountNum = 1.0;
				if ((*SpecObj)->TryGetNumberField(TEXT("count"), CountNum))
				{
					Count = FMath::RoundToInt32(CountNum);
				}
				(*SpecObj)->TryGetStringField(TEXT("direction"), Direction);
			}
			// MaxProposalItems 0 = unlimited (repo-wide convention; live-verify: a 0 cap read as
			// "between 1 and 0" and rejected every extension).
			if (Count < 1 || (Config.Actions.MaxProposalItems > 0 && Count > Config.Actions.MaxProposalItems))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
					Config.Actions.MaxProposalItems > 0
						? FString::Printf(TEXT("count must be between 1 and %d"), Config.Actions.MaxProposalItems)
						: FString(TEXT("count must be at least 1")), {}));
			}

			// Revision id validated up front (mirrors propose_build).
			FGuid ReplaceId;
			{
				FString ReplaceIdStr;
				Args->TryGetStringField(TEXT("replaceProposalId"), ReplaceIdStr);
				if (!ReplaceIdStr.TrimStartAndEnd().IsEmpty())
				{
					const FAIDAProposal* Replaced = FGuid::Parse(ReplaceIdStr.TrimStartAndEnd(), ReplaceId)
						? Actions.Store().Find(ReplaceId) : nullptr;
					if (!Replaced || Replaced->State != EAIDAProposalState::Pending)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("replaceProposalId doesn't match a pending proposal (it may have been decided or expired) — call get_proposal_status"), {}));
					}
				}
			}

			FAIDASlabCensus Census;
			if (!FAIDAActionSeam::CensusFoundationSlab(this, Ctx.PlayerId, Direction, Census))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Census.Error, {}));
			}
			const FAIDASlabExtensionPlan Plan = AIDAActionSpec::PlanSlabExtension(Census.Cells,
				Census.ExtendDir, Count, Config.Actions.MaxProposalItems);
			if (!Plan.Error.IsEmpty())
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Plan.Error, {}));
			}

			// Placements grouped by part (contiguous runs) — the composite machinery (dry-run,
			// ghost preview, batched execute, even pending manifolds) handles mixed classes for free.
			TArray<int32> UsedParts;
			for (const FAIDASlabCell& Cell : Plan.NewCells)
			{
				UsedParts.AddUnique(Cell.Part);
			}
			TArray<FString> PartRecipePaths;
			TArray<FString> PartNamesUsed;
			for (const int32 Part : UsedParts)
			{
				PartRecipePaths.Add(Census.PartRecipePaths.IsValidIndex(Part) ? Census.PartRecipePaths[Part] : FString());
				PartNamesUsed.Add(Census.PartNames.IsValidIndex(Part) ? Census.PartNames[Part] : TEXT("Foundation"));
			}
			TArray<FTransform> Placements;
			TArray<int32> PlacementPartIndex;
			TArray<int32> PartCounts;
			PartCounts.SetNumZeroed(UsedParts.Num());
			const FRotator Rotation(0.0, Census.YawDeg, 0.0);
			for (int32 NewPart = 0; NewPart < UsedParts.Num(); ++NewPart)
			{
				for (const FAIDASlabCell& Cell : Plan.NewCells)
				{
					if (Cell.Part != UsedParts[NewPart]) { continue; }
					FVector Loc = Census.OriginCm
						+ Census.AxisU * (Cell.Coord.X * Census.StepCm)
						+ Census.AxisV * (Cell.Coord.Y * Census.StepCm);
					Loc.Z = Cell.ZCm;
					Placements.Emplace(Rotation, Loc);
					PlacementPartIndex.Add(NewPart);
					++PartCounts[NewPart];
				}
			}

			FAIDADryRunResult DryRun;
			if (!FAIDAActionSeam::DryRunBuildParts(this, PartRecipePaths, PlacementPartIndex, Placements, DryRun, Ctx.PlayerId))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(DryRun.Error, {}));
			}

			FAIDAProposal Proposal;
			Proposal.Id = FGuid::NewGuid();
			{
				// The stored spec is descriptive (this tool has no grid to re-expand) — revisions
				// re-census live instead of merging the old spec.
				TSharedRef<FJsonObject> StoredSpec = MakeShared<FJsonObject>();
				StoredSpec->SetStringField(TEXT("tool"), TEXT("propose_extend_foundations"));
				StoredSpec->SetNumberField(TEXT("count"), Count);
				StoredSpec->SetStringField(TEXT("direction"), Census.DirectionNote);
				Proposal.SpecJson = AIDAToCompactJson(StoredSpec);
			}
			Proposal.RequesterId = Ctx.PlayerId;
			Proposal.RequesterName = Ctx.Author;
			Proposal.Placements = Placements;
			Proposal.RecipeClassPath = PartRecipePaths[0];
			Proposal.PartRecipePaths = PartRecipePaths;
			Proposal.PlacementPartIndex = MoveTemp(PlacementPartIndex);
			Proposal.Cost = DryRun.Cost;
			{
				FString Kinds;
				for (int32 i = 0; i < PartNamesUsed.Num(); ++i)
				{
					Kinds += FString::Printf(TEXT("%s%d x %s"), i > 0 ? TEXT(", ") : TEXT(""), PartCounts[i], *PartNamesUsed[i]);
				}
				Proposal.Summary = FString::Printf(TEXT("extend the foundation slab %d tile(s) %s (%s)"),
					Count, *Census.DirectionNote, *Kinds);
			}
			Proposal.InvalidCount = DryRun.Failures.Num(); // advisory — never blocks the ghost

			if (ReplaceId.IsValid())
			{
				SupersedeProposal(ReplaceId);
			}
			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			FString Error;
			if (!Actions.Store().Add(Proposal, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored: %s (by %s, %d blocked placement(s))"),
				*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal.Summary, *Ctx.Author, Proposal.InvalidCount);
			PublishProposal(Proposal.Id);
			FString Announce = FString::Printf(TEXT("AIDA proposes (for %s%s): %s — cost %s. Awaiting approval."),
				*Ctx.Author, ReplaceId.IsValid() ? TEXT(", revised") : TEXT(""),
				*Proposal.Summary, *AIDACostSummaryString(Proposal.Cost));
			if (Proposal.InvalidCount > 0)
			{
				Announce += FString::Printf(TEXT(" NOTE: %d of %d placement(s) are blocked at this spot — nudge the ghost onto clear ground before approving (approving as-is builds only the valid ones and refunds the rest)."),
					Proposal.InvalidCount, Proposal.Placements.Num());
			}
			AnnounceSystem(Announce);

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds,
				DryRun.bAffordable, 0.0, nullptr, DryRun.Failures.Num() > 0 ? &DryRun.Failures : nullptr));
		}
	});

	// propose_add_walls: census the slab underfoot/at-aim -> perimeter walls per floor (+ decks
	// between floors) -> composite-style proposal through the standard approval flow.
	Tools.Register({
		TEXT("propose_add_walls"),
		TEXT("PROPOSE walling in the foundation slab the player is standing on, hovering over, or aiming at — walls go up around the ENTIRE contiguous slab's perimeter (courtyard holes included), material-matched to the foundations (concrete foundations get concrete walls, grip metal gets steel; fallback is the basic FICSIT wall). FLOORS: spec.floors is an array, one entry per floor bottom-up; each floor's walls are stacked 4 m courses, tall enough to contain {contains:'buildable display name'} (its clearance height decides) and/or at least {heightM: metres} — an empty entry means one 4 m course. Between consecutive floors a DECK of foundations (mirroring the slab cell for cell, same foundation types) is added so the next floor has something to stand on; the top stays OPEN (no roof). Examples: 'add walls' -> {floors:[{}]}; 'walls 12 m tall' -> {floors:[{heightM:12}]}; '2 floors, refineries below, foundries above' -> {floors:[{contains:'Refinery'},{contains:'Foundry'}]}. ALWAYS use this when the player says to add walls / wall in / enclose / build floors on existing foundations — NEVER assemble walls with propose_build (it cannot find the slab's lattice), and never ask which foundations or for a location (their stance/aim decides). Returns the dry-run (count, cost, validity) + proposalId; same approval flow as propose_build (ghost preview, nudge, /aida approve). Blocked placements (e.g. where a belt crosses the perimeter) are advisory — approving builds the valid ones and refunds the rest, leaving a natural gap. REVISING: pass replaceProposalId to swap a pending walls proposal ('make it 2 floors' = call again with both floors)."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"{version:1, floors?: [{contains?: 'buildable display name this floor must be tall enough to contain', heightM?: metres}] — one entry per floor bottom-up; omit floors for a single 4 m wall course}"},"replaceProposalId":{"type":"string","description":"Optional: a PENDING proposal this one revises — it is retired and its ghost swaps to this proposal atomically."}},"required":["spec"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			SweepProposals();

			// Per floor: courses = enough 4 m walls for the named machine's clearance height
			// and/or the explicit metres; an empty entry is one course.
			struct FFloorSpec { int32 Courses = 1; FString Contains; };
			TArray<FFloorSpec> Floors;
			{
				const TSharedPtr<FJsonObject>* SpecObj = nullptr;
				Args->TryGetObjectField(TEXT("spec"), SpecObj);
				const TArray<TSharedPtr<FJsonValue>>* FloorsArr = nullptr;
				if (SpecObj && SpecObj->IsValid())
				{
					(*SpecObj)->TryGetArrayField(TEXT("floors"), FloorsArr);
				}
				if (FloorsArr)
				{
					if (FloorsArr->Num() > 8)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("at most 8 floors per proposal"), {}));
					}
					for (const TSharedPtr<FJsonValue>& Value : *FloorsArr)
					{
						const TSharedPtr<FJsonObject>* FloorObj = nullptr;
						FFloorSpec Floor;
						double NeedM = 0.0;
						if (Value.IsValid() && Value->TryGetObject(FloorObj) && FloorObj->IsValid())
						{
							(*FloorObj)->TryGetNumberField(TEXT("heightM"), NeedM);
							FString Contains;
							if ((*FloorObj)->TryGetStringField(TEXT("contains"), Contains) && !Contains.TrimStartAndEnd().IsEmpty())
							{
								FAIDARecipeResolution Machine;
								if (!FAIDAActionSeam::ResolveBuildRecipe(this, Contains, Machine))
								{
									FString Msg = FString::Printf(TEXT("floors[%d]: no unlocked buildable matches '%s'"), Floors.Num(), *Contains);
									if (Machine.Suggestions.Num() > 0)
									{
										Msg += FString::Printf(TEXT(" — closest: %s"), *FString::Join(Machine.Suggestions, TEXT(", ")));
									}
									return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
								}
								Floor.Contains = Machine.DisplayName;
								NeedM = FMath::Max(NeedM, Machine.FootprintZM);
							}
						}
						Floor.Courses = FMath::Max(1, FMath::CeilToInt32(NeedM / 4.0 - UE_KINDA_SMALL_NUMBER));
						if (Floor.Courses > 50)
						{
							return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(FString::Printf(
								TEXT("floors[%d] would be %d m tall — that can't be right"), Floors.Num(), Floor.Courses * 4), {}));
						}
						Floors.Add(Floor);
					}
				}
				if (Floors.Num() == 0) { Floors.Add(FFloorSpec()); }
			}

			// Revision id validated up front (mirrors propose_build).
			FGuid ReplaceId;
			{
				FString ReplaceIdStr;
				Args->TryGetStringField(TEXT("replaceProposalId"), ReplaceIdStr);
				if (!ReplaceIdStr.TrimStartAndEnd().IsEmpty())
				{
					const FAIDAProposal* Replaced = FGuid::Parse(ReplaceIdStr.TrimStartAndEnd(), ReplaceId)
						? Actions.Store().Find(ReplaceId) : nullptr;
					if (!Replaced || Replaced->State != EAIDAProposalState::Pending)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("replaceProposalId doesn't match a pending proposal (it may have been decided or expired) — call get_proposal_status"), {}));
					}
				}
			}

			// Walls have no direction — the whole perimeter is the target, so the census only
			// needs the stance/aim anchor (the direction hint stays empty).
			FAIDASlabCensus Census;
			if (!FAIDAActionSeam::CensusFoundationSlab(this, Ctx.PlayerId, FString(), Census))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Census.Error, {}));
			}
			TArray<int32> FloorCourses;
			for (const FFloorSpec& Floor : Floors) { FloorCourses.Add(Floor.Courses); }
			const FAIDAWallPlan Plan = AIDAActionSpec::PlanPerimeterWalls(Census.Cells, FloorCourses,
				Config.Actions.MaxProposalItems);
			if (!Plan.Error.IsEmpty())
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Plan.Error, {}));
			}

			// Proposal part table: one material-matched wall recipe per foundation class, plus the
			// decks' own foundation recipes — deduped by path (several foundation classes can share
			// one wall recipe).
			TArray<FString> PartRecipePaths;
			TArray<FString> PartNamesUsed;
			TMap<FString, int32> PartIndexByPath;
			const auto PartFor = [&PartRecipePaths, &PartNamesUsed, &PartIndexByPath](const FString& Path, const FString& Name) -> int32
			{
				if (const int32* Existing = PartIndexByPath.Find(Path)) { return *Existing; }
				const int32 Index = PartRecipePaths.Num();
				PartRecipePaths.Add(Path);
				PartNamesUsed.Add(Name);
				PartIndexByPath.Add(Path, Index);
				return Index;
			};
			TMap<int32, int32> WallPartByFoundationPart;
			for (const FAIDAWallSegment& Segment : Plan.Walls)
			{
				if (WallPartByFoundationPart.Contains(Segment.Part)) { continue; }
				const FString FoundationPath = Census.PartRecipePaths.IsValidIndex(Segment.Part)
					? Census.PartRecipePaths[Segment.Part] : FString();
				FString WallPath, WallName;
				if (!FAIDAActionSeam::ResolveWallRecipe(this, FoundationPath, WallPath, WallName))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("no wall recipe is unlocked yet — walls need at least the basic 4 m wall"), {}));
				}
				WallPartByFoundationPart.Add(Segment.Part, PartFor(WallPath, WallName));
			}

			// World placements, all per cell so terrain steps and mixed thicknesses stay coherent
			// per column: wall base = the cell's TOP face (pivot + half thickness), each floor adds
			// its courses, each deck below adds the cell's own thickness. Wall yaw faces its open
			// side (local +X outward); the deck keeps the slab's yaw.
			const double WallCourseCm = 400.0; // walls stack in 4 m courses
			TArray<double> FloorBaseOffsetCm;  // per floor: wall-height offset above the cell top
			{
				double Offset = 0.0;
				for (const FFloorSpec& Floor : Floors)
				{
					FloorBaseOffsetCm.Add(Offset);
					Offset += Floor.Courses * WallCourseCm;
				}
			}
			const auto CellCenter = [&Census](const FIntPoint& Coord)
			{
				return Census.OriginCm
					+ Census.AxisU * (Coord.X * Census.StepCm)
					+ Census.AxisV * (Coord.Y * Census.StepCm);
			};
			const auto PartHeight = [&Census](int32 Part)
			{
				return Census.PartHeightCm.IsValidIndex(Part) ? Census.PartHeightCm[Part] : 100.0;
			};
			struct FItem { int32 Part; FTransform T; };
			TArray<FItem> Items;
			Items.Reserve(Plan.Walls.Num() + Plan.Decks.Num());
			for (const FAIDAWallSegment& Segment : Plan.Walls)
			{
				const FVector WorldOut = Census.AxisU * Segment.OutDir.X + Census.AxisV * Segment.OutDir.Y;
				const double H = PartHeight(Segment.Part);
				FVector Loc = CellCenter(Segment.Cell) + WorldOut * (Census.StepCm * 0.5);
				Loc.Z = Segment.ZCm + H * 0.5
					+ FloorBaseOffsetCm[Segment.Floor] + Segment.Floor * H
					+ Segment.Course * WallCourseCm;
				const double YawDeg = FMath::RadiansToDegrees(FMath::Atan2(WorldOut.Y, WorldOut.X));
				Items.Add({ WallPartByFoundationPart.FindChecked(Segment.Part), FTransform(FRotator(0.0, YawDeg, 0.0), Loc) });
			}
			for (const FAIDAWallDeckCell& Deck : Plan.Decks)
			{
				const double H = PartHeight(Deck.Part);
				FVector Loc = CellCenter(Deck.Cell);
				Loc.Z = Deck.ZCm + H * 0.5
					+ FloorBaseOffsetCm[Deck.Floor] + Deck.Floor * H
					+ Floors[Deck.Floor].Courses * WallCourseCm
					+ H * 0.5;
				const int32 DeckPart = PartFor(
					Census.PartRecipePaths.IsValidIndex(Deck.Part) ? Census.PartRecipePaths[Deck.Part] : FString(),
					Census.PartNames.IsValidIndex(Deck.Part) ? Census.PartNames[Deck.Part] : TEXT("Foundation"));
				Items.Add({ DeckPart, FTransform(FRotator(0.0, Census.YawDeg, 0.0), Loc) });
			}

			// Placements grouped by part (contiguous runs) — the composite machinery (dry-run,
			// ghost preview, batched execute) handles mixed recipes for free.
			TArray<FTransform> Placements;
			TArray<int32> PlacementPartIndex;
			TArray<int32> PartCounts;
			PartCounts.SetNumZeroed(PartRecipePaths.Num());
			Placements.Reserve(Items.Num());
			PlacementPartIndex.Reserve(Items.Num());
			for (int32 Part = 0; Part < PartRecipePaths.Num(); ++Part)
			{
				for (const FItem& Item : Items)
				{
					if (Item.Part != Part) { continue; }
					Placements.Add(Item.T);
					PlacementPartIndex.Add(Part);
					++PartCounts[Part];
				}
			}

			FAIDADryRunResult DryRun;
			if (!FAIDAActionSeam::DryRunBuildParts(this, PartRecipePaths, PlacementPartIndex, Placements, DryRun, Ctx.PlayerId))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(DryRun.Error, {}));
			}

			FAIDAProposal Proposal;
			Proposal.Id = FGuid::NewGuid();
			{
				// The stored spec is descriptive (revisions re-census live, like extend_foundations).
				TSharedRef<FJsonObject> StoredSpec = MakeShared<FJsonObject>();
				StoredSpec->SetStringField(TEXT("tool"), TEXT("propose_add_walls"));
				TArray<TSharedPtr<FJsonValue>> FloorsJson;
				for (const FFloorSpec& Floor : Floors)
				{
					TSharedRef<FJsonObject> FloorJson = MakeShared<FJsonObject>();
					FloorJson->SetNumberField(TEXT("courses"), Floor.Courses);
					if (!Floor.Contains.IsEmpty()) { FloorJson->SetStringField(TEXT("contains"), Floor.Contains); }
					FloorsJson.Add(MakeShared<FJsonValueObject>(FloorJson));
				}
				StoredSpec->SetArrayField(TEXT("floors"), FloorsJson);
				Proposal.SpecJson = AIDAToCompactJson(StoredSpec);
			}
			Proposal.RequesterId = Ctx.PlayerId;
			Proposal.RequesterName = Ctx.Author;
			Proposal.Placements = Placements;
			Proposal.RecipeClassPath = PartRecipePaths[0];
			Proposal.PartRecipePaths = PartRecipePaths;
			Proposal.PlacementPartIndex = MoveTemp(PlacementPartIndex);
			Proposal.Cost = DryRun.Cost;
			{
				FString FloorsDesc;
				for (int32 i = 0; i < Floors.Num(); ++i)
				{
					FloorsDesc += FString::Printf(TEXT("%s%d m%s"), i > 0 ? TEXT(" + ") : TEXT(""), Floors[i].Courses * 4,
						Floors[i].Contains.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (fits %s)"), *Floors[i].Contains));
				}
				FString Kinds;
				for (int32 i = 0; i < PartNamesUsed.Num(); ++i)
				{
					Kinds += FString::Printf(TEXT("%s%d x %s"), i > 0 ? TEXT(", ") : TEXT(""), PartCounts[i], *PartNamesUsed[i]);
				}
				Proposal.Summary = FString::Printf(TEXT("wall in the foundation slab — %d floor(s): %s (%s)"),
					Floors.Num(), *FloorsDesc, *Kinds);
			}
			Proposal.InvalidCount = DryRun.Failures.Num(); // advisory — never blocks the ghost

			if (ReplaceId.IsValid())
			{
				SupersedeProposal(ReplaceId);
			}
			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			FString Error;
			if (!Actions.Store().Add(Proposal, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored: %s (by %s, %d blocked placement(s))"),
				*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal.Summary, *Ctx.Author, Proposal.InvalidCount);
			PublishProposal(Proposal.Id);
			FString Announce = FString::Printf(TEXT("AIDA proposes (for %s%s): %s — cost %s. Awaiting approval."),
				*Ctx.Author, ReplaceId.IsValid() ? TEXT(", revised") : TEXT(""),
				*Proposal.Summary, *AIDACostSummaryString(Proposal.Cost));
			if (Proposal.InvalidCount > 0)
			{
				Announce += FString::Printf(TEXT(" NOTE: %d of %d placement(s) are blocked at this spot — approving as-is builds only the valid ones and refunds the rest (a belt crossing the perimeter keeps its gap)."),
					Proposal.InvalidCount, Proposal.Placements.Num());
			}
			AnnounceSystem(Announce);

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds,
				DryRun.bAffordable, 0.0, nullptr, DryRun.Failures.Num() > 0 ? &DryRun.Failures : nullptr));
		}
	});

	// P8 Slice 2 in-place mutations: propose -> approve -> mutate, journaled as before/after
	// values (undo restores). No placements, so no ghost — the summary is the preview.
	{
		// Selector parsing + center defaulting shared by the three mutation tools.
		const auto ParseMutationSelector = [this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx,
			FAIDADismantleSpec& OutSelector, FString& OutError) -> bool
		{
			const TSharedPtr<FJsonObject>* SelectorObj = nullptr;
			Args->TryGetObjectField(TEXT("selector"), SelectorObj);
			if (!AIDAActionSpec::ParseDismantleSpec(SelectorObj ? *SelectorObj : nullptr,
				Config.Actions.MaxProposalItems, OutSelector, OutError))
			{
				return false;
			}
			if (!OutSelector.bHasCenter)
			{
				FVector AimCm;
				if (FAIDAActionSeam::ResolveAimPoint(this, Ctx.PlayerId, AimCm))
				{
					OutSelector.CenterM = AimCm / 100.0;
				}
				else if (Ctx.bHasLocation)
				{
					OutSelector.CenterM = Ctx.Location / 100.0;
				}
				else
				{
					OutError = TEXT("couldn't resolve the requesting player's aim or position — pass an explicit 'center' {x,y in metres}");
					return false;
				}
				OutSelector.bHasCenter = true; // the struct is stored on the proposal — no JSON write-back needed
			}
			return true;
		};
		// Store + publish + announce, shared tail. Returns the tool result JSON.
		const auto StoreMutationProposal = [this](FAIDAProposal&& Proposal, const FAIDAToolContext& Ctx,
			const FGuid& ReplaceId, const FString& Warning) -> FAIDAToolResult
		{
			Proposal.Id = FGuid::NewGuid();
			Proposal.RequesterId = Ctx.PlayerId;
			Proposal.RequesterName = Ctx.Author;
			if (ReplaceId.IsValid())
			{
				SupersedeProposal(ReplaceId);
			}
			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			FString Error;
			if (!Actions.Store().Add(Proposal, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored: %s (by %s)"),
				*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal.Summary, *Ctx.Author);
			PublishProposal(Proposal.Id);
			FString Announce = FString::Printf(TEXT("AIDA proposes (for %s%s): %s. Awaiting approval."),
				*Ctx.Author, ReplaceId.IsValid() ? TEXT(", revised") : TEXT(""), *Proposal.Summary);
			if (!Warning.IsEmpty()) { Announce += FString::Printf(TEXT(" NOTE: %s"), *Warning); }
			AnnounceSystem(Announce);

			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("proposalId"), Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens));
			Root->SetNumberField(TEXT("targets"), Proposal.TargetCount);
			Root->SetStringField(TEXT("summary"), Proposal.Summary);
			Root->SetNumberField(TEXT("expiresInSeconds"), Config.Actions.TtlSeconds);
			if (!Warning.IsEmpty()) { Root->SetStringField(TEXT("note"), Warning); }
			return FAIDAToolResult::Ok(AIDAToCompactJson(Root));
		};
		// replaceProposalId validation, shared (mirrors propose_build).
		const auto ParseReplaceId = [this](const TSharedRef<FJsonObject>& Args, FGuid& OutReplaceId, FString& OutError) -> bool
		{
			FString ReplaceIdStr;
			Args->TryGetStringField(TEXT("replaceProposalId"), ReplaceIdStr);
			if (ReplaceIdStr.TrimStartAndEnd().IsEmpty()) { return true; }
			const FAIDAProposal* Replaced = FGuid::Parse(ReplaceIdStr.TrimStartAndEnd(), OutReplaceId)
				? Actions.Store().Find(OutReplaceId) : nullptr;
			if (!Replaced || Replaced->State != EAIDAProposalState::Pending)
			{
				OutError = TEXT("replaceProposalId doesn't match a pending proposal (it may have been decided or expired) — call get_proposal_status");
				return false;
			}
			return true;
		};

		Tools.Register({
			TEXT("propose_set_clocks"),
			TEXT("PROPOSE changing machine clock speeds (SetPendingPotential). Two modes: (1) selector + percent — every matching machine gets that clock; (2) {advised:true} — apply get_clock_advice's per-machine recommendations exactly ('underclock what you recommended' = one call). v1 caps at 100% (overclocking needs power shards — not handled yet). Selector: {buildable?: name substring, center?: {x,y,z metres}, radiusM, maxCount?} — OMIT center to use the player's aim/position, never ask for coordinates. Nothing changes until approved; undo restores the previous clocks. Returns targets + proposalId (no ghost — the summary is the preview). REVISING: pass replaceProposalId."),
			TEXT(R"({"type":"object","properties":{"selector":{"type":"object","description":"{buildable?, center?:{x,y,z metres}, radiusM, maxCount?} — machines to re-clock (ignored when advised:true)."},"percent":{"type":"number","description":"Target clock percent, 1-100."},"advised":{"type":"boolean","description":"true = apply the live get_clock_advice recommendations (per-machine percents)."},"replaceProposalId":{"type":"string","description":"Optional: a PENDING proposal this one revises."}},"required":[]})"),
			EAIDAToolTier::Act,
			[this, ParseMutationSelector, StoreMutationProposal, ParseReplaceId](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
			{
				SweepProposals();
				FGuid ReplaceId;
				FString Error;
				if (!ParseReplaceId(Args, ReplaceId, Error))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
				}

				FAIDAProposal Proposal;
				Proposal.MutationKind = EAIDAMutationKind::Clock;

				bool bAdvised = false;
				Args->TryGetBoolField(TEXT("advised"), bAdvised);
				if (bAdvised)
				{
					UWorld* World = GetWorld();
					const double Now = World ? World->GetTimeSeconds() : 0.0;
					const FAIDAFactorySnapshot& Snapshot = FactoryIndex.GetSnapshot(World, Now);
					const FAIDAClockAdviceReport Advice = FAIDAFactoryAggregator::BuildClockAdvice(Snapshot.Machines);
					if (Advice.Advice.Num() == 0)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("no underclock advice right now — every producer is either busy or already tuned"), {}));
					}
					// Advice rows are snapshot data — bind each to its live actor by location now;
					// approve re-checks the weak ptrs (dead machines just drop out).
					AFGBuildableSubsystem* Subsystem = World ? AFGBuildableSubsystem::Get(World) : nullptr;
					if (!Subsystem)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(TEXT("no buildables to search"), {}));
					}
					for (const FAIDAClockAdvice& Entry : Advice.Advice)
					{
						for (AFGBuildable* Buildable : Subsystem->GetAllBuildablesRef())
						{
							AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Buildable);
							if (!Factory || !Factory->GetCanChangePotential()) { continue; }
							if (!Factory->GetActorLocation().Equals(Entry.Location, 100.0)) { continue; }
							FAIDAMutationTarget Target;
							Target.Actor = Factory;
							Target.Detail = Entry.BuildingClass;
							Target.ClockPct = FMath::Clamp(static_cast<double>(Entry.SuggestedClock) * 100.0, 1.0, 100.0);
							Proposal.MutationAdvised.Add(MoveTemp(Target));
							break;
						}
					}
					if (Proposal.MutationAdvised.Num() == 0)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("couldn't bind the advice to live machines — re-run get_clock_advice and try again"), {}));
					}
					Proposal.TargetCount = Proposal.MutationAdvised.Num();
					Proposal.Summary = FString::Printf(TEXT("underclock %d machine(s) as advised (saves ~%.0f MW)"),
						Proposal.TargetCount, Advice.TotalSavableMW);
				}
				else
				{
					double Percent = 0.0;
					if (!Args->TryGetNumberField(TEXT("percent"), Percent))
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("pass percent (1-100) or advised:true"), {}));
					}
					if (Percent > 100.0)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("clocks above 100% need power shards — not supported yet; use 1-100"), {}));
					}
					Proposal.MutationClockPct = FMath::Clamp(Percent, 1.0, 100.0);

					if (!ParseMutationSelector(Args, Ctx, Proposal.MutationSelector, Error))
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
					}
					TArray<FAIDAMutationTarget> Targets;
					if (!FAIDAActionSeam::ResolveMutationTargets(this, EAIDAMutationKind::Clock,
						Proposal.MutationSelector, FString(), Targets, Error))
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
					}
					if (Targets.Num() == 0)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("no machine with an adjustable clock matches the selector"), {}));
					}
					Proposal.TargetCount = Targets.Num();
					Proposal.Summary = FString::Printf(TEXT("set the clock to %.4g%% on %d machine(s)%s%s"),
						Proposal.MutationClockPct, Targets.Num(),
						Proposal.MutationSelector.Buildable.IsEmpty() ? TEXT("") : TEXT(" matching "),
						*Proposal.MutationSelector.Buildable);
				}
				Proposal.SpecJson = TEXT("{\"tool\":\"propose_set_clocks\"}");
				return StoreMutationProposal(MoveTemp(Proposal), Ctx, ReplaceId, FString());
			}
		});

		Tools.Register({
			TEXT("propose_set_recipe"),
			TEXT("PROPOSE changing what machines produce (SetRecipe on manufacturers). Selector: {buildable?: machine name substring, center?: {x,y,z metres}, radiusM, maxCount?} — OMIT center to use the player's aim/position; never ask for coordinates. recipe = the production recipe's display name (alternates work when unlocked). Machines already running that recipe are excluded. Machines with items inside are SKIPPED unless force:true — force DESTROYS their contents (say so before proposing it). Nothing changes until approved; undo restores each machine's previous recipe. Returns targets + proposalId. REVISING: pass replaceProposalId."),
			TEXT(R"({"type":"object","properties":{"selector":{"type":"object","description":"{buildable?, center?:{x,y,z metres}, radiusM, maxCount?} — the machines to retask."},"recipe":{"type":"string","description":"Production recipe display name (e.g. \"Iron Plate\", \"Alternate: Cast Screw\")."},"force":{"type":"boolean","description":"true = machines with items get their contents DESTROYED instead of being skipped."},"replaceProposalId":{"type":"string","description":"Optional: a PENDING proposal this one revises."}},"required":["recipe"]})"),
			EAIDAToolTier::Act,
			[this, ParseMutationSelector, StoreMutationProposal, ParseReplaceId](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
			{
				SweepProposals();
				FGuid ReplaceId;
				FString Error;
				if (!ParseReplaceId(Args, ReplaceId, Error))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
				}

				FString RecipeName;
				Args->TryGetStringField(TEXT("recipe"), RecipeName);
				FString RecipePath, RecipeDisplay;
				TArray<FString> Suggestions;
				if (!FAIDAActionSeam::ResolveProductionRecipe(this, RecipeName, RecipePath, RecipeDisplay, Suggestions))
				{
					FString Msg = FString::Printf(TEXT("no available production recipe matches '%s'"), *RecipeName);
					if (Suggestions.Num() > 0)
					{
						Msg += FString::Printf(TEXT(" — closest: %s"), *FString::Join(Suggestions, TEXT(", ")));
					}
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
				}

				FAIDAProposal Proposal;
				Proposal.MutationKind = EAIDAMutationKind::Recipe;
				Proposal.MutationRecipePath = RecipePath;
				Proposal.MutationRecipeName = RecipeDisplay;
				Args->TryGetBoolField(TEXT("force"), Proposal.bMutationForce);
				if (!ParseMutationSelector(Args, Ctx, Proposal.MutationSelector, Error))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
				}

				TArray<FAIDAMutationTarget> Targets;
				if (!FAIDAActionSeam::ResolveMutationTargets(this, EAIDAMutationKind::Recipe,
					Proposal.MutationSelector, RecipePath, Targets, Error))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
				}
				if (Targets.Num() == 0)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("no manufacturer matches the selector (machines already on that recipe are excluded)"), {}));
				}
				int32 WithContents = 0;
				for (const FAIDAMutationTarget& Target : Targets)
				{
					if (Target.bHasContents) { ++WithContents; }
				}
				Proposal.TargetCount = Targets.Num();
				Proposal.Summary = FString::Printf(TEXT("set %d machine(s) to the %s recipe"), Targets.Num(), *RecipeDisplay);
				FString Warning;
				if (WithContents > 0)
				{
					Warning = Proposal.bMutationForce
						? FString::Printf(TEXT("%d machine(s) have items inside — approving DESTROYS those contents."), WithContents)
						: FString::Printf(TEXT("%d machine(s) have items inside and will be SKIPPED (re-propose with force:true to destroy their contents)."), WithContents);
				}
				Proposal.SpecJson = TEXT("{\"tool\":\"propose_set_recipe\"}");
				return StoreMutationProposal(MoveTemp(Proposal), Ctx, ReplaceId, Warning);
			}
		});

		Tools.Register({
			TEXT("propose_upgrade_belts"),
			TEXT("PROPOSE upgrading (or downgrading) conveyor belts to a target mark via the game's own upgrade path — connections and items in transit are preserved. Selector: {buildable?: e.g. 'Mk.1' to only touch that mark, center?: {x,y,z metres}, radiusM, maxCount?} — OMIT center to use the player's aim/position; never ask for coordinates. targetMk 1-6 picks the belt tier (must be unlocked). Belts already at the target are excluded. Cost = each new belt's length-scaled cost, charged as built at execute (unaffordable belts are skipped and reported). Nothing changes until approved; undo drives the same path back to each belt's previous mark. Returns targets + proposalId. REVISING: pass replaceProposalId."),
			TEXT(R"({"type":"object","properties":{"selector":{"type":"object","description":"{buildable?, center?:{x,y,z metres}, radiusM, maxCount?} — the belts to upgrade."},"targetMk":{"type":"number","description":"Target conveyor belt mark, 1-6."},"replaceProposalId":{"type":"string","description":"Optional: a PENDING proposal this one revises."}},"required":["targetMk"]})"),
			EAIDAToolTier::Act,
			[this, ParseMutationSelector, StoreMutationProposal, ParseReplaceId](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
			{
				SweepProposals();
				FGuid ReplaceId;
				FString Error;
				if (!ParseReplaceId(Args, ReplaceId, Error))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
				}

				double TargetMk = 0.0;
				Args->TryGetNumberField(TEXT("targetMk"), TargetMk);
				const int32 Mk = FMath::RoundToInt32(TargetMk);
				if (Mk < 1 || Mk > 6)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(TEXT("targetMk must be 1-6"), {}));
				}
				FAIDARecipeResolution Belt;
				if (!FAIDAActionSeam::ResolveBuildRecipe(this, FString::Printf(TEXT("Conveyor Belt Mk.%d"), Mk), Belt))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						FString::Printf(TEXT("Conveyor Belt Mk.%d is not unlocked yet"), Mk), {}));
				}

				FAIDAProposal Proposal;
				Proposal.MutationKind = EAIDAMutationKind::BeltMk;
				Proposal.MutationBeltRecipePath = Belt.RecipeClassPath;
				Proposal.MutationBeltName = Belt.DisplayName;
				if (!ParseMutationSelector(Args, Ctx, Proposal.MutationSelector, Error))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
				}

				TArray<FAIDAMutationTarget> Targets;
				if (!FAIDAActionSeam::ResolveMutationTargets(this, EAIDAMutationKind::BeltMk,
					Proposal.MutationSelector, Proposal.MutationBeltRecipePath, Targets, Error))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
				}
				if (Targets.Num() == 0)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("no belt matches the selector (belts already at that mark are excluded)"), {}));
				}
				Proposal.TargetCount = Targets.Num();
				Proposal.Summary = FString::Printf(TEXT("upgrade %d belt(s) to %s (length-scaled cost charged per belt at execute)"),
					Targets.Num(), *Belt.DisplayName);
				Proposal.SpecJson = TEXT("{\"tool\":\"propose_upgrade_belts\"}");
				return StoreMutationProposal(MoveTemp(Proposal), Ctx, ReplaceId, FString());
			}
		});
	}

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
		TEXT("PROPOSE the belt/pipe plumbing (a manifold) for a row of machines: one splitter or merger per machine on a straight trunk line in front of their ports, plus all connecting belt runs (or a pipe-junction + pipe equivalent). Nothing is built until a player with act permission approves. Spec: {version:1, kind:'belt'|'pipe', direction:'in' (feed inputs, splitters) | 'out' (collect outputs, mergers), transport:'belt or pipe display name', attachment?:'override display name', machines:{buildable:'machine display name', center?:{x,y in metres}, radiusM?:30, maxCount?:0=all}, standoffM?:4, port?:0}. OMIT machines.center to use the machines the requesting player is looking at. Machines whose matching port is already connected are skipped automatically. The machines must roughly face the same direction. Every port on a machine side gets its OWN row distance automatically (pipes hug the machines, belt rows further out, a second belt input the next row) — propose multiple manifolds in any order; the rows will not collide. Returns the attachment dry-run (cost, count) + run count; runs are charged as they build. Blocked ground never fails the proposal: attachments that can't validate are reported (invalidCount), skipped at execute, and refunded — their runs then fail loudly. TO MANIFOLD A PENDING (UNBUILT) PROPOSAL: pass forProposalId with that proposal's id and OMIT spec.machines — the machines do NOT need to be built or approved first; the pending proposal is replaced by ONE combined proposal (machines + manifold ghosts together, one approval). This works for v1 grids AND v2 composites (a composite's ports come from its machine parts; foundations/walls contribute none) — NEVER tell the player a pending proposal must be approved or built before manifolds can be added. Call again with the NEW id to add more manifolds (both sides = one call per side)."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"The versioned manifold spec (see tool description)."},"forProposalId":{"type":"string","description":"Optional: a PENDING machine-build proposal to manifold — the machines do NOT need to exist yet. The pending proposal is replaced by ONE combined proposal (machines + manifold, ghosts for both, one approval). Omit spec.machines when using this."}},"required":["spec"]})"),
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

			// forProposalId = a manifold FOR A PENDING BUILD (revise-by-prompt): the machines come
			// from that proposal's placements, so the spec's machines selector is optional.
			FString ForIdStr;
			Args->TryGetStringField(TEXT("forProposalId"), ForIdStr);
			ForIdStr = ForIdStr.TrimStartAndEnd();

			FAIDAManifoldSpec Spec;
			FString Error;
			if (!AIDAActionSpec::ParseManifoldSpec(SpecObj ? *SpecObj : nullptr, Config.Actions.MaxProposalItems, Spec, Error,
				/*bRequireMachines*/ ForIdStr.IsEmpty()))
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

			// ---- Manifold for a PENDING build (revise-by-prompt) --------------------------------
			// The machines are the pending proposal's PLANNED placements: ports are read from a
			// machine hologram walked across them, the row is planned exactly like a live manifold,
			// and the pending proposal is superseded by ONE combined proposal (machines + every
			// manifold added so far) — one ghost preview, one approval, TickConnected executes.
			if (!ForIdStr.IsEmpty())
			{
				FGuid ForId;
				const FAIDAProposal* Target = FGuid::Parse(ForIdStr, ForId) ? Actions.Store().Find(ForId) : nullptr;
				if (!Target || Target->State != EAIDAProposalState::Pending)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("forProposalId doesn't match a pending proposal (it may have been decided or expired) — call get_proposal_status"), {}));
				}
				if (Target->bDismantle || Target->bLabel || Target->bManifold || Target->bPowerOnly
					|| Target->Placements.Num() == 0)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("forProposalId must reference a pending machine-build proposal (dismantles, labels and power/manifold proposals can't take a manifold)"), {}));
				}
				// v2 composites qualify too: ports come from the parts that are machines — parts
				// without matching connectors (foundations, walls) simply contribute no ports.
				const bool bCompositeTarget = Target->PartRecipePaths.Num() > 0
					&& Target->PlacementPartIndex.Num() == Target->Placements.Num();
				Stage(FString::Printf(TEXT("planning against pending proposal %s (%d machine(s))"),
					*ForId.ToString(EGuidFormats::DigitsWithHyphens), Target->Placements.Num()));

				// The machine's display name + the stored build spec (for the compound record).
				TSharedPtr<FJsonObject> StoredSpec;
				{
					const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Target->SpecJson);
					FJsonSerializer::Deserialize(Reader, StoredSpec);
				}
				TSharedPtr<FJsonObject> BuildSpecObj = StoredSpec;
				if (StoredSpec.IsValid() && StoredSpec->HasTypedField<EJson::Object>(TEXT("build")))
				{
					BuildSpecObj = StoredSpec->GetObjectField(TEXT("build"));
				}
				FString MachineName = TEXT("machine");
				if (BuildSpecObj.IsValid())
				{
					BuildSpecObj->TryGetStringField(TEXT("buildable"), MachineName);
				}
				// Composite part display names ride the stored v2 spec's parts array, in the same
				// order ParseBuildSpec resolved them into PartRecipePaths.
				TArray<FString> PartNames;
				if (bCompositeTarget && BuildSpecObj.IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* PartsArr = nullptr;
					if (BuildSpecObj->TryGetArrayField(TEXT("parts"), PartsArr) && PartsArr)
					{
						for (const TSharedPtr<FJsonValue>& PartVal : *PartsArr)
						{
							FString PartName = TEXT("machine");
							const TSharedPtr<FJsonObject> PartObj = PartVal.IsValid() ? PartVal->AsObject() : nullptr;
							if (PartObj.IsValid()) { PartObj->TryGetStringField(TEXT("buildable"), PartName); }
							PartNames.Add(MoveTemp(PartName));
						}
					}
				}

				// Ports already claimed by earlier sets on this proposal are off the table.
				TArray<FVector> UsedPorts;
				for (const FAIDAManifoldSet& Set : Target->ManifoldSets)
				{
					for (const FAIDAManifoldPort& Port : Set.Ports) { UsedPorts.Add(Port.PosCm); }
				}

				TArray<FAIDAManifoldPort> Ports;
				TArray<int32> PortMachineIndex;
				if (!bCompositeTarget)
				{
					FAIDAActionSeam::ResolvePlannedPorts(this, Target->RecipeClassPath, MachineName,
						Target->Placements, Spec.bPipe, Spec.bOutput, Spec.PortIndex, UsedPorts, Ports, PortMachineIndex);
				}
				else
				{
					// Placements are grouped by part (contiguous runs) — resolve each run with its
					// own recipe and remap the run-local machine indices to GLOBAL placement indices,
					// which is what phase 0's per-index actor capture rebinds against at execute.
					int32 Start = 0;
					while (Start < Target->Placements.Num())
					{
						const int32 Part = Target->PlacementPartIndex[Start];
						int32 End = Start;
						while (End < Target->PlacementPartIndex.Num() && Target->PlacementPartIndex[End] == Part) { ++End; }
						const TArray<FTransform> Run(Target->Placements.GetData() + Start, End - Start);
						const FString& PartRecipe = Target->PartRecipePaths.IsValidIndex(Part)
							? Target->PartRecipePaths[Part] : Target->RecipeClassPath;
						TArray<FAIDAManifoldPort> RunPorts;
						TArray<int32> RunIndex;
						if (FAIDAActionSeam::ResolvePlannedPorts(this, PartRecipe,
								PartNames.IsValidIndex(Part) ? PartNames[Part] : MachineName, Run,
								Spec.bPipe, Spec.bOutput, Spec.PortIndex, UsedPorts, RunPorts, RunIndex))
						{
							for (int32 i = 0; i < RunPorts.Num(); ++i)
							{
								Ports.Add(MoveTemp(RunPorts[i]));
								PortMachineIndex.Add(Start + RunIndex[i]);
							}
						}
						Start = End;
					}
				}
				if (Ports.Num() == 0)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(FString::Printf(
						TEXT("the proposed %s has no free %s %s port for this manifold — earlier manifolds may have claimed them, or the machine has none"),
						bCompositeTarget ? TEXT("composite (none of its parts)") : *MachineName,
						Spec.bPipe ? TEXT("pipe") : TEXT("belt"), Spec.bOutput ? TEXT("output") : TEXT("input")), {}));
				}
				Stage(FString::Printf(TEXT("planned ports resolved: %d — planning row"), Ports.Num()));

				TArray<FAIDAManifoldPortPoint> Points;
				Points.Reserve(Ports.Num());
				FVector AvgNormal = FVector::ZeroVector;
				for (const FAIDAManifoldPort& Port : Ports)
				{
					Points.Add({ Port.PosCm, Port.NormalCm });
					AvgNormal += Port.NormalCm;
				}
				AvgNormal = AvgNormal.GetSafeNormal();

				// Deterministic lanes without a world to inspect: every EARLIER set on the same
				// machine side pushes this row one lane further out (mirrors ResolveManifoldLane).
				const double FootprintM = FMath::Max(Attachment.FootprintXM, Attachment.FootprintYM);
				const double LaneWidthM = FMath::Max(4.0, FootprintM) + 1.0;
				int32 Lane = 0;
				for (const FAIDAManifoldSet& Set : Target->ManifoldSets)
				{
					FVector SetNormal = FVector::ZeroVector;
					for (const FAIDAManifoldPort& Port : Set.Ports) { SetNormal += Port.NormalCm; }
					if (FVector::DotProduct(SetNormal.GetSafeNormal(), AvgNormal) > 0.5) { ++Lane; }
				}
				const double BaseStandoffM = Spec.StandoffM + Lane * LaneWidthM;

				// Step outward on trouble, same fallback ladder as live manifolds: clean lane >
				// valid-but-clipping > blocked-with-advisory (validation never prohibits a ghost).
				FAIDAManifoldPlan Plan;
				FAIDADryRunResult DryRun;
				double UsedStandoffM = BaseStandoffM;
				FAIDAManifoldPlan NearestValidPlan;
				FAIDADryRunResult NearestValidDryRun;
				double NearestValidStandoffM = -1.0;
				FAIDAManifoldPlan FirstFailPlan;
				FAIDADryRunResult FirstFailRun;
				double FirstFailStandoffM = -1.0;
				bool bPlaced = false;
				bool bClips = false;
				bool bBlockedSome = false;
				for (int32 Attempt = 0; Attempt < 4; ++Attempt)
				{
					UsedStandoffM = BaseStandoffM + Attempt * 3.0;
					Plan = AIDAActionSpec::PlanManifold(Points, Spec.bOutput, Spec.bPipe,
						UsedStandoffM, FootprintM, /*MaxRunM*/ 56.0);
					if (!Plan.Error.IsEmpty())
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Plan.Error, {}));
					}
					for (FTransform& Placement : Plan.Attachments)
					{
						double GroundZ;
						if (FAIDAActionSeam::ProbeGroundZ(this, Placement.GetLocation(), GroundZ))
						{
							// Never bury an attachment under a floor that is ITSELF still pending
							// (revision chains) — ground far below the port means the real floor
							// doesn't exist yet, so stay near the port's level instead.
							FVector Location = Placement.GetLocation();
							Location.Z = (Location.Z - GroundZ > 400.0) ? Location.Z - 100.0 : GroundZ;
							Placement.SetLocation(Location);
						}
					}
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
					if (DryRun.bOk && NearestValidStandoffM < 0.0)
					{
						NearestValidPlan = Plan;
						NearestValidDryRun = DryRun;
						NearestValidStandoffM = UsedStandoffM;
					}
					else if (!DryRun.bOk && FirstFailStandoffM < 0.0)
					{
						FirstFailPlan = Plan;
						FirstFailRun = DryRun;
						FirstFailStandoffM = UsedStandoffM;
					}
				}
				if (!bPlaced && NearestValidStandoffM >= 0.0)
				{
					Plan = MoveTemp(NearestValidPlan);
					DryRun = MoveTemp(NearestValidDryRun);
					UsedStandoffM = NearestValidStandoffM;
					bPlaced = true;
					bClips = true;
				}
				if (!bPlaced && FirstFailStandoffM >= 0.0)
				{
					Plan = MoveTemp(FirstFailPlan);
					DryRun = MoveTemp(FirstFailRun);
					UsedStandoffM = FirstFailStandoffM;
					bPlaced = true;
					bBlockedSome = true;
				}
				if (!bPlaced)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("no attachment lane could be evaluated for the planned machines"), DryRun.Failures));
				}

				// Reorder the ports to the plan's sort so everything downstream is index-aligned.
				FAIDAManifoldSet NewSet;
				NewSet.bPipe = Spec.bPipe;
				NewSet.bOutput = Spec.bOutput;
				NewSet.TransportRecipePath = Transport.RecipeClassPath;
				NewSet.TransportName = Transport.DisplayName;
				NewSet.AttachmentRecipePath = Attachment.RecipeClassPath;
				NewSet.AttachmentName = Attachment.DisplayName;
				for (const int32 PortIdx : Plan.PortOrder)
				{
					NewSet.Ports.Add(Ports[PortIdx]);
					NewSet.PortMachineIndex.Add(PortMachineIndex[PortIdx]);
				}
				NewSet.Attachments = MoveTemp(Plan.Attachments);
				NewSet.RowAxis = Plan.RowAxis;
				NewSet.DropDir = Plan.DropDir;
				const int32 SetCount = NewSet.Attachments.Num();
				const int32 RunCount = 2 * SetCount - 1;

				// The combined proposal: the target build + every manifold so far, ONE approval.
				FAIDAProposal Combined = *Target;
				Combined.Id = FGuid::NewGuid();
				Combined.RequesterId = Ctx.PlayerId; // the reviser owns the revision
				Combined.RequesterName = Ctx.Author;
				Combined.ManifoldSets.Add(MoveTemp(NewSet));
				for (const FAIDACostItem& Item : DryRun.Cost)
				{
					bool bMerged = false;
					for (FAIDACostItem& Existing : Combined.Cost)
					{
						if (Existing.Item == Item.Item) { Existing.Amount += Item.Amount; bMerged = true; break; }
					}
					if (!bMerged) { Combined.Cost.Add(Item); }
				}
				if (Config.Actions.CostMode == TEXT("central") && !FAIDAActionSeam::CheckAffordable(this, Combined.Cost, Ctx.PlayerId))
				{
					FString Msg = TEXT("not affordable from central storage + your inventory with the manifold included: needs ");
					for (int32 i = 0; i < Combined.Cost.Num(); ++i)
					{
						Msg += FString::Printf(TEXT("%s%d %s"), i > 0 ? TEXT(", ") : TEXT(""), Combined.Cost[i].Amount, *Combined.Cost[i].Item);
					}
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
				}
				Combined.InvalidCount = Target->InvalidCount + DryRun.Failures.Num();
				Combined.Summary += FString::Printf(TEXT(" + %s-%s manifold (%d x %s + %d runs)"),
					Spec.bPipe ? TEXT("pipe") : TEXT("belt"), Spec.bOutput ? TEXT("out") : TEXT("in"),
					SetCount, *Attachment.DisplayName, RunCount);
				if (bClips)
				{
					Combined.Summary += TEXT(" [manifold clips nearby structures]");
				}

				// Compound stored spec {"build":…, "manifolds":[…]} — later revisions read it back.
				{
					const double EffectiveBaseM = UsedStandoffM - Lane * LaneWidthM;
					(*SpecObj)->SetNumberField(TEXT("standoffM"), EffectiveBaseM);
					TSharedRef<FJsonObject> Compound = MakeShared<FJsonObject>();
					TArray<TSharedPtr<FJsonValue>> ManifoldSpecs;
					if (StoredSpec.IsValid() && StoredSpec->HasTypedField<EJson::Object>(TEXT("build")))
					{
						Compound = StoredSpec.ToSharedRef();
						const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
						if (Compound->TryGetArrayField(TEXT("manifolds"), Existing) && Existing)
						{
							ManifoldSpecs = *Existing;
						}
					}
					else if (StoredSpec.IsValid())
					{
						Compound->SetObjectField(TEXT("build"), StoredSpec);
					}
					ManifoldSpecs.Add(MakeShared<FJsonValueObject>(*SpecObj));
					Compound->SetArrayField(TEXT("manifolds"), ManifoldSpecs);
					Combined.SpecJson = AIDAToCompactJson(Compound);
				}

				Stage(TEXT("combined proposal ready — superseding + storing"));
				SupersedeProposal(ForId); // Target dangles from here — Combined carries everything
				Target = nullptr;

				const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
				if (!Actions.Store().Add(Combined, Now, Config.Actions.MaxPendingProposals, Error))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
				}
				UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored (connected): %s (by %s)"),
					*Combined.Id.ToString(EGuidFormats::DigitsWithHyphens), *Combined.Summary, *Ctx.Author);
				PublishProposal(Combined.Id);
				FString Announce = FString::Printf(TEXT("AIDA proposes (for %s, revised): %s — cost %s. Awaiting approval."),
					*Ctx.Author, *Combined.Summary, *AIDACostSummaryString(Combined.Cost));
				if (Combined.InvalidCount > 0)
				{
					Announce += FString::Printf(TEXT(" NOTE: %d placement(s) are blocked at this spot — nudge the ghost onto clear ground before approving."),
						Combined.InvalidCount);
				}
				AnnounceSystem(Announce);

				return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Combined, Config.Actions.TtlSeconds,
					/*bAffordable*/ true, 0.0, nullptr, DryRun.Failures.Num() > 0 ? &DryRun.Failures : nullptr));
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
			FAIDAManifoldPlan FirstFailPlan;     // the BASE lane when every lane has blocked spots (last resort — still ghosts)
			FAIDADryRunResult FirstFailRun;
			double FirstFailStandoffM = -1.0;
			bool bPlaced = false;
			bool bClips = false;
			bool bOverlaps = false;
			bool bBlockedSome = false;
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
				else if (!DryRun.bOk && FirstFailStandoffM < 0.0)
				{
					// The nearest lane with blocked spots — the last-resort ghost when no lane in
					// range validates (validation trouble never prohibits a proposal, user rule).
					FirstFailPlan = Plan;
					FirstFailRun = DryRun;
					FirstFailStandoffM = UsedStandoffM;
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
			if (!bPlaced && FirstFailStandoffM >= 0.0)
			{
				// Every lane in range has blocked attachment spots: the nearest lane still ghosts
				// (user rule — validation trouble never prohibits a proposal). Blocked attachments
				// skip at execute (their cost refunds) and their runs then fail loudly.
				Plan = MoveTemp(FirstFailPlan);
				DryRun = MoveTemp(FirstFailRun);
				UsedStandoffM = FirstFailStandoffM;
				bPlaced = true;
				bBlockedSome = true;
			}
			if (!bPlaced)
			{
				// Unreachable in practice (every attempt lands in one of the buckets above).
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
					TEXT("no attachment lane could be evaluated"), DryRun.Failures));
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
			Proposal.InvalidCount = DryRun.Failures.Num(); // advisory — never blocks the ghost
			if (bBlockedSome)
			{
				// Manifolds can't be nudged (anchored to ports) — the escape hatch is a re-propose.
				Proposal.Summary += FString::Printf(TEXT(" [%d of %d attachment spot(s) blocked — skipped + refunded at execute; reject and re-propose with a different standoffM if the row must be complete]"),
					Proposal.InvalidCount, Proposal.Placements.Num());
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

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds, DryRun.bAffordable, 0.0,
				nullptr, DryRun.Failures.Num() > 0 ? &DryRun.Failures : nullptr));
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
			FAIDAPowerPlan FirstFailPlan;    // nearest lane when EVERY lane has blocked pole spots (still ghosts)
			FAIDADryRunResult FirstFailRun;
			bool bPlaced = false;
			bool bHaveFallback = false;
			bool bHaveFirstFail = false;
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
				else if (!AttemptRun.bOk && !bHaveFirstFail)
				{
					FirstFailPlan = MoveTemp(AttemptPlan);
					FirstFailRun = MoveTemp(AttemptRun);
					bHaveFirstFail = true;
				}
			}
			if (!bPlaced && bHaveFallback)
			{
				Plan = MoveTemp(FallbackPlan);
				PoleDryRun = MoveTemp(FallbackDryRun);
				bPlaced = true;
			}
			if (!bPlaced && bHaveFirstFail)
			{
				// Both sides have blocked pole spots: the nearest lane still ghosts (user rule —
				// validation trouble never prohibits a proposal). Blocked poles skip at execute
				// (refunded) and their machines' wires then fail loudly.
				Plan = MoveTemp(FirstFailPlan);
				PoleDryRun = MoveTemp(FirstFailRun);
				bPlaced = true;
			}
			if (!bPlaced)
			{
				// Unreachable in practice (every offset lands in one of the buckets above).
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
					TEXT("no pole lane could be evaluated beside those machines — pass 'pole' or a different center"), {}));
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
			// Blocked pole spots are advisory like every other placement (user rule) — poles skip
			// at execute (refunded) and their machines' wires then fail loudly.
			Proposal.InvalidCount = PoleDryRun.Failures.Num();

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

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Proposal, Config.Actions.TtlSeconds, PoleDryRun.bAffordable, 0.0,
				nullptr, PoleDryRun.Failures.Num() > 0 ? &PoleDryRun.Failures : nullptr));
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

	// propose_belt_tap: find the nearest matching source belt -> merge a tap (splice splitter or
	// free end + feed run) into a PENDING belt-in manifold proposal. Executes as extra phases
	// after the manifold (TickManifold/TickConnected tap phases).
	Tools.Register({
		TEXT("propose_belt_tap"),
		TEXT("Feed machines from an EXISTING belt: the server finds the nearest belt whose riding items match spec.item (omit = the nearest belt of any kind), taps it — by SPLICING a Conveyor Splitter into it mid-belt (the source keeps flowing; the tap takes a share) or by using a FREE (unconnected) belt end — and routes a feed belt from the tap to the destination, CHAINING several runs automatically for long distances (up to ~2 km; failures are reported per hop and refunded). TWO TARGET MODES. (1) forProposalId = a PENDING proposal with a belt-INPUT manifold (propose_manifold direction 'in', or a connected build with a belt-in set): the tap merges into that proposal, ONE approval builds everything. (2) OMIT forProposalId when the machines/manifold are ALREADY BUILT: the server finds the nearest FREE belt input near where the player is aiming (a built splitter row's open trunk end, or a machine input) and feeds that — this is the mode for 'connect it' after a manifold has executed. Spec: {version:1, item?: item display name ('Coal'), maxDistanceM?: source-belt search radius from the destination (default 250, up to 2000), transport?: belt display name for the feed (default: best unlocked)}. Typical flow: propose_manifold {kind belt, direction in} then propose_belt_tap {item:'Coal'} with forProposalId; or, for built manifolds, aim at them and call propose_belt_tap alone. NOTE: /aida undo removes the tap splitter and feed belts but the source belt stays cut."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"{version:1, item?: item riding the source belt, maxDistanceM?: default 250, transport?: feed belt display name}"},"forProposalId":{"type":"string","description":"Optional: a PENDING belt-in manifold (or connected build) proposal to feed — replaced by ONE combined proposal. OMIT when the machines/manifold are already built; the destination is then the nearest free belt input near the player's aim."}},"required":["spec"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			SweepProposals();

			const TSharedPtr<FJsonObject>* SpecObj = nullptr;
			Args->TryGetObjectField(TEXT("spec"), SpecObj);
			FString Item;
			FString TransportName;
			double MaxDistanceM = 250.0;
			if (SpecObj && SpecObj->IsValid())
			{
				(*SpecObj)->TryGetStringField(TEXT("item"), Item);
				(*SpecObj)->TryGetStringField(TEXT("transport"), TransportName);
				(*SpecObj)->TryGetNumberField(TEXT("maxDistanceM"), MaxDistanceM);
			}
			MaxDistanceM = FMath::Clamp(MaxDistanceM, 5.0, 2000.0);

			FString ForIdStr;
			Args->TryGetStringField(TEXT("forProposalId"), ForIdStr);
			ForIdStr = ForIdStr.TrimStartAndEnd();
			FGuid ForId;
			const FAIDAProposal* Target = nullptr;
			if (!ForIdStr.IsEmpty())
			{
				Target = FGuid::Parse(ForIdStr, ForId) ? Actions.Store().Find(ForId) : nullptr;
				if (!Target || Target->State != EAIDAProposalState::Pending)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("forProposalId doesn't match a pending proposal (it may have been decided or expired) — for machines/manifolds that are ALREADY BUILT, call again WITHOUT forProposalId while aiming at them"), {}));
				}
				if (Target->bTap)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("that proposal already has a belt tap — approve it, or revise the manifold first"), {}));
				}
			}

			// The destination: a pending proposal's open feed end (index 0 of the belt-in row), or —
			// standalone — the nearest FREE belt input on BUILT structures near the player's aim.
			FVector FeedPointCm = FVector::ZeroVector;
			int32 TapSetIndex = INDEX_NONE;
			FAIDAManifoldPort DestPort;
			if (Target)
			{
				if (Target->bManifold && !Target->bManifoldPipe && !Target->bManifoldOutput
					&& Target->Placements.Num() > 0)
				{
					FeedPointCm = Target->Placements[0].GetLocation();
				}
				else
				{
					for (int32 i = 0; i < Target->ManifoldSets.Num(); ++i)
					{
						const FAIDAManifoldSet& Set = Target->ManifoldSets[i];
						if (!Set.bPipe && !Set.bOutput && Set.Attachments.Num() > 0)
						{
							TapSetIndex = i;
							FeedPointCm = Set.Attachments[0].GetLocation();
							break;
						}
					}
					if (TapSetIndex == INDEX_NONE)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("forProposalId must reference a pending proposal with a belt-INPUT manifold — call propose_manifold {kind belt, direction in} first, then tap that proposal"), {}));
					}
				}
			}
			else
			{
				FVector Center = Ctx.Location;
				FVector Aim;
				if (FAIDAActionSeam::ResolveAimPoint(this, Ctx.PlayerId, Aim))
				{
					Center = Aim;
				}
				else if (!Ctx.bHasLocation)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("could not resolve where the player is aiming — have them aim at the machines or their manifold"), {}));
				}
				FString PortError;
				if (!FAIDAActionSeam::FindFreeBeltInputPort(this, Center, 3000.0, DestPort, PortError))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(PortError, {}));
				}
				FeedPointCm = DestPort.PosCm;
			}

			FAIDATapSource Source;
			if (!FAIDAActionSeam::FindTapSource(this, Item, FeedPointCm, MaxDistanceM * AIDAMetersToCm, Source))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Source.Error, {}));
			}

			// Long feeds chain hops through waypoints (Z ground-probed here at propose). ~50 m per
			// hop; the LAST hop lands on the destination port itself.
			TArray<FVector> ChainPoints;
			const double FeedGapCm = FVector::Dist(Source.PointCm, FeedPointCm);
			const double FeedGapM = FeedGapCm / AIDAMetersToCm;
			if (FeedGapM > 2000.0)
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(FString::Printf(
					TEXT("the nearest matching belt (%s carrying %s) is %.0f m from the destination — beyond the 2 km feed limit"),
					*Source.BeltName, *Source.ItemNote, FeedGapM), {}));
			}
			{
				constexpr double HopCm = 5000.0;
				const int32 Hops = FMath::CeilToInt32(FeedGapCm / HopCm);
				for (int32 i = 1; i < Hops; ++i)
				{
					FVector Point = FMath::Lerp(Source.PointCm, FeedPointCm, static_cast<double>(i) / Hops);
					double GroundZ;
					if (FAIDAActionSeam::ProbeGroundZ(this, Point, GroundZ))
					{
						Point.Z = GroundZ + 150.0;
					}
					ChainPoints.Add(Point);
				}
			}

			// The tap splitter (cut variant): resolved + costed upfront like any placement.
			FAIDARecipeResolution Splitter;
			TArray<FAIDACostItem> SplitterCost;
			if (!Source.bDangling)
			{
				if (!FAIDAActionSeam::ResolveBuildRecipe(this, TEXT("Conveyor Splitter"), Splitter))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("no unlocked buildable matches 'Conveyor Splitter' — cannot splice a tap into the belt"), {}));
				}
				FAIDAActionSeam::TallyRecipeCost(this, Splitter.RecipeClassPath, 1, SplitterCost);
			}

			// Standalone taps carry their own feed belt; combined taps reuse the manifold's.
			FAIDARecipeResolution Transport;
			if (!Target)
			{
				TArray<FString> Candidates;
				if (!TransportName.IsEmpty())
				{
					Candidates.Add(TransportName);
				}
				else
				{
					Candidates = { TEXT("Conveyor Belt Mk.6"), TEXT("Conveyor Belt Mk.5"),
						TEXT("Conveyor Belt Mk.4"), TEXT("Conveyor Belt Mk.3"),
						TEXT("Conveyor Belt Mk.2"), TEXT("Conveyor Belt Mk.1") };
				}
				bool bResolved = false;
				for (const FString& Candidate : Candidates)
				{
					if (FAIDAActionSeam::ResolveBuildRecipe(this, Candidate, Transport))
					{
						bResolved = true;
						break;
					}
				}
				if (!bResolved)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(FString::Printf(
						TEXT("no unlocked belt matches '%s' for the feed"),
						TransportName.IsEmpty() ? TEXT("any conveyor belt") : *TransportName), {}));
				}
			}

			// The proposal: the pending target + tap merged (one approval), or a standalone tap
			// against the already-built destination port.
			FAIDAProposal Combined = Target ? *Target : FAIDAProposal();
			Combined.Id = FGuid::NewGuid();
			Combined.RequesterId = Ctx.PlayerId;
			Combined.RequesterName = Ctx.Author;
			Combined.bTap = true;
			Combined.TapBelt = Source.Belt;
			Combined.TapBeltName = Source.BeltName;
			Combined.bTapDangling = Source.bDangling;
			Combined.TapOffsetCm = Source.OffsetCm;
			Combined.TapPointCm = Source.PointCm;
			Combined.TapDirCm = Source.DirCm;
			Combined.TapSplitterRecipePath = Source.bDangling ? FString() : Splitter.RecipeClassPath;
			Combined.TapSplitterName = Source.bDangling ? FString() : Splitter.DisplayName;
			Combined.TapSetIndex = TapSetIndex;
			Combined.TapChainPointsCm = MoveTemp(ChainPoints);
			if (!Target)
			{
				Combined.Ports.Add(DestPort);
				Combined.TransportRecipePath = Transport.RecipeClassPath;
				Combined.TransportName = Transport.DisplayName;
			}
			for (const FAIDACostItem& CostItem : SplitterCost)
			{
				bool bMerged = false;
				for (FAIDACostItem& Existing : Combined.Cost)
				{
					if (Existing.Item == CostItem.Item)
					{
						Existing.Amount += CostItem.Amount;
						bMerged = true;
						break;
					}
				}
				if (!bMerged) { Combined.Cost.Add(CostItem); }
			}
			if (Config.Actions.CostMode == TEXT("central") && Combined.Cost.Num() > 0
				&& !FAIDAActionSeam::CheckAffordable(this, Combined.Cost, Ctx.PlayerId))
			{
				FString Msg = TEXT("not affordable from central storage + your inventory with the tap included: needs ");
				for (int32 i = 0; i < Combined.Cost.Num(); ++i)
				{
					Msg += FString::Printf(TEXT("%s%d %s"), i > 0 ? TEXT(", ") : TEXT(""), Combined.Cost[i].Amount, *Combined.Cost[i].Item);
				}
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}
			const int32 FeedRuns = Combined.TapChainPointsCm.Num() + 1;
			const FString TapPhrase = Source.bDangling
				? FString::Printf(TEXT("tap the free end of %s carrying %s, %.0f m away (%d feed run(s), charged as built)"),
					*Source.BeltName, *Source.ItemNote, Source.DistanceM, FeedRuns)
				: FString::Printf(TEXT("tap %s carrying %s, %.0f m away (splice a %s in + %d feed run(s), charged as built)"),
					*Source.BeltName, *Source.ItemNote, Source.DistanceM, *Splitter.DisplayName, FeedRuns);
			Combined.Summary = Target
				? Combined.Summary + TEXT(" + ") + TapPhrase
				: FString::Printf(TEXT("%s to feed %s"), *TapPhrase, *DestPort.MachineName);

			// Stored spec: append the tap to the target's spec, or stand alone.
			{
				TSharedPtr<FJsonObject> StoredSpec;
				if (Target)
				{
					const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Target->SpecJson);
					FJsonSerializer::Deserialize(Reader, StoredSpec);
				}
				if (!StoredSpec.IsValid())
				{
					StoredSpec = MakeShared<FJsonObject>();
					StoredSpec->SetStringField(TEXT("tool"), TEXT("propose_belt_tap"));
				}
				TSharedRef<FJsonObject> Tap = MakeShared<FJsonObject>();
				if (!Item.IsEmpty()) { Tap->SetStringField(TEXT("item"), Item); }
				Tap->SetStringField(TEXT("belt"), Source.BeltName);
				Tap->SetStringField(TEXT("mode"), Source.bDangling ? TEXT("freeEnd") : TEXT("splice"));
				Tap->SetNumberField(TEXT("distanceM"), Source.DistanceM);
				Tap->SetNumberField(TEXT("feedRuns"), FeedRuns);
				StoredSpec->SetObjectField(TEXT("tap"), Tap);
				Combined.SpecJson = AIDAToCompactJson(StoredSpec.ToSharedRef());
			}

			if (Target)
			{
				SupersedeProposal(ForId);
				Target = nullptr;
			}

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			FString Error;
			if (!Actions.Store().Add(Combined, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored (tap): %s (by %s)"),
				*Combined.Id.ToString(EGuidFormats::DigitsWithHyphens), *Combined.Summary, *Ctx.Author);
			PublishProposal(Combined.Id);
			FString Announce = FString::Printf(TEXT("AIDA proposes (for %s%s): %s — cost %s. Awaiting approval."),
				*Ctx.Author, ForIdStr.IsEmpty() ? TEXT("") : TEXT(", revised"),
				*Combined.Summary, *AIDACostSummaryString(Combined.Cost));
			if (!Combined.bTapDangling)
			{
				Announce += TEXT(" NOTE: approving cuts the source belt to splice the tap in (undo removes the tap but the cut stays).");
			}
			AnnounceSystem(Announce);

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Combined, Config.Actions.TtlSeconds, true, 0.0));
		}
	});

	// P8 Slice 3: propose_pipe_tap — the belt-tap flow for fluids. Junction-on-pipe is the same
	// native splice family as splitter-on-belt; pipes are direction-agnostic at the junction, and
	// v1 feeds are SINGLE runs (chain hops must end engine-snapped — belt-only until proven).
	Tools.Register({
		TEXT("propose_pipe_tap"),
		TEXT("Feed a machine from an EXISTING pipeline: the server finds the nearest pipeline whose fluid matches spec.fluid (omit = the nearest pipe of any kind), taps it — by SPLICING a Pipeline Junction into it mid-pipe (the source keeps flowing; the tap takes a share) or by using a FREE pipe end — and routes ONE pipe run from the tap to the destination (v1 pipe feeds are single runs, up to ~56 m — no chaining; belts chain, pipes don't yet). TWO TARGET MODES, like propose_belt_tap. (1) forProposalId = a PENDING proposal with a pipe-INPUT manifold (propose_manifold {kind pipe, direction in}): the tap merges into it, ONE approval builds everything. (2) OMIT forProposalId when the machines are ALREADY BUILT: the server finds the nearest FREE pipe input near where the player is aiming (a junction row's open end, or a machine's fluid input) and feeds that. Spec: {version:1, fluid?: fluid name ('Water'), maxDistanceM?: source search radius (default 250), transport?: pipeline display name (default: best unlocked)}. Typical: 'hook the coal generators to the water line' = aim at them and call with {fluid:'Water'}. NOTE: /aida undo removes the junction and feed pipe but the source pipe stays cut."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"{version:1, fluid?: fluid in the source pipe, maxDistanceM?: default 250, transport?: feed pipeline display name}"},"forProposalId":{"type":"string","description":"Optional: a PENDING pipe-in manifold proposal to feed — replaced by ONE combined proposal. OMIT when the destination is already built; the nearest free pipe input near the player's aim is used."}},"required":["spec"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			SweepProposals();

			const TSharedPtr<FJsonObject>* SpecObj = nullptr;
			Args->TryGetObjectField(TEXT("spec"), SpecObj);
			FString Fluid;
			FString TransportName;
			double MaxDistanceM = 250.0;
			if (SpecObj && SpecObj->IsValid())
			{
				(*SpecObj)->TryGetStringField(TEXT("fluid"), Fluid);
				(*SpecObj)->TryGetStringField(TEXT("transport"), TransportName);
				(*SpecObj)->TryGetNumberField(TEXT("maxDistanceM"), MaxDistanceM);
			}
			MaxDistanceM = FMath::Clamp(MaxDistanceM, 5.0, 2000.0);

			FString ForIdStr;
			Args->TryGetStringField(TEXT("forProposalId"), ForIdStr);
			ForIdStr = ForIdStr.TrimStartAndEnd();
			FGuid ForId;
			const FAIDAProposal* Target = nullptr;
			if (!ForIdStr.IsEmpty())
			{
				Target = FGuid::Parse(ForIdStr, ForId) ? Actions.Store().Find(ForId) : nullptr;
				if (!Target || Target->State != EAIDAProposalState::Pending)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("forProposalId doesn't match a pending proposal (it may have been decided or expired) — for machines that are ALREADY BUILT, call again WITHOUT forProposalId while aiming at them"), {}));
				}
				if (Target->bTap)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("that proposal already has a tap — approve it, or revise the manifold first"), {}));
				}
			}

			// The destination: a pending proposal's open pipe-in end, or — standalone — the nearest
			// FREE pipe input on BUILT structures near the player's aim.
			FVector FeedPointCm = FVector::ZeroVector;
			int32 TapSetIndex = INDEX_NONE;
			FAIDAManifoldPort DestPort;
			if (Target)
			{
				if (Target->bManifold && Target->bManifoldPipe && !Target->bManifoldOutput
					&& Target->Placements.Num() > 0)
				{
					FeedPointCm = Target->Placements[0].GetLocation();
				}
				else
				{
					for (int32 i = 0; i < Target->ManifoldSets.Num(); ++i)
					{
						const FAIDAManifoldSet& Set = Target->ManifoldSets[i];
						if (Set.bPipe && !Set.bOutput && Set.Attachments.Num() > 0)
						{
							TapSetIndex = i;
							FeedPointCm = Set.Attachments[0].GetLocation();
							break;
						}
					}
					if (TapSetIndex == INDEX_NONE)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("forProposalId must reference a pending proposal with a pipe-INPUT manifold — call propose_manifold {kind pipe, direction in} first, then tap that proposal"), {}));
					}
				}
			}
			else
			{
				FVector Center = Ctx.Location;
				FVector Aim;
				if (FAIDAActionSeam::ResolveAimPoint(this, Ctx.PlayerId, Aim))
				{
					Center = Aim;
				}
				else if (!Ctx.bHasLocation)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("could not resolve where the player is aiming — have them aim at the machine that needs the fluid"), {}));
				}
				FString PortError;
				if (!FAIDAActionSeam::FindFreePipeInputPort(this, Center, 3000.0, DestPort, PortError))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(PortError, {}));
				}
				FeedPointCm = DestPort.PosCm;
			}

			FAIDATapSource Source;
			if (!FAIDAActionSeam::FindPipeTapSource(this, Fluid, FeedPointCm, MaxDistanceM * AIDAMetersToCm, Source))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Source.Error, {}));
			}

			// v1 pipe feeds are ONE run — a pipe hop must end engine-snapped, which the chain
			// machinery can't guarantee headlessly yet (docs/PHASE8.md Slice 3).
			const double FeedGapM = FVector::Dist(Source.PointCm, FeedPointCm) / AIDAMetersToCm;
			if (FeedGapM > 56.0)
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(FString::Printf(
					TEXT("the nearest matching pipeline (%s carrying %s) is %.0f m from the destination — pipe taps reach ~56 m in v1 (one run, no chaining); build the gap with propose_build pipes or move the tap closer"),
					*Source.BeltName, *Source.ItemNote, FeedGapM), {}));
			}

			// The tap junction (cut variant): resolved + costed upfront like any placement.
			FAIDARecipeResolution Junction;
			TArray<FAIDACostItem> JunctionCost;
			if (!Source.bDangling)
			{
				if (!FAIDAActionSeam::ResolveBuildRecipe(this, TEXT("Pipeline Junction Cross"), Junction))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("no unlocked buildable matches 'Pipeline Junction Cross' — cannot splice a tap into the pipe"), {}));
				}
				FAIDAActionSeam::TallyRecipeCost(this, Junction.RecipeClassPath, 1, JunctionCost);
			}

			// Standalone taps carry their own feed pipe; combined taps reuse the manifold's.
			FAIDARecipeResolution Transport;
			if (!Target)
			{
				TArray<FString> Candidates;
				if (!TransportName.IsEmpty())
				{
					Candidates.Add(TransportName);
				}
				else
				{
					Candidates = { TEXT("Pipeline Mk.2"), TEXT("Pipeline") };
				}
				bool bResolved = false;
				for (const FString& Candidate : Candidates)
				{
					if (FAIDAActionSeam::ResolveBuildRecipe(this, Candidate, Transport))
					{
						bResolved = true;
						break;
					}
				}
				if (!bResolved)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(FString::Printf(
						TEXT("no unlocked pipeline matches '%s' for the feed"),
						TransportName.IsEmpty() ? TEXT("any pipeline") : *TransportName), {}));
				}
			}

			FAIDAProposal Combined = Target ? *Target : FAIDAProposal();
			Combined.Id = FGuid::NewGuid();
			Combined.RequesterId = Ctx.PlayerId;
			Combined.RequesterName = Ctx.Author;
			Combined.bTap = true;
			Combined.bTapPipe = true;
			Combined.TapBelt = Source.Belt;
			Combined.TapBeltName = Source.BeltName;
			Combined.bTapDangling = Source.bDangling;
			Combined.TapOffsetCm = Source.OffsetCm;
			Combined.TapPointCm = Source.PointCm;
			Combined.TapDirCm = Source.DirCm;
			Combined.TapSplitterRecipePath = Source.bDangling ? FString() : Junction.RecipeClassPath;
			Combined.TapSplitterName = Source.bDangling ? FString() : Junction.DisplayName;
			Combined.TapSetIndex = TapSetIndex;
			Combined.TapChainPointsCm.Reset(); // single run — never chained
			if (!Target)
			{
				Combined.Ports.Add(DestPort);
				Combined.TransportRecipePath = Transport.RecipeClassPath;
				Combined.TransportName = Transport.DisplayName;
			}
			for (const FAIDACostItem& CostItem : JunctionCost)
			{
				bool bMerged = false;
				for (FAIDACostItem& Existing : Combined.Cost)
				{
					if (Existing.Item == CostItem.Item)
					{
						Existing.Amount += CostItem.Amount;
						bMerged = true;
						break;
					}
				}
				if (!bMerged) { Combined.Cost.Add(CostItem); }
			}
			if (Config.Actions.CostMode == TEXT("central") && Combined.Cost.Num() > 0
				&& !FAIDAActionSeam::CheckAffordable(this, Combined.Cost, Ctx.PlayerId))
			{
				FString Msg = TEXT("not affordable from central storage + your inventory with the tap included: needs ");
				for (int32 i = 0; i < Combined.Cost.Num(); ++i)
				{
					Msg += FString::Printf(TEXT("%s%d %s"), i > 0 ? TEXT(", ") : TEXT(""), Combined.Cost[i].Amount, *Combined.Cost[i].Item);
				}
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}
			const FString TapPhrase = Source.bDangling
				? FString::Printf(TEXT("tap the free end of %s carrying %s, %.0f m away (one pipe run, charged as built)"),
					*Source.BeltName, *Source.ItemNote, Source.DistanceM)
				: FString::Printf(TEXT("tap %s carrying %s, %.0f m away (splice a %s in + one pipe run, charged as built)"),
					*Source.BeltName, *Source.ItemNote, Source.DistanceM, *Junction.DisplayName);
			Combined.Summary = Target
				? Combined.Summary + TEXT(" + ") + TapPhrase
				: FString::Printf(TEXT("%s to feed %s"), *TapPhrase, *DestPort.MachineName);

			{
				TSharedPtr<FJsonObject> StoredSpec;
				if (Target)
				{
					const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Target->SpecJson);
					FJsonSerializer::Deserialize(Reader, StoredSpec);
				}
				if (!StoredSpec.IsValid())
				{
					StoredSpec = MakeShared<FJsonObject>();
					StoredSpec->SetStringField(TEXT("tool"), TEXT("propose_pipe_tap"));
				}
				TSharedRef<FJsonObject> Tap = MakeShared<FJsonObject>();
				if (!Fluid.IsEmpty()) { Tap->SetStringField(TEXT("fluid"), Fluid); }
				Tap->SetStringField(TEXT("pipe"), Source.BeltName);
				Tap->SetStringField(TEXT("mode"), Source.bDangling ? TEXT("freeEnd") : TEXT("splice"));
				Tap->SetNumberField(TEXT("distanceM"), Source.DistanceM);
				StoredSpec->SetObjectField(TEXT("pipeTap"), Tap);
				Combined.SpecJson = AIDAToCompactJson(StoredSpec.ToSharedRef());
			}

			if (Target)
			{
				SupersedeProposal(ForId);
				Target = nullptr;
			}

			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			FString Error;
			if (!Actions.Store().Add(Combined, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored (pipe tap): %s (by %s)"),
				*Combined.Id.ToString(EGuidFormats::DigitsWithHyphens), *Combined.Summary, *Ctx.Author);
			PublishProposal(Combined.Id);
			FString Announce = FString::Printf(TEXT("AIDA proposes (for %s%s): %s — cost %s. Awaiting approval."),
				*Ctx.Author, ForIdStr.IsEmpty() ? TEXT("") : TEXT(", revised"),
				*Combined.Summary, *AIDACostSummaryString(Combined.Cost));
			if (!Combined.bTapDangling)
			{
				Announce += TEXT(" NOTE: approving cuts the source pipe to splice the junction in (undo removes the tap but the cut stays).");
			}
			AnnounceSystem(Announce);

			return FAIDAToolResult::Ok(AIDAActionSpec::BuildDryRunJson(Combined, Config.Actions.TtlSeconds, true, 0.0));
		}
	});

	// P8 Slice 4 flagship: propose_factory — plan_factory's numbers turned into ONE connected
	// proposal: foundations + machine rows + per-row manifold sets + inter-step links + power kit,
	// with per-machine recipe/clock applied at execute. One approval builds the line.
	Tools.Register({
		TEXT("propose_factory"),
		TEXT("PROPOSE a whole production line: runs the plan_factory math for spec.item at spec.ratePerMin, then lays out ONE connected proposal — a foundation slab, one machine row per recipe step (raw-most step nearest the player, product row last), belt/pipe manifold rows on each step's inputs and outputs, runs linking each step's collected output into the next step's feed row, an auto-power kit, and each machine's recipe + exact clock assigned as it builds. ONE approval builds everything; /aida undo removes it all. RAW INPUTS (ore, water) are NOT magically fed: the result lists them — offer to follow up with propose_belt_tap / propose_pipe_tap (forProposalId works on the pending factory) or leave the feed rows' open ends for manual hookup. Spec: {version:1, item, ratePerMin, origin?: {x,y,z metres — omit to build where the player is aiming}, yawDeg?}. Use when the player asks to BUILD production of something ('build me 30 iron plates a minute right here') — for just the numbers, plan_factory; for a single machine group, propose_build. The ghost previews machines, manifolds and runs — tell the player to check it before approving. REVISING: pass replaceProposalId."),
		TEXT(R"({"type":"object","properties":{"spec":{"type":"object","description":"{version:1, item: product display name, ratePerMin: target rate (fluids m3/min), origin?: {x,y,z metres — omit = player's aim}, yawDeg?: row direction (default 0)}"},"replaceProposalId":{"type":"string","description":"Optional: a PENDING proposal this one revises."}},"required":["spec"]})"),
		EAIDAToolTier::Act,
		[this](const TSharedRef<FJsonObject>& Args, const FAIDAToolContext& Ctx) -> FAIDAToolResult
		{
			SweepProposals();

			const TSharedPtr<FJsonObject>* SpecObj = nullptr;
			Args->TryGetObjectField(TEXT("spec"), SpecObj);
			FString Item;
			double RatePerMin = 0.0;
			double YawDegIn = 0.0;
			FVector OriginM = FVector::ZeroVector;
			bool bHasOrigin = false;
			if (SpecObj && SpecObj->IsValid())
			{
				(*SpecObj)->TryGetStringField(TEXT("item"), Item);
				(*SpecObj)->TryGetNumberField(TEXT("ratePerMin"), RatePerMin);
				(*SpecObj)->TryGetNumberField(TEXT("yawDeg"), YawDegIn);
				const TSharedPtr<FJsonObject>* OriginObj = nullptr;
				if ((*SpecObj)->TryGetObjectField(TEXT("origin"), OriginObj) && OriginObj->IsValid())
				{
					(*OriginObj)->TryGetNumberField(TEXT("x"), OriginM.X);
					(*OriginObj)->TryGetNumberField(TEXT("y"), OriginM.Y);
					(*OriginObj)->TryGetNumberField(TEXT("z"), OriginM.Z);
					bHasOrigin = true;
				}
			}
			if (Item.TrimStartAndEnd().IsEmpty() || RatePerMin <= 0.0 || RatePerMin > 10000.0)
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
					TEXT("spec needs item and a ratePerMin between 0 and 10000"), {}));
			}

			// Revision id validated up front (mirrors propose_build).
			FGuid ReplaceId;
			{
				FString ReplaceIdStr;
				Args->TryGetStringField(TEXT("replaceProposalId"), ReplaceIdStr);
				if (!ReplaceIdStr.TrimStartAndEnd().IsEmpty())
				{
					const FAIDAProposal* Replaced = FGuid::Parse(ReplaceIdStr.TrimStartAndEnd(), ReplaceId)
						? Actions.Store().Find(ReplaceId) : nullptr;
					if (!Replaced || Replaced->State != EAIDAProposalState::Pending)
					{
						return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
							TEXT("replaceProposalId doesn't match a pending proposal (it may have been decided or expired) — call get_proposal_status"), {}));
					}
				}
			}

			// The math half — same planner plan_factory exposes.
			UWorld* World = GetWorld();
			const double NowSeconds = World ? World->GetTimeSeconds() : 0.0;
			const FAIDAFactoryPlan Plan = FAIDAFactoryPlanner::Plan(Item, RatePerMin,
				RecipeCatalog.GetRecipes(World, NowSeconds), RecipeCatalog.GetBuildings(World, NowSeconds));
			if (!Plan.Error.IsEmpty())
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Plan.Error, {}));
			}
			if (Plan.Steps.Num() == 0)
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
					FString::Printf(TEXT("the plan for %s has no buildable steps"), *Item), {}));
			}

			// Build order: raw-most step first (row 0 nearest the origin), the product row last —
			// items flow along +Y away from the player.
			struct FRow
			{
				const FAIDAPlanStep* Step = nullptr;
				FString BuildRecipePath;         // the machine buildable
				FString BuildName;
				FString ProductionRecipePath;    // what it runs
				double FootXM = 8.0, FootYM = 8.0;
				double StepXM = 10.0;
				double RowYM = 0.0;              // row centerline, metres from origin along +Y
				int32 PlacementStart = 0;        // global placement index of the row's first machine
			};
			TArray<FRow> Rows;
			for (int32 i = Plan.Steps.Num() - 1; i >= 0; --i)
			{
				FRow Row;
				Row.Step = &Plan.Steps[i];
				FAIDARecipeResolution Machine;
				if (!FAIDAActionSeam::ResolveBuildRecipe(this, Row.Step->Building, Machine))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(FString::Printf(
						TEXT("the plan needs a %s but no unlocked buildable matches that name"), *Row.Step->Building), {}));
				}
				Row.BuildRecipePath = Machine.RecipeClassPath;
				Row.BuildName = Machine.DisplayName;
				Row.FootXM = FMath::Max(4.0, Row.Step->FootprintXM > 0.0 ? Row.Step->FootprintXM : Machine.FootprintXM);
				Row.FootYM = FMath::Max(4.0, Row.Step->FootprintYM > 0.0 ? Row.Step->FootprintYM : Machine.FootprintYM);
				Row.StepXM = Row.FootXM + 2.0;
				FString ProductionName;
				TArray<FString> Suggestions;
				if (!FAIDAActionSeam::ResolveProductionRecipe(this, Row.Step->Recipe, Row.ProductionRecipePath, ProductionName, Suggestions))
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(FString::Printf(
						TEXT("the plan's recipe '%s' did not resolve to a production recipe"), *Row.Step->Recipe), {}));
				}
				Rows.Add(MoveTemp(Row));
			}

			// Row lines: half of this row + half of the previous + a manifold corridor between.
			constexpr double RowGapM = 13.0; // out-row (4 m) + in-row (4 m) + belt clearance
			double CursorYM = 0.0;
			for (int32 i = 0; i < Rows.Num(); ++i)
			{
				if (i > 0) { CursorYM += Rows[i - 1].FootYM * 0.5 + RowGapM + Rows[i].FootYM * 0.5; }
				Rows[i].RowYM = CursorYM;
			}

			// Origin: spec > aim > player position, ground-probed for the slab base.
			FVector OriginCm;
			if (bHasOrigin)
			{
				OriginCm = OriginM * 100.0;
			}
			else if (!FAIDAActionSeam::ResolveAimPoint(this, Ctx.PlayerId, OriginCm))
			{
				if (!Ctx.bHasLocation)
				{
					return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(
						TEXT("could not resolve where the player is aiming — pass spec.origin {x,y,z in metres}"), {}));
				}
				OriginCm = Ctx.Location;
			}
			double GroundZ = OriginCm.Z;
			FAIDAActionSeam::ProbeGroundZ(this, OriginCm, GroundZ);

			const int32 YawDeg = FMath::RoundToInt32(YawDegIn);
			const double YawRad = FMath::DegreesToRadians(static_cast<double>(YawDeg));
			const FVector AxisX(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.0);
			const FVector AxisY(-AxisX.Y, AxisX.X, 0.0);

			// Foundation slab under everything (1 m thick, tops at GroundZ + 100): the rows' bounding
			// rect plus manifold corridors. Optional — an unresolved foundation just means terrain.
			FAIDARecipeResolution Foundation;
			const bool bFoundations = FAIDAActionSeam::ResolveBuildRecipe(this, TEXT("Foundation 8m x 1m"), Foundation);
			double MaxRowWidthM = 0.0;
			for (const FRow& Row : Rows)
			{
				MaxRowWidthM = FMath::Max(MaxRowWidthM, Row.Step->Machines * Row.StepXM);
			}
			const double SlabMinXM = -10.0, SlabMaxXM = MaxRowWidthM + 10.0;
			const double SlabMinYM = -Rows[0].FootYM * 0.5 - 8.0;
			const double SlabMaxYM = Rows.Last().RowYM + Rows.Last().FootYM * 0.5 + 8.0;
			const double MachineZ = bFoundations ? GroundZ + 100.0 : GroundZ;

			TArray<FString> PartRecipePaths;
			TArray<FString> PartNames;
			TArray<FTransform> Placements;
			TArray<int32> PlacementPartIndex;
			TArray<FString> MachineRecipeToSet;
			TArray<double> MachineClockToSet;
			int32 FoundationCount = 0;
			if (bFoundations)
			{
				PartRecipePaths.Add(Foundation.RecipeClassPath);
				PartNames.Add(Foundation.DisplayName);
				for (double XM = SlabMinXM + 4.0; XM < SlabMaxXM; XM += 8.0)
				{
					for (double YM = SlabMinYM + 4.0; YM < SlabMaxYM; YM += 8.0)
					{
						const FVector Loc = OriginCm + AxisX * (XM * 100.0) + AxisY * (YM * 100.0)
							+ FVector(0.0, 0.0, GroundZ + 50.0 - OriginCm.Z); // pivot mid-thickness
						Placements.Emplace(FRotator(0.0, YawDeg, 0.0), FVector(Loc.X, Loc.Y, GroundZ + 50.0));
						PlacementPartIndex.Add(0);
						MachineRecipeToSet.Add(FString());
						MachineClockToSet.Add(0.0);
						++FoundationCount;
					}
				}
			}
			for (FRow& Row : Rows)
			{
				const int32 Part = PartRecipePaths.Num();
				PartRecipePaths.Add(Row.BuildRecipePath);
				PartNames.Add(Row.BuildName);
				Row.PlacementStart = Placements.Num();
				for (int32 m = 0; m < Row.Step->Machines; ++m)
				{
					const FVector Loc = OriginCm
						+ AxisX * ((m * Row.StepXM + Row.StepXM * 0.5) * 100.0)
						+ AxisY * (Row.RowYM * 100.0);
					Placements.Emplace(FRotator(0.0, YawDeg, 0.0), FVector(Loc.X, Loc.Y, MachineZ));
					PlacementPartIndex.Add(Part);
					MachineRecipeToSet.Add(Row.ProductionRecipePath);
					MachineClockToSet.Add(FMath::Clamp(Row.Step->Clock * 100.0, 1.0, 100.0));
				}
			}
			if (Config.Actions.MaxProposalItems > 0 && Placements.Num() > Config.Actions.MaxProposalItems)
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(FString::Printf(
					TEXT("the factory needs %d placements — more than maxProposalItems (%d); lower the rate or raise the cap"),
					Placements.Num(), Config.Actions.MaxProposalItems), {}));
			}

			FAIDADryRunResult DryRun;
			if (!FAIDAActionSeam::DryRunBuildParts(this, PartRecipePaths, PlacementPartIndex, Placements, DryRun, Ctx.PlayerId))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(DryRun.Error, {}));
			}

			// Manifold rows per step: every belt/pipe port rank on each side gets its own row (an
			// assembler's two inputs = two feed rows). Attachment/transport recipes cache per kind.
			FAIDAProposal Proposal;
			TArray<FVector> UsedPorts;
			struct FSetMeta { int32 Row = 0; bool bPipe = false; bool bOutput = false; int32 Ordinal = 0; };
			TArray<FSetMeta> SetMeta;
			FAIDARecipeResolution Splitter, Merger, Junction, Belt, Pipe;
			const bool bHaveSplitter = FAIDAActionSeam::ResolveBuildRecipe(this, TEXT("Conveyor Splitter"), Splitter);
			const bool bHaveMerger = FAIDAActionSeam::ResolveBuildRecipe(this, TEXT("Conveyor Merger"), Merger);
			const bool bHaveJunction = FAIDAActionSeam::ResolveBuildRecipe(this, TEXT("Pipeline Junction Cross"), Junction);
			bool bHaveBelt = false, bHavePipe = false;
			for (const TCHAR* Name : { TEXT("Conveyor Belt Mk.6"), TEXT("Conveyor Belt Mk.5"), TEXT("Conveyor Belt Mk.4"),
				TEXT("Conveyor Belt Mk.3"), TEXT("Conveyor Belt Mk.2"), TEXT("Conveyor Belt Mk.1") })
			{
				if (FAIDAActionSeam::ResolveBuildRecipe(this, Name, Belt)) { bHaveBelt = true; break; }
			}
			for (const TCHAR* Name : { TEXT("Pipeline Mk.2"), TEXT("Pipeline") })
			{
				if (FAIDAActionSeam::ResolveBuildRecipe(this, Name, Pipe)) { bHavePipe = true; break; }
			}
			int32 RowsSkippedNoKit = 0;
			for (int32 r = 0; r < Rows.Num(); ++r)
			{
				const FRow& Row = Rows[r];
				const TArray<FTransform> RowPlacements(Placements.GetData() + Row.PlacementStart, Row.Step->Machines);
				for (const bool bPipeKind : { false, true })
				{
					for (const bool bOutput : { false, true })
					{
						for (int32 Ordinal = 0; Ordinal < 3; ++Ordinal)
						{
							TArray<FAIDAManifoldPort> Ports;
							TArray<int32> PortIdx;
							if (!FAIDAActionSeam::ResolvePlannedPorts(this, Row.BuildRecipePath, Row.BuildName,
								RowPlacements, bPipeKind, bOutput, Ordinal, UsedPorts, Ports, PortIdx)
								|| Ports.Num() == 0)
							{
								break; // no more ports of this kind/side
							}
							const FAIDARecipeResolution& Attachment = bPipeKind ? Junction : (bOutput ? Merger : Splitter);
							const bool bHaveAttachment = bPipeKind ? bHaveJunction : (bOutput ? bHaveMerger : bHaveSplitter);
							const bool bHaveTransport = bPipeKind ? bHavePipe : bHaveBelt;
							if (!bHaveAttachment || !bHaveTransport)
							{
								++RowsSkippedNoKit;
								break;
							}

							TArray<FAIDAManifoldPortPoint> Points;
							FVector AvgNormal = FVector::ZeroVector;
							for (const FAIDAManifoldPort& Port : Ports)
							{
								Points.Add({ Port.PosCm, Port.NormalCm });
								AvgNormal += Port.NormalCm;
							}
							AvgNormal = AvgNormal.GetSafeNormal();
							// Same-side rows on this step stack one lane further out.
							int32 Lane = 0;
							for (int32 s = 0; s < Proposal.ManifoldSets.Num(); ++s)
							{
								if (SetMeta[s].Row != r) { continue; }
								FVector SetNormal = FVector::ZeroVector;
								for (const FAIDAManifoldPort& Port : Proposal.ManifoldSets[s].Ports) { SetNormal += Port.NormalCm; }
								if (FVector::DotProduct(SetNormal.GetSafeNormal(), AvgNormal) > 0.5) { ++Lane; }
							}
							FAIDAManifoldPlan RowPlan = AIDAActionSpec::PlanManifold(Points, bOutput, bPipeKind,
								4.0 + Lane * 5.0, 4.0, /*MaxRunM*/ 56.0);
							if (!RowPlan.Error.IsEmpty())
							{
								++RowsSkippedNoKit;
								break;
							}
							for (FTransform& Placement : RowPlan.Attachments)
							{
								// Attachments sit at the ports' level — the slab is PENDING, so a
								// terrain probe would bury them (same guard as propose_manifold).
								FVector Location = Placement.GetLocation();
								double AttachGroundZ;
								if (FAIDAActionSeam::ProbeGroundZ(this, Location, AttachGroundZ)
									&& Location.Z - AttachGroundZ <= 400.0)
								{
									Location.Z = AttachGroundZ;
								}
								else
								{
									Location.Z -= 100.0;
								}
								Placement.SetLocation(Location);
							}

							FAIDAManifoldSet NewSet;
							NewSet.bPipe = bPipeKind;
							NewSet.bOutput = bOutput;
							NewSet.TransportRecipePath = (bPipeKind ? Pipe : Belt).RecipeClassPath;
							NewSet.TransportName = (bPipeKind ? Pipe : Belt).DisplayName;
							NewSet.AttachmentRecipePath = Attachment.RecipeClassPath;
							NewSet.AttachmentName = Attachment.DisplayName;
							for (const int32 P : RowPlan.PortOrder)
							{
								NewSet.Ports.Add(Ports[P]);
								NewSet.PortMachineIndex.Add(Row.PlacementStart + PortIdx[P]);
								UsedPorts.Add(Ports[P].PosCm);
							}
							NewSet.Attachments = MoveTemp(RowPlan.Attachments);
							NewSet.RowAxis = RowPlan.RowAxis;
							NewSet.DropDir = RowPlan.DropDir;
							TArray<FAIDACostItem> AttachmentCost;
							FAIDAActionSeam::TallyRecipeCost(this, NewSet.AttachmentRecipePath, NewSet.Attachments.Num(), AttachmentCost);
							for (const FAIDACostItem& CostItem : AttachmentCost)
							{
								bool bMerged = false;
								for (FAIDACostItem& Existing : DryRun.Cost)
								{
									if (Existing.Item == CostItem.Item) { Existing.Amount += CostItem.Amount; bMerged = true; break; }
								}
								if (!bMerged) { DryRun.Cost.Add(CostItem); }
							}
							Proposal.ManifoldSets.Add(MoveTemp(NewSet));
							SetMeta.Add({ r, bPipeKind, bOutput, Ordinal });
						}
					}
				}
			}

			// Inter-step links: consumer row's k-th same-kind ingredient row hooks to the producer's
			// out row of that kind — recipe ingredients decide who feeds whom, not adjacency.
			int32 LinksPlanned = 0;
			for (int32 c = 0; c < Rows.Num(); ++c)
			{
				UClass* ProductionClass = FSoftClassPath(Rows[c].ProductionRecipePath).TryLoadClass<UFGRecipe>();
				if (!ProductionClass) { continue; }
				int32 SolidOrdinal = 0, FluidOrdinal = 0;
				for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(this, ProductionClass))
				{
					if (!Ingredient.ItemClass) { continue; }
					const EResourceForm Form = UFGItemDescriptor::GetForm(Ingredient.ItemClass);
					const bool bFluid = Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS;
					const int32 Ordinal = bFluid ? FluidOrdinal++ : SolidOrdinal++;
					const FString IngredientName = UFGItemDescriptor::GetItemName(Ingredient.ItemClass).ToString();

					int32 Producer = INDEX_NONE;
					for (int32 p = 0; p < Rows.Num(); ++p)
					{
						if (p != c && Rows[p].Step->Item == IngredientName) { Producer = p; break; }
					}
					if (Producer == INDEX_NONE) { continue; } // a raw input — listed for taps

					int32 FromSet = INDEX_NONE, ToSet = INDEX_NONE;
					for (int32 s = 0; s < SetMeta.Num(); ++s)
					{
						if (SetMeta[s].Row == Producer && SetMeta[s].bOutput && SetMeta[s].bPipe == bFluid
							&& SetMeta[s].Ordinal == 0) { FromSet = s; }
						if (SetMeta[s].Row == c && !SetMeta[s].bOutput && SetMeta[s].bPipe == bFluid
							&& SetMeta[s].Ordinal == Ordinal) { ToSet = s; }
					}
					if (ToSet == INDEX_NONE)
					{
						for (int32 s = 0; s < SetMeta.Num(); ++s)
						{
							if (SetMeta[s].Row == c && !SetMeta[s].bOutput && SetMeta[s].bPipe == bFluid) { ToSet = s; break; }
						}
					}
					if (FromSet != INDEX_NONE && ToSet != INDEX_NONE)
					{
						Proposal.StepLinks.Add(FIntPoint(FromSet, ToSet));
						++LinksPlanned;
					}
				}
			}

			// Auto-power: one kit across all rows (poles behind each row chunk, machines wired to
			// their chunk's pole, poles chained + tied to the grid at execute).
			FAIDAActionSeam::FAIDAPowerInfo Power;
			FString PowerError;
			bool bAutoPower = false;
			if (Rows.Num() > 0 && FAIDAActionSeam::ResolveAutoPower(this, Rows[0].BuildRecipePath, FString(), Power, PowerError))
			{
				const int32 PerPole = FMath::Max(1, Power.PoleConnectionCap - 2);
				for (const FRow& Row : Rows)
				{
					const int32 Chunks = FMath::DivideAndRoundUp(Row.Step->Machines, PerPole);
					for (int32 ChunkIdx = 0; ChunkIdx < Chunks; ++ChunkIdx)
					{
						const int32 First = ChunkIdx * PerPole;
						const int32 Count = FMath::Min(PerPole, Row.Step->Machines - First);
						const double MidXM = (First + Count * 0.5) * Row.StepXM;
						const FVector Loc = OriginCm + AxisX * (MidXM * 100.0)
							+ AxisY * ((Row.RowYM + Row.FootYM * 0.5 + 2.0) * 100.0);
						const int32 PoleIdx = Proposal.PolePlacements.Num();
						Proposal.PolePlacements.Emplace(FRotator(0.0, YawDeg, 0.0), FVector(Loc.X, Loc.Y, MachineZ));
						for (int32 m = 0; m < Count; ++m)
						{
							Proposal.MachineWires.Add(FIntPoint(Row.PlacementStart + First + m, PoleIdx));
						}
						if (PoleIdx > 0)
						{
							Proposal.ChainWires.Add(FIntPoint(PoleIdx - 1, PoleIdx));
						}
					}
				}
				if (Proposal.PolePlacements.Num() > 0)
				{
					Proposal.PoleRecipePath = Power.PoleRecipePath;
					Proposal.PoleName = Power.PoleName;
					Proposal.WireRecipePath = Power.WireRecipePath;
					bAutoPower = true;
					TArray<FAIDACostItem> PoleCost;
					FAIDAActionSeam::TallyRecipeCost(this, Power.PoleRecipePath, Proposal.PolePlacements.Num(), PoleCost);
					for (const FAIDACostItem& CostItem : PoleCost)
					{
						bool bMerged = false;
						for (FAIDACostItem& Existing : DryRun.Cost)
						{
							if (Existing.Item == CostItem.Item) { Existing.Amount += CostItem.Amount; bMerged = true; break; }
						}
						if (!bMerged) { DryRun.Cost.Add(CostItem); }
					}
				}
			}

			if (Config.Actions.CostMode == TEXT("central")
				&& !FAIDAActionSeam::CheckAffordable(this, DryRun.Cost, Ctx.PlayerId))
			{
				FString Msg = TEXT("not affordable from central storage + your inventory: needs ");
				for (int32 i = 0; i < DryRun.Cost.Num(); ++i)
				{
					Msg += FString::Printf(TEXT("%s%d %s"), i > 0 ? TEXT(", ") : TEXT(""), DryRun.Cost[i].Amount, *DryRun.Cost[i].Item);
				}
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Msg, {}));
			}

			Proposal.Id = FGuid::NewGuid();
			Proposal.RequesterId = Ctx.PlayerId;
			Proposal.RequesterName = Ctx.Author;
			Proposal.Placements = MoveTemp(Placements);
			Proposal.RecipeClassPath = PartRecipePaths[0];
			Proposal.PartRecipePaths = MoveTemp(PartRecipePaths);
			Proposal.PlacementPartIndex = MoveTemp(PlacementPartIndex);
			Proposal.MachineRecipeToSet = MoveTemp(MachineRecipeToSet);
			Proposal.MachineClockToSet = MoveTemp(MachineClockToSet);
			Proposal.bAutoPower = bAutoPower;
			Proposal.Cost = DryRun.Cost;
			Proposal.InvalidCount = DryRun.Failures.Num();
			{
				FString StepsDesc;
				for (int32 i = Rows.Num() - 1; i >= 0; --i)
				{
					StepsDesc += FString::Printf(TEXT("%s%d x %s @ %.0f%%"), StepsDesc.IsEmpty() ? TEXT("") : TEXT(", "),
						Rows[i].Step->Machines, *Rows[i].BuildName, Rows[i].Step->Clock * 100.0);
				}
				Proposal.Summary = FString::Printf(TEXT("factory for %.4g %s/min: %s — %d manifold row(s), %d step link(s)%s%s"),
					Plan.TargetPerMin, *Plan.TargetItem, *StepsDesc,
					Proposal.ManifoldSets.Num(), LinksPlanned,
					bAutoPower ? *FString::Printf(TEXT(", %d pole(s)"), Proposal.PolePlacements.Num()) : TEXT(""),
					bFoundations ? *FString::Printf(TEXT(", %d foundation(s)"), FoundationCount) : TEXT(""));
			}
			{
				TSharedRef<FJsonObject> StoredSpec = MakeShared<FJsonObject>();
				StoredSpec->SetStringField(TEXT("tool"), TEXT("propose_factory"));
				StoredSpec->SetStringField(TEXT("item"), Plan.TargetItem);
				StoredSpec->SetNumberField(TEXT("ratePerMin"), Plan.TargetPerMin);
				Proposal.SpecJson = AIDAToCompactJson(StoredSpec);
			}

			if (ReplaceId.IsValid())
			{
				SupersedeProposal(ReplaceId);
			}
			const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
			FString Error;
			if (!Actions.Store().Add(Proposal, Now, Config.Actions.MaxPendingProposals, Error))
			{
				return FAIDAToolResult::Error(AIDAActionSpec::BuildErrorJson(Error, {}));
			}
			UE_LOG(LogAIDA, Log, TEXT("[actions] proposal %s stored (factory): %s (by %s, %d blocked placement(s))"),
				*Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens), *Proposal.Summary, *Ctx.Author, Proposal.InvalidCount);
			PublishProposal(Proposal.Id);

			FString RawList;
			for (const FAIDAPlanResource& Raw : Plan.RawInputs)
			{
				RawList += FString::Printf(TEXT("%s%.4g %s/min"), RawList.IsEmpty() ? TEXT("") : TEXT(", "), Raw.RatePerMin, *Raw.Item);
			}
			FString Announce = FString::Printf(TEXT("AIDA proposes (for %s%s): %s — cost %s. Awaiting approval."),
				*Ctx.Author, ReplaceId.IsValid() ? TEXT(", revised") : TEXT(""),
				*Proposal.Summary, *AIDACostSummaryString(Proposal.Cost));
			if (!RawList.IsEmpty())
			{
				Announce += FString::Printf(TEXT(" Raw inputs to feed after approval: %s."), *RawList);
			}
			AnnounceSystem(Announce);

			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("proposalId"), Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens));
			Root->SetStringField(TEXT("summary"), Proposal.Summary);
			Root->SetNumberField(TEXT("placements"), Proposal.Placements.Num());
			Root->SetNumberField(TEXT("manifoldRows"), Proposal.ManifoldSets.Num());
			Root->SetNumberField(TEXT("stepLinks"), LinksPlanned);
			if (Proposal.InvalidCount > 0) { Root->SetNumberField(TEXT("invalidCount"), Proposal.InvalidCount); }
			if (RowsSkippedNoKit > 0) { Root->SetNumberField(TEXT("manifoldRowsSkipped"), RowsSkippedNoKit); }
			if (!RawList.IsEmpty())
			{
				Root->SetStringField(TEXT("rawInputs"), RawList);
				Root->SetStringField(TEXT("rawInputsHint"), TEXT("offer propose_belt_tap / propose_pipe_tap with forProposalId to feed these from existing lines"));
			}
			Root->SetNumberField(TEXT("expiresInSeconds"), Config.Actions.TtlSeconds);
			return FAIDAToolResult::Ok(AIDAToCompactJson(Root));
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
	int32 RoundsLeft, FAIDAOnChunk OnDelta, TFunction<void(const FString&)> OnDone, FAIDAOnError OnError,
	bool bReadOnly)
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
		[Weak, Messages, Requester, RoundsLeft, OnDelta, OnDone, OnError, bReadOnly](const FAIDACompletionResult& Result)
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
				O->RunToolLoop(Messages, Requester, RoundsLeft - 1, OnDelta, OnDone, OnError, bReadOnly);
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
				if (Spec && bReadOnly && Spec->Tier == EAIDAToolTier::Act)
				{
					// Standing tasks can never mutate (docs/PHASE7.md Slice 6) — a check that finds
					// drift REPORTS; a human decides.
					Part.bIsError = true;
					Part.Content = FString::Printf(TEXT("This is a read-only background check: '%s' can modify the world and is not available. Report the finding in your reply instead."), *Call.Name);
				}
				else if (Spec && !O->Permissions.IsAllowed(ToPermissionTier(Spec->Tier), Requester.PlayerId))
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

			O->RunToolLoop(Messages, Requester, RoundsLeft - 1, OnDelta, OnDone, OnError, bReadOnly);
		},
		OnError);
}

void UAIDAOrchestrator::HandleTaskCommand(const FAIDARequester& Requester, const FAIDAChatCommand& Command, const FGuid& ConversationId)
{
	AAIDAMemoryStore* Store = Memory.Store(this);
	if (!Store || !Session.IsValid())
	{
		return;
	}
	const auto Post = [this, &ConversationId](const FString& Message)
	{
		Session->PostSystemMessage(Message, ConversationId);
	};

	switch (Command.TaskOp)
	{
	case FAIDAChatCommand::ETaskOp::Add:
	{
		if (!Config.Tasks.bEnabled)
		{
			Post(TEXT("Standing tasks are disabled on this server — a server admin can turn them on with tasks.enabled in the AIDA config."));
			return;
		}
		FAIDAStandingTask Task;
		Task.Prompt = Command.TaskPrompt;
		Task.IntervalMinutes = FMath::Max(Command.TaskIntervalMinutes, Config.Tasks.MinIntervalMinutes);
		Task.CreatedById = Requester.PlayerId;
		Task.CreatedByName = Requester.Author;
		Task.LastRunUtc = FDateTime::UtcNow().ToUnixTimestamp(); // first run one interval from now
		Store->AddTask(Task);
		Post(FString::Printf(TEXT("Standing task #%d added: \"%s\" every %d min (read-only; first check in ~%d min). Quiet checks say nothing — you only hear about findings."),
			Store->GetTasks().Num(), *Task.Prompt, Task.IntervalMinutes, Task.IntervalMinutes));
		return;
	}
	case FAIDAChatCommand::ETaskOp::List:
	{
		const TArray<FAIDAStandingTask>& Tasks = Store->GetTasks();
		if (Tasks.Num() == 0)
		{
			Post(TEXT("No standing tasks. Add one with: /aida task add \"<what to check>\" every 10m"));
			return;
		}
		FString Lines = FString::Printf(TEXT("%d standing task(s)%s:"), Tasks.Num(),
			Config.Tasks.bEnabled ? TEXT("") : TEXT(" (tasks are DISABLED in config — none will run)"));
		const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
		for (int32 i = 0; i < Tasks.Num(); ++i)
		{
			const FAIDAStandingTask& Task = Tasks[i];
			const int64 AgeMin = Task.LastRunUtc > 0 ? (Now - Task.LastRunUtc) / 60 : -1;
			Lines += FString::Printf(TEXT("\n%d. [%s] \"%s\" every %d min (by %s%s%s)"),
				i + 1, Task.bEnabled ? TEXT("on") : TEXT("paused"), *Task.Prompt, Task.IntervalMinutes,
				*Task.CreatedByName,
				AgeMin >= 0 ? *FString::Printf(TEXT(", last run %lld min ago"), AgeMin) : TEXT(""),
				Task.LastResult.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(": %s"), *Task.LastResult.Left(120)));
		}
		Post(Lines);
		return;
	}
	case FAIDAChatCommand::ETaskOp::Remove:
	case FAIDAChatCommand::ETaskOp::Pause:
	case FAIDAChatCommand::ETaskOp::Resume:
	{
		const TArray<FAIDAStandingTask>& Tasks = Store->GetTasks();
		if (!Tasks.IsValidIndex(Command.TaskIndex - 1))
		{
			Post(FString::Printf(TEXT("No task #%d — /aida task list shows the numbers."), Command.TaskIndex));
			return;
		}
		const FGuid Id = Tasks[Command.TaskIndex - 1].Id;
		if (Command.TaskOp == FAIDAChatCommand::ETaskOp::Remove)
		{
			Store->RemoveTask(Id);
			Post(FString::Printf(TEXT("Standing task #%d removed."), Command.TaskIndex));
		}
		else if (FAIDAStandingTask* Task = Store->FindTask(Id))
		{
			Task->bEnabled = Command.TaskOp == FAIDAChatCommand::ETaskOp::Resume;
			Post(FString::Printf(TEXT("Standing task #%d %s."), Command.TaskIndex,
				Task->bEnabled ? TEXT("resumed") : TEXT("paused")));
		}
		return;
	}
	default:
		return;
	}
}

void UAIDAOrchestrator::OnTaskTimer()
{
	if (bTaskRunning || !Config.Tasks.bEnabled || !LLMClient.IsValid() || !LLMClient->IsReady())
	{
		return;
	}
	AAIDAMemoryStore* Store = Memory.Store(this);
	if (!Store)
	{
		return;
	}

	// Daily budget: a hard cap on unattended LLM runs, reset at UTC midnight.
	const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
	const int64 Day = Now / 86400;
	if (Day != TaskDayStamp)
	{
		TaskDayStamp = Day;
		TaskRunsToday = 0;
	}
	if (TaskRunsToday >= Config.Tasks.MaxPerDay)
	{
		return; // quiet — the budget line was logged when the last run spent it
	}

	// Most-overdue due task wins; one runs at a time.
	FGuid DueId;
	int64 MostOverdue = 0;
	for (const FAIDAStandingTask& Task : Store->GetTasks())
	{
		if (!Task.bEnabled) { continue; }
		const int64 IntervalSeconds = 60 * static_cast<int64>(FMath::Max(Task.IntervalMinutes, Config.Tasks.MinIntervalMinutes));
		const int64 Overdue = Now - Task.LastRunUtc - IntervalSeconds;
		if (Overdue >= 0 && Overdue >= MostOverdue)
		{
			MostOverdue = Overdue;
			DueId = Task.Id;
		}
	}
	FAIDAStandingTask* Due = DueId.IsValid() ? Store->FindTask(DueId) : nullptr;
	if (!Due)
	{
		return;
	}

	Due->LastRunUtc = Now; // stamped up front so a hung run can't hot-loop
	++TaskRunsToday;
	bTaskRunning = true;
	UE_LOG(LogAIDA, Log, TEXT("[tasks] running \"%s\" (run %d/%d today)."),
		*Due->Prompt, TaskRunsToday, Config.Tasks.MaxPerDay);

	// A compact task-specific system prompt (the chat prompt's build guidance is noise here); the
	// next chat request sets its own prompt again, so this never leaks into conversations.
	LLMClient->SetSystemPrompt(TEXT(
		"You are AIDA running a standing background check inside the game Satisfactory — no player is chatting "
		"with you right now. Use your read-only factory tools to perform the check below, then reply with ONE "
		"short line for the chat feed. CONTRACT: if everything is fine and nothing needs a player's attention, "
		"reply with exactly OK and nothing else — quiet results are silently discarded, and padding them wastes "
		"everyone's attention. If something IS wrong, say what and where (metres) in one or two sentences. "
		"World-modifying tools are unavailable in background checks — report, never fix."));

	const TSharedRef<TArray<FAIDAChatMessage>, ESPMode::ThreadSafe> Messages =
		MakeShared<TArray<FAIDAChatMessage>, ESPMode::ThreadSafe>();
	Messages->Add({ TEXT("user"), FString::Printf(TEXT("Standing check (created by %s): %s"), *Due->CreatedByName, *Due->Prompt) });

	FAIDARequester Requester;
	Requester.Author = Due->CreatedByName;
	Requester.PlayerId = Due->CreatedById;

	TWeakObjectPtr<UAIDAOrchestrator> Weak(this);
	const FGuid TaskId = Due->Id;
	RunToolLoop(Messages, Requester, MaxToolRoundTrips,
		[](const FString&) {}, // background — nothing streams to a widget
		[Weak, TaskId](const FString& Text)
		{
			UAIDAOrchestrator* O = Weak.Get();
			if (!O) { return; }
			O->bTaskRunning = false;
			const FString Result = Text.TrimStartAndEnd();
			if (AAIDAMemoryStore* S = O->Memory.Store(O))
			{
				if (FAIDAStandingTask* Task = S->FindTask(TaskId))
				{
					Task->LastResult = Result.Left(300);
				}
			}
			// The OK contract: quiet results are swallowed; findings go to chat as a System line.
			const bool bQuiet = Result.IsEmpty() || Result.Equals(TEXT("OK"), ESearchCase::IgnoreCase)
				|| (Result.Len() <= 4 && Result.StartsWith(TEXT("OK"), ESearchCase::IgnoreCase));
			if (bQuiet)
			{
				UE_LOG(LogAIDA, Log, TEXT("[tasks] check quiet (OK)."));
			}
			else
			{
				UE_LOG(LogAIDA, Log, TEXT("[tasks] check found something: %s"), *Result);
				O->AnnounceSystem(FString::Printf(TEXT("AIDA standing check: %s"), *Result.Left(400)));
			}
		},
		[Weak, TaskId](int32 /*Code*/, const FString& Message)
		{
			UAIDAOrchestrator* O = Weak.Get();
			if (!O) { return; }
			O->bTaskRunning = false;
			UE_LOG(LogAIDA, Warning, TEXT("[tasks] check failed: %s"), *Message);
			if (AAIDAMemoryStore* S = O->Memory.Store(O))
			{
				if (FAIDAStandingTask* Task = S->FindTask(TaskId))
				{
					Task->LastResult = FString::Printf(TEXT("error: %s"), *Message.Left(200));
				}
			}
		},
		/*bReadOnly*/ true);
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
