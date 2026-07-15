# Phase 4 addendum — Manifolds

AIDA can place grids of buildables (docs/PHASE4.md). This addendum teaches it the second half of
factory plumbing: **manifolds** — the row of splitters (or mergers) + connecting belts that feeds a
line of machines, and the pipe-junction + pipe equivalent for fluids. One new tool,
`propose_manifold`, flowing through the SAME pipeline: propose → dry-run → ghost + approval card →
time-sliced execute → journal → `/aida undo`.

## 1. What gets built

For a row of N machines, direction `"in"` (feed their inputs):

```
 feed here ──► [S0] ──trunk──► [S1] ──trunk──► [S2] ...   S = splitter (row axis →)
                │drop           │drop           │drop
                ▼               ▼               ▼
              [M0]            [M1]            [M2]        M = machine (input port)
```

- **Attachments**: one splitter per machine, on a straight trunk line `standoffM` (default 4 m) in
  front of the machines' ports, sorted along the row axis. Direction `"out"` uses mergers and the
  flow reverses (trunk collects toward index 0). `kind: "pipe"` uses a pipeline junction; pipes have
  no direction so only geometry differs.
- **Trunk runs**: N−1 belts (pipes) between consecutive attachments' pass-through ports.
- **Drop runs**: N belts (pipes) between each attachment's machine-facing port and the machine port.
- The **open end** (splitter row's feed / merger row's collection point) is always at index 0 — the
  first machine along the row axis — and is reported in the proposal summary so the player knows
  where to hook in.

## 2. Spec v1 (LLM contract, `propose_manifold`)

```json
{
  "version": 1,
  "kind": "belt",                      // "belt" | "pipe"
  "direction": "in",                   // "in" = feed inputs (splitters/junctions), "out" = collect outputs (mergers/junctions)
  "transport": "Conveyor Belt Mk.2",   // belt or pipe display name, fuzzy-resolved, unlocked only
  "attachment": "",                    // optional override; defaults: Conveyor Splitter / Conveyor Merger / Pipeline Junction Cross
  "machines": {                        // same selector shape as propose_dismantle
    "buildable": "Smelter",            // required, display name
    "center": {"x": -120, "y": 45},    // metres; omitted = where the requester is aiming
    "radiusM": 30,                     // default 30
    "maxCount": 0                      // 0 = all matches
  },
  "standoffM": 4,                      // optional, trunk-line distance in front of the ports
  "port": 0                            // optional, which matching machine port (0 = first unconnected)
}
```

## 3. Resolution + geometry (dry-run, all before storing the proposal)

1. **Machines** resolve live (never from a FactoryIndex snapshot): walk `AFGBuildableSubsystem`
   buildables matching the selector name within the radius. Machines with connections are always
   full actors (never lightweight), so ports enumerate directly.
2. **Ports**: per machine, the `port`-th UNCONNECTED `UFGFactoryConnectionComponent` with direction
   `FCD_INPUT` (in) / `FCD_OUTPUT` (out) — pipes: `UFGPipeConnectionComponent` of the consumer /
   producer type. Machines whose matching port is already connected are SKIPPED and counted in the
   report ("skipped 3 already connected") — re-running a manifold over a half-plumbed row extends
   only what's missing (drops to already-fed machines are never duplicated; the trunk is still
   planned over the remaining machines only, so true trunk-splicing into an existing manifold stays
   a v2 item — today you dismantle the old manifold first, which `propose_dismantle` already does).
3. **Row fit** (`AIDAActionSpec::PlanManifold`, pure, unit-tested): all port normals must agree
   within ~45° (mixed-facing machines → error, v1). Row axis = `Up × avgNormal`; trunk line runs
   through `centroid(ports) + avgNormal * standoff`, attachments at each port's projection onto
   that line, sorted along the axis. Attachment yaw points the pass-through at the axis (splitter
   input / merger output face −axis, so flow runs +axis for splitters, −axis for mergers — the open
   end lands at index 0 either way). Adjacent attachments closer than the attachment footprint →
   error ("machines too close together").
4. **Z**: attachment bases ground-probe (the existing two-stage build-gun-channel down-trace) under
   each trunk point; belts auto-route the height difference to the ports. (Machines at a platform
   edge can drop the trunk to the terrain below — ugly but valid; v1 accepts it.)
5. **Attachment validation**: existing `DryRunBuild` over the attachment transforms — real
   holograms, real disqualifiers, soft-clearance filtered as everywhere else.
6. **Run validation** (geometric only — the runs can't hologram-validate until the attachments
   exist): endpoint distance must be within [min spline length, 56 m]. Run costs are ESTIMATED from
   straight-line length and reported as `"costNote": "transport cost estimated; charged as built"`.

