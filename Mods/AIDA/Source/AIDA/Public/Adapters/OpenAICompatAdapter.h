#pragma once

#include "CoreMinimal.h"
#include "Adapters/ILLMAdapter.h"

struct FAIDAStreamAccumulator; // defined in Adapters/SSEStream.h; used only by reference below

/**
 * OpenAI-compatible Chat Completions adapter (POST {baseUrl}/chat/completions).
 * Works for OpenAI, Ollama, vLLM, and most gateways. Streams the reply over SSE.
 * Bearer auth is sent only when an API key is configured (Ollama on LAN needs none).
 */
class FOpenAICompatAdapter : public ILLMAdapter
{
public:
	FOpenAICompatAdapter(const FString& InBaseUrl, const FString& InApiKey);

	virtual void Complete(const FAIDACompletionRequest& Request, FAIDAOnChunk OnChunk,
		FAIDAOnCompleteResult OnComplete, FAIDAOnError OnError) override;
	virtual FString GetName() const override { return TEXT("openai-compatible"); }

	/** Public + static so unit tests can assert wire-format translation without a network call. */
	static FString BuildRequestBody(const FAIDACompletionRequest& Request, bool bStream = false);

	/**
	 * Process one OpenAI SSE `data:` payload: appends choices[0].delta.content to the return value and
	 * accumulates streamed tool_calls / finish_reason on Acc (finalizing pending tool calls when a
	 * finish_reason arrives). Public + static so tests can drive the accumulation offline.
	 */
	static FString HandleStreamEvent(const FString& DataJson, FAIDAStreamAccumulator& Acc);

private:
	FString BaseUrl;
	FString ApiKey;
};
