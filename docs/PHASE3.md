# Phase 3 — "Memory"

AIDA gains persistent recall across sessions: **player notes** (what the factory is *for*),
**history snapshots** (what *changed*), and the **journal schema** (what AIDA *did* — populated in
Phase 4). Two persistence backends behind one facade, per `ARCHITECTURE.md §5` — small critical state
**in-save**, bulky history in a **sidecar**.

**P3 exit gate:** (a) "AIDA, remember I want more power at the northern hub" → next session, "what did I
want to do about power?" recalls it; (b) "how has my iron output changed since my last snapshot?" returns a
real numeric diff. Verified on a packaged save across a save/reload.

---

## 1. Persistence design (the shared layer — build first)

Two backends behind one facade (`FAIDAMemory`), routed by data type:

| Data | Backend | Why |
|------|---------|-----|
| Session GUID, notes, action journal (P4), marker registry | **in-save** | small, critical, must travel with the save + survive server migration |
| Snapshots, full transcripts, prompt logs | **sidecar** | bulky, append-mostly, fine to orphan on rollback |

### 1a. Session GUID
An `FGuid` generated once for a save and stored **in-save**. It keys the sidecar directory to the save, so
a rolled-back save simply orphans newer sidecar data (never corrupts). Generated in `PostLoadGame` when
unset (fresh save) — `Math.random`/time-of-day not needed; `FGuid::NewGuid()` is fine here (runtime, not a
workflow script).

