#include "Adapters/SSEStream.h"

#include "Async/Async.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

TSharedRef<FJsonObject> AIDAParseObjectOrEmpty(const FString& JsonStr)
{
	const FString Trimmed = JsonStr.TrimStartAndEnd();
	if (!Trimmed.IsEmpty())
	{
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
		{
			return Parsed.ToSharedRef();
		}
	}
	return MakeShared<FJsonObject>();
}

void FAIDASSEBuffer::Feed(const void* Ptr, int64 Len, const TFunctionRef<void(const FString&)>& OnLine)
{
	if (Ptr && Len > 0)
	{
		Pending.Append(static_cast<const uint8*>(Ptr), static_cast<int32>(Len));
	}

	// Emit every complete line currently buffered, leaving the unterminated remainder in Pending.
	int32 LineStart = 0;
	for (int32 i = 0; i < Pending.Num(); ++i)
	{
		if (Pending[i] != '\n')
		{
			continue;
		}

		int32 LineEnd = i; // exclusive; drop the '\n'
		if (LineEnd > LineStart && Pending[LineEnd - 1] == '\r')
		{
			--LineEnd; // also drop a CR so CRLF and LF behave identically
		}

		const int32 ByteCount = LineEnd - LineStart;
		if (ByteCount > 0)
		{
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Pending.GetData() + LineStart), ByteCount);
			OnLine(FString(Converted.Length(), Converted.Get()));
		}
		else
		{
			OnLine(FString());
		}

		LineStart = i + 1;
	}

	if (LineStart > 0)
	{
		Pending.RemoveAt(0, LineStart, EAllowShrinking::No);
	}
}

namespace
{
	FString DecodeUtf8(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() == 0)
		{
			return FString();
		}
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}
}

void AIDADriveSSEStream(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& HttpRequest,
	FAIDASSEEventHandler Handle, FAIDAOnChunk OnChunk,
	FAIDAOnCompleteResult OnComplete, FAIDAOnError OnError)
{
	// Shared across the worker-thread stream delegate and the game-thread completion callback.
	// Only the worker thread mutates it, and completion happens-after the final Serialize, so the
	// completion read sees the finished state without extra locking.
	struct FState
	{
		FAIDASSEBuffer Framing;
		FAIDAStreamAccumulator Acc; // assembled text + tool calls + stop reason — the OnComplete payload
		TArray<uint8> RawBody;      // full body kept verbatim for error reporting (errors aren't SSE)
	};
	const TSharedRef<FState, ESPMode::ThreadSafe> State = MakeShared<FState, ESPMode::ThreadSafe>();

	HttpRequest->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda(
		[State, Handle = MoveTemp(Handle), OnChunk](void* Ptr, int64& InOutLength)
		{
			// WORKER THREAD — never touch game-thread/UObject state here.
			if (Ptr && InOutLength > 0)
			{
				State->RawBody.Append(static_cast<const uint8*>(Ptr), static_cast<int32>(InOutLength));
			}

			State->Framing.Feed(Ptr, InOutLength, [&State, &Handle, &OnChunk](const FString& Line)
			{
				if (!Line.StartsWith(TEXT("data:")))
				{
					return; // event: / id: / retry: / blank lines carry nothing we assemble
				}
				FString Data = Line.RightChop(5); // strip "data:"
				Data.TrimStartInline();
				if (Data.IsEmpty() || Data == TEXT("[DONE]"))
				{
					return; // OpenAI's terminator; Anthropic signals done via message_stop (no text)
				}

				const FString Delta = Handle(Data, State->Acc);
				if (Delta.IsEmpty())
				{
					return;
				}
				State->Acc.Text += Delta;
				if (OnChunk)
				{
					AsyncTask(ENamedThreads::GameThread, [OnChunk, Delta]() { OnChunk(Delta); });
				}
			});

			// Accept everything; leave InOutLength untouched (0 would signal a serialize failure).
		}));

	HttpRequest->OnProcessRequestComplete().BindLambda(
		[State, OnComplete = MoveTemp(OnComplete), OnError = MoveTemp(OnError)]
		(FHttpRequestPtr /*Req*/, FHttpResponsePtr Resp, bool bOk)
		{
			// GAME THREAD. Dispatch the terminal callback through the game-thread queue so it lands
			// after any chunk tasks still queued ahead of it — OnComplete never precedes its deltas.
			if (!bOk || !Resp.IsValid())
			{
				AsyncTask(ENamedThreads::GameThread,
					[OnError]() { if (OnError) { OnError(0, TEXT("no response / connection failed")); } });
				return;
			}

			const int32 Status = Resp->GetResponseCode();
			if (Status < 200 || Status >= 300)
			{
				FString Body = DecodeUtf8(State->RawBody);
				if (Body.IsEmpty()) { Body = TEXT("(no response body)"); }
				AsyncTask(ENamedThreads::GameThread,
					[OnError, Status, Body = MoveTemp(Body)]() { if (OnError) { OnError(Status, Body); } });
				return;
			}

			// Flush any tool calls still accumulating (a truncated stream, or a provider that finalizes
			// only at end). Handlers normally finalize their own; this is a belt-and-suspenders backstop.
			// Emit in ascending block/tool index so the order is deterministic.
			FAIDACompletionResult Result;
			Result.Text = State->Acc.Text;
			Result.StopReason = State->Acc.StopReason;
			Result.ToolCalls = State->Acc.ToolCalls;
			if (State->Acc.PendingByIndex.Num() > 0)
			{
				TArray<int32> Indices;
				State->Acc.PendingByIndex.GetKeys(Indices);
				Indices.Sort();
				for (int32 Index : Indices)
				{
					Result.ToolCalls.Add(State->Acc.PendingByIndex[Index]);
				}
			}

			AsyncTask(ENamedThreads::GameThread,
				[OnComplete, Result = MoveTemp(Result)]() { if (OnComplete) { OnComplete(Result); } });
		});
}
