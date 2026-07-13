# Index/

The extraction + aggregation seam. **Along with `Actions/`, the ONLY place FactoryGame
gameplay headers may be included** (patch-resilience rule — docs/DEV.md coding rule 3).

- **FactoryIndex** — walks buildables via the game's subsystems (TTL-cached), produces the
  normalized model (machines, belt/pipe graph, power circuits, storage), aggregates into
  clusters / balance sheet / logistics / power report. Versioned accessors + startup self-tests;
  flips to `degraded` mode on game-patch breakage rather than crashing.
- **MapService** — static resource-node index, named-region lookup, coordinate humanization,
  map-marker creation through the game's replicated marker system.

See `docs/ARCHITECTURE.md` §4.
