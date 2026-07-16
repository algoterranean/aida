#pragma once

#include "CoreMinimal.h"
#include "FGRemoteCallObject.h"
#include "Net/AIDANetTypes.h"
#include "AIDARemoteCallObject.generated.h"

/**
 * Per-player client→server RPC surface (the idiomatic SML pattern, `Within = FGPlayerController`).
 * This is the single choke point where a player's chat enters the server; permission + rate-limit
 * checks happen behind it in the orchestrator. Recovery/late-join RPCs live here too (Slice 3).
 *
 * Carries a dummy replicated field only to open its replication channel — no AIDA state, no secrets.
 */
UCLASS()
class UAIDARemoteCallObject : public UFGRemoteCallObject
{
	GENERATED_BODY()

public:
	/** Bind/unbind the GameModeInitialized hook that registers this RCO on the Satisfactory game mode. */
	static void RegisterHooks();
	static void UnregisterHooks();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Client→server: submit a chat line to a conversation. The only path player text takes in. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSendChat(const FString& Text, const FGuid& ConversationId);

	/** Client→server: fetch the authoritative full body of one message after a gap/hash mismatch. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestMessageBody(const FGuid& Id);

	/** Client→server: pull the recent transcript on login (late-join bulk sync). */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestRecentTranscript();

	/**
	 * Client→server: approve/reject a pending proposal by id ONLY — never a spec; the server
	 * executes what IT stored (docs/PHASE4.md §1). Both gates (act tier + approvalPolicy) are
	 * enforced server-side in the orchestrator; the UI's buttons are cosmetic.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerApproveProposal(const FGuid& ProposalId);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRejectProposal(const FGuid& ProposalId);

	/**
	 * Client→server: nudge/rotate the newest pending proposal (the ghost-adjust keybinds). The
	 * server act-gates, re-validates the moved placements, and republishes — same path as the
	 * /aida nudge and /aida rotate chat commands.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerAdjustProposal(const FVector& DeltaCm, int32 YawDeltaDeg);

	//~ Phase 5 chunked reference-image upload (docs/PHASE5.md §3). One in-flight upload per player;
	//~ the client paces chunks off ClientImageUploadAck (small window — a reliable-RPC burst can
	//~ overflow the reliable bunch buffer and disconnect). Commit answers with ClientImageUploadResult.
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerBeginImageUpload(const FString& MediaType, int32 TotalBytes, int32 ChunkCount);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerImageUploadChunk(int32 Seq, const TArray<uint8>& Data);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerCommitImageUpload(uint32 Crc32);

	/** Client→server: chat line plus the ids of previously committed uploads to attach (Phase 5). */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSendChatWithImages(const FString& Text, const FGuid& ConversationId, const TArray<FGuid>& ImageIds);

	/** Server→owning client: authoritative body for a single message. */
	UFUNCTION(Client, Reliable)
	void ClientReceiveMessageBody(FAIDATranscriptEntry Entry);

	/** Server→owning client: a batch of recent transcript entries (late-join). */
	UFUNCTION(Client, Reliable)
	void ClientReceiveTranscript(const TArray<FAIDATranscriptEntry>& Entries);

	/** Server→owning client: chunks up to UpToSeq landed — send the next window (Phase 5). */
	UFUNCTION(Client, Reliable)
	void ClientImageUploadAck(int32 UpToSeq);

	/** Server→owning client: terminal upload outcome. bOk ⇒ ImageId is live in the server store. */
	UFUNCTION(Client, Reliable)
	void ClientImageUploadResult(bool bOk, const FGuid& ImageId, const FString& Error);

private:
	/** The calling player's display name + stable id (shared by the chat and proposal RPCs). */
	FAIDARequester ResolveRequester() const;

	/** Required for the RCO's replication channel to open. */
	UPROPERTY(Replicated)
	int32 DummyReplicatedField = 0;
};
