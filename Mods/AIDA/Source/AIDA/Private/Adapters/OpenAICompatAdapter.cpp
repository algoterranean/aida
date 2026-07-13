#include "Adapters/OpenAICompatAdapter.h"

#include "AIDA.h"
#include "Adapters/SSEStream.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/** OpenAI SSE: each chunk is `choices[0].delta.content` (absent on role-only / finish frames). */
	FString ExtractOpenAIDelta(const FString& DataJson)
	{
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DataJson);
		if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
		{
			return FString();
		}

		const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
		if (!Json->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->Num() == 0)
		{
			return FString();
		}

		const TSharedPtr<FJsonObject> Choice = (*Choices)[0]->AsObject();
		if (!Choice.IsValid())
		{
			return FString();
		}

		const TSharedPtr<FJsonObject>* Delta = nullptr;
		if (!Choice->TryGetObjectField(TEXT("delta"), Delta) || !Delta)
		{
			return FString();
		}

		FString Content;
		(*Delta)->TryGetStringField(TEXT("content"), Content);
		return Content;
	}
}

FOpenAICompatAdapter::FOpenAICompatAdapter(const FString& InBaseUrl, const FString& InApiKey)
	: BaseUrl(InBaseUrl)
	, ApiKey(InApiKey)
{
}

FString FOpenAICompatAdapter::BuildRequestBody(const FAIDACompletionRequest& Request, bool bStream)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), Request.Model);
	Root->SetNumberField(TEXT("max_tokens"), Request.MaxTokens);
	if (bStream)
	{
		Root->SetBoolField(TEXT("stream"), true);
	}

	// OpenAI carries the system prompt as the first message (unlike Anthropic's top-level "system").
	TArray<TSharedPtr<FJsonValue>> Messages;
	if (!Request.System.IsEmpty())
	{
		const TSharedRef<FJsonObject> Sys = MakeShared<FJsonObject>();
		Sys->SetStringField(TEXT("role"), TEXT("system"));
		Sys->SetStringField(TEXT("content"), Request.System);
		Messages.Add(MakeShared<FJsonValueObject>(Sys));
	}
	for (const FAIDAChatMessage& Msg : Request.Messages)
	{
		const TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("role"), Msg.Role);
		M->SetStringField(TEXT("content"), Msg.Content);
		Messages.Add(MakeShared<FJsonValueObject>(M));
	}
	Root->SetArrayField(TEXT("messages"), Messages);

	FString Body;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Root, Writer);
	return Body;
}

void FOpenAICompatAdapter::Complete(const FAIDACompletionRequest& Request, FAIDAOnChunk OnChunk,
	FAIDAOnComplete OnComplete, FAIDAOnError OnError)
{
	FString Url = BaseUrl;
	Url.RemoveFromEnd(TEXT("/"));
	Url += TEXT("/chat/completions");

	const FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Url);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("content-type"), TEXT("application/json"));
	if (!ApiKey.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	}
	HttpRequest->SetContentAsString(BuildRequestBody(Request, /*bStream=*/true));

	AIDADriveSSEStream(HttpRequest, FAIDASSEDeltaExtractor(&ExtractOpenAIDelta),
		MoveTemp(OnChunk), MoveTemp(OnComplete), MoveTemp(OnError));

	HttpRequest->ProcessRequest();
}
