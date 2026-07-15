#pragma once

#include "CoreMinimal.h"
#include "Memory/AIDAMemoryTypes.h"
#include "Factory/AIDAFactoryModel.h"

/**
 * Pure snapshot build + diff for the Phase 3 history tools (docs/PHASE3.md §3-4). Turns the factory
 * aggregates into a compact FAIDASnapshot and diffs a baseline snapshot against the current state — no
 * game headers, so it unit-tests on synthetic aggregates. SnapshotService feeds it live data; compare_to
 * serves the result.
 */
namespace AIDASnapshotTools
{
	/** Flatten the aggregates into a storable snapshot: per-item net balance + headline power totals. */
	FAIDASnapshot MakeSnapshot(const FAIDAFactoryAggregates& Aggregates, int64 TakenUtc, const FString& Label);

	/**
	 * Pick the baseline to compare against: with a timestamp, the newest snapshot at/or-before it (else
	 * the earliest); without one, the most recent snapshot. Snaps are oldest-first. Null if empty.
	 */
	const FAIDASnapshot* PickBaseline(const TArray<FAIDASnapshot>& Snaps, int64 Timestamp, bool bHasTimestamp);

	/**
	 * compare_to: diff Current against Baseline — per-item net deltas (largest change first) and the
	 * power delta. ItemFilter (optional substring) narrows to one item. NowUtc humanizes the baseline age.
	 */
	FString BuildCompareJson(const FAIDASnapshot& Baseline, const FAIDASnapshot& Current, const FString& ItemFilter, int64 NowUtc);
}
