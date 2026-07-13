#pragma once

#include "CoreMinimal.h"
#include "AIDANetTypes.generated.h"

// Replicated chunk-protocol types (docs/ARCHITECTURE.md §5). Messages replicate as *events*
// (Begin / Chunk / End), never as one growing replicated string. These are the only AIDA
// structs that cross the wire to clients — they carry NOTHING provider-related (no key, no
// prompt), only player-visible transcript text.

/** Author category for a transcript message. */
UENUM(BlueprintType)
enum class EAIDAMsgKind : uint8
{
	/** A player's chat line. */
	Player,
	/** An AIDA (assistant) reply. */
	AIDA,
	/** A system notice (errors, rate-limit warnings, status lines). */
	System
};

/** Opens a message: assigns identity + author before any body text streams. */
USTRUCT(BlueprintType)
struct FAIDAMessageHeader
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FGuid Id;
	/** Display author — player name or "AIDA". Never an account id / secret. */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FString Author;
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") EAIDAMsgKind Kind = EAIDAMsgKind::Player;
};

/** One batched body fragment for a message already opened by its header. */
USTRUCT(BlueprintType)
struct FAIDAChunk
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FGuid Id;
	/** Monotonically increasing per message, starting at 0. Clients assemble by Seq and detect gaps. */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") int32 Seq = 0;
	/** UTF-8 text fragment (server-batched ~4–8 Hz, not per token). */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FString Delta;
};

/** A whole transcript message delivered in one shot (recovery / late-join bulk fetch). */
USTRUCT(BlueprintType)
struct FAIDATranscriptEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FAIDAMessageHeader Header;
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FString Body;
};

/**
 * Server-internal identity of a chat requester. Resolved in the Net/ layer (which owns the game
 * headers) and passed to the orchestrator so Core/ never touches FGPlayerController. Never replicated.
 */
struct FAIDARequester
{
	FString Author;    // display name shown in the transcript
	FString PlayerId;  // stable id for rate-limit buckets and the `act` allowlist (Slice 4)
};
