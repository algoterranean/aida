#pragma once

#include "CoreMinimal.h"

/**
 * Phase 4 "Hands" data model (docs/PHASE4.md §2) — plain C++, no game headers, so everything here
 * unit-tests without launching the game. Specs arrive from the LLM in metres (the established tool
 * convention) and are expanded into world-unit (cm) transforms by AIDAActionSpec::ExpandGrid; the
 * ActionEngine never trusts model coordinates — every placement passes dry-run validation.
 */

/** Specs speak metres (tool convention); the world speaks cm. One constant, shared by the Actions TUs
 *  (unity builds merge their anonymous namespaces, so per-.cpp copies would collide). */
inline constexpr double AIDAMetersToCm = 100.0;

/** Proposal lifecycle. Legal transitions are enforced by FAIDAProposalStore::IsLegalTransition. */
enum class EAIDAProposalState : uint8
{
	Pending,    // stored, awaiting approval (TTL running)
	Approved,   // an act-tier player approved; execution not yet started
	Executing,  // time-sliced executor in progress
	Executed,   // done; journaled
	Failed,     // execution aborted (cost, game error); partial work journaled
	Rejected,   // a player rejected it
	Expired,    // TTL elapsed unapproved
	Undone      // executed, then reversed via /aida undo
};

/** N×M repeat pattern for a build spec. Steps in metres; 0 = use the buildable's footprint. */
struct FAIDAGridSpec
{
	int32 CountX = 1;
	int32 CountY = 1;
	double StepXM = 0.0;
	double StepYM = 0.0;
};

/** One part of a spec-v2 composite build: a buildable placed (optionally grid-repeated) at an
 *  offset relative to the composite's shared origin, rotated with the composite's yaw. */
struct FAIDABuildPart
{
	FString Buildable;                     // display name, fuzzy-resolved server-side (unlocked only)
	FVector OffsetM = FVector::ZeroVector; // metres, relative to the composite origin (pre-rotation)
	int32 YawDeg = 0;                      // relative to the composite yaw; snapped to 0/90/180/270
	FAIDAGridSpec Grid;
};

/** propose_build spec (docs/PHASE4.md §2a): v1 = one recipe, one origin, one repeat pattern.
 *  v2 (spec-v2 composite, PHASE7.md Slice 4's `parts` half) = one shared origin/yaw + a part list —
 *  Parts non-empty ⇔ v2. v2 ignores followTerrain (parts carry explicit z) and skips auto-power
 *  (include poles as parts; wire routing lands with P7 belts/wires). */
struct FAIDABuildSpec
{
	int32 Version = 1;
	FString Buildable;                     // v1: display name, fuzzy-resolved server-side (unlocked only)
	FVector OriginM = FVector::ZeroVector; // metres
	bool bHasOrigin = false;               // omitted origin = at the requesting player (Ctx.Location)
	int32 YawDeg = 0;                      // snapped to 0/90/180/270 (v2: whole-composite rotation)
	FAIDAGridSpec Grid;                    // v1 only
	/** Default false = a FLAT grid at the origin's height; true = each tile drops to its own terrain. */
	bool bFollowTerrain = false;
	/** Auto-power (docs/PHASE4-POWER.md): default true — powered buildables get poles + power lines
	 *  + a grid tie-in as part of the proposal. "power": false opts out. v1 only. */
	bool bPower = true;
	/** Optional pole display-name override; "" = the lowest unlocked mk. */
	FString Pole;
	/** v2 composite parts (empty = v1 single-buildable spec). */
	TArray<FAIDABuildPart> Parts;
};

/** PlanPower output (docs/PHASE4-POWER.md §3): pole transforms + wire index pairs, pure geometry. */
struct FAIDAPowerPlan
{
	TArray<FTransform> Poles;
	TArray<FIntPoint> MachineWires;  // X = machine placement index, Y = pole index
	TArray<FIntPoint> ChainWires;    // pole index -> pole index (consecutive chain)
	FString Error;                   // non-empty = plan failed (degenerate inputs)
};

