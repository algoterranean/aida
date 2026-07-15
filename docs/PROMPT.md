# System Prompt v2 — proposal

Goal: given the README wishlist questions, make the model *reliably* answer them — right tool
sequences, right arithmetic, right spec geometry — by spending context on curated knowledge.
Budget is a non-issue for capacity (~1M tokens available); the real constraint is **cost per
request**, which prompt caching solves if we structure for it (§5).

Current state: `GAIDASystemPrompt` is ~600 tokens hardcoded in `AIDAOrchestrator.cpp:72`
(contradicting `Configs/prompts/system.md`, which says "never hardcode prompt text in C++" and is
itself never loaded — `prompts.systemPromptFile` config is parsed but unused). Tool documentation
is duplicated between the prompt and the tool definitions. The live proposal state-line is
string-appended to the system prompt each request.

---

## 1. Architecture: composed, file-based, generated

Assemble the system prompt from ordered sections at client creation; reload on config reload.
Most-stable content first (caching), dynamic content last:

```
[1] persona + rules            prompts/system.md          hand-written, ~1k tok, rarely changes
[2] game data pack             GENERATED at startup       ~15–25k tok, changes on unlock/patch/mod
[3] playbooks + spec guide     prompts/playbooks.md       hand-written, ~3k tok
[4] physics annex              prompts/physics.md         hand-written, ~0.5k tok
[5] live state line            generated per request      ~0.1k tok, ALWAYS LAST
```

- Fixes the file-vs-hardcoded contradiction: everything hand-written lives in `Configs/prompts/`
  (server admins can tune personas without a rebuild — also the P6 "persona packs" hook).
- **Stop duplicating tool docs in the prompt.** Tool *selection* guidance (when to call what — the
  playbooks) stays in the prompt; parameter documentation lives only in the tool definitions,
  which the API already delivers. Cuts drift (the current prompt hand-lists 15 tools and will rot
  every slice).

## 2. Section [2] — the generated game data pack (the big win)

