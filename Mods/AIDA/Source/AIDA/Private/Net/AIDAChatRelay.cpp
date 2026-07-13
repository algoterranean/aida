#include "Net/AIDAChatRelay.h"

#include "AIDA.h"
#include "Net/AIDARemoteCallObject.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "FGPlayerController.h"
#include "TimerManager.h"
#include "Net/UnrealNetwork.h"

namespace
{
	constexpr float kFlushIntervalSeconds = 0.15f; // ~6–7 Hz batched fan-out (docs §5: 4–8 Hz)
}

AAIDAChatRelay::AAIDAChatRelay()
{
	// Server-authoritative, replicated to all clients (base AFGSubsystem already sets
	// bReplicates + bAlwaysRelevant). SpawnOnServer_Replicate makes SML spawn it on the server
	// and mirror it down.
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer_Replicate;
}

void AAIDAChatRelay::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAIDAChatRelay, ReplicationDummy);
}

void AAIDAChatRelay::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(FlushTimerHandle);
	}
	Super::EndPlay(EndPlayReason);
}

bool AAIDAChatRelay::bDedicated() const
{
	return GetNetMode() == NM_DedicatedServer;
}

//~ ─────────────────────────────── Server API ───────────────────────────────

void AAIDAChatRelay::ServerBeginMessage(const FAIDAMessageHeader& Header)
{
	if (!HasAuthority())
	{
		return;
	}
	NextSeqByMsg.Add(Header.Id, 0);
	Multicast_MsgBegin(Header);
}

void AAIDAChatRelay::ServerPushDelta(const FGuid& Id, const FString& Delta)
{
	if (!HasAuthority() || Delta.IsEmpty())
	{
		return;
	}
	PendingDeltas.FindOrAdd(Id).Append(Delta);
	EnsureFlushTimer();
}

void AAIDAChatRelay::ServerEndMessage(const FGuid& Id, uint32 FullTextHash)
{
	if (!HasAuthority())
	{
		return;
	}

	// Flush whatever is still pending for this message before closing it, so MsgEnd's hash
	// always covers text the client has already received.
	if (FString* Pending = PendingDeltas.Find(Id))
	{
		if (!Pending->IsEmpty())
		{
			FAIDAChunk Chunk;
			Chunk.Id = Id;
			Chunk.Seq = NextSeqByMsg.FindOrAdd(Id)++;
			Chunk.Delta = MoveTemp(*Pending);
			Multicast_MsgChunk(Chunk);
		}
		PendingDeltas.Remove(Id);
	}

	Multicast_MsgEnd(Id, FullTextHash);
	NextSeqByMsg.Remove(Id);
}

void AAIDAChatRelay::EnsureFlushTimer()
{
	if (!GetWorld() || GetWorld()->GetTimerManager().IsTimerActive(FlushTimerHandle))
	{
		return;
	}
	GetWorld()->GetTimerManager().SetTimer(
		FlushTimerHandle, this, &AAIDAChatRelay::FlushPending, kFlushIntervalSeconds, /*bLoop=*/true);
}

void AAIDAChatRelay::FlushPending()
{
	if (PendingDeltas.Num() == 0)
	{
		// Nothing to send — stop the timer until the next delta re-arms it.
		if (GetWorld())
		{
			GetWorld()->GetTimerManager().ClearTimer(FlushTimerHandle);
		}
		return;
	}

	for (auto It = PendingDeltas.CreateIterator(); It; ++It)
	{
		if (It->Value.IsEmpty())
		{
			It.RemoveCurrent();
			continue;
		}
		FAIDAChunk Chunk;
		Chunk.Id = It->Key;
		Chunk.Seq = NextSeqByMsg.FindOrAdd(It->Key)++;
		Chunk.Delta = MoveTemp(It->Value);
		Multicast_MsgChunk(Chunk);
		It.RemoveCurrent();
	}
}

//~ ─────────────────────────── Multicast handlers ───────────────────────────
// Run on server + all clients. We build the client-side view everywhere EXCEPT a dedicated
// server (no local player there — keeps rule 2 "headless safety" intact).

void AAIDAChatRelay::Multicast_MsgBegin_Implementation(const FAIDAMessageHeader& Header)
{
	if (bDedicated())
	{
		return;
	}

	FClientMessage& Msg = ClientMessages.FindOrAdd(Header.Id);
	Msg.Header = Header;
	Msg.NextSeq = 0;
	Msg.bComplete = false;
	if (!ClientOrder.Contains(Header.Id))
	{
		ClientOrder.Add(Header.Id);
	}

	UE_LOG(LogAIDA, Verbose, TEXT("[relay] MsgBegin id=%s author=%s kind=%d"),
		*Header.Id.ToString(), *Header.Author, static_cast<int32>(Header.Kind));

	OnMsgBegin.Broadcast(Header);
}

void AAIDAChatRelay::Multicast_MsgChunk_Implementation(const FAIDAChunk& Chunk)
{
	if (bDedicated())
	{
		return;
	}

	FClientMessage& Msg = ClientMessages.FindOrAdd(Chunk.Id);
	if (!ClientOrder.Contains(Chunk.Id))
	{
		// Chunk for a message whose Begin we never saw (late joiner). Flag for recovery (Slice 3).
		ClientOrder.Add(Chunk.Id);
		Msg.bGapDetected = true;
	}

	if (Chunk.Seq != Msg.NextSeq)
	{
		// Reliable multicast preserves order for anyone present at Begin, so a mismatch means a
		// gap (missed earlier chunks). Record and keep going; recovery reconciles the body.
		Msg.bGapDetected = true;
		UE_LOG(LogAIDA, Warning, TEXT("[relay] seq gap on %s: expected %d got %d"),
			*Chunk.Id.ToString(), Msg.NextSeq, Chunk.Seq);
	}
	Msg.NextSeq = Chunk.Seq + 1;
	Msg.Body.Append(Chunk.Delta);

	UE_LOG(LogAIDA, Verbose, TEXT("[relay] MsgChunk id=%s seq=%d delta=\"%s\""),
		*Chunk.Id.ToString(), Chunk.Seq, *Chunk.Delta);

	OnMsgChunk.Broadcast(Chunk.Id, Chunk.Delta);
}

