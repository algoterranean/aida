#pragma once

#include "CoreMinimal.h"
#include "Subsystem/ModSubsystem.h"
#include "Net/AIDANetTypes.h"
#include "AIDAChatRelay.generated.h"

// Delegates the (Blueprint-authored) ChatWidget binds to. Client-side view events only.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAIDAOnMsgBeginDelegate, const FAIDAMessageHeader&, Header);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAIDAOnMsgChunkDelegate, const FGuid&, Id, const FString&, Delta);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAIDAOnMsgEndDelegate, const FGuid&, Id);

/**
 * Server-authoritative, always-relevant replicated subsystem actor (SpawnOnServer_Replicate).
 * The server→all-clients egress for the streaming chunk protocol (docs/ARCHITECTURE.md §5):
 * server batches text deltas and multicasts Begin/Chunk/End; clients assemble by Seq and expose
 * a read-only transcript + delegates for the ChatWidget.
 *
 * Business logic (permissions, rate limits, LLM calls) lives in the orchestrator; this actor is
 * purely the reliable fan-out + client-side reassembly seam.
 */
UCLASS()
class AAIDAChatRelay : public AModSubsystem
{
	GENERATED_BODY()

public:
	AAIDAChatRelay();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	//~ Server API (authority only) — driven by the orchestrator/session manager.
	/** Opens a message on all clients. Server-side; safe no-op without authority. */
	void ServerBeginMessage(const FAIDAMessageHeader& Header);
	/** Queues a body delta for a message; batched and flushed on a timer (~4–8 Hz). */
	void ServerPushDelta(const FGuid& Id, const FString& Delta);
	/** Flushes any pending delta then closes the message with a full-text hash for client verification. */
	void ServerEndMessage(const FGuid& Id, uint32 FullTextHash);
	//~ End server API

	//~ Client view (available everywhere the relay replicates; empty on dedicated server).
	/** Ordered transcript for rendering. */
	UFUNCTION(BlueprintCallable, Category = "AIDA")
	void GetTranscript(TArray<FAIDATranscriptEntry>& OutEntries) const;

	/** Look up one assembled message by id. Returns false if unknown to this client. */
	UFUNCTION(BlueprintCallable, Category = "AIDA")
	bool GetMessage(const FGuid& Id, FAIDATranscriptEntry& OutEntry) const;

	/** Client-side: overwrite a message with the authoritative body from a recovery fetch. */
	void ClientApplyBody(const FAIDATranscriptEntry& Entry);

	/** Client-side: seed the transcript from a late-join bulk fetch. */
	void ClientApplyTranscript(const TArray<FAIDATranscriptEntry>& Entries);

	/** Client→server: submit a chat line via the local player's RCO (the ChatWidget's send path). */
	UFUNCTION(BlueprintCallable, Category = "AIDA")
	void SubmitChat(const FString& Text, const FGuid& ConversationId);

	/** Client→server: request the recent transcript (call on widget construct / late join). */
	UFUNCTION(BlueprintCallable, Category = "AIDA")
	void RequestRecentTranscript();
	//~ End client view

	/** Fired on the client when a message header first arrives. */
	UPROPERTY(BlueprintAssignable, Category = "AIDA")
	FAIDAOnMsgBeginDelegate OnMsgBegin;

	/** Fired on the client for each assembled delta (already appended to the message body). */
	UPROPERTY(BlueprintAssignable, Category = "AIDA")
	FAIDAOnMsgChunkDelegate OnMsgChunk;

	/** Fired on the client when a message completes (hash verified, or after recovery). */
	UPROPERTY(BlueprintAssignable, Category = "AIDA")
	FAIDAOnMsgEndDelegate OnMsgEnd;

	//~ Replicated RPCs (reliable). Public so UHT can generate them; call only via the Server* API.
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_MsgBegin(const FAIDAMessageHeader& Header);

	UFUNCTION(NetMulticast, Reliable)
	void Multicast_MsgChunk(const FAIDAChunk& Chunk);

	UFUNCTION(NetMulticast, Reliable)
	void Multicast_MsgEnd(FGuid Id, uint32 FullTextHash);

private:
	/** Opens the RCO's replication channel; required for multicast fan-out to reach clients. */
	UPROPERTY(Replicated)
	int32 ReplicationDummy = 0;

	//~ Server-only batching state.
	TMap<FGuid, FString> PendingDeltas;   // accumulated, not yet flushed
	TMap<FGuid, int32> NextSeqByMsg;      // next Seq to emit per open message
	FTimerHandle FlushTimerHandle;

	void EnsureFlushTimer();
	void FlushPending();
	bool bDedicated() const;

	//~ Client-only helpers (safe no-ops on a dedicated server; no local player there).
	/** The local player's AIDA RCO, or null (e.g. on a dedicated server). */
	class UAIDARemoteCallObject* GetLocalRCO() const;
	/** Ask the server for the authoritative body after a gap / hash mismatch. */
	void RequestRecovery(const FGuid& Id);

	//~ Client-only assembly state.
	struct FClientMessage
	{
		FAIDAMessageHeader Header;
		FString Body;
		int32 NextSeq = 0;    // next Seq expected
		bool bComplete = false;
		bool bGapDetected = false;
	};
	TMap<FGuid, FClientMessage> ClientMessages;
	TArray<FGuid> ClientOrder;
};