Most wishlist failures so far were *knowledge* failures, not reasoning failures: the model assumed
a "Foundation (2 m)" is 2 m wide (it's 8×8 m — P4 bug #5), and it can't do "1 GW of coal" or "how
many floors" math without knowing rates, footprints, and power curves. Round-tripping through
`lookup_recipe` works but costs latency and only surfaces one item at a time.

**Generate the pack from the live game at startup** (RecipeService already walks
`GetAllAvailableRecipes`; extraction already reads buildings) — never bake numbers into the repo.
Generated = always matches the installed patch, active mods, and *this save's unlocks*:

- **Recipes** (~250 × ~40 tok ≈ 10k): name, inputs/outputs in items-per-minute at 100%, craft
  time, building. Alternates marked `[ALT]`. Only unlocked ones; note at the top: "recipes not
  listed exist but are not unlocked — use lookup_recipe if the player insists."
- **Buildings** (~100 × ~30 tok ≈ 3k): footprint W×D×H in metres (from CDO clearance — the
  Foundation lesson institutionalized), power draw or generation, **per-building power exponent**
  (`mPowerConsumptionExponent`) so under/overclock power math is exact, inputs/outputs count
  (ports).
- **Logistics tiers** (~0.5k): belt mk→items/min, pipe mk→m³/min, miner mk × purity→ore/min,
  train/drone throughput reference.
- **Power reference** (~0.5k): each generator's MW, fuel burn rate at 100%, water need; clock
  scaling rule (generators: linear fuel; producers: power = base × clock^exponent).
- **The save's context** (~0.5k): map region names that exist, current tier/phase, active mods.

At ~20k tokens this eliminates most lookup round-trips and makes ratio math (coal-power sizing,
manifold balancing, floor counts) single-pass. `lookup_*` tools stay for verification and for the
"not unlocked" tail.

## 3. Section [1]+[3] — rules and playbooks keyed to the wishlist

Persona/rules (§1 file) keeps the existing good content (tools-not-guesses, metres, chat-overlay
style, proposal honesty rule) and adds grounding rules that generalize the P4 lessons:

- **Numbers must have a source**: a tool result this conversation, or the data pack. If neither
  has it, say so. Never present arithmetic on invented rates.
- **Distances/directions**: compute from coordinates you actually have (tool results); cardinal
  directions from +X/+Y conventions (state them once).
- **Verify-after-act**: after any approved build/mutation, confirm with a read tool
  (`get_proposal_status`, then `get_item_balance` if production was the goal) before declaring
  success. (Generalizes the "never claim built" rule.)
- **Ambiguity**: when a request names a place/thing ambiguously ("my desert base — near the
  river, not the coastal one"), prefer `get_notes`/regions/markers to guessing; ask only when
  tools can't disambiguate.

Playbooks (§3 file): one short block per wishlist question *class* — the intended tool sequence +
answer shape. Sketch (full text drafted when slices land, since several reference P7 tools):

| Ask class | Playbook core |
|---|---|
| Bottlenecks / starving | overview → balance → find_bottleneck; answer = root cause + numbers + where |
| What changed / since login | compare_to (login snapshots exist P7-S0); lead with deltas, not totals |
| Upgrade belts to MkN | find_belt_mismatch → propose_upgrade_belts; report count+cost before proposing |
| Nearest node | get_resource_nodes(untapped) → offer tag_node |
| Design X/min + build | plan_factory → present plan (machines/clocks/power/raws) → on approval-to-proceed, emit spec v2 → propose_build |
| Energy waste / underclock | get_clock_advice; savings in MW via the pack's power exponents |
| Power plant sizing | pure math from the pack (worked example embedded: 1 GW coal) |
| Manifold + route | plan_factory → find_logistics_targets → spec v2 manifold sugar |
| Disconnected / slow belts | find_disconnected + find_belt_mismatch; answer = locations by region |
| Label containers | get_container_contents → propose_label_containers; recurring part → tell player about /aida task |
| Undo / approvals | unchanged (human-only; /aida commands) |
| Hypertube launch | physics annex math → advice + proposed n/angle; building the tube = normal propose flow |

Plus a **spec-authoring guide** with 3–4 few-shot spec v2 JSON examples (a manifold, a two-floor
tower with lifts, a belt route to a depot) once Slice 4 fixes the schema — few-shots are the
highest-leverage guidance for structured output and today's prompt has zero examples.

## 4. Section [4] — physics annex (~0.5k tok)

The hypertube ballistics from `docs/PHASE7.md §8`: v(n) ≈ 12.4×1.203ⁿ m/s, g_eff ≈ 11.76 m/s²,
range formula with height term, inverse (R → n at 45°), the 17-entrance map-boundary warning,
fall-damage-is-vertical-only note, and the ±20% disclaimer with "recommend a test shot."
Also: pioneer walk/sprint speeds, jump height, zipline/tube cruise speeds — cheap lines that help
"how do I get there" answers.

## 5. Caching & cost (why structure order matters)

With a ~25k-token prompt, an uncached request costs real money every chat turn; Anthropic prompt
caching makes repeat prefixes ~10× cheaper. Consequences:

- Sections [1]–[4] are byte-stable across requests (already true in spirit: `GetSpecs` is
  name-sorted precisely for a "cache-stable request prefix"). Regenerate [2] only on unlock
  changes/config reload, not per request.
- **Move the live state line out of the cached prefix**: keep it as the final system block (or a
  synthetic leading user message) and add a `cache_control` breakpoint after section [4] in the
  Anthropic adapter (OpenAI-compat: automatic prefix caching, ordering alone helps). Today the
  state line is concatenated into the one system string, which would invalidate the whole prefix
  every request once the prompt is large.
- Config: `prompts.packEnabled` (default true) + `prompts.packMaxTokens` guardrail; degraded mode
  (no key / pack generation failure) falls back to today's slim prompt.

## 6. What NOT to put in the prompt

- Live factory state beyond the one-line status (tempting with 1M context, but it's per-request
  cache poison and the tools already answer it fresher).
- Full API/tool parameter schemas (already in tool defs).
- The transcript-history policy, rate limits, permission tiers — server-enforced; the model only
  needs the user-facing consequences it already has.

## 7. Rollout

1. Loader: assemble-from-files + generated pack + cache breakpoint (small orchestrator/adapter
   change; the `systemPromptFile` config finally does something).
2. Pack generator behind `AIDA.DumpPack` console cmd first — eyeball the output on the real save
   before wiring it in.
3. A/B the wishlist questions live (the P2-style tuning session): each question asked cold, tool
   calls observed via the `[tools]` log line, before/after the pack.
4. Few-shot spec examples land with P7 Slice 4; playbook lines land with their slices.
