#include "Adapters/LLMClient.h"

#include "AIDA.h"
#include "Adapters/AnthropicAdapter.h"
#include "Adapters/OpenAICompatAdapter.h"

FLLMClient::FLLMClient(const FAIDAConfig& Config)
{
	Model = Config.Provider.Model;
	MaxTokens = Config.Provider.MaxOutputTokens;

	if (Config.Provider.Type == TEXT("anthropic"))
	{
		Adapter = MakeShared<FAnthropicAdapter>(Config.Provider.BaseUrl, Config.Provider.ApiKey);
	}
	else if (Config.Provider.Type == TEXT("openai-compatible"))
	{
		Adapter = MakeShared<FOpenAICompatAdapter>(Config.Provider.BaseUrl, Config.Provider.ApiKey);
	}
	else
	{
		UE_LOG(LogAIDA, Warning,
			TEXT("LLMClient: unknown provider type '%s' (expected 'anthropic' or 'openai-compatible')."),
			*Config.Provider.Type);
	}
}

FLLMClient::~FLLMClient() = default;

namespace
{
	/** Adapt a text-only OnComplete to the adapter's structured OnCompleteResult (passes .Text through). */
	FAIDAOnCompleteResult WrapTextComplete(FAIDAOnComplete OnComplete)
	{
		return [OnComplete = MoveTemp(OnComplete)](const FAIDACompletionResult& Result)
		{
			if (OnComplete) { OnComplete(Result.Text); }
		};
	}
}

void FLLMClient::Complete(const FString& UserText, FAIDAOnChunk OnChunk, FAIDAOnComplete OnComplete, FAIDAOnError OnError)
{
	if (!Adapter.IsValid())
	{
		if (OnError) { OnError(0, TEXT("no adapter (provider not implemented)")); }
		return;
	}

	FAIDACompletionRequest Req;
	Req.Model = Model;
	Req.MaxTokens = MaxTokens;
	Req.System = SystemPrompt;

	FAIDAChatMessage Msg;
	Msg.Role = TEXT("user");
	Msg.Content = UserText;
	Req.Messages.Add(MoveTemp(Msg));

	Adapter->Complete(Req, MoveTemp(OnChunk), WrapTextComplete(MoveTemp(OnComplete)), MoveTemp(OnError));
}

void FLLMClient::CompleteChat(const TArray<FAIDAChatMessage>& Messages, FAIDAOnChunk OnChunk, FAIDAOnComplete OnComplete, FAIDAOnError OnError)
{
	if (!Adapter.IsValid())
	{
		if (OnError) { OnError(0, TEXT("no adapter (provider not implemented)")); }
		return;
	}

	FAIDACompletionRequest Req;
	Req.Model = Model;
	Req.MaxTokens = MaxTokens;
	Req.System = SystemPrompt;
	Req.Messages = Messages;

	Adapter->Complete(Req, MoveTemp(OnChunk), WrapTextComplete(MoveTemp(OnComplete)), MoveTemp(OnError));
}

void FLLMClient::CompleteChat(const TArray<FAIDAChatMessage>& Messages, const TArray<FAIDAToolDef>& Tools,
	FAIDAOnChunk OnChunk, FAIDAOnCompleteResult OnComplete, FAIDAOnError OnError)
{
	if (!Adapter.IsValid())
	{
		if (OnError) { OnError(0, TEXT("no adapter (provider not implemented)")); }
		return;
	}

	FAIDACompletionRequest Req;
	Req.Model = Model;
	Req.MaxTokens = MaxTokens;
	Req.System = SystemPrompt;
	Req.Messages = Messages;
	Req.Tools = Tools;

	Adapter->Complete(Req, MoveTemp(OnChunk), MoveTemp(OnComplete), MoveTemp(OnError));
}
