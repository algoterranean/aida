#pragma once

#include "CoreMinimal.h"
#include "AIDAConfig.generated.h"

// Server-side AIDA configuration model (see docs/ARCHITECTURE.md §7).
// Field names map to the camelCase JSON keys via FJsonObjectConverter's case standardization
// (bool "b"-prefix handling included, e.g. bSendFactoryData <-> "sendFactoryData").

USTRUCT()
struct FAIDAProviderConfig
{
	GENERATED_BODY()

	/** "openai-compatible" | "anthropic" */
	UPROPERTY() FString Type = TEXT("openai-compatible");
	UPROPERTY() FString BaseUrl;
	/** SERVER-ONLY. Never replicate, never log — redact everywhere. */
	UPROPERTY() FString ApiKey;
	UPROPERTY() FString Model;
	/** 1024 starved spec-v2 composites: a detailed multi-part build spec is several KB of tool-call
	 *  JSON, and a reply truncated mid-call dispatches with empty arguments. Keep this generous —
	 *  it is a CAP, not a spend; 32768 fits every current Claude model (haiku 4.5 caps at 64K). */
	UPROPERTY() int32 MaxOutputTokens = 32768;
	UPROPERTY() FString VisionModel;
};

USTRUCT()
struct FAIDALimitsConfig
{
	GENERATED_BODY()

	UPROPERTY() int32 PerPlayerPerMinute = 4;
	UPROPERTY() int32 GlobalPerMinute = 12;
	UPROPERTY() int32 MaxToolRoundTrips = 5;
	UPROPERTY() int64 DailyTokenBudget = 0; // 0 = unlimited
};

USTRUCT()
struct FAIDAPermissionsConfig
{
	GENERATED_BODY()

	UPROPERTY() FString Chat = TEXT("everyone");
	UPROPERTY() FString Query = TEXT("everyone");
	UPROPERTY() TArray<FString> Act; // allowlist of Epic account IDs
};

USTRUCT()
struct FAIDAPrivacyConfig
{
	GENERATED_BODY()

	UPROPERTY() bool bSendFactoryData = true;
	UPROPERTY() bool bSendPlayerNames = true;
	UPROPERTY() int32 SendChatHistoryDepth = 20;
	UPROPERTY() bool bLogPromptsToSidecar = false;
};

USTRUCT()
struct FAIDASnapshotsConfig
{
	GENERATED_BODY()

	UPROPERTY() int32 IntervalMinutes = 30;
	UPROPERTY() int32 Keep = 200;
};

USTRUCT()
struct FAIDAActionsConfig
{
	GENERATED_BODY()

	/** Master kill-switch: off ⇒ the propose_* tools are never registered. */
	UPROPERTY() bool bEnabled = true;
	UPROPERTY() int32 TtlSeconds = 600;
	/** "any-act" | "requester" | "list" (set when the JSON value is an id array → ApprovalIds). */
	UPROPERTY() FString ApprovalPolicy = TEXT("any-act");
	UPROPERTY() TArray<FString> ApprovalIds;
	/** Cap on expanded placements per proposal; 0 = unlimited (the default). */
	UPROPERTY() int32 MaxProposalItems = 0;
	UPROPERTY() int32 MaxPendingProposals = 3;
	/** Executor slice size per 10 Hz tick. */
	UPROPERTY() int32 BatchPerTick = 10;
	/** How many journal entries back /aida undo may reach. */
	UPROPERTY() int32 UndoWindow = 25;
	/** "central" (tally + deduct vs central storage) | "free" (report only, never deduct). */
	UPROPERTY() FString CostMode = TEXT("central");
};

USTRUCT()
struct FAIDAUploadsConfig
{
	GENERATED_BODY()

	/** Master gate: off ⇒ upload RPCs refuse and the attach UI affordance is hidden. */
	UPROPERTY() bool bEnabled = true;
	/** Post-normalization per-image byte ceiling (server-checked on commit). */
	UPROPERTY() int32 MaxImageBytes = 3 * 1024 * 1024;
	UPROPERTY() int32 MaxImagesPerMessage = 4;
	/** Cap on images attached across the whole history window of one LLM request (newest win). */
	UPROPERTY() int32 MaxImagesPerRequest = 4;
	/** Client downscale target (long edge, px); server rejects decoded images beyond 2× this. */
	UPROPERTY() int32 MaxDimension = 1568;
	UPROPERTY() int32 MaxStoredImages = 16;
	/** TTL for uploads never referenced by a sent message; referenced images follow the transcript. */
	UPROPERTY() int32 TtlSeconds = 600;
};

USTRUCT()
struct FAIDAPromptsConfig
{
	GENERATED_BODY()

	UPROPERTY() FString SystemPromptFile = TEXT("prompts/system.md");
	UPROPERTY() FString ToolsFile = TEXT("tools.json");
	/** Append the generated game data pack (recipes/buildings/logistics, docs/PROMPT.md §2) to the system prompt. */
	UPROPERTY() bool bPackEnabled = true;
};

USTRUCT()
struct FAIDAConfig
{
	GENERATED_BODY()

	UPROPERTY() FAIDAProviderConfig Provider;
	UPROPERTY() FAIDALimitsConfig Limits;
	UPROPERTY() FAIDAPermissionsConfig Permissions;
	UPROPERTY() FAIDAPrivacyConfig Privacy;
	UPROPERTY() FAIDASnapshotsConfig Snapshots;
	UPROPERTY() FAIDAActionsConfig Actions;
	UPROPERTY() FAIDAUploadsConfig Uploads;
	UPROPERTY() FAIDAPromptsConfig Prompts;
};
