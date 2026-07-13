# Actions/ (Phase 4+)

The world-action pipeline: proposal → dry-run → confirm → execute → undo. No AI-initiated
world mutation ever bypasses this. May include FactoryGame headers (the second and last
game-API seam alongside `Index/`).

- **ActionEngine** — assigns `ProposalId` (TTL), runs dry-run validation via the game's
  hologram/validation paths, routes execute/dismantle through `AFGBuildableSubsystem` +
  blueprint subsystem in time-sliced batches, appends `FAIDAJournalEntry`, powers `/aida undo`.

See `docs/ARCHITECTURE.md` §6.
