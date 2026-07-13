#pragma once

#include "CoreMinimal.h"
#include "Net/AIDANetTypes.h"

class AAIDAChatRelay;

/**
 * Server-side owner of the shared chat transcript (docs/ARCHITECTURE.md §3.2 / §5).
 * Assigns message ids, keeps a bounded ring buffer of authoritative message bodies, and drives
 * the replicated fan-out through the relay. Plain C++ (no UObject) so the transcript/assembly
 * logic is unit-testable off the game thread.
 *
 * Authority only — the orchestrator (server) is the sole caller.
 */
class FAIDASessionManager
{
public:
	explicit FAIDASessionManager(int32 InMaxMessages = 200);

	/** The relay used for network fan-out. Weak: the actor's lifetime is the world's, not ours. */
	void SetRelay(AAIDAChatRelay* InRelay);

	/** Post a complete player line: stores it and fans out Begin + body + End. Returns its id. */
	FGuid PostPlayerMessage(const FString& Author, const FString& Text);

	/** Open a streaming AIDA reply (fans out Begin with an empty body). Returns its id. */
	FGuid BeginAIDAMessage(const FString& Author);

	/** Append a streamed delta to an open message (accumulates authoritatively + fans out batched). */
	void AppendDelta(const FGuid& Id, const FString& Delta);

	/** Close an open message: computes the full-text hash and fans out End. */
	void CompleteMessage(const FGuid& Id);

	/** Post a one-shot system notice (errors, rate-limit warnings) as its own message. */
	FGuid PostSystemMessage(const FString& Text);

	/** Authoritative full body for a message id. False if it has aged out of the ring buffer. */
	bool GetMessageBody(const FGuid& Id, FAIDATranscriptEntry& OutEntry) const;

	/** The recent transcript, oldest→newest (for late-join bulk sync). */
	void GetRecentTranscript(TArray<FAIDATranscriptEntry>& OutEntries) const;

	int32 Num() const { return Transcript.Num(); }

private:
	TWeakObjectPtr<AAIDAChatRelay> Relay;
	TArray<FAIDATranscriptEntry> Transcript; // bounded ring buffer, oldest→newest
	TMap<FGuid, int32> IndexById;            // id → index into Transcript
	int32 MaxMessages;

	void Store(const FAIDAMessageHeader& Header, const FString& Body);
	void Prune();
	FAIDATranscriptEntry* Find(const FGuid& Id);
	static uint32 HashBody(const FString& Body);
};
