#pragma once

#include "CoreMinimal.h"
#include "Adapters/AIDALLMTypes.h"
#include "Interfaces/IHttpRequest.h"

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
 * Pulls the text delta out of one SSE `data:` JSON payload for a specific provider.
 * Returns the delta text, or empty for events that carry no text (message_start, ping, [DONE], …).
 * Called on the HTTP worker thread, so it must not touch game-thread state.
 */
using FAIDASSEDeltaExtractor = TFunction<FString(const FString& /*DataJson*/)>;

/**
 * Wires an SSE streaming lifecycle onto an already-configured HTTP request (URL/verb/headers/body
 * all set, `"stream": true` in the body). Requests the body as a receive stream, frames + parses
 * `data:` lines via Extract on the worker thread, and marshals each non-empty delta plus the terminal
 * OnComplete/OnError onto the game thread in order.
 *
 * Does NOT call ProcessRequest() — the caller does that after this returns.
 */
void AIDADriveSSEStream(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& HttpRequest,
	FAIDASSEDeltaExtractor Extract, FAIDAOnChunk OnChunk,
	FAIDAOnComplete OnComplete, FAIDAOnError OnError);
