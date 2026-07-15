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
 * An executed AI action, for the undo journal. Schema is defined now (Phase 3) but only written in
 * Phase 4 "Hands" — the ActionEngine appends one per executed proposal. In-save.
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
