#pragma once

#include "CoreMinimal.h"
#include "Adapters/ILLMAdapter.h"

struct FAIDAStreamAccumulator; // defined in Adapters/SSEStream.h; used only by reference below

/**
 * Anthropic Messages API adapter (POST {baseUrl}/v1/messages).
 * Streams the reply over SSE (design decision #2, "streaming from day one").
 * The API key is held here and in LLMClient only — never logged, never replicated.
 */
class FAnthropicAdapter : public ILLMAdapter
{
public:
	FAnthropicAdapter(const FString& InBaseUrl, const FString& InApiKey);

	virtual void Complete(const FAIDACompletionRequest& Request, FAIDAOnChunk OnChunk,
		FAIDAOnCompleteResult OnComplete, FAIDAOnError OnError) override;
	virtual FString GetName() const override { return TEXT("anthropic"); }

	/** Public + static so unit tests can assert wire-format translation without a network call. */
	static FString BuildRequestBody(const FAIDACompletionRequest& Request, bool bStream = false);

	/**
	 * Process one Anthropic SSE `data:` payload: appends text_delta to the return value and records
	 * tool_use blocks / stop_reason on Acc. Public + static so tests can drive the tool-use
	 * accumulation (content_block_start(tool_use) -> input_json_delta* -> content_block_stop) offline.
	 */
	static FString HandleStreamEvent(const FString& DataJson, FAIDAStreamAccumulator& Acc);

private:
	FString BaseUrl;
	FString ApiKey;
};
