# Adapters/

LLM client and provider translation. Engine-independent where possible so wire-format
logic is unit-testable without the game.

- **LLMClient** — one internal interface: `Complete(request, onChunk, onToolCall, onDone, onError)`.
- **OpenAICompatAdapter** — native format (OpenAI, Ollama, vLLM, gateways); SSE parser.
- **AnthropicAdapter** — translates to the Anthropic Messages API; maps `tool_use`/`tool_result` and SSE events back to the common interface. Vision payloads in Phase 5.

The API key lives only in server config + `LLMClient` memory. Never log it, never replicate it.
See `docs/ARCHITECTURE.md` §3.2.
