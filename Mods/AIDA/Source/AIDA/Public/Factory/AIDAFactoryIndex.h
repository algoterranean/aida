#pragma once

#include "CoreMinimal.h"
#include "Factory/AIDAFactoryModel.h"

class UObject;

/**
 * The one game-header seam of Phase 2 "Eyes" (docs/PHASE2.md): walks the game's buildable subsystem
 * into the plain `FAIDAFactorySnapshot` the aggregator consumes. Server/authoritative worlds only —
 * the buildable subsystem is authoritative.
 *
 * Wrapped in a short TTL cache so a burst of tool calls in one request shares a single walk. Only the
 * .cpp includes FactoryGame headers; everything downstream (aggregator, tools) stays on plain structs
 * and is unit-testable. Extracts production machines (manufacturers, extractors, generators),
 * logistics nodes (splitters/mergers/pipe attachments), storage containers, power circuits, and — as
 * of P7 Slice 0 — the belt/pipe graph: segments collapse into node-to-node `Snapshot.Edges` via
 * AIDALogisticsGraph, with per-machine port-connectivity counts for dangling-port diagnostics.
 */
class FAIDAFactoryIndex
{
public:
	/**
	 * Return a snapshot no older than TtlSeconds, re-walking the world if the cache has expired.
	 * NowSeconds is the world time (World->GetTimeSeconds()); pass it in so this stays side-effect-free.
	 */
	const FAIDAFactorySnapshot& GetSnapshot(UObject* WorldContext, double NowSeconds, double TtlSeconds = 10.0);

	/** Force the next GetSnapshot to re-extract (e.g. after a known large build/dismantle). */
	void Invalidate() { bValid = false; }

	/** Walk the world's buildables into OutSnapshot right now. Clears OutSnapshot first. */
	static void ExtractInto(UObject* WorldContext, FAIDAFactorySnapshot& OutSnapshot);

private:
	FAIDAFactorySnapshot Cached;
	double LastExtractSeconds = 0.0;
	bool bValid = false;
};
