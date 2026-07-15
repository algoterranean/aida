#include "Core/AIDAOrchestrator.h"

#include "AIDA.h"
#include "Core/AIDAConfigLoader.h"
#include "Adapters/LLMClient.h"
#include "Factory/AIDAFactoryAggregator.h"
#include "Tools/AIDAFactoryTools.h"
#include "Tools/AIDAMapTools.h"
#include "Tools/AIDARecipeTools.h"
#include "Tools/AIDANotesTools.h"
#include "Tools/AIDAToolJson.h"
#include "Memory/AIDAMemoryStore.h"
#include "Net/AIDAChatRelay.h"
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

	/** Resolve a requesting player's pawn location by their net-id string (as set in the RCO). */
	bool AIDAResolvePlayerLocation(UWorld* World, const FString& PlayerId, FVector& OutLocation)
	{
		if (!World || PlayerId.IsEmpty() || PlayerId == TEXT("debug")) { return false; }
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PC = It->Get();
			APlayerState* PS = PC ? PC->PlayerState : nullptr;
			if (!PS) { continue; }
			const TSharedPtr<const FUniqueNetId> NetId = PS->GetUniqueId().GetUniqueNetId();
			if (NetId.IsValid() && NetId->ToString() == PlayerId)
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
		"player asks what they wanted to do, or refers to something they told you earlier.\n\n"
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

	// The in-save memory store (Phase 3). SpawnOnServer, not replicated — persists via IFGSaveInterface;
	// only the server touches it. Bind the memory facade to it (mints/reads the session GUID).
	if (World->GetNetMode() != NM_Client)
	{
		Mgr->RegisterSubsystemActor(AAIDAMemoryStore::StaticClass());
		Memory.Init(this);
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
		TEXT("Recall the player's saved notes. Filter with 'keyword' (matches text or a tag) and/or 'region'; set 'near' true to sort by distance to the player. Omit all for the most recent notes."),
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

	UE_LOG(LogAIDA, Log, TEXT("[tools] registered %d tool(s)."), Tools.Num());
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
