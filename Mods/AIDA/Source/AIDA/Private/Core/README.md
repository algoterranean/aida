# Core/

Server-authoritative orchestration. All code here asserts `HasAuthority()`.

- **AIDAOrchestrator** — SML world subsystem; owns the request lifecycle (chat RPC → permission → rate-limit → context build → LLM call → tool loop → stream → journal). Must be safe with zero players connected.
- **SessionManager** — shared transcript ring buffer, message IDs, streaming fan-out, history summarization.
- **PermissionService** — `chat` / `query` / `act` tiers, checked server-side on every RPC.
- **RateLimiter** — token-bucket per player + global.

See `docs/ARCHITECTURE.md` §3.2.
