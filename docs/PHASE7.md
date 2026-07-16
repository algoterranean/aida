# Phase 7 — "Engineer"

Phase 7 closes the gap between the shipped capability set (P1 Voice, P2 Eyes, P3 Memory, P4 Hands)
and the full README wishlist. Where P4 taught AIDA to *place things on a grid*, P7 teaches it to
**wire a factory**: see the logistics graph, modify existing buildables in place, build *connected*
multi-part structures (machines + belts + power), label the world, and run standing checks on a timer.

Vision ("see the attached picture, build it") is **deliberately NOT in P7** — it is already scoped as
Phase 5 "Imagination" (`ARCHITECTURE.md §10`) and stays there; P7's spec-v2 work is a prerequisite for
it anyway (a picture is useless until specs can express what's in it).

---

## 1. Scorecard — README wishlist vs. shipped capabilities

| README ask | Status | What covers it / what's missing |
|---|---|---|
| Where are my biggest bottlenecks? | ✅ shipped (P2) | `find_bottleneck`, `get_factory_overview`, `get_item_balance` |
| What changed in the past few hours / since I last logged in? | ✅ mostly (P3) | `compare_to` + snapshots. **Gap:** no snapshot anchored at player login → Slice 0 polish |
| Upgrade all nearby belts to Mk5 | ❌ | Needs belt extraction (Slice 0) + in-place upgrade action (Slice 2) |
| Nearest unused pure iron node? | ✅ shipped (P2) | `get_resource_nodes` + `tag_node` |
| Design a multi-floor HMF building, then build it | ❌ | Needs plan math (Slice 5) + connected multi-part builds (Slice 4) |
| Where am I wasting energy? Where should I underclock? | ◐ partial | Power report exists; needs an underclock advisor tool (Slice 1) + `propose_set_clock` (Slice 2) |
| 1 GW of coal power — how many pumps/generators/miners? | ✅ shipped (P2) | `lookup_recipe`/`lookup_building` + model arithmetic. Add to live-tuning checklist to confirm answer quality |
| See the attached picture. Build it. | ⏸ deferred | **Phase 5 "Imagination"**, unchanged. P7 spec v2 is its prerequisite |
| Manifold of assemblers making screws, routed to nearest train depot | ❌ | The P7 centerpiece: Slices 0+4 (+ depot lookup) |
| Undo what was just built | ✅ shipped (P4) | `/aida undo [n]` (human-only, by design) |
| Disconnected splitters/machines? Slow belts mixed with fast? | ❌ | Logistics graph + diagnostics tools (Slices 0–1) |
| Label containers with contents; re-check every 10 minutes | ❌ | Signs + inventory reads (Slice 3) + standing tasks (Slice 6) |
| Hypertube cannon to land in the desert base | 🎯 stretch | Spline builds arrive in Slice 4; launch ballistics is empirical → §8 |

---

## 2. Slices (build order)

Ordering rationale: **Slice 0 unblocks everything** (three wishlist items read the logistics graph, and
connected builds can't be verified without it). Slices 1–3 are small, independent, high-value wins.
Slices 4–5 are the big lift. Slice 6 (standing tasks) is orthogonal and can slot in anywhere after 3.

### Slice 0 — Logistics graph (Eyes v2)

Closes the deliberate P2 deferral: `FAIDAFactorySnapshot.Edges` has been empty since Slice 2 of P2,
with `BuildLogistics` already written and waiting for real edges.

- Extend `FAIDAFactoryIndex::ExtractInto`: belts are real actors in `AFGBuildableSubsystem` (not
  lightweights). Walk `AFGBuildableConveyorBase` → `GetSpeed()`, `GetConnection0()` (input) /
  `GetConnection1()` (output) → `UFGFactoryConnectionComponent::GetConnection()` / `IsConnected()` →
  owner buildable. Chains of belt segments collapse to one logical edge (walk until a non-belt owner).
- Same pass for pipes (`AFGBuildablePipeline` + `UFGPipeConnectionComponent`) and splitters/mergers
  (`AFGBuildableAttachmentSplitter`, `AFGBuildableSplitterSmart`) — splitters/mergers become graph
  nodes so "dangling splitter" is detectable.
- Machine-side: unconnected input/output `UFGFactoryConnectionComponent`s on manufacturers flagged
  per machine.
- New plain-struct graph model + tests on synthetic graphs (same discipline as P2 Slice 1: the math
  is game-header-free).
- Polish: take a snapshot on player join, labeled as such, so "since I last logged in" anchors cleanly.

**Gate:** `AIDA.Index` on the real save shows edges; `BuildLogistics` produces non-empty inter-cluster flows.

### Slice 1 — Flow diagnostics (Query tier, pure analysis)

- `find_disconnected` — machines/splitters with unconnected or wrongly-directed ports; belts whose far
  end dangles. Returns locations (metres + region names) so the model can `tag_node`-style report them.
- `find_belt_mismatch` — belts slower than both their neighbors on a producing path ("slow belts mixed
  in with fast belts"), grouped by cluster.
- `get_clock_advice` — per-machine: productivity vs. clock vs. power draw; recommends underclocks where
  a machine idles ≥ X% (output-limited) and quantifies the MW saved. Pure math over the snapshot; the
  power-shard question doesn't arise (underclocking needs none).

**Gate:** deliberately misplace one belt on the test save → `find_disconnected` names the exact splitter.

### Slice 2 — In-place mutation actions (Hands v2a)

Three new Act-tier proposals through the *existing* pipeline (propose → dry-run → approve → execute →
journal). New journal entry kind: **mutation** (records before/after values; undo = restore before-value)
alongside build/dismantle.

- `propose_upgrade_belts(selector, targetMk)` — per belt, spawn the target-mk `AFGConveyorBeltHologram`
  and drive `TryUpgrade(hit)` / `GetUpgradedActor()` — the hologram path we already know how to drive
  headlessly (P4's reset-disqualifiers / build-mode / UpdateHologramPlacement recipe). Upgrade cost =
  new belt cost (game refunds the old one); dry-run affordability vs. central storage as in P4.
  Undo = downgrade back to journaled mk.
- `propose_set_clock(selector, percent)` — `AFGBuildableFactory::SetPendingPotential()`. **v1 caps at
  100%** (no power-shard handling); >100% is a later extension. Undo = restore journaled clock.
- `propose_set_recipe(machineSelector, recipe)` — `AFGBuildableManufacturer::SetRecipe()` on machines
  matching the selector. Refuses machines with items in flight unless `force:true` (contents are
  destroyed — surface that in the dry-run report). Undo = restore journaled recipe.

**Gate:** "upgrade all belts within 100 m to Mk.3" on the real save → propose → approve → belts visibly
faster → `/aida undo` restores Mk.1/2 correctly.

### Slice 3 — Signs & container labeling

- Read side: extend extraction to storage containers — `AFGBuildableStorage::GetStorageInventory()` →
  stack enumeration; new `get_container_contents(selector)` Query tool.
- Write side: `propose_label_containers(selector)` — for each container, build a small sign
  (`AFGBuildableWidgetSign` via its hologram, snapped to the container) and set its text with
  `SetPrefabSignData(FPrefabSignData&)` to the dominant item name. Journal entries are normal builds
  (signs are buildables → undo already works). Re-labeling an existing AIDA sign = mutation entry.

**Gate:** "label all of these containers with their contents" on a row of 6 mixed containers → 6 signs,
correct names, undo removes them.

### Slice 4 — Connected builds: spec v2 (Hands v2b)

> **Status (2026-07-16): the `parts` half SHIPPED early** as part of Phase 5's fidelity fix —
> `propose_build` accepts `{version:2, origin, yawDeg, parts:[{buildable, at, yawDeg?, grid?}]}`
> (per-part grid repeat is an addition to the sketch below; `id`/`recipe`/`clock` and the
> `belts`/`wires` port routing remain THIS slice). A v2 spec with `belts`/`wires` is rejected with
> a clear error until then; v2 skips auto-power (poles are parts; wiring lands here).

The centerpiece. Build spec v2 expresses a *machine group*: typed parts + connections, not one
buildable × N.

```jsonc
{ "version": 2,
  "origin": { "x": 0, "y": 0, "z": 0 },        // same omit-= -aim rule as v1
  "parts": [
    { "id": "a1", "buildable": "Assembler", "recipe": "Screw", "clock": 100,
      "at": { "x": 0, "y": 0, "z": 0 }, "yawDeg": 0 },
    { "id": "s1", "buildable": "Conveyor Splitter", "at": { "x": -4, "y": 0, "z": 1 } },
    { "id": "p1", "buildable": "Power Pole Mk.1", "at": { "x": 2, "y": 3, "z": 0 } } ],
  "belts": [
    { "buildable": "Conveyor Belt Mk.4", "from": { "part": "s1", "port": "out0" },
      "to": { "part": "a1", "port": "in0" } } ],
  "wires": [ { "from": "p1", "to": { "part": "a1" } } ] }
```

Key design rules (same trust posture as v1 — the engine never trusts model geometry):

- **The model places boxes and names endpoints; the engine does all routing.** Belt paths are computed
  server-side (straight or single-bend L, pole auto-insert deferred); the model never emits spline
  points. Ports resolve to `UFGFactoryConnectionComponent`s by direction + index after the part builds.
- Two-phase execution through the existing time-sliced executor: parts first (existing hologram path +
  `SetRecipe`/`SetPendingPotential` on completion), then belts/wires once both endpoints exist.
  Belt construction = `AFGConveyorBeltHologram` driven headlessly; fallback if headless spline
  holograms fight back: construct a minimal straight belt, then `AFGBuildableConveyorBelt::Respline()`
  (public static, takes `TArray<FSplinePointData>`) to the computed path. Wires = `AFGWireHologram`
  between `UFGCircuitConnectionComponent`s.
- Vertical: conveyor lifts (`AFGConveyorLiftHologram`) as a belt kind (`"lift": true` or auto when
  Δz dominates) — this is what makes "tall instead of wide" buildable.
- Dry-run validates every part AND every belt (endpoint ports exist, are free, direction-compatible)
  before anything is stored. Partial-failure semantics identical to P4: per-part errors with indices.
- One journal entry per proposal, all entities included → `/aida undo` of a whole group already works.
- Manifold convenience: `"manifold": { "machines": 5, ... }` sugar that server-side expands to
  parts+belts (splitter chain along the inputs, merger chain along the outputs) — the model asks for a
  manifold, the engine emits the graph. Depot routing = new `find_logistics_targets` Query tool
  (train cargo platforms via `AFGBuildableTrainPlatformCargo`, drone ports, truck stations with
  locations) + a long belt from the manifold merger to the chosen platform's input port.

**Gate (= the P7 exit gate, §6):** the screws-manifold ask, end to end.

### Slice 5 — Design planner (`plan_factory`)

"Design a building for 10 HMF/min, tall" is two separable problems; only the first is deterministic:

- `plan_factory(item, ratePerMin, constraints)` — **pure math, Query tier**: walk the recipe tree
  (RecipeService already has it) → machine counts per step, clocks to hit the target exactly, belt mk
  minimums per edge, power total, raw-resource requirements. Returns the production graph as JSON.
  This also single-handedly upgrades the "1 GW coal" class of question from model-arithmetic to
  tool-verified numbers.
- Layout ("tall, as many floors as necessary") stays **the model's job**, assisted: the planner output
  includes per-machine footprints (from BuildingInfo) and a suggested floor assignment (greedy
  bin-pack of machines into W×D floors, foundations + walls + lifts emitted as spec-v2 parts). The
  model reviews/edits the generated spec v2 and proposes it. v1 keeps the generator simple
  (rectangular floors, manifold rows per floor, lifts in a corner riser).

**Gate:** "design a building for 10 HMF/min, tall" → plan numbers verified by hand → generated spec
dry-runs clean → build it → machines actually produce 10/min (check with `get_item_balance`).

### Slice 6 — Standing tasks (autonomy, "check every ten minutes")

A server-side scheduler for recurring checks. This is the one slice that spends tokens *without a
player asking in the moment* — the design is guard-rails-first:

- `FAIDAStandingTask { id, prompt, intervalMinutes, createdBy, enabled, lastRunUtc, lastResult }`,
  persisted in-save via the existing P3 persistence layer.
- Created **only by humans**: `/aida task add "<prompt>" every 10m`, `/aida task list|rm|pause` —
  deliberately NOT an LLM tool in v1 (same reasoning as undo: the model must not be able to grant
  itself recurring execution). Requires act tier.
- Execution: orchestrator timer → runs the prompt through the normal tool loop with the creator's
  identity and Query-tier tools only (**standing tasks can never mutate** in v1 — a task that finds
  drift *reports*; a human decides). Output goes to chat as a System line only when the result is
  actionable (the prompt contract asks the model to reply `OK` when nothing's wrong; `OK` is
  swallowed).
- Cost guards (config §5): per-task minimum interval (5 min), global `tasks.maxPerDay` LLM-call
  budget, `tasks.enabled:false` by default, all runs logged with token counts.

**Gate:** "label these containers … check every ten minutes that they're still consistent" → labels
built (Slice 3), task created, a manually-swapped container contents produces a System warning within
10 minutes; a quiet run produces nothing.

---

## 3. New/changed tools summary

| Tool | Tier | Slice |
|---|---|---|
| `find_disconnected` | Query | 1 |
| `find_belt_mismatch` | Query | 1 |
| `get_clock_advice` | Query | 1 |
| `get_container_contents` | Query | 3 |
| `find_logistics_targets` | Query | 4 |
| `plan_factory` | Query | 5 |
| `propose_upgrade_belts` | Act | 2 |
| `propose_set_clock` | Act | 2 |
| `propose_set_recipe` | Act | 2 |
| `propose_label_containers` | Act | 3 |
| `propose_build` (spec v2) | Act | 4 |

Plus `/aida task …` chat commands (Slice 6, human-only) and the mutation journal-entry kind (Slice 2).

## 4. API grounding (verified against real headers, 2026-07-15)

| Need | API | Header |
|---|---|---|
| Belt speed + endpoints | `AFGBuildableConveyorBase::GetSpeed/GetConnection0/GetConnection1` (0=in, 1=out) | `Buildables/FGBuildableConveyorBase.h:160-164` |
| Connection peer | `UFGFactoryConnectionComponent::GetConnection/IsConnected/SetConnection` | `FGFactoryConnectionComponent.h:90-111` |
| In-place upgrade | `AFGHologram::TryUpgrade(FHitResult)/GetUpgradedActor` (overridden by `AFGConveyorBeltHologram`, `mUpgradedConveyorBelt`) | `Hologram/FGHologram.h:163,309-314`; `Hologram/FGConveyorBeltHologram.h:32-36` |
| Belt respline fallback | `static AFGBuildableConveyorBelt::Respline(belt, TArray<FSplinePointData>)` | `Buildables/FGBuildableConveyorBelt.h:103` |
| Set recipe | `AFGBuildableManufacturer::SetRecipe(TSubclassOf<UFGRecipe>)` | `Buildables/FGBuildableManufacturer.h:174` |
| Set clock | `AFGBuildableFactory::SetPendingPotential(float)` / `GetCurrentPotential` | `Buildables/FGBuildableFactory.h:225-233` |
| Sign text | `AFGBuildableWidgetSign::SetPrefabSignData(FPrefabSignData&)` | `Buildables/FGBuildableWidgetSign.h:101`; `FGSignTypes.h:138` |
| Container contents | `AFGBuildableStorage::GetStorageInventory()` → `UFGInventoryComponent` | `Buildables/FGBuildableStorage.h:30` |
| Lifts / wires / hypertubes | `AFGConveyorLiftHologram`, `AFGWireHologram`, `AFGBuildablePipeHyper*`, `AFGPipeHyperStart` | `Hologram/`, `Buildables/` (exist; methods to ground per-slice) |
| Train cargo platforms | `AFGBuildableTrainPlatformCargo` | `Buildables/FGBuildableTrainPlatformCargo.h` |

Still to ground when the slice starts (flagged, not blocking the plan): headless
`AFGConveyorBeltHologram` endpoint-connection driving (Slice 4's top risk — Respline is the fallback),
`UFGPipeConnectionComponent` specifics, sign hologram snapping, wire hologram headless driving.

## 5. Config additions (`Configs/AIDA/config.jsonc`)

```jsonc
"actions": { /* existing */
  "maxBeltsPerUpgrade": 0,          // 0 = unlimited, same convention as maxProposalItems
  "allowRecipeChangeWithContents": false },
"tasks": { "enabled": false,        // standing tasks are opt-in
  "minIntervalMinutes": 5, "maxPerDay": 200 }
```

## 6. Exit gate

**The screws manifold, end to end, two players:** *"Plop down a manifold of 5 assemblers making
screws and route them to the nearest train depot"* → `plan_factory`/`find_logistics_targets` inform
the spec → propose (dry-run report shows parts, belts, cost) → **other** player approves → group
builds time-sliced, machines have the Screw recipe set, belts connect manifold → depot input, power
wired, screws visibly flowing → `get_item_balance` confirms the new production → `/aida undo` removes
the whole group with refunds.

Plus per-slice gates (§2) run on the packaged game as each slice lands — same SML-not-in-PIE
constraint as always.

## 7. Prerequisites & standing debt (not absorbed into P7 slices)

- **P1 final gate** still deferred: streaming relay round-trip in a packaged game, 2-client + late-join.
- **P4 exit gate** live-verify still pending: 2-client propose→approve→build→undo of a 10×10 grid.
  Do this **before** Slice 2 builds on the same pipeline.
- README `Installation` / `Usage` / `Capabilities` sections are empty — fill once P7 stabilizes the
  tool catalog.

## 8. Stretch / explicitly out of scope

- **Hypertube cannon** (#13): after Slice 4, hypertube *segments* are buildable in principle
  (`AFGBuildablePipeHyper` + entrances exist), but launch ballistics are not an exposed API — landing
  "precisely in the middle of the desert base" needs empirical velocity/drag modeling. Treat as a
  post-P7 experiment: first `plan_hypertube_launch` as advice-only (math + a proposed aim), building
  the tube manually; automate only if the physics model proves accurate.

  **Ballistics annex (researched 2026-07-15).** In-tube constants from `FGCharacterMovementComponent.h:119-136`:
  min speed 3 m/s, constant accel 1.1 m/s², friction 0.05, in-tube slope gravity 15 m/s², curve damping 0.4;
  entrance `mInitialMinSpeedFactor = 1.4` (`FGPipeHyperStart.h:28`). Cannon boost is *emergent* (entry
  velocity + entrance boost per pass, FPS-dependent); community-measured exit speed
  **v(n) ≈ 12.395 × 1.203ⁿ m/s** for n chained entrances (wiki.gg, single-experiment fit — treat ±20%).
  Airborne: no horizontal drag (UE `FallingLateralFriction = 0`), effective gravity **g ≈ 11.76 m/s²**
  (wiki's measured range divisor = 1.2 × 9.81 → GravityScale 1.2); vertical fall likely capped by the UE
  physics-volume terminal velocity (default 40 m/s — VERIFY in game, matters for >500 m flights).
  Parabola (launch height h, angle α): **R = (v·cosα / g)·(v·sinα + √(v²sin²α + 2gh))**, flat-ground
  max at 45°: R = v²/g. Working points: 10 entrances ≈ 79 m/s ≈ 530 m; 12 ≈ 114 m/s ≈ 1.1 km;
  14 ≈ 165 m/s ≈ 2.3 km; 17 ≈ 287 m/s ≈ 7 km (beyond the ~5.4 km map — the wiki's "max 17" warning).
  Inverse (what `plan_hypertube_launch` computes): v = √(g·R) at 45°, n = ln(v/12.395)/ln(1.203),
  rounded up + trimmed by angle. Verification plan: build 5/10/14-entrance test cannons, measure exit
  speed (mPipeVelocity is readable on the movement component) + landing point, refit the two constants.
- **Vision** (#8): stays Phase 5 "Imagination". After P7, a picture → spec v2 → propose pipeline has
  everything it needs on the output side.
- **Power shards / >100% overclock**, smart-splitter rule programming, auto pole-insertion on long
  belt runs, blueprint (.sbp) generation: all deferred, as before.

## 9. Open questions (tracked, not blocking)

1. Belt-chain collapsing across splitters that merely pass through (does a 2-in/1-out merger node make
   "slow belt" attribution ambiguous?) — settle in Slice 1 with real saves.
2. Should `plan_factory` prefer alternate recipes the players have unlocked, and who chooses among
   them (model vs. deterministic "fewest machines")? Leaning: return the top 2–3 plans, model presents.
3. Standing-task results: chat System line vs. a persistent "AIDA findings" note (P3 NotesStore) —
   maybe both (note always, chat only on change).
4. Spec v2 `manifold` sugar: server-side expansion (locked above) vs. making the model emit full
   graphs with a validator — revisit if the expansion code turns out hairier than expected.
