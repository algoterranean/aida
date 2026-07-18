#pragma once

#include "CoreMinimal.h"
#include "AIDAMemoryTypes.generated.h"

/**
 * Phase 3 "Memory" data schemas (docs/PHASE3.md §2). The in-save structs carry UPROPERTY(SaveGame)
 * fields so the game's save archive (which serializes only SaveGame-flagged properties, recursing into
 * struct members) persists them inside AAIDAMemoryStore. Timestamps are Unix seconds (int64) — portable,
 * sortable, and free of FDateTime serialization quirks. FAIDASnapshot is a plain struct: it lives in the
 * sidecar as JSON, not in the save.
 */

/** A location-tagged player annotation ("what the factory is for"). In-save. */
USTRUCT()
struct FAIDANote
{
	GENERATED_BODY()

	UPROPERTY(SaveGame) FGuid Id;
	UPROPERTY(SaveGame) FString Author;               // display name at time of writing
	UPROPERTY(SaveGame) FString AuthorId;             // stable player id (may be empty for a solo host)
	UPROPERTY(SaveGame) FString Text;
	UPROPERTY(SaveGame) FVector Location = FVector::ZeroVector;
	UPROPERTY(SaveGame) FString Region;               // map-area name when resolvable
	UPROPERTY(SaveGame) int64 CreatedUtc = 0;
	UPROPERTY(SaveGame) TArray<FString> Tags;
};

/** A record of a marker AIDA placed via tag_node, so it can later list/clear its own markers. In-save. */
USTRUCT()
struct FAIDAMarkerRecord
{
	GENERATED_BODY()

	UPROPERTY(SaveGame) FGuid Id;
	UPROPERTY(SaveGame) FString Label;
	UPROPERTY(SaveGame) FString Resource;
	UPROPERTY(SaveGame) FVector Location = FVector::ZeroVector;
	UPROPERTY(SaveGame) int64 CreatedUtc = 0;
};

/**
 * An executed AI action, for the undo journal — the ActionEngine appends one per executed proposal
 * (docs/PHASE4.md §2d). AffectedEntityIds hold AIDAActionSpec-encoded entity handles; undo re-resolves
 * them since the game has no persistent per-buildable ids. In-save.
 */
USTRUCT()
struct FAIDAJournalEntry
{
	GENERATED_BODY()

	UPROPERTY(SaveGame) FGuid Id;
	UPROPERTY(SaveGame) FString ProposalSpecJson;
	UPROPERTY(SaveGame) FString RequesterId;
	UPROPERTY(SaveGame) FString ApproverId;
	UPROPERTY(SaveGame) int64 ProposedUtc = 0;
	UPROPERTY(SaveGame) int64 ExecutedUtc = 0;
	UPROPERTY(SaveGame) TArray<FString> AffectedEntityIds;
	UPROPERTY(SaveGame) FString RefundJson;
	// Phase 4 additions — tagged-property serialization defaults them on saves that predate the field.
	UPROPERTY(SaveGame) bool bDismantle = false;
	UPROPERTY(SaveGame) bool bUndone = false;
	/** P8 Slice 2: non-empty = a mutation entry — {kind, changes:[{id, before, after}]}; undo
	 *  restores the before values instead of dismantling/rebuilding. */
	UPROPERTY(SaveGame) FString MutationJson;
};

/**
 * A standing task (P8 Slice 5): a HUMAN-created recurring check, run through the tool loop with
 * Query-tier tools only — it can look but never mutate; findings are reported, a human decides.
 * Persisted in-save so checks survive sessions.
 */
USTRUCT()
struct FAIDAStandingTask
{
	GENERATED_BODY()

	UPROPERTY(SaveGame) FGuid Id;
	UPROPERTY(SaveGame) FString Prompt;
	UPROPERTY(SaveGame) int32 IntervalMinutes = 10;
	UPROPERTY(SaveGame) FString CreatedById;     // runs use the creator's identity
	UPROPERTY(SaveGame) FString CreatedByName;
	UPROPERTY(SaveGame) bool bEnabled = true;
	UPROPERTY(SaveGame) int64 LastRunUtc = 0;
	UPROPERTY(SaveGame) FString LastResult;      // last reply ("OK" = quiet)
};

/** One item's net production−consumption at snapshot time. */
struct FAIDASnapshotItem
{
	FString Item;
	double Net = 0.0;
};

/**
 * A timestamped aggregate of the factory ("what changed"). Sidecar-only (JSON), ring-buffered. Slice 0
 * carries the per-item balance + headline power; Slice 2 fills it from FAIDAFactoryAggregates and powers
 * compare_to. Plain struct — serialized by FAIDASidecarStore, never by the game save.
 */
struct FAIDASnapshot
{
	int64 TakenUtc = 0;
	FString Label;
	TArray<FAIDASnapshotItem> ItemBalance;
	double PowerConsumedMW = 0.0;
	double PowerCapacityMW = 0.0;
};
