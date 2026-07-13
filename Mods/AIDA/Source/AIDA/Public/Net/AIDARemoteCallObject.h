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

	/** Client→server: submit a chat line. The only path player text takes into the orchestrator. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSendChat(const FString& Text);

	/** Client→server: fetch the authoritative full body of one message after a gap/hash mismatch. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestMessageBody(const FGuid& Id);

	/** Client→server: pull the recent transcript on login (late-join bulk sync). */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestRecentTranscript();

	/** Server→owning client: authoritative body for a single message. */
	UFUNCTION(Client, Reliable)
	void ClientReceiveMessageBody(FAIDATranscriptEntry Entry);

	/** Server→owning client: a batch of recent transcript entries (late-join). */
	UFUNCTION(Client, Reliable)
	void ClientReceiveTranscript(const TArray<FAIDATranscriptEntry>& Entries);

private:
	/** Required for the RCO's replication channel to open. */
	UPROPERTY(Replicated)
	int32 DummyReplicatedField = 0;
};
