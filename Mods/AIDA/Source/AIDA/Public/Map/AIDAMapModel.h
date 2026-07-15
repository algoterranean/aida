#pragma once

#include "CoreMinimal.h"

/**
 * Plain-data map model for Phase 2 "Eyes" Slice 3 (docs/PHASE2.md). Like the factory model, the game
 * seam (AIDAMapService) fills these and the pure tools serialize them, so the grouping/filtering logic
 * stays unit-testable.
 */

/** A resource node on the map: what it is, how rich, whether something is already mining it, and where. */
struct FAIDAResourceNode
{
	FString Resource;                  // resource display name ("Iron Ore", "Crude Oil", ...)
	FString Purity;                    // "Impure" | "Normal" | "Pure"
	bool bOccupied = false;            // an extractor is built on it
	FVector Location = FVector::ZeroVector;
	FIntPoint Grid = FIntPoint::ZeroValue; // world-grid cell (humanized location fallback)
	FString Region;                    // map-area display name ("Grass Fields", ...) — empty if unavailable
};
