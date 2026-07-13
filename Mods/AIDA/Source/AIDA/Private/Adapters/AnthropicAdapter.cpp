#include "Adapters/AnthropicAdapter.h"

#include "AIDA.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FAnthropicAdapter::FAnthropicAdapter(const FString& InBaseUrl, const FString& InApiKey)
	: BaseUrl(InBaseUrl)
	, ApiKey(InApiKey)
{
}

FString FAnthropicAdapter::BuildRequestBody(const FAIDACompletionRequest& Request)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), Request.Model);
	Root->SetNumberField(TEXT("max_tokens"), Request.MaxTokens);
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

void FAnthropicAdapter::Complete(const FAIDACompletionRequest& Request, FAIDAOnComplete OnComplete, FAIDAOnError OnError)
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
	HttpRequest->SetContentAsString(BuildRequestBody(Request));

	HttpRequest->OnProcessRequestComplete().BindLambda(
		[OnComplete = MoveTemp(OnComplete), OnError = MoveTemp(OnError)]
		(FHttpRequestPtr /*Req*/, FHttpResponsePtr Resp, bool bOk)
		{
			if (!bOk || !Resp.IsValid())
			{
				if (OnError) { OnError(0, TEXT("no response / connection failed")); }
				return;
			}

			const int32 Status = Resp->GetResponseCode();
			const FString Content = Resp->GetContentAsString();

			if (Status < 200 || Status >= 300)
			{
				// Body carries Anthropic's error object; callers log it (never the key).
				if (OnError) { OnError(Status, Content); }
				return;
			}

			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				if (OnError) { OnError(Status, TEXT("could not parse response JSON")); }
				return;
			}

			// { "content": [ { "type": "text", "text": "..." }, ... ] }
			FString Text;
			const TArray<TSharedPtr<FJsonValue>>* ContentArr = nullptr;
			if (Json->TryGetArrayField(TEXT("content"), ContentArr) && ContentArr)
			{
				for (const TSharedPtr<FJsonValue>& Item : *ContentArr)
				{
					const TSharedPtr<FJsonObject> Obj = Item.IsValid() ? Item->AsObject() : nullptr;
					if (!Obj.IsValid())
					{
						continue;
					}
					FString Type;
					if (Obj->TryGetStringField(TEXT("type"), Type) && Type == TEXT("text"))
					{
						FString Fragment;
						Obj->TryGetStringField(TEXT("text"), Fragment);
						Text += Fragment;
					}
				}
			}

			if (OnComplete) { OnComplete(Text); }
		});

	HttpRequest->ProcessRequest();
}
