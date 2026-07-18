# Phase 8 — "Foreman"

Phase 7 taught AIDA to *wire a factory*; much of it shipped early and sideways (spec-v2 parts,
connected builds, manifolds-on-pending, belt taps with chained feeds, slab extension, perimeter
walls). Phase 8 makes AIDA an **operator**: it adjusts the factory that exists (clocks, recipes,
belts, fuses), answers "what's wrong / what's next" in one call, builds *whole production lines*
from a plan with one approval, and eventually watches the factory on a timer.

Scope decision (2026-07-18): the tool surface will be **consolidated later** — this phase adds
tools freely, but every new tool must reuse the existing selector/spec conventions (buildable +
center-omitted-means-aim + radiusM) so future merging is mechanical, not archaeological.

---

## 1. Where PHASE7.md stands now

| P7 slice | Status |
|---|---|
| 0 Logistics graph | ✅ shipped |
| 1 Flow diagnostics (`find_disconnected`, `find_belt_mismatch`, `get_clock_advice`) | ✅ shipped |
| 2 In-place mutations (`propose_upgrade_belts`, `propose_set_clock`, `propose_set_recipe`) | ❌ → **P8 Slice 2** |
| 3 Signs & container labeling | ✅ shipped |
| 4 Spec-v2 connected builds | ◐ parts + manifold-sugar + belt-tap shipped; arbitrary `belts`/`wires` port routing, `recipe`/`clock` in specs, lifts remain → folded into **P8 Slice 4** |
| 5 `plan_factory` | ◐ read half shipped; the generator half → **P8 Slice 4** |
| 6 Standing tasks | ❌ → **P8 Slice 5** |

New capability landed since PHASE7.md was written (used below as building blocks):
`propose_extend_foundations` (slab census), `propose_add_walls`, `propose_belt_tap`
(source census + native attachment-on-belt splice + chained multi-hop feeds via
`BuildChainSegment`/`TickFeedHop`), run-ghost previews, composite manifolds.

---

## 2. Slices (build order)

### Slice 1 — Quick strikes (small, independent, one session)

Three tools with outsized value-to-effort. All Query-tier except the fuse.

- **`reset_fuse`** (Act, but auto-approvable candidate): find tripped circuits
  (`UFGPowerCircuit::IsFuseTriggered`), reset them (`ResetFuse()`), report which circuits and the
  current load/capacity so the model can warn "it will trip again — 412 MW load on 300 MW capacity."
  Because it is trivially reversible and players ask in a panic, consider executing WITHOUT the
  proposal flow (like `tag_node`) — precedent: mark/tag tools already write world state directly.
- **`get_milestone_status`** (Query): active milestone + remaining cost
  (`AFGSchematicManager::GetActiveSchematic` / `GetRemainingCostFor`), plus the next few available
  schematics. Unlocks the "what should I do next / what do I still owe" conversation and gives
  `plan_factory` a target ("plan production for the remaining milestone cost").
- **`get_alerts`** (Query): one consolidated health sweep — tripped fuses, machines with
  `GetProductivity()` ≈ 0 (split by starved-input vs. full-output via inventory checks),
  `IsProductionPaused`, dangling belts (reuse `find_disconnected` internals), generators out of
  fuel. Each alert: location (metres + region) + one-line cause. This becomes the standing-task
  workhorse in Slice 5 and the model's first call for any "is something wrong" ask.

**Gate:** trip a fuse + starve one smelter on the test save → `get_alerts` names both precisely;
`reset_fuse` restores power and warns about the overload.

### Slice 2 — In-place mutations (unchanged from P7 Slice 2, confidence upgraded)

`propose_set_clocks(selector, percent)`, `propose_set_recipe(selector, recipe)`,
`propose_upgrade_belts(selector, targetMk)` + the **mutation journal kind** (before/after values;
undo restores). Design exactly as PHASE7.md §Slice 2; two updates from this week's experience:

- The hologram-driving fear has shrunk: the belt-tap splice proved we can drive attachment
  holograms onto live buildables headlessly and verify outcomes post-construct. `TryUpgrade` is the
  same family. Keep the P7 fallback notes but expect the direct path to work.