/** propose_dismantle selector v1. */
struct FAIDADismantleSpec
{
	int32 Version = 1;
	FString Buildable;                     // display-name match; "" = anything
	FVector CenterM = FVector::ZeroVector; // metres
	bool bHasCenter = false;               // omitted center = at the requesting player (Ctx.Location)
	double RadiusM = 0.0;
	int32 MaxCount = 20;
};

/** propose_manifold spec v1 (docs/PHASE4-MANIFOLDS.md §2): splitter/merger (or pipe-junction) rows
 *  + the connecting belt/pipe runs, derived from live machine ports — the model never supplies
 *  transforms, only the selector and the tiers. */
struct FAIDAManifoldSpec
{
	int32 Version = 1;
	bool bPipe = false;                    // kind: "belt" (default) | "pipe"
	bool bOutput = false;                  // direction: "in" (feed inputs, default) | "out" (collect outputs)
	FString Transport;                     // belt/pipe display name, fuzzy-resolved, unlocked only
	FString Attachment;                    // optional override; "" = the kind/direction default
	FAIDADismantleSpec Machines;           // selector reuse (buildable required, center optional = aim)
	double StandoffM = 4.0;                // trunk-line distance in front of the ports
	int32 PortIndex = 0;                   // which matching unconnected machine port (0 = first)
};

/** propose_label_containers spec v1 (P7 Slice 3 write side): put a sign with the dominant item name
 *  on each storage container near a point. The model supplies a selector, never transforms. */
struct FAIDALabelSpec
{
	int32 Version = 1;
	FString Sign;                          // optional sign display-name override; "" = smallest unlocked sign
	FVector CenterM = FVector::ZeroVector; // metres
	bool bHasCenter = false;               // omitted center = the requester's aim (falling back to position)
	double RadiusM = 30.0;
	int32 MaxCount = 20;
	FString ItemFilter;                    // optional: only containers holding this item (substring)
};

/** One container a label proposal holds between propose and execute (world units). */
struct FAIDALabelTarget
{
	TWeakObjectPtr<AActor> Container;      // re-verified at execute; a dead container = one failed sign
	FString ContainerClass;                // for failure reports
	FVector SignPosCm = FVector::ZeroVector;   // where the sign goes (just off the chosen face)
	FVector OutwardCm = FVector::XAxisVector;  // unit, from the container face toward the viewer
	FString Text;                          // the label (dominant item), resolved at propose time
};

/** propose_power spec v1 (live-verify gap): wire EXISTING unpowered machines to the grid. */
struct FAIDAPowerSpec
{
	int32 Version = 1;
	FString Buildable;                     // display-name match; "" = any unpowered machine
	FString Pole;                          // optional pole override; "" = lowest unlocked mk
	FVector CenterM = FVector::ZeroVector; // metres
	bool bHasCenter = false;               // omitted center = the requester's aim (then position)
	double RadiusM = 30.0;
	int32 MaxCount = 0;                    // 0 = all in radius (clamped to MaxItems when capped)
};

/** One machine-port point for the pure planner (world units; the seam resolves these live). */
struct FAIDAManifoldPortPoint
{
	FVector PosCm = FVector::ZeroVector;
	FVector NormalCm = FVector::XAxisVector; // outward connector normal (XY significant)
};

/**
 * PlanManifold output: one attachment per port, sorted along the fitted row axis. Attachments[i]
 * serves the caller's Ports[PortOrder[i]] — callers reorder their parallel arrays by PortOrder so
 * everything downstream is index-aligned. Z is carried from each port; callers re-probe ground.
 */
struct FAIDAManifoldPlan
{
	TArray<FTransform> Attachments;
	TArray<int32> PortOrder;
	FVector RowAxis = FVector::XAxisVector; // unit; +axis = ascending sort order (open end at index 0)
	FVector DropDir = FVector::ZeroVector;  // unit, from the trunk line toward the machines (-avg normal)
	int32 YawDeg = 0;                       // attachment yaw (pass-through along the axis; mergers flipped 180°)
	FString Error;                          // non-empty = plan failed (model-facing reason)
};

/** One machine port a manifold proposal holds between propose and execute (world units). */
struct FAIDAManifoldPort
{
	TWeakObjectPtr<AActor> Machine;        // re-verified at execute; a dead machine = one failed drop
	FString MachineName;                   // display name for failure reports
	FVector PosCm = FVector::ZeroVector;
	FVector NormalCm = FVector::XAxisVector;
};

