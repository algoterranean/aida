#pragma once

#include "CoreMinimal.h"
#include "Actions/AIDAActionTypes.h"

class FJsonObject;

/**
 * Pure spec handling for the Phase 4 proposal pipeline (docs/PHASE4.md §2-3): parse + validate the
 * versioned LLM specs, expand a grid spec into world transforms, build the model-facing JSON reports,
 * and encode/decode journal entity ids. No game headers — everything here unit-tests without the game.
 * FAIDAActionSeam feeds it live data; the propose_* tool lambdas serve its output.
 */
namespace AIDAActionSpec
{
	/**
	 * Parse + validate a propose_build spec (docs/PHASE4.md §2a). Strict: version must be 1, buildable
	 * non-empty, grid counts >= 1, countX*countY <= MaxItems. Yaw is snapped to the nearest 90° and
	 * normalized to 0/90/180/270. Returns false with a model-facing reason in OutError.
	 */
	bool ParseBuildSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDABuildSpec& Out, FString& OutError);

	/** Parse + validate a propose_dismantle selector: version 1, radius > 0, maxCount clamped to MaxItems. */
	bool ParseDismantleSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDADismantleSpec& Out, FString& OutError);

	/**
	 * Expand the grid into world-unit (cm) transforms, row-major from the origin. Steps default to the
	 * caller-supplied footprint (metres — the seam knows the buildable's size) when the spec left them 0.
	 * The step axes rotate with YawDeg so a rotated grid stays coherent.
	 */
	TArray<FTransform> ExpandGrid(const FAIDABuildSpec& Spec, double DefaultStepXM, double DefaultStepYM);

	/** Human diff line: "place 100 x Foundation 8m x 2m in a 10x10 grid". */
	FString SummarizeBuild(const FAIDABuildSpec& Spec);

	/** Human diff line: "dismantle up to 20 x Smelter within 50 m of (-120, 45)". */
	FString SummarizeDismantle(const FAIDADismantleSpec& Spec);

	/** Proposal-state name for reports ("pending", "awaiting approval" is phrased by callers). */
	FString StateToString(EAIDAProposalState State);

	/** Dry-run success report (docs/PHASE4.md §2b) — the propose_* tool result once a proposal is stored. */
	FString BuildDryRunJson(const FAIDAProposal& Proposal, int32 ExpiresInSec, bool bAffordable, double PowerDrawMW);

	/** Dry-run failure report: bounded per-index reasons so the model can revise the spec. */
	FString BuildErrorJson(const FString& Error, const TArray<FAIDAPlacementFailure>& FirstFailures, int32 MaxShown = 5);

	/** get_proposal_status: one entry per proposal (all of them if Filter is invalid/unset). */
	FString BuildStatusJson(const TArray<FAIDAProposal>& Proposals, const FGuid& Filter, int64 NowUtc, int32 TtlSeconds);

	/** Encode a journal entity handle (docs/PHASE4.md §2d) as one compact self-describing JSON string. */
	FString EncodeEntityId(const FAIDAEntityId& Entity);

	/** Decode a journaled entity handle. Returns false on garbage or an unknown "t". */
	bool DecodeEntityId(const FString& Encoded, FAIDAEntityId& Out);
}
