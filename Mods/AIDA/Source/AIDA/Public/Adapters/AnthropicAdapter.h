#pragma once

#include "CoreMinimal.h"
#include "Adapters/ILLMAdapter.h"

/**
 * Anthropic Messages API adapter (POST {baseUrl}/v1/messages).
 * Non-streaming first cut; SSE streaming is a later increment (design decision #2).
 * The API key is held here and in LLMClient only — never logged, never replicated.
 */
class FAnthropicAdapter : public ILLMAdapter
{
public:
	FAnthropicAdapter(const FString& InBaseUrl, const FString& InApiKey);

	virtual void Complete(const FAIDACompletionRequest& Request, FAIDAOnComplete OnComplete, FAIDAOnError OnError) override;
	virtual FString GetName() const override { return TEXT("anthropic"); }

	/** Public + static so unit tests can assert wire-format translation without a network call. */
	static FString BuildRequestBody(const FAIDACompletionRequest& Request);

private:
	FString BaseUrl;
	FString ApiKey;
};