/**
 * One manifold planned against a PENDING build's machines (a "connected build" — the revise-by-
 * prompt flow: "build a line of generators" … "add an input manifold"). The attachments and their
 * runs execute AFTER the machines; Ports carry PLANNED geometry read from the machine hologram's
 * connection components, and Ports[i].Machine is rebound to the BUILT actor at execute (the run
 * builder resolves live ports from the actors, so planned positions only steer the plan).
 */
struct FAIDAManifoldSet
{
	bool bPipe = false;
	bool bOutput = false;
	FString TransportRecipePath;
	FString TransportName;
	FString AttachmentRecipePath;
	FString AttachmentName;
	TArray<FTransform> Attachments;   // plan-sorted; index-aligned with Ports/PortMachineIndex
	TArray<FAIDAManifoldPort> Ports;  // planned pos/normal; Machine is null until phase 0 builds
	TArray<int32> PortMachineIndex;   // per attachment: index into the proposal's machine Placements
	FVector RowAxis = FVector::XAxisVector;
	FVector DropDir = FVector::ZeroVector;
};

/**
 * FindTapSource output (propose_belt_tap): the existing belt to feed a manifold from, and how —
 * either a free (unconnected) output end, or a mid-belt cut where the game's own attachment-on-belt
 * splice inserts the tap splitter.
 */
struct FAIDATapSource
{
	FString Error;                          // model-facing reason when no source qualifies
	TWeakObjectPtr<AActor> Belt;            // the source conveyor (re-verified at execute)
	FString BeltName;                       // display name for summaries
	FString ItemNote;                       // what is riding it ("Coal"), or "empty"
	bool bDangling = false;                 // free output end — no cut, feed straight off the end
	double OffsetCm = 0.0;                  // cut offset along the belt (cut variant)
	FVector PointCm = FVector::ZeroVector;  // tap point (cut center, or the free end connector)
	FVector DirCm = FVector::XAxisVector;   // belt tangent (flow direction) at the tap
	double DistanceM = 0.0;                 // tap point -> feed point, metres
};

/** One occupied cell of a foundation-slab census (pure lattice space; propose_extend_foundations). */
struct FAIDASlabCell
{
	FIntPoint Coord = FIntPoint::ZeroValue; // (u,v) lattice cell
	double ZCm = 0.0;                       // the cell's foundation Z (terrain-following slabs step)
	int32 Part = 0;                         // index into the census's per-class tables
};

/** PlanSlabExtension output: new cells extending every lane's frontier, Part/Z copied per lane.
 *  (New cells can never collide with the slab: same lane would have been the frontier, other
 *  lanes differ in the perpendicular coordinate.) */
struct FAIDASlabExtensionPlan
{
	TArray<FAIDASlabCell> NewCells;
	FString Error;                          // non-empty = nothing to plan (model-facing reason)
};

/**
 * CensusFoundationSlab output (seam): the CONTIGUOUS foundation slab under the player's feet (or
 * aim) resolved onto its own lattice, plus the direction the player means by "extend". Cells carry
 * per-part class indices so mixed slabs (1 m + 2 m foundations) extend with matching types.
 */
struct FAIDASlabCensus
{
	FString Error;                          // model-facing reason when the census fails
	FVector OriginCm = FVector::ZeroVector; // world center of cell (0,0), at its foundation Z
	FVector AxisU = FVector::XAxisVector;   // unit lattice axes (horizontal)
	FVector AxisV = FVector::YAxisVector;
	double StepCm = 800.0;                  // cell size (standard foundations are 8x8 m)
	double YawDeg = 0.0;                    // rotation for new cells (the anchor's)
	TArray<FString> PartRecipePaths;        // per part: build recipe class path
	TArray<FString> PartNames;              // per part: display name for summaries
	TArray<FAIDASlabCell> Cells;            // the flood-filled slab
	FIntPoint ExtendDir = FIntPoint(1, 0);  // resolved lattice step to extend along
	FString DirectionNote;                  // human-readable ("west — your look direction")
};

