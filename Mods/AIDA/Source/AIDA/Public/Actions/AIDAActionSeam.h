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
	 *
	 * OutAlternateOrigins (optional): fallback anchors for TOP-FACE aims, preference-ordered —
	 * centered on the aim, then growing away from the player. A top-face growth direction is a bet
	 * (the HUB / a platform edge may stand that way while the aimed cell itself is fine); callers
	 * dry-run the candidates in order instead of failing on the first bet. Side-face and terrain
	 * aims add none (a reversed side-face grid would sit INSIDE the aimed structure, and clipping
	 * never blocks — so it must never be offered).
	 */
	static bool ResolveAimSnappedOrigin(UObject* WorldContext, const FString& PlayerId,
		const FString& RecipeClassPath, int32& InOutYawDeg,
		int32 CountX, int32 CountY, double StepXCm, double StepYCm, FVector& OutOriginCm,
		TArray<FVector>* OutAlternateOrigins = nullptr);

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
	 * Spec-v2 composite dry-run: DryRunBuild per part over its slice of the placements (grouped by
	 * PlacementPartIndex), with failures remapped to GLOBAL placement indices, costs merged by item,
	 * clipping summed, and affordability checked against the merged tally. PartRecipePaths is indexed
	 * by the values in PlacementPartIndex (parallel to Placements).
	 */
	static bool DryRunBuildParts(UObject* WorldContext, const TArray<FString>& PartRecipePaths,
		const TArray<int32>& PlacementPartIndex, const TArray<FTransform>& Placements, FAIDADryRunResult& Out);

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
	 * of placements consumed (advanced past), built or skipped. OutPerIndexActors (optional) gets
	 * EXACTLY one entry per consumed index — the built actor, or null for skipped/lightweight —
	 * for callers that must keep placements index-aligned (manifold attachments).
	 */
	static int32 ExecuteBuildBatch(UObject* WorldContext, const FString& RecipeClassPath,
		const TArray<FTransform>& Placements, int32 Cursor, int32 BatchSize,
		TArray<FString>& OutEntityIds, TArray<TWeakObjectPtr<AActor>>& OutActors, int32& OutSkipped,
		TArray<TWeakObjectPtr<AActor>>* OutPerIndexActors = nullptr);

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

	//~ Manifolds (docs/PHASE4-MANIFOLDS.md).

	/**
	 * Resolve a manifold's machine ports LIVE: machines matching the selector, each contributing its
	 * PortIndex-th UNCONNECTED port of the wanted kind/direction (belt: factory connection input/
	 * output; pipe: consumer/producer pipe connection). Machines whose matching ports are all taken
	 * are counted in OutSkippedConnected, not returned — re-running a manifold extends only what's
	 * missing. OutMachineName reports the first match's canonical display name.
	 */
	static bool ResolveMachinePorts(UObject* WorldContext, const FAIDADismantleSpec& Selector,
		bool bPipe, bool bOutput, int32 PortIndex,
		TArray<FAIDAManifoldPort>& OutPorts, int32& OutSkippedConnected, FString& OutMachineName);

	/**
	 * Deterministic lane assignment for a manifold row (user rule: don't probe — every port on a
	 * machine side gets its OWN row). From the first machine matching the selector: pipe ports of
	 * this direction occupy the inner lanes (0..P-1, they hug the machines), belt ports the outer
	 * ones (P..P+B-1); this manifold's lane = its port's ordinal. Counts use ALL ports of the side
	 * (connected or not) so the lane is stable across re-runs. False when no machine matches.
	 */
	static bool ResolveManifoldLane(UObject* WorldContext, const FAIDADismantleSpec& Selector,
		bool bPipe, bool bOutput, int32 PortIndex, int32& OutLane, int32& OutRowsOnSide);

	/**
	 * How many of these placements would bodily overlap an EXISTING conveyor/pipeline attachment
	 * (splitter, merger, junction, pump)? The engine's clearance flags miss this — attachment
	 * clearance boxes are tiny and soft, so a splitter interpenetrating a pipe junction validates
	 * clean (live-verify: belt and pipe manifold rows landed in the same lane). Centers closer than
	 * the two half-footprints (2D, same floor) count as overlap; the manifold planner treats any
	 * overlap as "lane occupied" and steps the row outward.
	 */
	static int32 CountAttachmentOverlaps(UObject* WorldContext, const TArray<FTransform>& Placements, double FootprintCm);

	/**
	 * Build ONE connecting run (belt or pipe) between two live actors' ports, exactly like the build
	 * gun: spawn the spline hologram, place at the from-port, DoMultiStepPlacement, place at the
	 * to-port — BOTH ends must report IsConnectionSnapped (an unsnapped end would silently construct
	 * a support pole instead of a connection) — validate, then Construct. Ports are picked on the
	 * live actor by direction + best normal alignment with WantDir (belts: from = an output, to = an
	 * input; pipes are direction-agnostic). bChargeCost deducts the ACTUAL length-scaled cost from
	 * central storage first (runs are charged as built — docs/PHASE4-MANIFOLDS.md §4); an
	 * unaffordable or unsnappable run fails loudly via OutError and builds nothing.
	 */
	static bool BuildConnectingRun(UObject* WorldContext, const FString& TransportRecipePath, bool bPipe,
		AActor* FromActor, const FVector& FromWantDir, AActor* ToActor, const FVector& ToWantDir,
		bool bChargeCost, TArray<FAIDACostItem>& OutCost,
		TArray<FString>& OutEntityIds, TArray<TWeakObjectPtr<AActor>>& OutActors, FString& OutError);

	//~ Container labels (P7 Slice 3 write side).

	/**
	 * Resolve the sign recipe for labels: the override name when given (normal fuzzy resolve),
	 * otherwise the SMALLEST unlocked sign whose buildable is an AFGBuildableWidgetSign — no display-
	 * name guessing across game versions. False with OutError when nothing sign-like is unlocked.
	 */
	static bool ResolveSignRecipe(UObject* WorldContext, const FString& OverrideName, FAIDARecipeResolution& Out, FString& OutError);

	/**
	 * Storage containers within RadiusCm of CenterCm that need a label: dominant inventory item →
	 * Text; empty containers and containers that already have a sign within a face's reach are
	 * skipped (counted separately). The sign spot sits just off the container face nearest the
	 * viewer (falling back to the container's front). Returns the number of targets.
	 */
	static int32 ResolveLabelTargets(UObject* WorldContext, const FVector& CenterCm, double RadiusCm,
		int32 MaxCount, const FString& ItemFilter, const FVector& ViewerCm, bool bHasViewer,
		TArray<FAIDALabelTarget>& OutTargets, int32& OutSkippedEmpty, int32& OutSkippedLabeled);

	/**
	 * Build ONE label sign on a container: trace from the sign spot into the container face for a
	 * real hit, drive the sign hologram through the standard headless recipe (reset disqualifiers →
	 * build mode → UpdateHologramPlacement → validate), Construct, then write the label text into
	 * the sign via GetSignPrefabData/SetPrefabSignData (all text elements; descriptor defaults when
	 * the fresh sign has none). Cost is NOT charged here — label proposals pay upfront like builds.
	 */
	static bool BuildLabelSign(UObject* WorldContext, const FString& SignRecipePath, AActor* Container,
		const FVector& SignPosCm, const FVector& OutwardCm, const FString& Text,
		TArray<FString>& OutEntityIds, TArray<TWeakObjectPtr<AActor>>& OutActors, FString& OutError);

	/** Tally one recipe's ingredient cost × Count (the label proposal's upfront cost line). */
	static bool TallyRecipeCost(UObject* WorldContext, const FString& RecipeClassPath, int32 Count, TArray<FAIDACostItem>& Out);

	//~ Auto-power (docs/PHASE4-POWER.md).

	/** Resolved power kit for an auto-powered build: pole recipe (lowest unlocked mk or the
	 *  override), its connection cap, and the Power Line recipe. */
	struct FAIDAPowerInfo
	{
		bool bMachineNeedsPower = false;   // the machine CDO has a power connection
		FString PoleRecipePath;
		FString PoleName;
		int32 PoleConnectionCap = 4;       // mk.1's cap = the conservative fallback
		FString WireRecipePath;
		FString WireName;
	};

	/**
	 * Does this recipe's buildable need power, and what do we wire it with? Pole candidates resolve
	 * lowest-unlocked-first ("Power Pole Mk.1" → Mk.2 → Mk.3), or just the override when given; the
	 * cap comes from the pole CDO's power connection. False = machine needs no power OR no pole/
	 * wire recipe is unlocked (OutError says which).
	 */
	static bool ResolveAutoPower(UObject* WorldContext, const FString& MachineRecipePath,
		const FString& PoleOverrideName, FAIDAPowerInfo& Out, FString& OutError);

	/**
	 * EXISTING machines near a point whose power connection is unwired (propose_power). Name filter
	 * optional; machines whose power connection is already on a circuit count in OutSkippedPowered;
	 * buildables with no power connection at all are silently not matches. Reuses FAIDAManifoldPort
	 * as the carrier (Machine + display name + location; NormalCm unused).
	 */
	static int32 ResolveUnpoweredMachines(UObject* WorldContext, const FString& Buildable,
		const FVector& CenterCm, double RadiusCm, int32 MaxCount,
		TArray<FAIDAManifoldPort>& OutMachines, int32& OutSkippedPowered);

	/** Can central storage cover this WHOLE tally right now? (Merged machine+pole affordability.) */
	static bool CheckAffordable(UObject* WorldContext, const TArray<FAIDACostItem>& Cost);

	/**
	 * Spawn one power line between two live actors' circuit connections and Connect() both ends —
	 * wires never hologram-snap (the belt lesson, docs/PHASE4-MANIFOLDS.md §5). Endpoints pick the
	 * first power connection WITH FREE CAPACITY on each actor (poles expose several); the wire is
	 * recipe-stamped for dismantle refunds, length-priced, and charged from central storage when
	 * bChargeCost. False = nothing spawned (capacity, endpoints gone, unaffordable — OutError).
	 */
	static bool BuildWire(UObject* WorldContext, const FString& WireRecipePath,
		AActor* A, AActor* B, bool bChargeCost, TArray<FAIDACostItem>& OutCost,
		TArray<FString>& OutEntityIds, TArray<TWeakObjectPtr<AActor>>& OutActors, FString& OutError);

	/**
	 * The nearest EXTERNAL circuit connection with a free slot within RangeCm of any given pole
	 * (existing power poles preferred, then any powered buildable), excluding the poles themselves.
	 * Returns the external actor + which of our poles to tie from; false = nothing in reach.
	 */
	static bool FindGridTie(UObject* WorldContext, const TArray<TWeakObjectPtr<AActor>>& OurPoles,
		double RangeCm, AActor*& OutExternal, int32& OutPoleIndex);
};
