# Phase 4 — "Hands"

AIDA gains the ability to *change* the world — but only through the proposal pipeline:
**proposal → dry-run → confirm → execute → undo** (`ARCHITECTURE.md §6`; no AI-initiated mutation ever
bypasses it). Builds are **grid-constrained** in v1: the spec language expresses "N×M copies of one
buildable on a snapped grid" — enough for the exit gate and a row of smelters, and nothing more.

**P4 exit gate:** propose→approve→build→undo of a **10×10 foundation grid with two players connected** —
the requesting player proposes in chat, the *other* `act`-tier player approves in the ProposalUI, the grid
builds without hitching, `/aida undo` removes it with refunds.

**Prerequisite (not absorbed into P4 slices):** finish Phase 3 first — commit Slice 2 (snapshots), wire
`snapshots.{intervalMinutes,keep}` config into the orchestrator (currently hardcoded `kSnapshot*`
constants), and run the P3 packaged exit gate.

**Decisions locked in up front:** builds **cost real resources** from central storage (`costMode:
"central"`, with a `"free"` escape hatch); default approval policy is **any act-tier player**; blueprint
(`.sbp` / blueprint-subsystem) generation is **deferred past Phase 4** entirely — per-buildable holograms
cover the v1 promise.

---

## 1. Pipeline (lifecycle)

```
propose_build / propose_dismantle   (act-tier tools, inside the normal tool loop)
  ├─ parse + validate spec (pure)             ── bad spec → tool ERROR (model revises)
  ├─ dry-run (hologram validate-and-discard)  ── disqualifiers → tool ERROR w/ per-index reasons
  ├─ store FAIDAProposal (TTL 600 s), state Pending
  ├─ replicate FAIDAProposalView to clients + System chat line
  └─ tool result = dry-run report ("awaiting approval, id …") → model tells the player
ServerApproveProposal(Id)           (RCO; any act-tier player per approvalPolicy)
  └─ Approved → Executing: time-sliced executor, batchPerTick per tick
       └─ done → FAIDAJournalEntry (in-save) + Executed → view updated, System chat line
/aida undo [n]                      (chat command, act tier; AIDA.Undo console mirror)
  └─ re-resolve journaled entities → dismantle-what-was-built / rebuild-what-was-dismantled
       └─ partial failures reported verbatim ("undid 97 of 100 — 3 already gone")
```

**Two distinct gates, deliberately.** The existing tool-tier gate in `RunToolLoop`
(`FAIDAPermissionService::IsAllowed(Act, PlayerId)`) answers *"may this player even ask AIDA to build."*
The new per-proposal approval gate answers *"who pulls the trigger."* Approval never carries a spec — the
RPC references `ProposalId` only; the server executes what *it* stored.

**Undo is human-only** — a chat command, deliberately *not* an LLM tool. Reversal authority stays with
players; undo must never depend on a model round-trip.

---

## 2. Data schemas

### 2a. Proposal spec v1 (the LLM contract — versioned independently)

One recipe, one origin, one repeat pattern. Coordinates in **metres** (the established tool convention),
yaw snapped to 90°, converted to cm/`FTransform` internally. The engine never trusts model coordinates —
everything passes dry-run.

```jsonc
// propose_build "spec"
{ "version": 1,
  "buildable": "Foundation 8m x 2m",        // display name, fuzzy-resolved server-side (unlocked only)
  "origin": { "x": -120.0, "y": 45.0, "z": 30.0 },
  "yawDeg": 90,                              // snapped to 0/90/180/270
  "grid": { "countX": 10, "countY": 10,      // total = countX*countY, capped by maxProposalItems
            "stepX": 8.0, "stepY": 8.0 } }   // metres; defaults = buildable footprint

// propose_dismantle "selector"
{ "version": 1,
  "buildable": "Smelter",                    // display-name match; "" = anything
  "center": { "x": -120.0, "y": 45.0, "z": 30.0 },
  "radiusM": 50.0, "maxCount": 20 }
```

Parsed into plain structs `FAIDABuildSpec` / `FAIDADismantleSpec` (`Public/Actions/AIDAActionTypes.h`,
game-header-free). `AIDAActionSpec::ExpandGrid(FAIDABuildSpec) → TArray<FTransform>` is pure math — the
main unit-test surface. An unknown `buildable` returns a tool error listing the closest matches so the
model self-corrects.