## 4. Costs

Attachment cost is exact (hologram-tallied) and deducted UPFRONT at approval, like any build. Belt
and pipe cost scales with actual routed spline length, which only exists at execute time — so each
run is charged from central storage AS IT BUILDS (costMode `"free"` skips both, as everywhere).
Central storage running dry mid-manifold fails the remaining runs individually ("unaffordable"),
reported like any other partial failure — same philosophy as "world changed since dry-run".

## 5. Execution (time-sliced, per the existing 10 Hz engine tick)

Manifold proposals execute in three phases, cursor per phase, `batchPerTick` attachments but ONE
run per tick (a run = spawn spline hologram → two-step place → validate → construct — heavier):

- **Phase A — attachments**: the existing hologram batch path, but tracking the constructed actor
  PER INDEX (a skipped attachment must not shift its neighbors' port mapping; its trunk hop and
  drop simply fail later with "attachment missing").
- **Phase B — trunk runs**: for i in 0..N−2, a spline hologram driven exactly like the build gun:
  `UpdateHologramPlacement(hit at from-port)` → `DoMultiStepPlacement` →
  `UpdateHologramPlacement(hit at to-port)` → both ends must report `IsConnectionSnapped` (an
  unsnapped end would silently construct a conveyor pole instead of a connection — hard per-run
  failure, never a half-connected surprise) → validate (soft-clearance filtered) → `Construct`.
  Ports are picked on the LIVE actor by direction + best normal-dot with the wanted world
  direction — robust to local component naming/layout.
- **Phase C — drop runs**: same procedure, attachment ↔ machine port (direction per `in`/`out`).
  The machine's weak ptr re-verifies; a machine dismantled since propose = one failed drop.

Every constructed entity — attachments, belts/pipes, any auto-constructed children — is journaled
individually with the existing entity-id codec, so `/aida undo` reverses a manifold exactly like a
grid (refunds included, belts refund their length-scaled cost).

## 6. UI / ghosts / adjust

- **Ghosts**: the proposal view carries the ATTACHMENT recipe + attachment tile centers — players
  see the splitter row exactly where it will stand. Run paths are not ghosted in v1 (spline ghosts
  need the endpoints to exist); the card summary carries the run count instead: "manifold: 10 ×
  Conveyor Splitter + 19 × Conveyor Belt Mk.2 runs feeding 10 × Smelter (feed at west end)".
- **Nudge/rotate**: REFUSED for manifold proposals ("manifolds are anchored to machine ports —
  reject and re-propose instead"). The geometry is derived, not free.
- Approval card, `/aida approve|reject`, TTL, pending caps: unchanged.

## 7. Slices

- **M0 (pure)**: `FAIDAManifoldSpec` + `ParseManifoldSpec`; `PlanManifold` geometry (row fit, sort,
  transforms, yaw, spacing/length checks); summaries; unit tests.
- **M1 (read/validate)**: seam `ResolveMachinePorts`; dry-run assembly (plan → attachment DryRun →
  run length checks + estimate); `propose_manifold` registration + system-prompt/tool docs; ghost
  publish (existing payload, attachment recipe).
- **M2 (execute)**: per-index attachment capture; `ResolveAttachmentPort` (live actor, direction +
  normal-dot); `BuildConnectingRun` (two-step spline hologram drive, snap-verified, per-run cost
  deduction); engine phase machinery; journal + undo coverage; adjust refusal.

Live-verify checklist (single player): manifold onto a freshly built 1×5 smelter row (in), mergers
off the outputs (out), pipes onto refineries, re-run over a half-connected row (skip counting),
undo the whole thing, central-storage dry-out mid-run.

## 8. Risks / open questions

- **Belt hologram two-step drive** is the load-bearing assumption (build-gun flow replicated
  server-side: `DoMultiStepPlacement` step transitions + `TrySnapToActor` via synthetic hits at
  connector locations). First thing M2 live-verifies; the snap-check hard-fail keeps failures loud.
- **Splitter lightweight conversion**: assumed NOT to convert (attachments have inventories +
  factory tick). If one converts post-Construct, its runs fail with "attachment missing" — loud,
  recoverable, and the fallback (resolve temp from the lightweight ref) is a known pattern.
- **Cost estimate drift**: straight-line estimate vs routed spline can under/overshoot; charged as
  built (§4), so the report's affordability line is advisory for the run portion.
- Deferred: trunk-splicing into an existing manifold, lifts for vertical drops, load balancers,
  ghosting the run paths, mixed-facing machine rows.
