#pragma once

#include "CoreMinimal.h"
#include "Adapters/AIDALLMTypes.h"
#include "Core/AIDAConfig.h"

class ILLMAdapter;

/**
 * Server-side LLM entry point. Picks the provider adapter from config and exposes one
 * Complete() call. Holds the API key (in the chosen adapter) — never logged, never replicated.
 */
class FLLMClient
{
public:
	explicit FLLMClient(const FAIDAConfig& Config);
	~FLLMClient();

	bool IsReady() const { return Adapter.IsValid(); }

	/** Convenience: a single user turn using the configured model / system prompt. */
	void Complete(const FString& UserText, FAIDAOnComplete OnComplete, FAIDAOnError OnError);

private:
	TSharedPtr<ILLMAdapter> Adapter;
	FString Model;
	int32 MaxTokens = 1024;
	FString SystemPrompt;
};
