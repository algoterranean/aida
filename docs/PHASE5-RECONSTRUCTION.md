# Phase 5 addendum — reconstruction aids

P5 shipped the *pipeline* (attachments → vision model → spec-v2 composite). This addendum ships the
*quality* layer: everything between "the model sees pixels" and "a buildable parts array" used to be
left to its imagination. Four slices, all landed together; none change the wire format or the RPCs.

**Future work (out of scope here):** feeding a viewport capture of the finished build back to the
model for visual comparison. That reverses P5's locked "no screenshot" decision and needs its own
design (client capture + upload channel + multiplayer semantics).

## 1. Reconstruction protocol (system prompt, `uploads.enabled` gated)

The old one-paragraph guidance became a 5-step protocol in `StartAIDAReply` (stable text, before the
volatile state line — prompt-cache friendly):

1. **SCALE** — estimate real dimensions from human-scale cues (door ≈ 2 m, person ≈ 1.8 m, one
   storey ≈ 3–4 m); state the bounding box in metres. Photos have no scale bar; without this the
   model builds doll-house or hangar scale.
2. **PLAN IN TEXT FIRST** — a dimensioned parts table (buildable, size, `at` offset, what it
   represents) *before* the tool call. Committing to numbers in text is chain-of-thought for
   spatial reasoning.
3. **SNAP** — 8×8 m foundation tiles, 1/2/4 m thicknesses, 8×4 m walls, storeys at 4 m of z.
4. **BUILD** — one v2 composite; the placement conventions are spelled out (offsets run origin →
   part's FIRST placement pivot, grids expand +X then +Y, everything rotates with the composite
   yaw). On uneven ground: `probe_terrain` first (§3).
5. **VERIFY** — after execution, check `get_proposal_status`'s `asBuilt` (§4) against the plan and
   propose corrections.

Plus: ask for a second angle when massing is ambiguous, never claim to see an absent image (kept).

## 2. Architecture palette (prompt pack, static)

`AIDAPromptPack::Build` gained an "Architecture palette" section after the Structures list: a photo
material → buildable mapping (slab → foundation, glazing → glass wall, chimney → pillar shaft, …),
the 1/2/4/8 m module rule, and ONE worked v2 composite ("two offset storeys with an 8 m cantilevered
deck over posts") with a prose legend. The example is strictly valid JSON — the unit test parses it
through `ParseBuildSpec` so it can never drift from the real schema. A caveat line redirects to the
Structures list when a named piece isn't unlocked.

## 3. `probe_terrain` (new Query tool)

Ground-height sampling so composite z offsets can follow real terrain (the Fallingwater case — the
design only works *because* of the cliff). Square grid around explicit `x`/`y` or the requester's
aim; `radius_m` (default 32, max 128), `step_m` (default 8, auto-coarsened to cap the grid at 33×33
traces). One `FAIDAActionSeam::ProbeGroundZ` line trace per cell (conveyor-ignoring, pawn-ignoring —
the established seam probe). Report: `AIDAMapTools::BuildTerrainProbeJson` (pure, unit-tested) —
height rows in metres (row 0 = north edge/-Y, columns west→east/+X, `null` = no hit), min/max/spread,
and an orientation legend.

## 4. As-built report (`get_proposal_status.asBuilt`)

The model used to build blind — nothing told it where a build actually landed. Now every **plain
build** proposal that placed something (Executed, or Failed partway = first `Cursor` placements)
carries `asBuilt` in the status JSON: `placed` (+ `planned` when short), resolved `origin`, world
AABB (`min`/`max`, metres, placement pivots — not clearance extents), and for v2 composites one
entry per part run (`buildable` name recovered from the stored spec, `count`, `first`, per-part
AABB; capped at 16 runs). Manifold/label/power-only proposals are excluded — their `Placements`
mean something else. The LIVE PROPOSAL STATE outcome line for an executed build also appends its
as-built origin and points at `get_proposal_status`.

## Verification

Unit (all green, 62 total): `AIDA.PromptPack.ArchitecturePalette` (section present; worked example
parses through `ParseBuildSpec`), `AIDA.MapTools.TerrainProbeJson` (grid/null/min-max/legend),
`AIDA.Actions.StatusAsBuilt` (executed composite per-part runs + metres conversion; failed-partway
`placed`/`planned`; pending and manifold excluded).

**Live-verify (packaged game, pending):** attach a real photo → the reply shows the §1 protocol
(scale estimate + parts table before the tool call); `probe_terrain` on a slope returns sane heights
and the composite follows them; after approval the model reads `asBuilt` and correctly states
whether the build matches its plan.
