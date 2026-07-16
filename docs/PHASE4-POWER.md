# Phase 4 addendum — Auto-power

Machines AIDA places should come online, not sit dark waiting for hand-wiring. Every `propose_build`
of a powered buildable now includes power distribution BY DEFAULT: power poles (the minimum unlocked
mk, unless directed otherwise) spread evenly through the grid, power lines from every machine to its
pole, poles chained into one circuit, and a tie-in to the nearest existing grid when one is in reach.
`"power": false` in the spec opts out; `"pole": "Power Pole Mk.3"` overrides the pole tier.

## 1. What gets built (one proposal, three phases)

```
 [M] [M] │ [M] [M]      row-major machine grid (phase 0)
     P   │     P        one pole per chunk of G machines, offset half a cell
 [M] [M] │ [M] [M]        off the row line (phase 1)
     P ──chain── P      machine→pole lines, pole→pole chain, one tie to the
                └──► existing grid   (phase 2, wires + tie)
```

- **G (machines per pole)** = pole connection cap − 2 (chain links reserved); mk.1 (4) → 2, mk.2
  (7) → 5, mk.3 (10) → 8. The cap is read from the pole CDO's power connection; the pole recipe is
  the LOWEST unlocked mk unless the spec overrides.
- **Wires are `AFGBuildableWire` actors spawned directly and joined with `Connect()`** — the same
  connection-authoring approach that made belt runs reliable (docs/PHASE4-MANIFOLDS.md §5); no
  hologram snapping involved. `Connect` doesn't enforce a max length, so chain hops always succeed;
  the grouping radius just keeps things looking sane.
- **Grid tie**: after wiring, the nearest EXTERNAL circuit connection with a free slot (poles
  preferred) within ~100 m of any built pole gets one line to an end pole. None in reach → a loud
  report line ("no existing power grid within reach — connect manually"), never a failure.

## 2. Costs / validation / undo

Machine + pole cost is exact (hologram dry-run, deducted upfront at approval, merged affordability
check). Wires are length-priced and charged as built, like manifold runs. Pole placements ground-
probe and dry-run like any build — clipping is advisory ([[clipping-never-blocks]]), and a pole that
fails hard validation is skipped at execute; its machines' wires then fail LOUDLY in the run report
instead of silently. Every pole and wire is journaled (wires get `SetBuiltWithRecipe` so dismantle
refunds work) — `/aida undo` reverses the whole powered build.

## 3. Pieces

- `AIDAActionSpec::PlanPower` (pure, tested): row-major chunking, pole transforms (between rows,
  half-step offset; last row folds back), machine→pole + chain wire index pairs.
- Seam: `ResolveAutoPower` (does this recipe's CDO have a power connection; resolve pole candidates
  lowest-unlocked-first + read cap; resolve the Power Line recipe), `CheckAffordable` (merged tally
  vs central storage), `BuildWire` (spawn + Connect + charge, capacity-checked, journaled),
  `FindGridTie` (nearest external free circuit connection near the poles).
- Engine: powered builds run phases 0 machines → 1 poles (both per-index captured) → 2 wires
  (batched per tick) + grid tie, reusing the manifold run-report machinery for wire failures.

## 4. Risks / deferred

- Pole CDO cap read could fail on odd pole classes → falls back to 4 (mk.1's cap, the conservative
  grouping). Poles assumed not lightweight-convertible (they carry circuit connections).
- Wire cost multiplier is estimated from straight-line length (charged as built, advisory in the
  dry-run report), same policy as manifold runs.
- Deferred: powering machines built OUTSIDE this proposal (a propose_power selector tool), priority
  power switches, wall outlets/ceiling mounts, generator hookups.