/** One line of a cost/refund tally. ClassPath (the item descriptor) is what deduction/refund acts
 *  on; Item is the display name the model and players see. */
struct FAIDACostItem
{
	FString Item;
	int32 Amount = 0;
	FString ClassPath;
};

/** One placement the dry-run rejected — bounded to a handful in the error report. */
struct FAIDAPlacementFailure
{
	int32 Index = INDEX_NONE;
	FVector AtM = FVector::ZeroVector;     // metres, humanized for the model
	FString Reason;                        // UFGConstructDisqualifier text
};

/**
 * A journaled entity handle (docs/PHASE4.md §2d): the game has no persistent per-buildable GUID, so
 * undo re-resolves from what we record. "lw" = lightweight instance (class + index + transform; the
 * index can be recycled — the transform disambiguates, and is the post-reload fallback). "actor" =
 * full buildable actor (class + recipe + transform; re-resolved by class + transform epsilon).
 * Positions are world units (cm) — these are re-resolve keys, not model-facing text.
 */
struct FAIDAEntityId
{
	FString Type;                          // "lw" | "actor"
	FString ClassPath;
	FString RecipePath;                    // actor only
	int32 Index = INDEX_NONE;              // lw only
	FVector Pos = FVector::ZeroVector;     // cm
	int32 YawDeg = 0;
};

/** Outcome of resolving a spec's display name to an unlocked build recipe (FAIDAActionSeam). */
struct FAIDARecipeResolution
{
	FString RecipeClassPath;
	FString DisplayName;                   // canonical building name (for summaries)
	double FootprintXM = 8.0;              // clearance-box size, metres — the default grid step
	double FootprintYM = 8.0;
	TArray<FString> Suggestions;           // close matches when resolution failed
};

/** Outcome of a non-mutating dry run over expanded placements (FAIDAActionSeam::DryRunBuild). */
struct FAIDADryRunResult
{
	bool bOk = false;
	FString Error;                         // set when the run failed wholesale (no hologram, bad recipe)
	TArray<FAIDAPlacementFailure> Failures;
	TArray<FAIDACostItem> Cost;            // total for all placements
	bool bAffordable = false;              // vs central storage (callers ignore under costMode "free")
	/** Placements that would clip existing structures/entities. ADVISORY ONLY — clipping never
	 *  blocks a build (user rule); manifold rows use it to prefer a clean lane. */
	int32 ClippingCount = 0;
};

/** Outcome of resolving a dismantle selector against live buildables (count + refund tally). */
struct FAIDADismantleResolution
{
	int32 Count = 0;
	TArray<FAIDACostItem> Refund;
};

/**
 * One dismantle target resolved at approval time (docs/PHASE4.md §3): a live weak actor ptr for
 * full buildables, or an encoded "lw" entity id (class + transform) for lightweight instances,
 * re-resolved per batch. EncodedId doubles as the journal entry for undo.
 */
struct FAIDADismantleHandle
{
	TWeakObjectPtr<AActor> Actor;
	FString EncodedId;
};

/**
 * A stored proposal (docs/PHASE4.md §2c). Server-side and in-memory only — a restart voids pending
 * proposals (TTL is 10 min; approval must reference live server state); only the journal persists.
 */
struct FAIDAProposal
{
	FGuid Id;
	FString SpecJson;                      // verbatim spec; journaled on execute
	bool bDismantle = false;
	FString RequesterId;
	FString RequesterName;
	FString ApproverId;
	int64 ProposedUtc = 0;                 // TTL anchor
	int64 ResolvedUtc = 0;                 // stamped on entering a terminal state (drives UI linger/retire)
	EAIDAProposalState State = EAIDAProposalState::Pending;
	TArray<FTransform> Placements;         // build: expanded + validated grid (world units)
	int32 TargetCount = 0;                 // dismantle: matches found at dry-run (re-resolved at execute)
	FString RecipeClassPath;               // build: resolved once at dry-run (v2: parts[0], legacy reads)
	//~ Spec-v2 composites: per-part recipes + a per-placement part map. Placements are stored grouped
	//  by part (contiguous runs) so the executor batches one recipe at a time. Empty = v1 proposal.
	TArray<FString> PartRecipePaths;
	TArray<int32> PlacementPartIndex;      // parallel to Placements; values index PartRecipePaths
	TArray<FAIDACostItem> Cost;            // tallied at dry-run; deducted upfront at execute
	FString Summary;                       // human diff line ("place 100 x Foundation …")
	int32 Cursor = 0;                      // executor progress into Placements/targets
	TArray<FString> AffectedEntityIds;     // encoded FAIDAEntityIds, filled during execute
	/** Blocked placements at the LAST dry-run (propose or nudge). ADVISORY ONLY — placement
	 *  validity never blocks a proposal or a nudge (user rule: the ghost always goes up and the
	 *  player nudges it somewhere valid); execute re-validates per tile, skips what still fails,
	 *  and refunds the skips. */
	int32 InvalidCount = 0;

