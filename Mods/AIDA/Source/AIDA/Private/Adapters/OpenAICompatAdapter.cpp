#include "Adapters/OpenAICompatAdapter.h"

#include "AIDA.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FOpenAICompatAdapter::FOpenAICompatAdapter(const FString& InBaseUrl, const FString& InApiKey)
	: BaseUrl(InBaseUrl)
	, ApiKey(InApiKey)
{
}

FString FOpenAICompatAdapter::BuildRequestBody(const FAIDACompletionRequest& Request)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), Request.Model);
	Root->SetNumberField(TEXT("max_tokens"), Request.MaxTokens);

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

void FOpenAICompatAdapter::Complete(const FAIDACompletionRequest& Request, FAIDAOnComplete OnComplete, FAIDAOnError OnError)
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

			// { "choices": [ { "message": { "role": "assistant", "content": "..." } } ] }
			FString Text;
			const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
			if (Json->TryGetArrayField(TEXT("choices"), Choices) && Choices && Choices->Num() > 0)
			{
				const TSharedPtr<FJsonObject> Choice = (*Choices)[0]->AsObject();
				if (Choice.IsValid())
				{
					const TSharedPtr<FJsonObject>* MsgObj = nullptr;
					if (Choice->TryGetObjectField(TEXT("message"), MsgObj) && MsgObj)
					{
						(*MsgObj)->TryGetStringField(TEXT("content"), Text);
					}
				}
			}

			if (OnComplete) { OnComplete(Text); }
		});

	HttpRequest->ProcessRequest();
}