### 2b. Dry-run report (tool result / tool error)

```jsonc
// success (proposal stored):
{ "proposalId": "…", "summary": "place 100 x Foundation 8m x 2m in a 10x10 grid",
  "count": 100, "cost": [ { "item": "Concrete", "amount": 300 } ],
  "affordable": true, "powerDrawMW": 0,       // powerDrawMW informational only (building catalog)
  "status": "awaiting approval", "expiresInSec": 600 }

// blocked placements (proposal STILL stored — the ghost goes up; validity is advisory):
{ "proposalId": "…", "summary": "…", "count": 100, "cost": [ … ], "affordable": true,
  "invalidCount": 12, "firstFailures": [
    { "index": 37, "at": { "x": -96, "y": 45 }, "reason": "Encroaching other building" } ],  // ≤5 shown
  "note": "the ghost preview is up anyway — tell the player to nudge it onto clear ground …",
  "status": "awaiting approval", "expiresInSec": 600 }
```

Disqualifier text comes from `UFGConstructDisqualifier::GetDisqualifyingText`. **Placement validity
never fails a proposal** (user rule — uneven terrain must not stop the ghost): blocked placements ride
along as `invalidCount`/`firstFailures`, nudging always applies (the nudge reply reports live validity),
and execute re-validates per tile — still-blocked placements are skipped and their recipe cost is
refunded to the requester (the journal records the NET cost so undo can't double-refund).

### 2c. Proposal record (server-side, in-memory — deliberately not saved)

```cpp
// Public/Actions/AIDAActionTypes.h — plain C++, unit-testable
enum class EAIDAProposalState : uint8 { Pending, Approved, Executing, Executed, Failed, Rejected, Expired, Undone };
struct FAIDAProposal {
    FGuid Id;
    FString SpecJson;                    // verbatim; journaled on execute
    bool bDismantle = false;
    FString RequesterId, RequesterName, ApproverId;
    int64 ProposedUtc = 0;               // TTL anchor
    EAIDAProposalState State = EAIDAProposalState::Pending;
    TArray<FTransform> Placements;       // build: expanded, validated grid
    FString RecipeClassPath;             // build: resolved once at dry-run
    TArray<FString> CostSummary;         // for the view + journal RefundJson
    FString Summary;                     // human diff line
    int32 Cursor = 0;                    // executor progress
    TArray<FString> AffectedEntityIds;   // filled during execute (§2d encoding)
};
```

Proposals live in a `TMap<FGuid, FAIDAProposal>` inside `FAIDAProposalStore`. A server restart voids
pending proposals — correct behavior with a 10-minute TTL, since approval must reference live server
state. Only the *journal* persists.

### 2d. Journal + entity-id encoding (the undo contract)

`FAIDAJournalEntry` already exists in-save (`Public/Memory/AIDAMemoryTypes.h`, Phase 3). Phase 4 adds two
save-compatible fields (`UPROPERTY(SaveGame) bool bDismantle`, `bool bUndone` — tagged-property
serialization defaults missing fields on old saves) and the missing accessors:
`AAIDAMemoryStore::AppendJournal/GetJournal/MarkUndone` + `FAIDAMemory` facade passthroughs.

Each `AffectedEntityIds` element is one compact, self-describing JSON string:

```jsonc
{ "t": "lw",    "class": "/Game/…/Build_Foundation8x2_01_C", "idx": 4127,
  "pos": [x,y,z], "yaw": 90 }                                   // lightweight instance
{ "t": "actor", "class": "/Game/…/Build_SmelterMk1_C",
  "recipe": "/Game/…/Recipe_SmelterMk1_C", "pos": [x,y,z], "yaw": 0 }  // full actor
```

- **Lightweight** (foundations/walls, 1.0 instanced): stable handle is `FLightweightBuildableInstanceRef`
  (class + instance index + cached transform). We journal class+index+transform; re-resolve via the
  lightweight subsystem and confirm the transform matches (indices are recycled — the transform
  disambiguates, exactly as the game's own ref does). After save/load the index may shift; fallback =
  scan the class's instances and match by transform epsilon.
- **Full actors**: the game has **no persistent per-buildable GUID**. In-session the ActionEngine keeps a
  side cache `TMap<FGuid /*JournalId*/, TArray<TWeakObjectPtr<AActor>>>` (never saved). Across save/load,
  undo re-resolves by journaled class + transform epsilon — cheap and unambiguous within a 25-entry window.
- Anything that fails to re-resolve is **reported, never silently skipped** (ARCHITECTURE §8):
  *"undid 97 of 100 (3 foundations were already removed)."*

### 2e. Replicated proposal view (client-facing, bounded)

```cpp
// Public/Net/AIDANetTypes.h — carries NO spec JSON
USTRUCT(BlueprintType) struct FAIDAProposalView {
    UPROPERTY(BlueprintReadOnly) FGuid Id;
    UPROPERTY(BlueprintReadOnly) FString Requester;    // display name
    UPROPERTY(BlueprintReadOnly) FString Summary;      // "place 100 x Foundation …"
    UPROPERTY(BlueprintReadOnly) FString CostSummary;  // "300 Concrete"
    UPROPERTY(BlueprintReadOnly) uint8 State = 0;      // EAIDAProposalState
    UPROPERTY(BlueprintReadOnly) int64 ExpiresUtc = 0;
};
```

---

## 3. ActionEngine internals

Four pieces, split along the established pure-core / game-seam line. `Private/Actions/` becomes the
**second and last game-API seam** (its README already says so).

**`FAIDAProposalStore`** *(pure, `Public/Actions/AIDAProposalStore.h`)* — the map plus a validated state
machine (`Pending→Approved/Rejected/Expired`, `Approved→Executing`, `Executing→Executed/Failed`,
`Executed→Undone`; illegal transitions return false and log), `SweepExpired(NowUtc, TtlSec)` with an
injected clock (unit-testable TTL), and the `maxPendingProposals` cap (default 3 — a fourth proposal is a
tool error: "resolve or wait for pending proposals first").

**`AIDAActionSpec`** *(pure namespace, `Public/Actions/AIDAActionSpec.h`)* — `ParseBuildSpec` /
`ParseDismantleSpec` (strict; unknown version → error), `ExpandGrid`, `Summarize`, `BuildDryRunJson` /
`BuildErrorJson` / `BuildStatusJson`, and the journal codec `EncodeEntityId` / `DecodeEntityId`. Compact
JSON via the existing `AIDAToolJson.h`. This file plus the store is the bulk of the unit-test surface.

**`FAIDAActionSeam`** *(static fns; header game-free, only the .cpp includes FactoryGame — mirrors the
`FAIDAMapService::PlaceMapMarker` pattern)*:

- `ResolveBuildRecipe(World, DisplayName, OutClassPath, OutSuggestions)` — a separate, cached walk of
  **build-gun** recipes (the P2 `FAIDARecipeCatalog` covers crafting recipes and carries no class handle);
  fuzzy display-name match; **only unlocked recipes resolve** — AIDA cannot build what players haven't earned.
- `DryRunBuild(World, RecipeClassPath, Placements, OutReport)` — spawns **one** hologram via
  `AFGHologram::SpawnHologramFromRecipe`, using the `preSpawnFunction` hook to `SetReplicates(false)` and
  hide it (clients never see a ghost). Per placement: position it (`SetHologramLocationAndRotation` with a
  synthetic upward-normal hit, then `PreHologramPlacement`/`PostHologramPlacement`), run `CheckClearance()`
  + `CanConstruct()`, collect `GetConstructDisqualifiers()` on failure. Cost = `GetCost(true)` × count.
  `Destroy()` the hologram at the end — **fully non-mutating**. Synchronous in v1 (≤ `maxProposalItems`
  checks; the build gun does this every frame — if live-verify shows a spike, dry-run moves onto the
  execute time-slicer).
- `ExecuteBuildBatch(World, Proposal&, BatchSize, OutNewEntityIds)` — the same hologram walk, ending each
  placement with `Construct(out_children, FNetConstructionID{})` and advancing `Proposal.Cursor`. After
  each construct: if the buildable registered as lightweight (`FLightweightBuildableInstanceRef::
  InitializeFromTemporary` resolves), encode `"lw"`; else cache the weak ptr and encode `"actor"`. Build
  effects skipped for bulk (not needed for correctness).
- `ResolveDismantleTargets(World, Selector, OutTargets)` — walks `AFGBuildableSubsystem::
  GetAllBuildablesRef()` **live at call time** (never from a `FAIDAFactoryIndex` snapshot —
  `FAIDAMachine.Id` is per-snapshot, not a handle), filters by display name + radius; refund tally via
  `IFGDismantleInterface::Execute_GetDismantleRefund` and, for lightweights,
  `GetLightweightBuildableDismantleRefundReturns`. Called at dry-run *and again* at execute (count drift
  between the two is reported, not fatal).
- `DismantleBatch(…)` — `Execute_CanDismantle` + `Execute_Dismantle` per target; lightweight targets via
  `FLightweightBuildableInstanceRef::Remove()`.
- `TallyCost / DeductCost / RefundCost(World, Items)` — `AFGCentralStorageSubsystem`
  (`GetNumItemsFromCentralStorage` / withdraw). Deduction happens **once, upfront**, when execution starts
  — a single failure point; a mid-run affordability change can't strand a half-built grid.
- `UndoEntry(World, JournalEntry, SessionCache, OutFailures)` — decode ids → re-resolve (§2d) → dismantle
  (for builds) or rebuild-from-spec via the execute path (for dismantles), refunding per `costMode`.

**`FAIDAActionEngine`** *(coordinator, plain orchestrator member like `FAIDAMemory`)* — owns the store,
requester/approver bookkeeping, the in-session weak-ptr cache, and the executor:

- **Time-slicer:** an `FTimerHandle` on the orchestrator (the `SnapshotTimer` pattern) firing at 10 Hz
  *only while something is Executing*; each fire runs one batch of `batchPerTick` (default 10) items — a
  10×10 grid completes in ~1 s without a single-frame spike. On completion: journal append through
  `FAIDAMemory`, `FactoryIndex`/`MapService` cache invalidation, relay view update, System chat line
  ("built 100 foundations").
- **TTL sweep:** piggybacks the timer cadence plus a lazy check on access; expired proposals transition to
  `Expired` and the view updates.
- **Undo window:** `Undo(N)` walks the journal backwards over non-`bUndone` entries, at most `undoWindow`
  (default 25) deep, invoking `UndoEntry` per entry (time-sliced through the same executor for large
  entries) and reporting per-entry partial failures.

---

## 4. Net + UI surface

**`AAIDAProposalRelay : AModSubsystem`** (`Public/Net/AIDAProposalRelay.h`) — a parallel relay to
`AAIDAChatRelay` (same `SpawnOnServer_Replicate` + always-relevant + `SubsystemActorManager` registration
recipe, registered alongside it in the orchestrator). **Deliberate deviation** from the chat relay's
multicast-event style: proposals replicate as a **replicated array**

```cpp
UPROPERTY(ReplicatedUsing=OnRep_Proposals) TArray<FAIDAProposalView> Proposals;
UPROPERTY(BlueprintAssignable) FAIDAOnProposalsChangedDelegate OnProposalsChanged;  // fired from OnRep
```

because proposals must **survive late-join within their TTL** — state replication gives late joiners the
pending list for free, where fire-and-forget multicasts required a bulk-fetch RPC to patch that hole.
Terminal-state entries (Executed/Rejected/Expired/Failed) linger ~60 s so approvers see the outcome, then
drop. Server API `ServerUpsertProposal(View)` / `ServerRemoveProposal(Id)`, driven only by the ActionEngine.

**RCO additions** (`Public/Net/AIDARemoteCallObject.h`):

```cpp
UFUNCTION(Server, Reliable, WithValidation) void ServerApproveProposal(const FGuid& ProposalId);
UFUNCTION(Server, Reliable, WithValidation) void ServerRejectProposal(const FGuid& ProposalId);
```

Both resolve the caller's identity exactly like `ServerSendChat` and call
`UAIDAOrchestrator::HandleProposalDecision(Requester, Id, bApprove)`, which checks **both gates**:
`Permissions.IsAllowed(Act, PlayerId)` *and* `approvalPolicy` (`"any-act"` default | `"requester"` |
explicit id list). Denials post a System chat line and log with the player id (audit rule, §7 of
ARCHITECTURE).

**ProposalUI v1 — extend `UAIDAChatWidget`, no second window.** The exit gate needs two players *seeing
and clicking*, not a new HUD: four `BindWidgetOptional` sub-widgets — `ProposalPanel` (collapsed when
nothing is pending), `ProposalText` (summary + cost + requester + countdown), `ApproveButton`,
`RejectButton` — self-wired in C++ to `OnProposalsChanged`, zero Blueprint logic, exactly like the
transcript wiring. Clicks go through the local RCO (`GetLocalRCO()` pattern). Buttons are hidden for
non-act players *cosmetically only* — the server always enforces. Each proposal is also announced as a
System chat message. Ghost/hologram preview at the site: **stretch, not v1**.

**`/aida undo [n]`.** No slash-command path exists today (chat goes straight into `HandleChatRequest`), so
a tiny pure parser `AIDAChatCommands::TryParse(Text)` is consulted at the **top of `HandleChatRequest`**:
`/aida undo [n]` short-circuits the LLM entirely, gates on act tier, calls `ActionEngine.Undo(N)`, posts
the result as a System message. `AIDA.Undo [n]` console command mirrors it, joining the existing `AIDA.*`
set.

---

## 5. Tools (Phase 4)

| Tool | Tier | Notes |
|------|------|-------|
| `propose_build(spec)` | act | Parse → dry-run → store + replicate. **Never executes.** Returns the dry-run report, or a tool error (bad spec / disqualifiers / unaffordable / too many pending) so the model revises. |
| `propose_dismantle(selector)` | act | Same shape; report = target count + refund tally. Targets re-resolved fresh at execute. |
| `get_proposal_status(proposalId?)` | query | Status of one or all live proposals — lets the model answer "did anyone approve that yet?" without hallucinating. |

The system prompt gains a short paragraph: propose only when a player asks to build/remove something;
always relay the returned summary plus "a player with act permission must approve"; never claim something
was built until status says Executed.

---

## 6. Config (`actions` block)

```jsonc
"actions": {
  "enabled": true,               // master kill-switch; off ⇒ propose_* tools not registered
  "ttlSeconds": 600,
  "approvalPolicy": "any-act",   // "any-act" | "requester" | ["<epic-account-id>", …]
  "maxProposalItems": 200,       // hard cap on expanded placements per proposal
  "maxPendingProposals": 3,
  "batchPerTick": 10,            // executor slice size (10 Hz timer)
  "undoWindow": 25,
  "costMode": "central"          // "central" (tally + deduct vs central storage) | "free"
}
```

`FAIDAActionsConfig` in `Public/Core/AIDAConfig.h` + a parse block in `Private/Core/AIDAConfigLoader.cpp`
mirroring the Snapshots block — and, unlike the P3 Snapshots block, **every field is actually consumed**.

**Cost handling.** `"central"` (the default): dry-run reports `affordable` via central storage and
execution refuses to start if it no longer holds; execute deducts upfront; undo-of-build refunds to
central storage; undo-of-dismantle re-deducts. `"free"`: the tally is reported but never deducted
(creative/testing servers).

---

## 7. Build order (slices)

- **Slice 0 — Spec + store + config** *(pure)*: `FAIDAActionsConfig` + parse; `AIDAActionTypes.h`;
  `AIDAActionSpec` (parse/validate/grid-expand/summarize/journal codec); `FAIDAProposalStore` (state
  machine + TTL, injected clock); `AppendJournal/GetJournal/MarkUndone` on `AAIDAMemoryStore` +
  `FAIDAMemory`; journal field additions. Everything here unit-tests without the game.
- **Slice 1 — Dry-run + propose tools**: `FAIDAActionSeam::ResolveBuildRecipe/DryRunBuild/
  ResolveDismantleTargets/TallyCost`; register the three tools; TTL sweep; `AIDA.Propose <json>` console
  driver (widget-free, like `AIDA.Say`). Live-verify (packaged, host): "build me a 3×3 of foundations
  here" yields a proposal with correct cost; an intentionally colliding spec comes back as a tool error the
  model visibly revises from; **no ghost appears for clients** during dry-run.
- **Slice 2 — Execute + journal** *(approval stubbed via `AIDA.Approve <id>`)*: time-sliced executor,
  upfront cost deduction, entity capture (lightweight refs + weak ptrs), journal append, index
  invalidation. Live-verify: a 10×10 grid builds smoothly (watch frame time); cost leaves central storage;
  the journal entry survives save/reload. **This slice resolves the two flagged risks** (§9): central-
  storage refund deposits, and `InitializeFromTemporary` lightweight capture timing.
- **Slice 3 — Approval net + ProposalUI**: `AAIDAProposalRelay` (replicated array + delegate +
  registration); RCO Approve/Reject + `HandleProposalDecision` (both gates + policy); chat-widget proposal
  panel + System announcements. Live-verify (two clients): B sees A's proposal appear (including after a
  late join), approves, both see the build; a non-act player's approve is refused server-side.
- **Slice 4 — Undo**: `AIDAChatCommands::TryParse` + `/aida undo [n]` + `AIDA.Undo`; `UndoEntry`
  re-resolution (both entity kinds, both directions); refunds; partial-undo reporting; `bUndone`
  persistence. Live-verify = **the full exit gate**, plus undo *after* save/reload (re-resolve path), and
  manually dismantling 3 foundations before undoing to see "undid 97 of 100…".

Blueprint generation (`AFGBlueprintHologram` + programmatic `FBlueprintRecord`) is explicitly **deferred
past Phase 4** — the grid spec covers the v1 promise and the exit gate doesn't need it.

---

## 8. Verification

**Unit (no game; `Automation RunTests AIDA.` as in P1–P3, `Private/Tests/AIDATests.cpp`):** spec parse
(good/bad/versioned/unknown-field), grid expansion (counts, steps, yaw snap, metre→cm, cap), summaries,
dry-run/error JSON shapes, proposal state machine (every legal/illegal transition), TTL expiry with a fake
clock, pending cap, journal entity-id encode/decode round-trip, undo-window selection over synthetic
journals, `/aida undo` parsing, config-block parsing.

**Packaged game only (SML subsystems don't run in PIE):** hologram validate-and-discard really is
non-mutating and invisible to clients; disqualifier fidelity; `Construct` entity capture (especially
lightweight); executor hitch behavior; central-storage deduct/refund; relay replication + late join; RCO
gating; save/reload re-resolution; the exit gate itself.

---

## 9. Decisions & open questions

- ~~**Dry-run is all-or-nothing** in v1~~ — superseded: placement validity is now ADVISORY at propose and
  nudge time (see §2b). Every proposal ghosts; execute builds what re-validates, skips the rest, and
  refunds the skips. The per-index failure contract survives as the `firstFailures` advisory.
- **Proposals don't survive restart** (in-memory store; the journal is the only persisted artifact).
  Within a 10-minute TTL this is correct behavior, not a gap.
- **Replicated-array relay** deviates from the chat relay's multicast style — justified by the late-join
  requirement (§4). The one intentional pattern divergence.
- **Undo is human-only** (command, not a tool) — a slight narrowing of ARCHITECTURE §6, which is silent on
  model-initiated undo; the safer default.
- **Costs are real by default** (`costMode: "central"`); the honest mode, and it exercises the refund path
  for the exit gate. `"free"` remains for creative servers.
- **Refund destination:** central storage is assumed to accept programmatic deposits server-side; if the
  packaged game refuses, fallback is refund-to-approver-inventory — resolved in Slice 2 live-verify.
- **Lightweight capture timing** (`InitializeFromTemporary` right after `Construct`) is the highest-risk
  game-API assumption — Slice 2 tests it first, and the §2d encoding already carries the transform
  fallback if index capture proves unreliable.
- **Blueprint generation deferred past P4** (revising the ARCHITECTURE §10 phase table, which listed it
  under Phase 4) — programmatic `FBlueprintRecord` authoring is underdocumented and the grid spec covers
  v1. Tracked with: ghost preview at the proposal site, `skipInvalid`, and composite/multi-recipe specs.
