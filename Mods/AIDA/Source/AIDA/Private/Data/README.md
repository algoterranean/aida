# Data/

Persistence and memory. No FactoryGame gameplay headers here — consumes normalized types.

- **NotesStore** — location-tagged player annotations; retrieved by proximity/region/keyword.
- **SnapshotService** — periodic aggregate snapshots (default 30 min); ring buffer in sidecar; powers `compare_to(timestamp)`.
- **Persistence** — two backends behind one interface: in-save SML component (notes, journal, marker registry, session GUID) and sidecar JSON files under `Configs/AIDA/data/<session-guid>/`.

See `docs/ARCHITECTURE.md` §3.2 and §10 (Phase 3).