- `propose_set_clocks` should accept the `get_clock_advice` output shape directly (selector = "as
  advised") so "apply your recommendations" is one call. v1 still caps at 100% (no shards).

**Gate:** P7's gate verbatim (upgrade belts within 100 m → approve → visibly faster → undo restores),
plus "underclock what you recommended" round-trip.

### Slice 3 — Tap symmetry & buffers

> **Status (2026-07-18): `propose_pipe_tap` SHIPPED (81e3bf6)** — free-end or junction splice +
> ONE feed run (≤56 m; chains stay belt-only until the belt chain live-verifies), both target
> modes. **`propose_storage` deferred by decision:** the revise-by-prompt chain already collapses
> the buffer to one approval (`propose_build` containers → `propose_manifold {direction in,
> forProposalId}` → `propose_belt_tap {forProposalId}`), so a dedicated tool would duplicate the
> manifold planner for sugar. Revisit only if the model proves bad at the 3-call chain in live use.
> Slices 1 (f487585) and 2 (9a62735) shipped the same day.

- **`propose_pipe_tap`** — the belt-tap flow for fluids. Source census over `AFGBuildablePipeline`
  (contents via fluid descriptor + `FindOffsetClosestToLocation` equivalents), splice via
  `AFGPipelineAttachmentHologram` (junction-on-pipe is the same native splice family as
  splitter-on-belt), feed = pipe runs. Two knowns from live-verify to respect: pipes REQUIRE engine
  snap (no direct-wire fallback), and pipes have no flow direction at the junction (simpler than
  belts). Chained pipe feeds reuse `TapChainPointsCm` but each hop must end snapped — if waypoint
  hops fail live, v1 ships single-run pipe taps (≤56 m) and the chain stays belt-only.
- **`propose_storage`** — a buffer: container(s) + input manifold (+ optional belt-tap feed in the
  same combined proposal). Mostly composition: `propose_build` v1 containers + `ManifoldSets` +
  the tap machinery. Spec: `{count?, direction 'in'|'overflow', item?}`.

**Gate:** coal-power water loop tapped from an existing water line, one approval; "add a 2-container
screw buffer here fed from the nearest screw belt" builds container + splitter row + feed.

### Slice 4 — `propose_factory` (the flagship)

> **Status (2026-07-18): CODE SHIPPED** — 4a generator + 4b inter-step routing in one pass, per
> the user's call to defer ALL live testing to one consolidated session (the belt-tap-chain-first
> rule below is superseded by that decision). Implementation notes: rows raw-most-first along +Y,
> per-port-rank manifold rows, ingredient-driven StepLinks (a new TickConnected phase joining
> open-end attachments), MachineRecipeToSet/MachineClockToSet applied right after the machine
> phase, chunked pole kit. Raw-input feeds stay taps-by-follow-up, exactly as sketched. The P8
> exit gate below is now the live session's headline test.

`plan_factory` already computes machines-per-step, exact clocks, belt marks, power, raw inputs.
This slice turns a chosen plan into ONE connected proposal:

- **4a — generator:** new tool `propose_factory(item, ratePerMin, origin?, constraints?)`. Runs the
  planner, then emits placements server-side: one machine row per recipe step (row spacing from
  footprints), belt-in + belt-out manifold sets per row (existing `ManifoldSets` machinery),
  auto-power kit, foundations under everything (slab math from extend-foundations exists —
  `followTerrain`-style Z). Ghost previews the whole thing (tiles + run ghosts already render).
  Store per-machine recipe + clock on the proposal (new fields), applied at execute completion —
  which is exactly Slice 2's `SetRecipe`/`SetPendingPotential` code, so **Slice 2 is a hard
  prerequisite**.
- **4b — inter-step routing:** connect step N's out-manifold to step N+1's in-manifold with the
  chained-run machinery (`TickFeedHop` generalized to N source→dest pairs). Raw-input side:
  optionally end with belt-taps from existing source belts (`propose_belt_tap` internals with
  multiple destinations). v1 keeps steps on one floor in a line; multi-floor ("tall") stays the
  model's job via spec-v2 until this is proven.
- P7 Slice 4's remaining spec-v2 `belts`/`wires`/`recipe`/`clock`/lifts items fold in here as the
  underlying mechanism rather than a separate model-facing spec format — the generator emits the
  internal structures directly; the model never hand-writes port graphs.

**Gate (= the P8 exit gate):** *"build me 30 iron plates/min right here"* → one proposal (ghosts for
machines, manifolds, runs, poles, foundations) → one approval → machines produce with correct
recipes/clocks, belts connected across steps, powered → `get_item_balance` confirms ≥30/min →
`/aida undo` removes the entire line with refunds. Then P7's screws-manifold exit gate as written.

### Slice 5 — Standing tasks (unchanged design from P7 Slice 6)

