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
	/** Append one message to the OpenAI `messages` array (a tool-result turn expands to N `tool` msgs). */
	void AppendOpenAIMessage(const FAIDAChatMessage& Msg, TArray<TSharedPtr<FJsonValue>>& OutMessages)
	{
		if (Msg.ToolResults.Num() > 0)
		{
			// OpenAI represents each tool result as its own `role:"tool"` message keyed by tool_call_id.
			for (const FAIDAToolResultPart& Part : Msg.ToolResults)
			{
				const TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
				M->SetStringField(TEXT("role"), TEXT("tool"));
				M->SetStringField(TEXT("tool_call_id"), Part.ToolCallId);
				M->SetStringField(TEXT("content"), Part.Content);
				OutMessages.Add(MakeShared<FJsonValueObject>(M));
			}
			return;
		}

		const TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("role"), Msg.Role);
		if (Msg.Images.Num() > 0)
		{
			// Multimodal user turn: content becomes a part array (image_url data URLs, then text).
			TArray<TSharedPtr<FJsonValue>> Parts;
			for (const FAIDAImagePart& Img : Msg.Images)
			{
				const TSharedRef<FJsonObject> Url = MakeShared<FJsonObject>();
				Url->SetStringField(TEXT("url"),
					FString::Printf(TEXT("data:%s;base64,%s"), *Img.MediaType, *Img.Base64Data));
				const TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("type"), TEXT("image_url"));
				P->SetObjectField(TEXT("image_url"), Url);
				Parts.Add(MakeShared<FJsonValueObject>(P));
			}
			if (!Msg.Content.IsEmpty())
			{
				const TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
				P->SetStringField(TEXT("type"), TEXT("text"));
				P->SetStringField(TEXT("text"), Msg.Content);
				Parts.Add(MakeShared<FJsonValueObject>(P));
			}
			M->SetArrayField(TEXT("content"), Parts);
		}
		else
		{
			M->SetStringField(TEXT("content"), Msg.Content);
		}
		if (Msg.ToolCalls.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Calls;
			for (const FAIDAToolCall& Call : Msg.ToolCalls)
			{
				const TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
				C->SetStringField(TEXT("id"), Call.Id);
				C->SetStringField(TEXT("type"), TEXT("function"));
				const TSharedRef<FJsonObject> Fn = MakeShared<FJsonObject>();
				Fn->SetStringField(TEXT("name"), Call.Name);
				Fn->SetStringField(TEXT("arguments"), Call.ArgsJson);
				C->SetObjectField(TEXT("function"), Fn);
				Calls.Add(MakeShared<FJsonValueObject>(C));
			}
			M->SetArrayField(TEXT("tool_calls"), Calls);
		}
		OutMessages.Add(MakeShared<FJsonValueObject>(M));
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

	if (Request.Tools.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		for (const FAIDAToolDef& Def : Request.Tools)
		{
			const TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
			T->SetStringField(TEXT("type"), TEXT("function"));
			const TSharedRef<FJsonObject> Fn = MakeShared<FJsonObject>();
			Fn->SetStringField(TEXT("name"), Def.Name);
			if (!Def.Description.IsEmpty())
			{
				Fn->SetStringField(TEXT("description"), Def.Description);
			}
			Fn->SetObjectField(TEXT("parameters"), AIDAParseToolSchema(Def.InputSchemaJson));
			T->SetObjectField(TEXT("function"), Fn);
			Tools.Add(MakeShared<FJsonValueObject>(T));
		}
		Root->SetArrayField(TEXT("tools"), Tools);
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
		AppendOpenAIMessage(Msg, Messages);
	}
	Root->SetArrayField(TEXT("messages"), Messages);

	FString Body;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Root, Writer);
	return Body;
}

FString FOpenAICompatAdapter::HandleStreamEvent(const FString& DataJson, FAIDAStreamAccumulator& Acc)
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

	// A non-empty finish_reason ends the turn — finalize any tool calls accumulated so far.
	FString Finish;
	if (Choice->TryGetStringField(TEXT("finish_reason"), Finish) && !Finish.IsEmpty())
	{
		Acc.StopReason = Finish;
		if (Acc.PendingByIndex.Num() > 0)
		{
			TArray<int32> Indices;
			Acc.PendingByIndex.GetKeys(Indices);
			Indices.Sort();
			for (int32 Index : Indices)
			{
				Acc.ToolCalls.Add(Acc.PendingByIndex[Index]);
			}
			Acc.PendingByIndex.Reset();
		}
	}

	const TSharedPtr<FJsonObject>* Delta = nullptr;
	if (!Choice->TryGetObjectField(TEXT("delta"), Delta) || !Delta)
	{
		return FString();
	}

	// Streamed tool calls: the first fragment for an index carries id + function.name; later fragments
	// append function.arguments. Accumulate by index until a finish_reason flushes them.
	const TArray<TSharedPtr<FJsonValue>>* ToolCalls = nullptr;
	if ((*Delta)->TryGetArrayField(TEXT("tool_calls"), ToolCalls) && ToolCalls)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ToolCalls)
		{
			const TSharedPtr<FJsonObject> TC = Value->AsObject();
			if (!TC.IsValid())
			{
				continue;
			}
			int32 Index = 0;
			TC->TryGetNumberField(TEXT("index"), Index);
			FAIDAToolCall& Pending = Acc.PendingByIndex.FindOrAdd(Index);

			FString Id;
			if (TC->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
			{
				Pending.Id = Id;
			}
			const TSharedPtr<FJsonObject>* Fn = nullptr;
			if (TC->TryGetObjectField(TEXT("function"), Fn) && Fn)
			{
				FString Name;
				if ((*Fn)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
				{
					Pending.Name = Name;
				}
				FString ArgsFragment;
				if ((*Fn)->TryGetStringField(TEXT("arguments"), ArgsFragment))
				{
					Pending.ArgsJson += ArgsFragment;
				}
			}
		}
	}

	FString Content;
	(*Delta)->TryGetStringField(TEXT("content"), Content);
	return Content;
}

void FOpenAICompatAdapter::Complete(const FAIDACompletionRequest& Request, FAIDAOnChunk OnChunk,
	FAIDAOnCompleteResult OnComplete, FAIDAOnError OnError)
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

	AIDADriveSSEStream(HttpRequest, FAIDASSEEventHandler(&FOpenAICompatAdapter::HandleStreamEvent),
		MoveTemp(OnChunk), MoveTemp(OnComplete), MoveTemp(OnError));

	HttpRequest->ProcessRequest();
}
