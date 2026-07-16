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
	 * Parse + validate a propose_build spec (docs/PHASE4.md §2a). v1: buildable non-empty, grid counts
	 * >= 1, countX*countY <= MaxItems. v2 (composite): a non-empty `parts` array (each part = buildable
	 * + optional at:{x,y,z} offset in metres + optional yawDeg + optional grid), total placements across
	 * parts <= MaxItems. Yaws snap to the nearest 90° and normalize to 0/90/180/270. Returns false with
	 * a model-facing reason in OutError.
	 */
	bool ParseBuildSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDABuildSpec& Out, FString& OutError);

	/** Parse + validate a propose_dismantle selector: version 1, radius > 0, maxCount clamped to MaxItems. */
	bool ParseDismantleSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDADismantleSpec& Out, FString& OutError);

	/**
	 * Parse + validate a propose_label_containers spec (P7 Slice 3): version 1, everything optional —
	 * sign override, center (omitted = requester's aim), radiusM (default 30), maxCount (default 20,
	 * clamped to MaxItems), item filter.
	 */
	bool ParseLabelSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDALabelSpec& Out, FString& OutError);

	/** Human diff line: "label 6 container(s) with a Label Sign each (contents as text) within 30 m of (x, y)". */
	FString SummarizeLabel(const FAIDALabelSpec& Spec, const FString& SignName, int32 Count);

	/**
	 * Parse + validate a propose_manifold spec (docs/PHASE4-MANIFOLDS.md §2): version 1, kind
	 * belt/pipe, direction in/out, transport + machines.buildable required. Machine selector
	 * defaults: radiusM 30, maxCount 0 (= all matches, clamped to MaxItems when capped).
	 */
	bool ParseManifoldSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDAManifoldSpec& Out, FString& OutError);

	/**
	 * Fit the manifold row (docs/PHASE4-MANIFOLDS.md §3, pure geometry): all port normals must agree
	 * within ~45°, row axis = Up × avgNormal, one attachment per port at its projection onto the
	 * trunk line standoff in front of the ports, sorted along the axis. Attachment yaw points the
	 * pass-through along the axis (mergers flipped 180° so the collection end is index 0, matching
	 * the splitters' feed end). Fails (Out.Error) on mixed facing, attachments closer than the
	 * footprint, or trunk hops beyond MaxRunM.
	 */
	FAIDAManifoldPlan PlanManifold(const TArray<FAIDAManifoldPortPoint>& Ports, bool bOutput, bool bPipe,
		double StandoffM, double FootprintM, double MaxRunM);

	/**
	 * Parse + validate a propose_power spec: version 1, everything optional — buildable filter,
	 * pole override, center (omitted = requester's aim), radiusM (default 30), maxCount (0 = all,
	 * clamped to MaxItems).
	 */
	bool ParsePowerSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDAPowerSpec& Out, FString& OutError);

	/**
	 * Plan poles + wires for EXISTING machines at arbitrary points (propose_power, pure geometry):
	 * machines sorted along the dominant world axis, chunked MachinesPerPole per pole, pole at each
	 * chunk's centroid pushed OffsetCm perpendicular to the row, consecutive poles chained.
	 * MachineWires.X indexes the input array. Callers ground-probe and validate the poles (and
	 * retry with flipped/larger offsets).
	 */
	FAIDAPowerPlan PlanPowerForPoints(const TArray<FVector>& MachinesCm, int32 MachinesPerPole, double OffsetCm);

	/**
	 * Plan the power layout for a row-major machine grid (docs/PHASE4-POWER.md, pure geometry):
	 * chunks of MachinesPerPole per row get one pole at the chunk's midpoint, half a step off the
	 * row line (between rows; the last row of a multi-row grid folds back), plus machine→pole wire
	 * pairs and a consecutive pole chain. Yaw follows the grid.
	 */
	FAIDAPowerPlan PlanPower(int32 CountX, int32 CountY, double StepXCm, double StepYCm, int32 YawDeg,
		const FVector& OriginCm, int32 MachinesPerPole);

	/** 8-way compass name for a world direction (game convention: north = -Y, east = +X). */
	FString CompassName(const FVector& Dir);

	/** Human diff line: "manifold: 10 x Conveyor Splitter + 19 x Belt runs feeding 10 x Smelter (feed at the west end)". */
	FString SummarizeManifold(const FAIDAManifoldSpec& Spec, const FString& AttachmentName, const FString& TransportName,
		int32 MachineCount, int32 RunCount, const FString& OpenEndCompass);

	/**
	 * Expand the grid into world-unit (cm) transforms, row-major from the origin. Steps default to the
	 * caller-supplied footprint (metres — the seam knows the buildable's size) when the spec left them 0.
	 * The step axes rotate with YawDeg so a rotated grid stays coherent.
	 */
	TArray<FTransform> ExpandGrid(const FAIDABuildSpec& Spec, double DefaultStepXM, double DefaultStepYM);

	/**
	 * Expand a v2 composite (Spec.Parts non-empty) into world-unit transforms, grouped by part
	 * (contiguous runs, part order preserved — the executor batches one recipe at a time). Each part's
	 * origin = composite origin + composite-yaw-rotated offset; each part's yaw = composite yaw + part
	 * yaw. FootprintsM is parallel to Spec.Parts (per-part default/clamp grid steps, metres).
	 * OutPartIndex is filled parallel to the returned placements.
	 */
	TArray<FTransform> ExpandParts(const FAIDABuildSpec& Spec, const TArray<FVector2D>& FootprintsM,
		TArray<int32>& OutPartIndex);

	/** Human diff line: "place 100 x Foundation 8m x 2m in a 10x10 grid" (v2: parts breakdown). */
	FString SummarizeBuild(const FAIDABuildSpec& Spec);

	/** Human diff line: "dismantle up to 20 x Smelter within 50 m of (-120, 45)". */
	FString SummarizeDismantle(const FAIDADismantleSpec& Spec);

	/** Proposal-state name for reports ("pending", "awaiting approval" is phrased by callers). */
	FString StateToString(EAIDAProposalState State);

	/** Dry-run success report (docs/PHASE4.md §2b) — the propose_* tool result once a proposal is
	 *  stored. OriginM (metres), when supplied, is echoed as "origin" so the model can place follow-up
	 *  proposals relative to what it just anchored. InvalidPlacements (when non-empty) reports blocked
	 *  spots as an ADVISORY — validity never blocks a proposal (user rule); the ghost goes up and the
	 *  player nudges it, or approval builds the valid subset and refunds the rest. */
	FString BuildDryRunJson(const FAIDAProposal& Proposal, int32 ExpiresInSec, bool bAffordable, double PowerDrawMW,
		const FVector* OriginM = nullptr, const TArray<FAIDAPlacementFailure>* InvalidPlacements = nullptr);

	/** Dry-run failure report: bounded per-index reasons so the model can revise the spec. */
	FString BuildErrorJson(const FString& Error, const TArray<FAIDAPlacementFailure>& FirstFailures, int32 MaxShown = 5);

	/** get_proposal_status: one entry per proposal (all of them if Filter is invalid/unset). */
	FString BuildStatusJson(const TArray<FAIDAProposal>& Proposals, const FGuid& Filter, int64 NowUtc, int32 TtlSeconds);

	/** Cost/refund items as a compact JSON array (the journal's RefundJson). */
	FString CostItemsToJson(const TArray<FAIDACostItem>& Items);

	/** Decode a journaled RefundJson back into cost items (undo's refund/re-deduct input). */
	TArray<FAIDACostItem> ParseCostItems(const FString& Json);

	/** Encode a journal entity handle (docs/PHASE4.md §2d) as one compact self-describing JSON string. */
	FString EncodeEntityId(const FAIDAEntityId& Entity);

	/** Decode a journaled entity handle. Returns false on garbage or an unknown "t". */
	bool DecodeEntityId(const FString& Encoded, FAIDAEntityId& Out);
}
