#include "Adapters/AnthropicAdapter.h"

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
	/** Anthropic SSE: a `content_block_delta` event whose delta is a `text_delta` carries reply text. */
	FString ExtractAnthropicDelta(const FString& DataJson)
	{
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DataJson);
		if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
		{
			return FString();
		}

		FString Type;
		if (!Json->TryGetStringField(TEXT("type"), Type) || Type != TEXT("content_block_delta"))
		{
			return FString();
		}

		const TSharedPtr<FJsonObject>* Delta = nullptr;
		if (!Json->TryGetObjectField(TEXT("delta"), Delta) || !Delta)
		{
			return FString();
		}

		FString DeltaType;
		if (!(*Delta)->TryGetStringField(TEXT("type"), DeltaType) || DeltaType != TEXT("text_delta"))
		{
			return FString();
		}

		FString Text;
		(*Delta)->TryGetStringField(TEXT("text"), Text);
		return Text;
	}
}

FAnthropicAdapter::FAnthropicAdapter(const FString& InBaseUrl, const FString& InApiKey)
	: BaseUrl(InBaseUrl)
	, ApiKey(InApiKey)
{
}

FString FAnthropicAdapter::BuildRequestBody(const FAIDACompletionRequest& Request, bool bStream)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), Request.Model);
	Root->SetNumberField(TEXT("max_tokens"), Request.MaxTokens);
	if (bStream)
	{
		Root->SetBoolField(TEXT("stream"), true);
	}
	if (!Request.System.IsEmpty())
	{
		Root->SetStringField(TEXT("system"), Request.System);
	}

	TArray<TSharedPtr<FJsonValue>> Messages;
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

void FAnthropicAdapter::Complete(const FAIDACompletionRequest& Request, FAIDAOnChunk OnChunk,
	FAIDAOnComplete OnComplete, FAIDAOnError OnError)
{
	FString Url = BaseUrl;
	Url.RemoveFromEnd(TEXT("/"));
	Url += TEXT("/v1/messages");

	const FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(Url);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("content-type"), TEXT("application/json"));
	HttpRequest->SetHeader(TEXT("anthropic-version"), TEXT("2023-06-01"));
	HttpRequest->SetHeader(TEXT("x-api-key"), ApiKey);
	HttpRequest->SetContentAsString(BuildRequestBody(Request, /*bStream=*/true));

	AIDADriveSSEStream(HttpRequest, FAIDASSEDeltaExtractor(&ExtractAnthropicDelta),
		MoveTemp(OnChunk), MoveTemp(OnComplete), MoveTemp(OnError));

	HttpRequest->ProcessRequest();
}