	//~ Connected build (revise-by-prompt): Placements/RecipeClassPath are the MACHINES, and each
	//  set is a manifold planned against them that executes after they build (TickConnected:
	//  machines → per-set attachments → runs → power kit when bAutoPower). Empty = not connected.
	TArray<FAIDAManifoldSet> ManifoldSets;

	//~ Manifold extensions (docs/PHASE4-MANIFOLDS.md). Defaults = a plain build/dismantle proposal.
	//  For manifolds, Placements/RecipeClassPath/Cost describe the ATTACHMENTS (so ghosts, upfront
	//  deduction and the pending cap all reuse the existing paths); runs are charged as built.
	bool bManifold = false;
	bool bManifoldPipe = false;
	bool bManifoldOutput = false;
	FString TransportRecipePath;           // belt/pipe recipe, resolved at dry-run
	FString TransportName;                 // display name for summaries/reports
	TArray<FAIDAManifoldPort> Ports;       // index-aligned with Placements (plan-sorted)
	FVector RowAxis = FVector::XAxisVector;
	FVector DropDir = FVector::ZeroVector; // unit, from each attachment toward its machine
	int32 Phase = 0;                       // executor: 0 = attachments/machines, then kind-specific

	//~ Belt-tap extensions (P7: feed a pending belt-in manifold from an existing belt). The tap
	//  executes AFTER the base phases: cut/claim the source, then one feed run from the tap to the
	//  trunk's open end (index-0 attachment). Cut variant relies on the game's native
	//  attachment-on-belt splice (the splitter hologram snapped onto the belt splits + rewires it).
	bool bTap = false;
	TWeakObjectPtr<AActor> TapBelt;        // source conveyor; a dead weak ptr = tap fails loudly
	FString TapBeltName;
	bool bTapDangling = false;             // free end — no cut, no splitter
	double TapOffsetCm = 0.0;              // cut offset along the source belt
	FVector TapPointCm = FVector::ZeroVector;
	FVector TapDirCm = FVector::XAxisVector;   // belt tangent at the tap
	FString TapSplitterRecipePath;         // cut variant: the tap splitter
	FString TapSplitterName;
	int32 TapSetIndex = INDEX_NONE;        // connected builds: which ManifoldSet the tap feeds (bManifold: INDEX_NONE)

	//~ Container-label extensions (P7 Slice 3). Placements carry the sign spots (ghost preview +
	//  count + upfront cost reuse the build paths); Targets carry the containers and their texts.
	bool bLabel = false;
	TArray<FAIDALabelTarget> LabelTargets; // index-aligned with Placements

	//~ Auto-power extensions (docs/PHASE4-POWER.md). Powered builds run phases 0 machines →
	//  1 poles → 2 wires + grid tie. Empty/false = a plain (or manifold) proposal.
	//  bPowerOnly (propose_power): the machines already EXIST — phase 0 fills the machine slots
	//  from Ports instead of building; Placements mirror PolePlacements for the ghost preview.
	bool bAutoPower = false;
	bool bPowerOnly = false;
	TArray<FTransform> PolePlacements;
	FString PoleRecipePath;
	FString PoleName;
	FString WireRecipePath;
	TArray<FIntPoint> MachineWires;        // (machine placement idx, pole idx)
	TArray<FIntPoint> ChainWires;          // (pole idx, pole idx)
};
