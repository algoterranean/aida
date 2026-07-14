#pragma once

#include "CoreMinimal.h"
#include "Adapters/AIDALLMTypes.h"
#include "Interfaces/IHttpRequest.h"

class FJsonObject;

/**
 * Line framing for a Server-Sent Events (SSE) response body that arrives in arbitrary chunks.
 *
 * The HTTP worker thread hands us raw byte ranges with no regard for line or UTF-8 boundaries.
 * We buffer the raw bytes and only split on '\n' (0x0A) — which, being ASCII, never appears as a
 * UTF-8 continuation byte — so each emitted line is a complete, safely decodable UTF-8 string even
 * when a multi-byte codepoint straddles two chunks.
 *
 * NOT thread-safe: a single request's Serialize/stream delegate is invoked sequentially, so all
 * Feed() calls for one FAIDASSEBuffer happen on the same worker thread, one at a time.
 */
struct FAIDASSEBuffer
{
	/** Raw bytes received but not yet terminated by a newline. */
	TArray<uint8> Pending;

	/**
	 * Append newly-arrived bytes and invoke OnLine for each complete line (newline consumed, a
	 * trailing '\r' trimmed so CRLF and LF both yield a clean line). Blank lines are emitted too —
	 * callers that don't care can ignore them.
	 */
	void Feed(const void* Ptr, int64 Len, const TFunctionRef<void(const FString&)>& OnLine);
};

/**
 * Running state a provider's stream handler fills in as SSE events arrive. Text is assembled by the
 * driver (from the deltas the handler returns), so a handler must NOT write Text — it only records
 * tool calls, the stop reason, and its own per-block scratch (PendingByIndex, keyed by content-block
 * index for Anthropic or tool_call index for OpenAI). Lives on the worker thread only.
 */
struct FAIDAStreamAccumulator
{
	FString Text;
	FString StopReason;
	TArray<FAIDAToolCall> ToolCalls;
	TMap<int32, FAIDAToolCall> PendingByIndex; // provider scratch for tool calls still accumulating args
};

/**
 * Processes one SSE `data:` JSON payload for a specific provider: mutates the accumulator (tool
 * calls / stop reason) and returns any NEW visible-text delta (empty for events that carry no text —
 * message_start, ping, tool events, [DONE], …). Called on the HTTP worker thread, so it must not
 * touch game-thread state.
 */
using FAIDASSEEventHandler = TFunction<FString(const FString& /*DataJson*/, FAIDAStreamAccumulator& /*Acc*/)>;

/**
 * Wires an SSE streaming lifecycle onto an already-configured HTTP request (URL/verb/headers/body
 * all set, `"stream": true` in the body). Requests the body as a receive stream, frames + parses
 * `data:` lines via Handle on the worker thread, marshals each non-empty text delta onto the game
 * thread in order, and finally delivers the assembled structured result (text + tool calls + stop
 * reason) via OnComplete — or OnError on a non-2xx / transport failure.
 *
 * Does NOT call ProcessRequest() — the caller does that after this returns.
 */
void AIDADriveSSEStream(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& HttpRequest,
	FAIDASSEEventHandler Handle, FAIDAOnChunk OnChunk,
	FAIDAOnCompleteResult OnComplete, FAIDAOnError OnError);

/**
 * Parse a JSON object string; returns an empty object on empty/invalid input (never null). Shared by
 * both provider adapters (each serializes tool args/schemas), defined once here so the two adapter
 * translation units don't emit a duplicate symbol in unity builds.
 */
TSharedRef<FJsonObject> AIDAParseObjectOrEmpty(const FString& JsonStr);