void AAIDAChatRelay::Multicast_MsgEnd_Implementation(FGuid Id, uint32 FullTextHash)
{
	if (bDedicated())
	{
		return;
	}

	FClientMessage& Msg = ClientMessages.FindOrAdd(Id);
	Msg.bComplete = true;

	const uint32 LocalHash = FCrc::StrCrc32(*Msg.Body);
	if (LocalHash != FullTextHash || Msg.bGapDetected)
	{
		// Body diverged from the server's — refetch the authoritative full text.
		Msg.bGapDetected = true;
		UE_LOG(LogAIDA, Warning, TEXT("[relay] MsgEnd hash mismatch/gap on %s (local=%u server=%u) — requesting recovery"),
			*Id.ToString(), LocalHash, FullTextHash);
		RequestRecovery(Id);
	}
	else
	{
		UE_LOG(LogAIDA, Verbose, TEXT("[relay] MsgEnd id=%s ok (%d chars)"), *Id.ToString(), Msg.Body.Len());
	}

	OnMsgEnd.Broadcast(Id);
}

//~ ─────────────────────────────── Client view ──────────────────────────────

void AAIDAChatRelay::GetTranscript(TArray<FAIDATranscriptEntry>& OutEntries) const
{
	OutEntries.Reset(ClientOrder.Num());
	for (const FGuid& Id : ClientOrder)
	{
		if (const FClientMessage* Msg = ClientMessages.Find(Id))
		{
			FAIDATranscriptEntry Entry;
			Entry.Header = Msg->Header;
			Entry.Body = Msg->Body;
			OutEntries.Add(MoveTemp(Entry));
		}
	}
}

bool AAIDAChatRelay::GetMessage(const FGuid& Id, FAIDATranscriptEntry& OutEntry) const
{
	if (const FClientMessage* Msg = ClientMessages.Find(Id))
	{
		OutEntry.Header = Msg->Header;
		OutEntry.Body = Msg->Body;
		return true;
	}
	return false;
}

void AAIDAChatRelay::ClientApplyBody(const FAIDATranscriptEntry& Entry)
{
	FClientMessage& Msg = ClientMessages.FindOrAdd(Entry.Header.Id);
	Msg.Header = Entry.Header;
	Msg.Body = Entry.Body;
	Msg.bComplete = true;
	Msg.bGapDetected = false;
	Msg.NextSeq = MAX_int32; // authoritative; ignore any late in-flight chunks for this id
	if (!ClientOrder.Contains(Entry.Header.Id))
	{
		ClientOrder.Add(Entry.Header.Id);
	}

	UE_LOG(LogAIDA, Verbose, TEXT("[relay] recovered body id=%s (%d chars)"),
		*Entry.Header.Id.ToString(), Entry.Body.Len());

	OnMsgBegin.Broadcast(Msg.Header);
	OnMsgChunk.Broadcast(Entry.Header.Id, Entry.Body);
	OnMsgEnd.Broadcast(Entry.Header.Id);
}

void AAIDAChatRelay::ClientApplyTranscript(const TArray<FAIDATranscriptEntry>& Entries)
{
	for (const FAIDATranscriptEntry& Entry : Entries)
	{
		// Don't clobber a live in-progress local message with a stale snapshot.
		if (const FClientMessage* Existing = ClientMessages.Find(Entry.Header.Id))
		{
			if (!Existing->bComplete)
			{
				continue;
			}
		}
		ClientApplyBody(Entry);
	}
}

//~ ─────────────────────────── Client → server helpers ──────────────────────

UAIDARemoteCallObject* AAIDAChatRelay::GetLocalRCO() const
{
	// The local player's controller: null on a dedicated server (no local player) — keeps rule 2 safe.
	const UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (AFGPlayerController* FGPC = Cast<AFGPlayerController>(PC))
	{
		return FGPC->GetRemoteCallObjectOfClass<UAIDARemoteCallObject>();
	}
	return nullptr;
}

void AAIDAChatRelay::RequestRecovery(const FGuid& Id)
{
	// The server already holds the authoritative text; only remote clients need to refetch.
	if (HasAuthority())
	{
		return;
	}
	if (UAIDARemoteCallObject* RCO = GetLocalRCO())
	{
		RCO->ServerRequestMessageBody(Id);
	}
}

void AAIDAChatRelay::SubmitChat(const FString& Text)
{
	if (Text.TrimStartAndEnd().IsEmpty())
	{
		return;
	}
	if (UAIDARemoteCallObject* RCO = GetLocalRCO())
	{
		RCO->ServerSendChat(Text);
	}
	else
	{
		UE_LOG(LogAIDA, Warning, TEXT("[relay] SubmitChat: no local RCO available."));
	}
}

void AAIDAChatRelay::RequestRecentTranscript()
{
	if (UAIDARemoteCallObject* RCO = GetLocalRCO())
	{
		RCO->ServerRequestRecentTranscript();
	}
}