> **Status (2026-07-18): SHIPPED (5fe94a2)** — `/aida task` commands (act tier, quoted prompt,
> Nm/Nh cadence), in-save `FAIDAStandingTask`, 60 s scheduler (one run at a time, most-overdue
> first, `minIntervalMinutes` floor, `maxPerDay` UTC budget, pre-stamped LastRunUtc), and a
> `bReadOnly` cap on the tool loop so Act tools are refused mid-check. OK contract swallows quiet
> runs. Built AHEAD of Slice 4 because 4 is gated on the belt-tap live-verify. Gate still pending
> a live pass (container-consistency watch + "warn me when coal drops below 500").

Human-created only, Query-tier only, opt-in, budgeted — exactly as PHASE7.md specifies. Two
refinements now available: the default task prompt is effectively "call `get_alerts`, report only
changes," and results can anchor against `take_snapshot` for drift detection. Ship AFTER Slice 1 so
the workhorse tool exists.

**Gate:** P7's gate (container-consistency watch) plus "warn me when coal storage drops below 500."

---

## 3. New/changed tools summary

| Tool | Tier | Slice | Notes |
|---|---|---|---|
| `reset_fuse` | Act (direct-execute candidate) | 1 | trivially reversible |
| `get_milestone_status` | Query | 1 | |
| `get_alerts` | Query | 1 | consolidates diagnostics; standing-task workhorse |
| `propose_set_clocks` | Act | 2 | accepts `get_clock_advice` output |
| `propose_set_recipe` | Act | 2 | mutation journal kind |
| `propose_upgrade_belts` | Act | 2 | `TryUpgrade` hologram path |
| `propose_pipe_tap` | Act | 3 | engine-snap-only; chain may be belt-only in v1 |
| `propose_storage` | Act | 3 | container + manifold + optional tap |
| `propose_factory` | Act | 4 | plan → one connected proposal; needs Slice 2 |
| `/aida task …` commands | human-only | 5 | unchanged from P7 |

Deferred by decision: tool-surface consolidation (merge diagnostics into `get_alerts(kind?)`,
selector unification) — tracked, not scheduled.

## 4. API grounding (verified against real headers, 2026-07-18)

| Need | API | Header |
|---|---|---|
| Fuse state / reset | `UFGPowerCircuit::IsFuseTriggered` / `ResetFuse(instigator)` | `FGPowerCircuit.h:159-163` |
| Milestone + remaining cost | `AFGSchematicManager::GetActiveSchematic` / `GetRemainingCostFor` / `PayOffOnSchematic` | `FGSchematicManager.h:239-263` |
| Stall detection | `AFGBuildableFactory::GetProductivity` / `IsProductionPaused` | `Buildables/FGBuildableFactory.h:180-213` |
| Pipe splice hologram | `AFGPipelineAttachmentHologram` (junction-on-pipe, same family as the belt splice) | `Hologram/FGPipelineAttachmentHologram.h:15` |
| Set recipe / clock | `AFGBuildableManufacturer::SetRecipe`, `AFGBuildableFactory::SetPendingPotential` | (grounded in PHASE7.md §4) |
| Belt upgrade | `AFGHologram::TryUpgrade` / `GetUpgradedActor` | (grounded in PHASE7.md §4) |

Still to ground per-slice: pipeline contents census (fluid descriptor on `AFGBuildablePipeline`),
junction free-port conventions, generator fuel-level reads (`AFGBuildableGeneratorFuel`).

## 5. Config additions

```jsonc
"actions": { /* existing */
  "maxBeltsPerUpgrade": 0,               // 0 = unlimited (maxProposalItems convention)
  "allowRecipeChangeWithContents": false,
  "allowDirectFuseReset": true },        // false = fuse reset goes through the proposal flow
"tasks": { "enabled": false, "minIntervalMinutes": 5, "maxPerDay": 200 }
```

## 6. Risks & standing debt

- **Slice 4 is the only big lift**; everything else is a day-or-two each. Its riskiest piece
  (inter-step chained routing) is the same machinery as belt-tap feeds, which still has live-verify
  assumptions open (splice latch, chain hop validation, belt-to-belt flow at hop boundaries).
  **Live-verify the belt-tap chain BEFORE starting Slice 4.**
- Mutation undo introduces the first non-build journal kind — get the before/after capture right
  once in Slice 2; Slice 4 reuses it for recipe/clock assignment.
- Standing debt unchanged: P1 final gate (2-client streaming), P4 2-client exit gate, README
  Installation/Usage sections.
- Tool count keeps growing until the consolidation pass; watch small-model tool-selection accuracy
  if the Gemma experiment starts before consolidation.
