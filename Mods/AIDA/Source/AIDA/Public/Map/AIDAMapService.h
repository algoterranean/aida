#pragma once

#include "CoreMinimal.h"
#include "Map/AIDAMapModel.h"

class UObject;

/**
 * Game-header seam for map data (docs/PHASE2.md Slice 3): iterates the world's resource nodes into
 * plain `FAIDAResourceNode`s (resource, purity, occupancy, location + world-grid cell). Only the .cpp
 * touches FactoryGame headers; the pure tools serialize the result. Wrapped in a longer TTL cache than
 * the factory index — nodes are static and only their occupancy changes on build/dismantle.
 */
class FAIDAMapService
{
public:
	/** Cached node list, re-scanned if older than TtlSeconds. NowSeconds is world time. */
	const TArray<FAIDAResourceNode>& GetNodes(UObject* WorldContext, double NowSeconds, double TtlSeconds = 30.0);

	/** Force a re-scan on the next GetNodes (e.g. after a miner is built/removed). */
	void Invalidate() { bValid = false; }

	/** Scan the world's resource nodes into OutNodes right now. Clears OutNodes first. */
	static void ExtractNodesInto(UObject* WorldContext, TArray<FAIDAResourceNode>& OutNodes);

private:
	TArray<FAIDAResourceNode> Cached;
	double LastExtractSeconds = 0.0;
	bool bValid = false;
};
