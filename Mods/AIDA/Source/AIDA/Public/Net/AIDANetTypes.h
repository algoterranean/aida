#pragma once

#include "CoreMinimal.h"
#include "AIDANetTypes.generated.h"

// Replicated chunk-protocol types (docs/ARCHITECTURE.md §5). Messages replicate as *events*
// (Begin / Chunk / End), never as one growing replicated string. These are the only AIDA
// structs that cross the wire to clients — they carry NOTHING provider-related (no key, no
// prompt), only player-visible transcript text.

/** The default conversation/tab id — used before multiple tabs exist, and by the AIDA.Say debug command. */
FORCEINLINE FGuid AIDADefaultConversationId() { return FGuid(0x1DA00001, 0x1DA00002, 0x1DA00003, 0x1DA00004); }

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
	/** Which conversation (tab) this message belongs to. Clients group the transcript by this into tabs. */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FGuid ConversationId;
	/** Display author — player name or "AIDA". Never an account id / secret. */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FString Author;
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") EAIDAMsgKind Kind = EAIDAMsgKind::Player;
	/** Reference images attached to this (player) message — clients render a 📎 marker only (Phase 5). */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") int32 ImageCount = 0;
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
	/** Server-side references into the Phase 5 image store (ids only — pixels never fan out). */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") TArray<FGuid> ImageIds;
};

/** One recipe's slice of a pending proposal's ghost preview (spec-v2 composites have several). */
USTRUCT(BlueprintType)
struct FAIDAGhostPart
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FString RecipeClassPath;
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") TArray<FVector> TileCenters; // world cm
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") float YawDeg = 0.f;
};

/**
 * One belt/pipe run of a pending proposal's ghost preview: a display-only spline hologram driven
 * from A to B (trunk hops between attachments + drops to machine ports). Normals are the port /
 * flow directions the executor will use, so the ghost belt curves the way the real one will.
 */
USTRUCT(BlueprintType)
struct FAIDAGhostRun
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FString RecipeClassPath; // the transport (belt/pipe)
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FVector FromCm = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FVector FromNormal = FVector::XAxisVector;
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FVector ToCm = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FVector ToNormal = FVector::XAxisVector;
};

/**
 * Client-facing view of one build/dismantle proposal (docs/PHASE4.md §2e). Replicated as a state
 * array on AAIDAProposalRelay so late joiners see pending proposals for free. Carries display
 * strings only — no spec JSON, no ids beyond the ProposalId the approve RPC echoes back.
 */
USTRUCT(BlueprintType)
struct FAIDAProposalView
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FGuid Id;
	/** Display name of the proposing player. */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FString Requester;
	/** Human diff line ("place 100 x Foundation 8m x 2m in a 10x10 grid"). */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FString Summary;
	/** "300 Concrete" (or the refund tally for a dismantle). */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FString CostSummary;
	/** Display state: "pending" | "approved" | "executing" | "executed" | "failed" | "rejected" | "expired" | "undone". */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") FString State;
	/** Unix time the pending proposal expires (0 once resolved). */
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") int64 ExpiresUtc = 0;

	//~ Ghost-preview payload (build proposals while pending; empty otherwise). Clients spawn local
	//~ holograms per part at each tile so players see EXACTLY where the build will land — one part
	//~ for v1 grids, several for spec-v2 composites.
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") TArray<FAIDAGhostPart> GhostParts;

	//~ Belt/pipe runs of manifold and connected-build proposals — previewed as display-only spline
	//~ holograms so the WHOLE build (machines + attachments + belts/pipes) is visible pre-approval.
	UPROPERTY(BlueprintReadOnly, Category = "AIDA") TArray<FAIDAGhostRun> GhostRuns;
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
