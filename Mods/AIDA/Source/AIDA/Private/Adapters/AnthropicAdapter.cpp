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
	/** Serialize one message to Anthropic wire form: a plain string, or a content-block array for tools. */
	TSharedRef<FJsonObject> BuildAnthropicMessage(const FAIDAChatMessage& Msg)
	{
		const TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("role"), Msg.Role);

		if (Msg.ToolResults.Num() > 0)
		{
			// A user turn carrying tool_result blocks answering the model's prior tool_use blocks.
			TArray<TSharedPtr<FJsonValue>> Blocks;
			for (const FAIDAToolResultPart& Part : Msg.ToolResults)
			{
				const TSharedRef<FJsonObject> B = MakeShared<FJsonObject>();
				B->SetStringField(TEXT("type"), TEXT("tool_result"));
				B->SetStringField(TEXT("tool_use_id"), Part.ToolCallId);
				B->SetStringField(TEXT("content"), Part.Content);
				if (Part.bIsError)
				{
					B->SetBoolField(TEXT("is_error"), true);
				}
				Blocks.Add(MakeShared<FJsonValueObject>(B));
			}
			M->SetArrayField(TEXT("content"), Blocks);
		}
		else if (Msg.ToolCalls.Num() > 0)
		{
			// An assistant turn: optional leading text, then one tool_use block per call.
			TArray<TSharedPtr<FJsonValue>> Blocks;
			if (!Msg.Content.IsEmpty())
			{
				const TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
				T->SetStringField(TEXT("type"), TEXT("text"));
				T->SetStringField(TEXT("text"), Msg.Content);
				Blocks.Add(MakeShared<FJsonValueObject>(T));
			}
			for (const FAIDAToolCall& Call : Msg.ToolCalls)
			{
				const TSharedRef<FJsonObject> U = MakeShared<FJsonObject>();
				U->SetStringField(TEXT("type"), TEXT("tool_use"));
				U->SetStringField(TEXT("id"), Call.Id);
				U->SetStringField(TEXT("name"), Call.Name);
				U->SetObjectField(TEXT("input"), AIDAParseObjectOrEmpty(Call.ArgsJson));
				Blocks.Add(MakeShared<FJsonValueObject>(U));
			}
			M->SetArrayField(TEXT("content"), Blocks);
		}
		else
		{
			M->SetStringField(TEXT("content"), Msg.Content);
		}
		return M;
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

	if (Request.Tools.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		for (const FAIDAToolDef& Def : Request.Tools)
		{
			const TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
			T->SetStringField(TEXT("name"), Def.Name);
			if (!Def.Description.IsEmpty())
			{
				T->SetStringField(TEXT("description"), Def.Description);
			}
			T->SetObjectField(TEXT("input_schema"), AIDAParseToolSchema(Def.InputSchemaJson));
			Tools.Add(MakeShared<FJsonValueObject>(T));
		}
		Root->SetArrayField(TEXT("tools"), Tools);
	}

	TArray<TSharedPtr<FJsonValue>> Messages;
	for (const FAIDAChatMessage& Msg : Request.Messages)
	{
		Messages.Add(MakeShared<FJsonValueObject>(BuildAnthropicMessage(Msg)));
	}
	Root->SetArrayField(TEXT("messages"), Messages);

	FString Body;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Root, Writer);
	return Body;
}

FString FAnthropicAdapter::HandleStreamEvent(const FString& DataJson, FAIDAStreamAccumulator& Acc)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DataJson);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return FString();
	}

	FString Type;
	if (!Json->TryGetStringField(TEXT("type"), Type))
	{
		return FString();
	}

	if (Type == TEXT("content_block_start"))
	{
		int32 Index = 0;
		Json->TryGetNumberField(TEXT("index"), Index);
		const TSharedPtr<FJsonObject>* Block = nullptr;
		if (Json->TryGetObjectField(TEXT("content_block"), Block) && Block)
		{
			FString BlockType;
			if ((*Block)->TryGetStringField(TEXT("type"), BlockType) && BlockType == TEXT("tool_use"))
			{
				FAIDAToolCall Call;
				(*Block)->TryGetStringField(TEXT("id"), Call.Id);
				(*Block)->TryGetStringField(TEXT("name"), Call.Name);
				Acc.PendingByIndex.Add(Index, MoveTemp(Call));
			}
		}
		return FString();
	}

	if (Type == TEXT("content_block_delta"))
	{
		const TSharedPtr<FJsonObject>* Delta = nullptr;
		if (!Json->TryGetObjectField(TEXT("delta"), Delta) || !Delta)
		{
			return FString();
		}
		FString DeltaType;
		(*Delta)->TryGetStringField(TEXT("type"), DeltaType);
		if (DeltaType == TEXT("text_delta"))
		{
			FString Text;
			(*Delta)->TryGetStringField(TEXT("text"), Text);
			return Text;
		}
		if (DeltaType == TEXT("input_json_delta"))
		{
			int32 Index = 0;
			Json->TryGetNumberField(TEXT("index"), Index);
			FString Partial;
			(*Delta)->TryGetStringField(TEXT("partial_json"), Partial);
			if (FAIDAToolCall* Pending = Acc.PendingByIndex.Find(Index))
			{
				Pending->ArgsJson += Partial;
			}
		}
		return FString();
	}

	if (Type == TEXT("content_block_stop"))
	{
		int32 Index = 0;
		Json->TryGetNumberField(TEXT("index"), Index);
		if (FAIDAToolCall* Pending = Acc.PendingByIndex.Find(Index))
		{
			Acc.ToolCalls.Add(*Pending);
			Acc.PendingByIndex.Remove(Index);
		}
		return FString();
	}

	if (Type == TEXT("message_delta"))
	{
		const TSharedPtr<FJsonObject>* Delta = nullptr;
		if (Json->TryGetObjectField(TEXT("delta"), Delta) && Delta)
		{
			FString StopReason;
			if ((*Delta)->TryGetStringField(TEXT("stop_reason"), StopReason) && !StopReason.IsEmpty())
			{
				Acc.StopReason = StopReason;
			}
		}
		return FString();
	}

	return FString();
}

void FAnthropicAdapter::Complete(const FAIDACompletionRequest& Request, FAIDAOnChunk OnChunk,
	FAIDAOnCompleteResult OnComplete, FAIDAOnError OnError)
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

	AIDADriveSSEStream(HttpRequest, FAIDASSEEventHandler(&FAnthropicAdapter::HandleStreamEvent),
		MoveTemp(OnChunk), MoveTemp(OnComplete), MoveTemp(OnError));

	HttpRequest->ProcessRequest();
}
