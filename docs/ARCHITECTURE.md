# AIDA — Architecture & Design Document

**AIDA** (*AI Design Assistant* / a respectful nod to ADA) is an open-source Satisfactory mod that embeds an LLM-powered assistant into the game: conversational help, factory-state awareness, map and resource-node intelligence, shared memory, and — in later phases — AI-proposed construction with dry-run, confirmation, and undo. Built on SML for multiplayer, with dedicated servers as a first-class target.

- **Mod reference:** `AIDA`
- **License:** MIT (open source from first commit)
- **Distribution:** ficsit.app (SMR)
- **Status:** Design draft v0.1

---

## 1. Design Decisions (Record)

These were settled before design began and are treated as constraints, not open questions.

| # | Decision | Choice |
|---|----------|--------|
| 1 | LLM API shape | OpenAI-compatible native; translation adapter for Anthropic. Base URL configurable (supports Ollama/local). |
| 2 | Response delivery | Streaming from day one (chunked replication protocol). |
| 3 | World-action safety | Mandatory proposal → dry-run → confirm → execute pipeline, with action journal and undo. |
| 4 | Persistence | Small critical state in the save file (SML save hooks); bulky data (history, transcripts) in server-side sidecar files keyed to session. |
| 5 | Update resilience | All game-class access isolated behind the extraction layer; graceful degradation when extraction breaks. |
| 6 | Source model | Open source, MIT. |
| 7 | Privacy | Loud disclosure; config flags controlling what context may leave the server. |
| 8 | Scope | Full vision, delivered in phases (see §10). Each phase ships something usable. |
| 9 | Topology | Client-server. Dedicated (Linux, headless) server is the primary deployment target. Server makes all LLM calls. |

---

## 2. System Overview

```
┌──────────────── CLIENT (per player) ────────────────┐
│  ChatWidget (UMG)      MapMarkerUI      ProposalUI  │
│        │ Server RPCs           ▲ replication        │
└────────┼───────────────────────┼────────────────────┘
         ▼                       │
┌──────────────── SERVER (authoritative) ─────────────┐
│  AIDAOrchestrator (subsystem)                       │
│   ├─ PermissionService      ├─ RateLimiter          │
│   ├─ SessionManager (chat state, streaming fan-out) │
│   ├─ LLMClient ──► ProviderAdapter (OpenAI | Claude │
│   │                 translator | custom base URL)   │
│   ├─ ToolRegistry ─► FactoryIndex (extract+aggregate)│
│   │                ─► MapService (nodes, regions)   │
│   │                ─► NotesStore                    │
│   │                ─► SnapshotService (history)     │
│   │                ─► ActionEngine (Phase 4+)       │
│   └─ Persistence (in-save component + sidecar files)│
└────────────────────────┬────────────────────────────┘
                         ▼  HTTPS (FHttpModule)
                 LLM endpoint (Anthropic / OpenAI-
                 compatible / Ollama on LAN)
```

Principles that govern everything below:

1. **The server is the only authority and the only network egress point.** Clients never hold API keys, never call the LLM, and never mutate the world directly.
2. **The LLM sees tools, not dumps.** Factory state is indexed and queried, never serialized wholesale into prompts.
3. **Everything player-visible replicates through standard Unreal mechanisms.** No custom netcode; RPCs and replicated actors/subsystems only.
4. **Every game-API touchpoint lives in one module** (FactoryIndex/MapService/ActionEngine) so game patches break one seam, not the whole mod.

---

## 3. Module Breakdown

### 3.1 Client modules

**ChatWidget (UMG, Blueprint-heavy).** Dockable chat panel. Renders the replicated shared transcript, per-message author badges (player name or AIDA), streaming partial messages, and a "thinking" indicator. Sends player messages via a Server RPC on the player's `AIDAPlayerComponent`. No business logic; purely view + input.

