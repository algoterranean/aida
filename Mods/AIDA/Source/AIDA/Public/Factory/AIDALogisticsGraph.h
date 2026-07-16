#pragma once

#include "CoreMinimal.h"
#include "Factory/AIDAFactoryModel.h"

/**
 * Pure logistics-graph math for P7 Slice 0: the extractor turns belt/pipe actors into flat
 * segments, and these helpers collapse segment chains into node-to-node `FAIDAConveyorEdge`s and
 * attribute items. No game headers — the collapsing rules are unit-tested on synthetic graphs.
 */
namespace AIDALogisticsGraph
{
	/**
	 * One belt/pipe actor as the extractor saw it. Each end is EITHER another segment (belt-to-belt
	 * joins, pipe-to-pipe) OR a snapshot node (machine / logistics node / container) OR nothing.
	 */
	struct FSegment
	{
		int32 SegmentId = 0;      // unique, > 0
		int32 FromSegment = 0;    // upstream segment feeding this one (0 = none)
		int32 FromNode = 0;       // upstream snapshot node (0 = none)
		int32 ToSegment = 0;      // downstream segment this one feeds (0 = none)
		int32 ToNode = 0;         // downstream snapshot node (0 = none)
		double PerMinute = 0.0;   // this segment's carrying capacity
		bool bPipe = false;
		FString Item;             // known cargo (pipes report their fluid); first non-empty wins per chain
	};

	/**
	 * Collapse chains (A→belt→belt→B) into one edge per chain, rate = the slowest segment. Chain
	 * heads are segments not fed by another segment; edges keep 0 endpoints for dangling ends so
	 * diagnostics can see them. Segment cycles (belt loops) terminate and are dropped.
	 */
	TArray<FAIDAConveyorEdge> CollapseChains(const TArray<FSegment>& Segments);

	/**
	 * Fill empty edge items from the source machine's largest output (its "main" product). Edges
	 * from logistics-only nodes or containers stay empty — belt contents there aren't knowable
	 * from the snapshot alone.
	 */
	void AttributeItems(TArray<FAIDAConveyorEdge>& Edges, const TArray<FAIDAMachine>& Machines);
}
