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
	UPROPERTY() int32 MaxOutputTokens = 1024;
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
	/** Hard cap on expanded placements per proposal. */
	UPROPERTY() int32 MaxProposalItems = 200;
	UPROPERTY() int32 MaxPendingProposals = 3;
	/** Executor slice size per 10 Hz tick. */
	UPROPERTY() int32 BatchPerTick = 10;
	/** How many journal entries back /aida undo may reach. */
	UPROPERTY() int32 UndoWindow = 25;
	/** "central" (tally + deduct vs central storage) | "free" (report only, never deduct). */
	UPROPERTY() FString CostMode = TEXT("central");
};

USTRUCT()
struct FAIDAPromptsConfig
{
	GENERATED_BODY()

	UPROPERTY() FString SystemPromptFile = TEXT("prompts/system.md");
	UPROPERTY() FString ToolsFile = TEXT("tools.json");
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
	UPROPERTY() FAIDAPromptsConfig Prompts;
};
