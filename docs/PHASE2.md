# Phase 2 "Eyes" — implementation plan

Phase 2 gives AIDA sight of the factory: an on-demand **FactoryIndex** (walk the game's buildables →
aggregate into clusters / item balance / power), a **MapService** (resource nodes, region names,
coordinate humanization, map markers), and the **ToolRegistry** + LLM **tool-calling loop** that lets the
model query all of it. See `ARCHITECTURE.md §4` (Index, Don't Dump) and `§10` (phase table).

**P2 exit gate:** *"why is my reinforced plate production starving?"* answered correctly on a real
~40-hour save.

## Testability note (shapes the build order)

Unlike Phase 1's networking, most of Phase 2 is **unit-testable without a live game** — if we keep the
same discipline as P1 (game-header code in one seam, pure logic behind plain structs):

- **Tool infra, aggregation math, static lookups** → unit tests + the live LLM (`AIDA.Ping`-style). No save.
- **Live extraction** (walking `AFGBuildableSubsystem`) → needs a real save with a factory, which means a
  **packaged Alpakit game** (SML doesn't load mods in PIE here — see `DEV.md` / the P1 gate). Same
  constraint as P1, so we isolate it to one slice and lean on synthetic-data unit tests for the rest.

## Module map

| Module | Files (planned) | Role | Game headers? |
|--------|-----------------|------|---------------|
| Tool types | `Adapters/AIDALLMTypes.h` (extend) | tool defs, tool-call/tool-result message parts | no |
| Adapters | `Adapters/{OpenAICompat,Anthropic}Adapter.cpp` (extend) | tool wire-format both ways (SSE `tool_calls` / `tool_use`) | no |
| ToolRegistry | `Tools/AIDAToolRegistry.{h,cpp}` | name/schema/tier/handler; loads `Configs/AIDA/tools.json`; dispatch + validation | no |
| Orchestrator | `Core/AIDAOrchestrator.cpp` (extend) | tool-call loop: execute → tool_result → continue, capped round-trips | no |
| FactoryIndex (model) | `Factory/AIDAFactoryModel.h` | plain structs: machine, edge, cluster, balance sheet, power report | no |
| FactoryIndex (aggregate) | `Factory/AIDAFactoryAggregator.{h,cpp}` | clustering / balance / logistics / power math on plain data | no |
| FactoryIndex (extract) | `Factory/AIDAFactoryIndex.{h,cpp}` | walk buildables → plain model; **the only game-header seam** | **yes** |
| MapService | `Map/AIDAMapService.{h,cpp}` | node index, region names, coord humanization, markers | yes |
| Tool handlers | `Tools/AIDAFactoryTools.cpp` etc. | the catalog below, each a ToolRegistry handler | via the above |

## Tool catalog (initial, `ARCHITECTURE.md §4`)

`get_factory_overview()`, `inspect_cluster(id)`, `get_item_balance(item?)`, `find_bottleneck(item)`,
`get_resource_nodes(region?, untapped_only?)`, `tag_node(node_id,label)`, `lookup_recipe(item)`,
`lookup_building(name)`. Tiers: mostly `query`; the two `lookup_*` are `chat`. Results are bounded JSON,
coordinates humanized before the model sees them, tool-call depth capped (default 5, from RateLimiter).

## Grounded FactoryGame API reference

All paths under `SatisfactoryModLoader/Source/FactoryGame/Public/`. Subsystems via static
`Get(worldContext)`. **Key gaps to code around:** circuits and resource nodes have no public "get-all"
list (enumerate via `TActorIterator` / via buildables' power infos); manufacturer connection arrays are
protected (use `GetConnectionComponents()`); `GetProductionProgress()` is a fraction [0,1].

**Enumerate machines** — `AFGBuildableSubsystem` (`FGBuildableSubsystem.h`):
`GetAllBuildablesRef()` (L321, fastest full list), `GetTypedBuildable<T>(out)` (L332),
`GetConnectedConveyorBelt(conn)` (L281). Foundations/walls are in `AFGLightweightBuildableSubsystem`
(skip — machines are all in `mBuildables`). Per-buildable: `GetBuiltWithRecipe()`, `GetActorLocation()`.

**Production machine** — `AFGBuildableManufacturer : AFGBuildableFactory` (`Buildables/…`):
`GetCurrentRecipe()` (Manuf L165), `GetInputInventory()`/`GetOutputInventory()` (L157/L161),
`GetCurrentPotential()` (Factory L225, 1.0=100%), `IsProducing()` (L151),
`GetProductionProgress()` (L193, [0,1]), `GetProductionCycleTime()` (L197, potential applied),
`GetProductivity()` (L213), `GetConnectionComponents()` (L78), `GetPowerInfo()` (L108).
Extractors: `AFGBuildableResourceExtractorBase` (no recipe) → `GetExtractableResource()` / `GetResourceNode()`.
Inventory: `UFGInventoryComponent::GetInventoryStacks(out)` (L451).

**Recipes/items** — `UFGRecipe` (static, `FGRecipe.h`): `GetIngredients(ctx,recipe)` (L59),
`GetProducts(recipe)` (L63), `GetManufacturingDuration(recipe)` (L75). `FItemAmount{ItemClass,Amount}`.
**items/min = product.Amount * 60 / machine->GetProductionCycleTime()** (cycle time already includes
potential); fluids in cm³ (÷1000 for m³). `AFGRecipeManager::GetAllAvailableRecipes(out)` (L66),
`FindRecipesByProduct(...)` (L129). `UFGItemDescriptor::GetItemName(cls)` (L191), `GetForm(cls)` (L158,
Solid/Liquid/Gas → belt vs pipe).

**Connections/graph** — `UFGFactoryConnectionComponent` (`FGFactoryConnectionComponent.h`):
`GetConnection()` (L96, the connected component on the neighbor), `GetDirection()` (L122, INPUT/OUTPUT),
`GetOuterBuildable()` (L180). Edge = `conn->GetConnection()->GetOuterBuildable()`. Belts:
`AFGBuildableConveyorBase::GetConnection0/1()`, `GetSpeed()`. Pipes: `UFGPipeConnectionComponent`
`GetConnection()` (L69), `GetFluidDescriptor()` (L224), `GetPipeNetworkID()` (L199).

**Resource nodes** — `AFGResourceNode : AFGResourceNodeBase` (`Resources/…`): `GetResourcePurity()`
(L113, `EResourcePurity` RP_Inpure/Normal/Pure), `GetResourceClass()` (base L144), `IsOccupied()` (L140),
`GetResourceNodeType()` (L176), `GetActorLocation()`. **Enumerate via `TActorIterator<AFGResourceNodeBase>`**
(manager list is private). Wells: `AFGResourceNodeFrackingCore::GetSatellites(out)`.

**Power** — machine link `UFGPowerInfoComponent::GetPowerCircuit()` (L80), `GetActualConsumption()` (L127).
Per-circuit `UFGPowerCircuit` (`FGPowerCircuit.h`): `GetStats(FPowerCircuitStats&)` (L171 → PowerProduced /
PowerConsumed / ProductionCapacity / MaximumPowerConsumption / BatteryPowerInput), `GetBatterySumPowerStore()`
(L215), `GetTimeToBatteriesEmpty()` (L235). **Enumerate circuits by deduping buildables'
`GetPowerInfo()->GetPowerCircuit()` on `GetCircuitID()`** (`AFGCircuitSubsystem::FindCircuit(id)` L84 to fetch).

**Location naming** — `UFGMapAreaTexture::GetMapAreaForWorldLocation(loc)` (L72) →
`UFGMapArea::GetAreaDisplayName(cls)` (L19) / `GetUserSetAreaDisplayName` (L23). Grid fallback:
`AFGWorldGridSubsystem::GetWorldGridCoordinatesForLocation(loc)` (L89). `AFGGameState::GetVisitedMapAreas(out)`
(L244) for discovered filtering. (`AFGMapManager` = fog/markers only, no region names.)

## LLM tool-calling design (Slice 0)

Extend the provider-agnostic types, not each adapter's callers:
- `FAIDAToolDef { FString Name; FString Description; FString ParametersJsonSchema; }` in the request.
- `FAIDAChatMessage` gains optional tool-call parts (assistant: id/name/args-json) and a `tool` role
  (tool_result: call-id + content). Keep it plain C++ so wire logic stays unit-testable.
- `ILLMAdapter::Complete` gains an `OnToolCalls(TArray<FAIDAToolCall>)` outcome (or fold into a richer
  `OnComplete`), so the orchestrator can run the loop. Streaming text deltas still fire via `OnChunk`.
- **OpenAI**: `tools`/`tool_choice` in the body; assemble `tool_calls` from streamed deltas; stop on
  `finish_reason:"tool_calls"`. **Anthropic**: `tools` array; parse `tool_use` content blocks; send
  results back as `tool_result` blocks. (Confirm current model IDs/shape against the `claude-api` skill.)
- Orchestrator loop: `CompleteChat(msgs, tools)` → if tool calls, dispatch via ToolRegistry (permission +
  arg-schema check), append `tool_result`, repeat until text or the cap (RateLimiter max round-trips).
  Surface each call as an inline status sub-event ("🔍 inspecting Steel complex…", `§5`).

## Build order (slices)

- **Slice 0 — Tool infrastructure** *(unit-test + live LLM; no save)*: the tool-calling design above +
  `AIDAToolRegistry` + a trivial `echo`/`ping_tool` to prove the round-trip. Tests: schema validation,
  dispatch, permission gating, round-trip cap.
- **Slice 1 — Aggregation on plain data** *(unit tests; no save)*: `AIDAFactoryModel` structs +
  `AIDAFactoryAggregator` (DBSCAN-ish clustering, per-item balance, logistics inference, power report) on
  synthetic entities. This is the math-heavy, bug-prone part — nail it in isolation.
- **Slice 2 — Extraction + first tools** *(needs save → packaged verify)*: `AIDAFactoryIndex` walks
  `AFGBuildableSubsystem` (TTL cache ~10 s) into the model. Tools: `get_factory_overview`,
  `inspect_cluster`, `get_item_balance`.
- **Slice 3 — MapService + static tools** *(mixed)*: node index (`TActorIterator` verify vs bundled data),
  region naming + coordinate humanization, `tag_node` (replicated marker), `get_resource_nodes`,
  `lookup_recipe`/`lookup_building` (static data, chat tier).
- **Slice 4 — `find_bottleneck` + gate**: trace the limiting stage for an item across the flow graph;
  finalize the tool catalog + system prompt; run the P2 exit-gate question on a real save.

## Verification

Slices 0–1 and the static parts of 3: unit tests (`Tests/AIDATests.cpp`) + `AIDA.Ping`-style live calls.
Slices 2/4 and live map data: **packaged Alpakit game on a real save** (bundle with the P1 packaged gate to
amortize the packaging cost). Add an `AIDA.Index` debug console command that dumps the aggregated overview
to the log, so extraction is checkable without the LLM in the loop.
