#include "Adapters/SSEStream.h"

#include "Async/Async.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"

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
	FAIDASSEDeltaExtractor Extract, FAIDAOnChunk OnChunk,
	FAIDAOnComplete OnComplete, FAIDAOnError OnError)
{
	// Shared across the worker-thread stream delegate and the game-thread completion callback.
	// Only the worker thread mutates it, and completion happens-after the final Serialize, so the
	// completion read sees the finished state without extra locking.
	struct FState
	{
		FAIDASSEBuffer Framing;
		FString Assembled;      // concatenation of all deltas — the OnComplete payload
		TArray<uint8> RawBody;  // full body kept verbatim for error reporting (errors aren't SSE)
	};
	const TSharedRef<FState, ESPMode::ThreadSafe> State = MakeShared<FState, ESPMode::ThreadSafe>();

	HttpRequest->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda(
		[State, Extract = MoveTemp(Extract), OnChunk](void* Ptr, int64& InOutLength)
		{
			// WORKER THREAD — never touch game-thread/UObject state here.
			if (Ptr && InOutLength > 0)
			{
				State->RawBody.Append(static_cast<const uint8*>(Ptr), static_cast<int32>(InOutLength));
			}

			State->Framing.Feed(Ptr, InOutLength, [&State, &Extract, &OnChunk](const FString& Line)
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

				const FString Delta = Extract(Data);
				if (Delta.IsEmpty())
				{
					return;
				}
				State->Assembled += Delta;
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

			AsyncTask(ENamedThreads::GameThread,
				[OnComplete, Full = State->Assembled]() { if (OnComplete) { OnComplete(Full); } });
		});
}
