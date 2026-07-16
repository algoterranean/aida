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

	/** Set the system prompt prepended to every completion (identity + tool guidance). */
	void SetSystemPrompt(const FString& InPrompt) { SystemPrompt = InPrompt; }

	/**
	 * Convenience: a single user turn using the configured model / system prompt.
	 * OnChunk fires per streamed delta (may be unset); then exactly one of OnComplete/OnError.
	 */
	void Complete(const FString& UserText, FAIDAOnChunk OnChunk, FAIDAOnComplete OnComplete, FAIDAOnError OnError);

	/**
	 * Full multi-turn completion: the caller supplies the assembled message history (already
	 * privacy-filtered). Text-only terminal callback. Same streaming/callback contract as Complete().
	 */
	void CompleteChat(const TArray<FAIDAChatMessage>& Messages, FAIDAOnChunk OnChunk, FAIDAOnComplete OnComplete, FAIDAOnError OnError);

	/**
	 * Tool-aware multi-turn completion: exposes the structured result (text + tool calls + stop
	 * reason) so the orchestrator's tool loop can dispatch tool calls and continue. Tools may be empty.
	 */
	void CompleteChat(const TArray<FAIDAChatMessage>& Messages, const TArray<FAIDAToolDef>& Tools,
		FAIDAOnChunk OnChunk, FAIDAOnCompleteResult OnComplete, FAIDAOnError OnError);

	/**
	 * Pure model-selection rule (Phase 5): a request that carries images uses VisionModel when one
	 * is configured; everything else (and an empty VisionModel) uses the default model.
	 */
	static FString ChooseModel(const FString& DefaultModel, const FString& VisionModel,
		const TArray<FAIDAChatMessage>& Messages);

private:
	TSharedPtr<ILLMAdapter> Adapter;
	FString Model;
	FString VisionModel;
	int32 MaxTokens = 1024;
	FString SystemPrompt;
};
