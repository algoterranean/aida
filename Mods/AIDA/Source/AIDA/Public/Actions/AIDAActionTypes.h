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

/** propose_build spec v1 (docs/PHASE4.md §2a) — one recipe, one origin, one repeat pattern. */
struct FAIDABuildSpec
{
	int32 Version = 1;
	FString Buildable;                     // display name, fuzzy-resolved server-side (unlocked only)
	FVector OriginM = FVector::ZeroVector; // metres
	bool bHasOrigin = false;               // omitted origin = at the requesting player (Ctx.Location)
	int32 YawDeg = 0;                      // snapped to 0/90/180/270
	FAIDAGridSpec Grid;
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
	FString RecipeClassPath;               // build: resolved once at dry-run
	TArray<FAIDACostItem> Cost;            // tallied at dry-run; deducted upfront at execute
	FString Summary;                       // human diff line ("place 100 x Foundation …")
	int32 Cursor = 0;                      // executor progress into Placements/targets
	TArray<FString> AffectedEntityIds;     // encoded FAIDAEntityIds, filled during execute
};