**ProposalUI.** Modal/panel that renders an `FAIDAProposal` (see §6): human-readable action summary, cost estimate, affected area, dry-run results. Buttons: Approve / Reject / Show on Map. Approval sends a Server RPC carrying the proposal ID (never the proposal contents — the server's copy is authoritative).

**MapMarkerUI.** Thin layer over the game's existing (already replicated) map marker system. Renders AIDA-created markers with a distinct icon set (tagged node, note, proposal footprint).

**AIDAPlayerComponent.** Actor component on the player controller/state. Owns the client↔server RPC surface for that player: `ServerSendChat`, `ServerApproveProposal`, `ServerAddNote`, etc. Also receives targeted (owner-only) RPCs for private data if per-player chat is later enabled.

### 3.2 Server modules

**AIDAOrchestrator** — an SML world subsystem, initialized with the world (never with a player; must be safe with zero players connected). Wires all other services, owns the request lifecycle: receive chat RPC → permission check → rate-limit check → build context → LLM call → tool loop → stream out → journal.

**PermissionService.** Config-driven. Three tiers: `chat` (default: everyone), `query` (factory/map tools; default: everyone), `act` (world-modifying proposals; default: allowlist of player IDs from config). Checked server-side on every RPC — client UI hiding is cosmetic, never security.

**RateLimiter.** Token-bucket per player and global, configurable (requests/min, max output tokens per request, max tool round-trips per request — default 5). Emits friendly in-chat errors when exceeded.

**SessionManager.** Owns the shared chat transcript (bounded ring buffer, e.g. last 200 messages), assigns message IDs, manages streaming fan-out (§5), and prunes/summarizes history for prompt context (recent N messages verbatim + rolling summary of older ones, generated cheaply by the LLM itself during idle time).

**LLMClient + ProviderAdapter.** `LLMClient` exposes one internal interface: `Complete(request, onChunk, onToolCall, onDone, onError)` with streaming and tool-use semantics. Adapters translate to the wire format:
- `OpenAICompatAdapter` — native format; works for OpenAI, Ollama, vLLM, most gateways. SSE streaming parser.
- `AnthropicAdapter` — translates messages/tools to the Anthropic Messages API, maps `tool_use`/`tool_result` blocks and SSE events back to the common interface. Also carries vision payloads in Phase 5.
Provider, base URL, model name, and API key are config-only, server-side, never replicated. HTTP via `FHttpModule`; all handling on the game thread via async callbacks — never block.

**ToolRegistry.** Declarative registry of tools exposed to the model: name, JSON schema, permission tier, handler. Tool definitions and the system prompt load from data files (`Configs/AIDA/prompts/`, `tools.json`) so server admins can customize without recompiling; the mod ships defaults and hot-validates admin overrides against the schema.

**FactoryIndex.** The extraction + aggregation layer (§4). The *only* module (besides MapService/ActionEngine) that includes game headers like `AFGBuildableSubsystem`.

**MapService.** Static resource-node index (positions, purity, type — loaded from bundled data + verified against world actors at startup), named-region lookup (community biome names + coordinate→region mapping), coordinate humanization (UU → meters, cardinal directions, "800 m west of Northern Forest hub"), and map-marker creation through the game's replicated marker system.

**NotesStore.** Location-tagged player annotations (`/aida note ...` or via UI). Fields: author, text, position, region, timestamp. Retrieved by proximity/region/keyword as a tool.

**SnapshotService.** Periodic (configurable, default 30 min) aggregate snapshots: per-item production/consumption balance, per-cluster efficiency, power per circuit. Kilobytes each; ring buffer in sidecar storage. Powers `compare_to(timestamp)`.

**ActionEngine (Phase 4).** The proposal pipeline — detailed in §6. Routes all placement/dismantle through the game's own build subsystems so replication and save integration come free.

**Persistence.** Two backends behind one interface:
- *In-save:* an SML save-game component storing notes, the action journal, marker registry, and a session GUID. Travels with the save; survives server migration.
- *Sidecar:* JSON/flat files under `Configs/AIDA/data/<session-guid>/` for snapshots, full transcripts, and prompt logs. Keyed to the save's session GUID so a rolled-back save simply orphans newer sidecar data (never corrupts).

---

## 4. Factory State: Index, Don't Dump

**Extraction.** On demand (with a short TTL cache, e.g. 10 s) the FactoryIndex walks buildables via the game's subsystems and produces a normalized model: machines (class, recipe, clock, position, connection stubs), belt/pipe graph edges, power circuits, vehicles/trains/drones, storage inventories.

**Aggregation.** Raw entities collapse into LLM-friendly structures:
- **Clusters** — DBSCAN-style spatial grouping of machines; each gets an ID, region name, machine census, net inputs/outputs per minute, and an efficiency estimate.
- **Balance sheet** — per-item net production/consumption across the whole factory, with deficit flags.
- **Logistics summary** — inter-cluster flows inferred from the belt/pipe graph.
- **Power report** — per-circuit capacity vs. draw vs. battery.

**Tool surface (initial catalog).**

| Tool | Tier | Purpose |
|------|------|---------|
| `get_factory_overview()` | query | Aggregated summary; always fits in context (~1–2 k tokens). |
| `inspect_cluster(id)` | query | Drill into one cluster's machines and flows. |
| `get_item_balance(item?)` | query | Net production/consumption; deficits. |
| `find_bottleneck(item)` | query | Trace the limiting stage for an item. |
| `get_resource_nodes(region?, untapped_only?)` | query | Node positions, purity, occupancy. |
| `tag_node(node_id, label)` | query | Create a replicated map marker. |
| `lookup_recipe(item)` / `lookup_building(name)` | chat | Static game data; keeps recipes out of the system prompt. |
| `get_notes(near?, keyword?)` | query | Player annotations. |
| `add_note(text, position?)` | query | Persist an annotation (attributed to requesting player). |
| `compare_to(timestamp)` | query | Diff current aggregates vs. a snapshot. |
| `propose_build(spec)` / `propose_dismantle(selector)` | act | Phase 4; enters the proposal pipeline, never executes directly. |

Rules: tool results are JSON, sizes bounded (paginate/truncate with "N more…" hints), and all coordinates humanized before the model sees them. Tool-call depth per user request is capped (default 5 round-trips).

**Intent and memory.** State says what exists; notes say what it's *for*; snapshots say what *changed*. All three feed context selection: overview + notes near the topic/player + relevant snapshot diffs.

---

## 5. Streaming Chat Round-Trip (Sequence)

```
Player A client        Server (Orchestrator)              LLM endpoint
     │                        │                                │
     │ ServerSendChat(text)   │                                │
     ├───────────────────────►│ permission + rate checks       │
     │                        │ append PlayerMsg to transcript │
     │   Multicast_MsgBegin(playerMsgId, author, text)         │
     │◄───────────────────────┤  (all clients render it)       │
     │                        │ build context (history summary,│
     │                        │  overview, notes) + tools      │
     │                        ├── HTTPS stream request ───────►│
     │   Multicast_MsgBegin(aiMsgId, author=AIDA, "")          │
     │◄───────────────────────┤  (clients show typing/partial) │
     │                        │◄── SSE: text delta ────────────┤
     │   Multicast_MsgChunk(aiMsgId, seq, delta)               │
     │◄───────────────────────┤            … repeated …        │
     │                        │◄── SSE: tool_call ─────────────┤
     │                        │ execute tool (FactoryIndex etc)│
     │                        ├── tool_result, continue ──────►│
     │                        │◄── SSE: done ──────────────────┤
     │   Multicast_MsgEnd(aiMsgId, finalHash)                  │
     │◄───────────────────────┤ journal + persist transcript   │
```

**Chunk protocol.** Messages are replicated as *events*, not as one growing replicated string (which would resend the whole message every net update):

```cpp
USTRUCT() struct FAIDAChunk {
  FGuid   MessageId;
  int32   Seq;        // monotonically increasing per message
  FString Delta;      // UTF-8 text fragment (batched ~100–250 ms)
};
// Multicast_MsgBegin(FAIDAMessageHeader)   — reliable
// Multicast_MsgChunk(FAIDAChunk)           — reliable, batched
// Multicast_MsgEnd(FGuid id, uint32 Hash)  — reliable; hash of full text
```

Client-side assembly buffers by `Seq`. On `MsgEnd`, the client verifies the hash; on mismatch or gap (and for late joiners), it calls `ServerRequestMessageBody(id)` to fetch the authoritative full text. Late joiners bulk-fetch the recent transcript the same way on login. Chunks are batched server-side (~4–8 Hz) so streaming costs a handful of small reliable RPCs per second, not one per token.

**Latency UX.** `MsgBegin` for AIDA fires immediately on request acceptance, so players see acknowledgment in <100 ms even though first tokens arrive seconds later. Tool executions surface as inline status lines ("🔍 inspecting Steel complex…") replicated as ephemeral sub-events.

---

## 6. World Actions: Proposal → Dry-Run → Confirm → Execute → Undo

No AI-initiated world mutation ever bypasses this pipeline.

1. **Proposal.** The model calls `propose_build`/`propose_dismantle` with a structured spec (buildables, positions/orientations or a generated blueprint, or a selector for dismantle). The ActionEngine assigns a `ProposalId`, stores it server-side with a TTL (default 10 min), and computes:
2. **Dry-run.** Validation without mutation: footprint/clearance checks via the game's hologram validation paths, terrain intersection, cost tally vs. available central/player storage, power implications, and a diff summary ("place 14 foundations, 6 smelters; consume 240 concrete…"). Failures return to the model as tool errors so it can revise.
3. **Confirm.** The proposal replicates to eligible clients (permission tier `act`); ProposalUI renders it, optionally with a ghost/hologram preview at the site. A player with `act` permission approves via RPC referencing `ProposalId`. Config option: require approval by the *requesting* player, or any `act` player, or a named admin list.
4. **Execute.** Server-side, through the game's own build/dismantle subsystems (`AFGBuildableSubsystem`, blueprint subsystem), in time-sliced batches to avoid hitching the server. Each created/destroyed entity is recorded.
5. **Journal + Undo.** Every executed action appends an `FAIDAJournalEntry` (proposal spec, requesting player, approver, timestamps, list of affected entity IDs, refunded/consumed items). `/aida undo [n]` reverses the last n AI actions (dismantle what was built, or rebuild-from-spec what was dismantled, with cost refund rules configurable). Journal lives in-save; a rolling window (default 25 actions) is undoable.

Design consequences worth noting: the tool schema for `propose_*` is versioned independently (it's the contract the LLM writes against, and prompt iteration will churn it), and the ActionEngine never trusts model-supplied coordinates blindly — everything passes dry-run validation, and specs are constrained to grid-snapped placements in v1 of Phase 4.

---

## 7. Configuration (server-side, `Configs/AIDA/`)

```jsonc
{
  "provider": {
    "type": "openai-compatible" | "anthropic",
    "baseUrl": "https://api.openai.com/v1",   // or Anthropic, or http://localhost:11434/v1
    "apiKey": "…",                             // SERVER-ONLY. Never replicated.
    "model": "…",
    "maxOutputTokens": 1024,
    "visionModel": "…"                         // optional, Phase 5
  },
  "limits": {
    "perPlayerPerMinute": 4,
    "globalPerMinute": 12,
    "maxToolRoundTrips": 5,
    "dailyTokenBudget": 0                      // 0 = unlimited
  },
  "permissions": {
    "chat": "everyone",
    "query": "everyone",
    "act":  ["<epic-account-id>", "…"]
  },
  "privacy": {
    "sendFactoryData": true,
    "sendPlayerNames": true,
    "sendChatHistoryDepth": 20,
    "logPromptsToSidecar": false
  },
  "snapshots": { "intervalMinutes": 30, "keep": 200 },
  "prompts": { "systemPromptFile": "prompts/system.md", "toolsFile": "tools.json" }
}
```

The API key field is tagged server-only in the SML config schema; the orchestrator reads it once at init and it never enters any replicated struct or client-visible path. All AI-initiated world actions and permission denials are written to the server log with requesting-player IDs.

---

## 8. Failure Modes & Resilience

**Game patch breaks extraction.** FactoryIndex wraps its game-API surface in versioned accessors with startup self-tests; on failure it flips to `degraded` mode — chat and static-data tools keep working, factory tools return a clear "factory data unavailable after game update" result, and the server log points admins at the compatibility note. The mod must never crash the server because a class layout changed.

**LLM endpoint down / slow.** Per-request timeout (default 60 s) with streamed partial preserved and marked "(response interrupted)". Circuit breaker after repeated failures posts a single in-chat notice instead of failing every message loudly.

**Linux TLS quirks.** `FHttpModule` on Linux dedicated servers occasionally needs certificate-bundle configuration; document the known fix in ops docs and detect/report the specific TLS error class in the log.

**Cost runaway.** Rate limits plus optional daily token budget; when exhausted, AIDA answers with a canned "out of budget until <time>" message (no API call).

**Desync/undo edge cases.** Undo validates that journaled entity IDs still exist (players may have manually dismantled AI builds); partial undos report exactly what couldn't be reversed.

---

## 9. Privacy & Trust

Shipped with a prominent disclosure in the mod description and first-run server log: *chat messages and (if enabled) factory metadata are sent to the configured third-party LLM endpoint.* The `privacy` config block lets admins restrict outbound context to chat-only. No telemetry of any kind is sent anywhere except the configured LLM endpoint. Open source (MIT) so the claim is auditable — this, plus SMR's malware review, is the trust story.

---

## 10. Phase Plan (mapped to modules)

| Phase | Name | Delivers | Modules touched |
|-------|------|----------|-----------------|
| 0 | Skeleton | Toolchain, repo+CI (Win64 client + Linux server builds), mod loads both sides, config system with server-only key, ProviderAdapter (OpenAI-compat + Anthropic), name reserved on ficsit.app | Orchestrator stub, LLMClient, Persistence stub |
| 1 | Voice | ChatWidget, full streaming round-trip, rate limiting, permission tiers, privacy flags, late-join transcript sync | SessionManager, chunk protocol, PermissionService, RateLimiter |
| 2 | Eyes | FactoryIndex extraction+aggregation, overview/balance/bottleneck/cluster tools, MapService regions + node tagging | FactoryIndex, MapService, ToolRegistry |
| 3 | Memory | NotesStore, SnapshotService, compare tools, in-save + sidecar persistence, journal schema | NotesStore, SnapshotService, Persistence |
| 4 | Hands | ActionEngine: proposal/dry-run/confirm/execute/undo; grid-constrained builds; blueprint (.sbp) generation | ActionEngine, ProposalUI |
| 5 | Imagination | Vision payloads in adapters; screenshot→description→draft-blueprint; landmark-based location ID | AnthropicAdapter/vision, MapService |
| 6 | Polish | Dedicated-server ops docs, prompt/persona packs, localization, MP hardening, release management | All |

Milestone gates: **P0 exit** = config round-trip test passes on Linux dedicated in Docker. **P1 exit** = two clients watch one streamed answer with a late-joiner recovering the transcript. **P2 exit** = "why is my reinforced plate production starving?" answered correctly on a real 40-hour save. **P4 exit** = propose→approve→build→undo of a 10×10 foundation grid with two players connected.

## 11. Open Questions (tracked, not blocking)

Whether chat later gains private per-player threads alongside the shared channel; whether snapshot summarization should use the LLM (costs tokens) or stay purely numeric; exact `.sbp` blueprint-format fidelity for generated blueprints vs. driving the in-game blueprint subsystem directly; and whether to depend on or vendor parts of FICSIT Foreman's (Apache-2.0) save parser for offline analysis tooling.