### 1b. In-save backend — `AAIDAMemoryStore : public AModSubsystem, public IFGSaveInterface`
`AFGSubsystem` (AModSubsystem's base) does **not** implement `IFGSaveInterface`, so we implement it
explicitly and tag persistent fields `SaveGame`. Server-authoritative: `ESubsystemReplicationPolicy::
SpawnOnServer` (tools read it server-side; no client replication needed). Registered like `AAIDAChatRelay`
via the `SubsystemActorManager`.

```cpp
UCLASS()
class AAIDAMemoryStore : public AModSubsystem, public IFGSaveInterface
{
    // IFGSaveInterface: ShouldSave()=true; the rest empty (no transform, no deps).
    UPROPERTY(SaveGame) FGuid SessionId;
    UPROPERTY(SaveGame) TArray<FAIDANote> Notes;
    UPROPERTY(SaveGame) TArray<FAIDAJournalEntry> Journal;   // schema now; written in P4
    UPROPERTY(SaveGame) TArray<FAIDAMarkerRecord> Markers;   // tag_node registry (so AIDA can list/clear)
    // PostLoadGame: mint SessionId if invalid. Static Get(world) accessor.
};
```

### 1c. Sidecar backend — `FAIDASidecarStore` (plain C++, host-side)
JSON/JSONL files under **`Configs/AIDA/data/<session-guid>/`** (same writable root as `config.jsonc`,
resolved via the config loader's base path). Files:
- `snapshots.jsonl` — one snapshot per line, ring-buffered to `snapshots.keep` (default 200).
- `transcripts/<conversation-guid>.jsonl` — optional, gated by `privacy`/`logPromptsToSidecar`.

Pure serialization (JSON round-trip) is **unit-testable** without a game; only path resolution touches the
environment. Writes are host-only and best-effort (a failed sidecar write logs + degrades, never blocks chat).

### 1d. Facade — `FAIDAMemory`
One object the services + orchestrator talk to. Holds a weak ptr to the in-save store (resolved per world)
and an `FAIDASidecarStore`. Methods route by type: `AddNote/GetNotes/AppendJournal/AddMarker` → in-save;
`AppendSnapshot/LoadSnapshots/AppendTranscript` → sidecar. Keeps the services backend-agnostic.

---

## 2. Data schemas (the "journal schema" deliverable)

```cpp
// In-save (USTRUCTs with SaveGame fields).
struct FAIDANote {
    FGuid Id; FString Author; FString AuthorId; FString Text;
    FVector Location; FString Region; int64 CreatedUtc; TArray<FString> Tags;
};
struct FAIDAMarkerRecord { FGuid Id; FString Label; FString Resource; FVector Location; int64 CreatedUtc; };
struct FAIDAJournalEntry {                  // defined now, populated in Phase 4 (undo)
    FGuid Id; FString ProposalSpecJson; FString RequesterId; FString ApproverId;
    int64 ProposedUtc; int64 ExecutedUtc; TArray<FString> AffectedEntityIds; FString RefundJson;
};

// Sidecar (plain structs → compact JSON, reuses the P2 aggregates).
struct FAIDASnapshot {
    int64 TakenUtc; FString Label;
    // flattened FAIDAFactoryAggregates: per-item balance, per-cluster efficiency, power per circuit.
};
```
`int64 CreatedUtc` = Unix seconds (portable, sortable, no `FDateTime` serialization quirks). Compare tools
resolve "timestamp" loosely (nearest snapshot ≤ requested time, or the previous one).

---

## 3. Services

**NotesStore** (thin, over the in-save note array): add, and query by proximity (`near` = player location),
`region`, or `keyword`. Location + region captured like `tag_node` (from the tool context / MapService).

**SnapshotService** (server timer, `snapshots.intervalMinutes` default 30): every interval, aggregate the
factory (reuse `SnapshotAggregates()`) → `FAIDASnapshot` → sidecar ring buffer. Also an on-demand
`take_snapshot`. Powers `compare_to`.

---

## 4. Tools (Phase 3)

| Tool | Tier | Notes |
|------|------|-------|
| `add_note(text, tags?)` | query | Attributed to the requester; captures their location + region (Ctx.Location). |
| `get_notes(near?, keyword?, region?)` | query | Bounded, sorted; proximity uses Ctx.Location when `near`. |
| `take_snapshot(label?)` | query | Force a snapshot now (besides the periodic timer). |
| `compare_to(timestamp?, item?)` | query | Diff current aggregates vs the nearest snapshot: per-item balance deltas, machine-count + efficiency changes, power deltas. |

All query-tier (read/annotate, not world-modifying). Registered in the orchestrator alongside the P2 tools;
`compare_to` + snapshots reuse `FAIDAFactoryAggregates`.

---

## 5. Build order (slices)

- **Slice 0 — Persistence foundation** *(mixed)*: schemas (§2) + `FAIDAMemory` facade + `FAIDASidecarStore`
  (JSON round-trip, path resolution) + `AAIDAMemoryStore` (in-save, session GUID). Unit-test the sidecar
  round-trip + ring-buffer; live-verify the in-save store persists the session GUID across save/reload.
- **Slice 1 — NotesStore + add_note/get_notes**: in-save notes; unit-test the query/filter (proximity,
  keyword, region) on synthetic notes; live-verify a note survives save→reload and recalls next session.
- **Slice 2 — SnapshotService + take_snapshot/compare_to**: periodic + on-demand snapshots to the sidecar
  ring buffer; unit-test the diff math on synthetic aggregate pairs; live-verify a real before/after diff.
- **Slice 3 — Journal schema + config wiring**: land `FAIDAJournalEntry` in-save (empty until P4);
  `snapshots.{intervalMinutes,keep}` + `privacy.logPromptsToSidecar` config; optional transcript sidecar.

## 6. Verification

Unit tests (no game): sidecar JSON round-trip + ring-buffer eviction; note query/filter; snapshot diff.
Packaged verify (SML not in PIE): session GUID stable across reload; a note persists + recalls; a real
`compare_to` diff. Headless suite via `Automation RunTests AIDA.` as in P1/P2.

## 7. Decisions & open questions

- **Snapshot summarization stays numeric** (no LLM cost) — the model reasons over the raw diff. (ARCHITECTURE
  open Q resolved this way for P3; revisit if diffs get noisy.)
- **Notes are shared** (one store, attributed per player), not per-player-private — matches the shared-memory
  intent; private threads remain an open question tracked in ARCHITECTURE §11.
- **Sidecar path** uses the config loader's resolved base (`Configs/AIDA/`), so it works the same in the
  packaged game where the real config already lives.
- **tag_node marker registry** folds into the in-save store now (so AIDA can later list/clear its markers) —
  a small addition that makes P2's tag_node stateful and sets up P4's marker cleanup.
