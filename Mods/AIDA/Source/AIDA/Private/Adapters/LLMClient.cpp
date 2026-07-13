#include "Adapters/LLMClient.h"

#include "AIDA.h"
#include "Adapters/AnthropicAdapter.h"

FLLMClient::FLLMClient(const FAIDAConfig& Config)
{
	Model = Config.Provider.Model;
	MaxTokens = Config.Provider.MaxOutputTokens;

	if (Config.Provider.Type == TEXT("anthropic"))
	{
		Adapter = MakeShared<FAnthropicAdapter>(Config.Provider.BaseUrl, Config.Provider.ApiKey);
	}
	else
	{
		// OpenAICompatAdapter (OpenAI / Ollama / vLLM) is a later increment.
		UE_LOG(LogAIDA, Warning,
			TEXT("LLMClient: provider type '%s' not implemented yet (only 'anthropic' for now)."),
			*Config.Provider.Type);
	}
}

FLLMClient::~FLLMClient() = default;

void FLLMClient::Complete(const FString& UserText, FAIDAOnComplete OnComplete, FAIDAOnError OnError)
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

	Adapter->Complete(Req, MoveTemp(OnComplete), MoveTemp(OnError));
}
