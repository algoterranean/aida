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
 * Slice 1 is the read/validate half; Slice 2 adds the execute half (cost deduction, batched
 * Construct/Dismantle, refunds). All world mutation stays behind player approval upstream.
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
	 * Where the requesting player is AIMING: their viewpoint traced along the camera direction on
	 * the game's own build-gun channel (TC_BuildGun). The default origin/center for specs that omit
	 * one — matches how players think ("build it there", not "at my feet"). World units (cm).
	 * False when the player can't be resolved or the aim hits nothing in range.
	 */
	static bool ResolveAimPoint(UObject* WorldContext, const FString& PlayerId, FVector& OutPointCm);

	/**
	 * The aim point, SNAPPED the way the build gun would place this recipe there: a probe hologram
	 * is placed at the player's aim hit (running the game's TrySnapToActor / world-grid snapping —
	 * lightweight-instance hits are first resolved to a temporary buildable so foundations actually
	 * snap) and its resulting position anchors the grid. InOutYawDeg goes in as the spec's yaw and
	 * comes back as the probe's SNAPPED yaw, so the whole grid shares the aimed structure's lattice.
	 * The grid geometry (counts + effective steps, cm) chooses the anchor corner: aiming past a tile
	 * edge grows the grid OUTWARD on that side (one cell clear of an aimed structure); an ambiguous
	 * axis centers on the aim. Falls back to the raw aim point.
	 */
	static bool ResolveAimSnappedOrigin(UObject* WorldContext, const FString& PlayerId,
		const FString& RecipeClassPath, int32& InOutYawDeg,
		int32 CountX, int32 CountY, double StepXCm, double StepYCm, FVector& OutOriginCm);

	/**
	 * Ground height (cm) under a point, via the same build-gun-channel trace placements use.
	 * Feeds followTerrain specs: placement Z is pre-adjusted per tile at propose time.
	 */
	static bool ProbeGroundZ(UObject* WorldContext, const FVector& AtCm, double& OutGroundZCm);

	/**
	 * Validate every placement without mutating the world: spawn ONE hidden-from-clients
	 * (non-replicated) hologram, walk it across the placements (synthetic upward hit +
	 * ValidatePlacementAndCost), collect per-index disqualifiers, tally cost × count, check
	 * central-storage affordability, then destroy the hologram. Synchronous — placements are
	 * capped upstream by actions.maxProposalItems.
	 */
	static bool DryRunBuild(UObject* WorldContext, const FString& RecipeClassPath,
		const TArray<FTransform>& Placements, FAIDADryRunResult& Out);

	/**
	 * Count live buildables matching the selector (display name + radius, full actors AND
	 * lightweight instances) and tally their dismantle refunds. Called at dry-run for the report
	 * and AGAIN at execute (Slice 2) — never from a cached FactoryIndex snapshot.
	 */
	static bool ResolveDismantleTargets(UObject* WorldContext, const FAIDADismantleSpec& Selector, FAIDADismantleResolution& Out);

	/**
	 * Deduct a tallied cost upfront from central storage (docs/PHASE4.md §3 — one failure point;
	 * a mid-run change can't strand a half-built grid). Re-verifies every line first; deducts only
	 * when the whole tally is payable. False = nothing was deducted.
	 */
	static bool DeductCost(UObject* WorldContext, const TArray<FAIDACostItem>& Cost);

	/**
	 * Refund items into a player's inventory — central storage has no server-side deposit API
	 * (upload requires a source inventory), so the approver's pockets are the refund destination.
	 * Partial adds allowed; whatever doesn't fit (or has no resolvable player) is reported lost.
	 */
	static void RefundToPlayer(UObject* WorldContext, const FString& PlayerId, const TArray<FAIDACostItem>& Refund,
		int32& OutRefunded, int32& OutLost);

	/**
	 * Construct one batch of placements [Cursor, Cursor+BatchSize): hologram walk ending in
	 * Construct(). Placements that fail re-validation (world changed since dry-run) are skipped and
	 * counted, not fatal. Captures journal entity ids (lightweight = class+transform; full actor =
	 * class+recipe+transform) plus live actor ptrs for the in-session undo cache. Returns the number
	 * of placements consumed (advanced past), built or skipped.
	 */
	static int32 ExecuteBuildBatch(UObject* WorldContext, const FString& RecipeClassPath,
		const TArray<FTransform>& Placements, int32 Cursor, int32 BatchSize,
		TArray<FString>& OutEntityIds, TArray<TWeakObjectPtr<AActor>>& OutActors, int32& OutSkipped);

	/**
	 * Re-resolve a dismantle selector into per-entity handles at APPROVAL time (never trusted from
	 * the dry-run): weak actor ptrs for full buildables, encoded lw ids for lightweight instances.
	 */
	static bool ResolveDismantleHandles(UObject* WorldContext, const FAIDADismantleSpec& Selector, TArray<FAIDADismantleHandle>& OutHandles);

	/**
	 * Dismantle one batch of handles [Cursor, Cursor+BatchSize): Execute_Dismantle for actors,
	 * transform-matched FLightweightBuildableInstanceRef::Remove() for lightweights. Tallies the
	 * actual refund and appends the encoded ids of what was ACTUALLY removed (the journal record);
	 * handles that no longer resolve count as missing. Returns handles consumed.
	 */
	static int32 DismantleHandleBatch(UObject* WorldContext, const TArray<FAIDADismantleHandle>& Handles,
		int32 Cursor, int32 BatchSize, TArray<FAIDACostItem>& InOutRefund, TArray<FString>& OutRemovedIds,
		int32& OutRemoved, int32& OutMissing);

	/**
	 * CLIENT-side ghost hologram for the proposal preview: the recipe's own hologram (the game's
	 * native ghost visuals) placed at one tile, never replicated, no validation. The caller owns
	 * destroying it when the proposal resolves or moves. Null when the recipe can't load.
	 */
	static AActor* SpawnGhostHologram(UObject* WorldContext, const FString& RecipeClassPath,
		const FVector& CenterCm, float YawDeg, AActor* Owner);

	/**
	 * Undo of a BUILD: remove one journaled entity (docs/PHASE4.md §2d). CachedActor is the
	 * in-session fast path; otherwise re-resolves — "lw" by class+index (transform-verified;
	 * transform scan when the index is unknown/recycled), "actor" by class + transform epsilon.
	 * No refund here — the caller refunds the journaled cost once. False = already gone.
	 */
	static bool UndoRemoveEntity(UObject* WorldContext, const FAIDAEntityId& Entity, AActor* CachedActor);

	/**
	 * Undo of a DISMANTLE: rebuild one journaled entity at its recorded transform via the hologram
	 * path (re-validated — something may occupy the spot now; that's a reported failure, not fatal).
	 */
	static bool UndoRebuildEntity(UObject* WorldContext, const FAIDAEntityId& Entity);
};
