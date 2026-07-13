#pragma once

#include "CoreMinimal.h"
#include "Adapters/ILLMAdapter.h"

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
		FAIDAOnComplete OnComplete, FAIDAOnError OnError) override;
	virtual FString GetName() const override { return TEXT("openai-compatible"); }

	/** Public + static so unit tests can assert wire-format translation without a network call. */
	static FString BuildRequestBody(const FAIDACompletionRequest& Request, bool bStream = false);

private:
	FString BaseUrl;
	FString ApiKey;
};
