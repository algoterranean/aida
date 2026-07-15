#pragma once

#include "CoreMinimal.h"
#include "Actions/AIDAActionTypes.h"

/**
 * The Phase 4 game-API seam (docs/PHASE4.md §3) — the second and last module (alongside the
 * Factory/Map extractors) allowed to include FactoryGame headers, and only in the .cpp. Static,
 * server-only functions over plain structs, mirroring FAIDAMapService::PlaceMapMarker: recipe
 * resolution, hologram-based dry-run validation (spawn → place → validate → destroy, fully
 * non-mutating), dismantle-target resolution, and central-storage affordability.
 *
 * Execution (Construct/Dismantle batches) lands in Slice 2; this file is the read/validate half.
 */
class FAIDAActionSeam
{
public:
	/**
	 * Resolve a display name ("Foundation 8m x 2m", "Smelter") to an UNLOCKED build-gun recipe —
	 * AIDA cannot build what the players haven't earned. Exact match wins, then unique substring;
	 * on failure returns false with close matches in Out.Suggestions. Footprint comes from the
	 * buildable CDO's clearance box (the default grid step).
	 */
	static bool ResolveBuildRecipe(UObject* WorldContext, const FString& DisplayName, FAIDARecipeResolution& Out);

	/**
	 * Validate every placement without mutating the world: spawn ONE hidden-from-clients
	 * (non-replicated) hologram, walk it across the placements (synthetic upward hit +
	 * ValidatePlacementAndCost), collect per-index disqualifiers, tally cost × count, check
	 * central-storage affordability, then destroy the hologram. Synchronous — placements are
	 * capped upstream by actions.maxProposalItems.
	 */
	static bool DryRunBuild(UObject* WorldContext, const FString& RecipeClassPath, int32 YawDeg,
		const TArray<FTransform>& Placements, FAIDADryRunResult& Out);

	/**
	 * Count live buildables matching the selector (display name + radius, full actors AND
	 * lightweight instances) and tally their dismantle refunds. Called at dry-run for the report
	 * and AGAIN at execute (Slice 2) — never from a cached FactoryIndex snapshot.
	 */
	static bool ResolveDismantleTargets(UObject* WorldContext, const FAIDADismantleSpec& Selector, FAIDADismantleResolution& Out);
};
