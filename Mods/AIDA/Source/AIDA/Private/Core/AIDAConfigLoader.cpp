#include "Core/AIDAConfigLoader.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FString FAIDAConfigLoader::StripJsonComments(const FString& Input)
{
	FString Out;
	Out.Reserve(Input.Len());

	bool bInString = false;
	bool bEscaped = false;
	const int32 Len = Input.Len();

	for (int32 i = 0; i < Len; ++i)
	{
		const TCHAR C = Input[i];
		const TCHAR Next = (i + 1 < Len) ? Input[i + 1] : TEXT('\0');

		if (bInString)
		{
			Out.AppendChar(C);
			if (bEscaped)            { bEscaped = false; }
			else if (C == TEXT('\\')) { bEscaped = true; }
			else if (C == TEXT('"'))  { bInString = false; }
			continue;
		}

		if (C == TEXT('"'))
		{
			bInString = true;
			Out.AppendChar(C);
			continue;
		}

		// Line comment: // ... to end of line. The newline is preserved.
		if (C == TEXT('/') && Next == TEXT('/'))
		{
			i += 2;
			while (i < Len && Input[i] != TEXT('\n') && Input[i] != TEXT('\r')) { ++i; }
			--i; // let the outer ++i land on (and keep) the line break
			continue;
		}

		// Block comment: /* ... */
		if (C == TEXT('/') && Next == TEXT('*'))
		{
			i += 2;
			while (i + 1 < Len && !(Input[i] == TEXT('*') && Input[i + 1] == TEXT('/'))) { ++i; }
			++i; // skip the closing '/'
			continue;
		}

		Out.AppendChar(C);
	}

	return Out;
}

bool FAIDAConfigLoader::LoadFromString(const FString& Jsonc, FAIDAConfig& OutConfig, FString& OutError)
{
	const FString Json = StripJsonComments(Jsonc);

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("config is not valid JSON after comment stripping");
		return false;
	}

	// Parse explicitly against the documented camelCase keys (docs/ARCHITECTURE.md §7).
	// We do NOT use FJsonObjectConverter: its case-standardization does not strip Unreal's
	// bool 'b' prefix on read, so "sendFactoryData" would silently miss bSendFactoryData.
	FAIDAConfig Parsed;

	const TSharedPtr<FJsonObject>* Provider = nullptr;
	if (Root->TryGetObjectField(TEXT("provider"), Provider) && Provider)
	{
		(*Provider)->TryGetStringField(TEXT("type"), Parsed.Provider.Type);
		(*Provider)->TryGetStringField(TEXT("baseUrl"), Parsed.Provider.BaseUrl);
		(*Provider)->TryGetStringField(TEXT("apiKey"), Parsed.Provider.ApiKey);
		(*Provider)->TryGetStringField(TEXT("model"), Parsed.Provider.Model);
		(*Provider)->TryGetStringField(TEXT("visionModel"), Parsed.Provider.VisionModel);
		int32 MaxTokens;
		if ((*Provider)->TryGetNumberField(TEXT("maxOutputTokens"), MaxTokens)) { Parsed.Provider.MaxOutputTokens = MaxTokens; }
	}

	const TSharedPtr<FJsonObject>* Limits = nullptr;
	if (Root->TryGetObjectField(TEXT("limits"), Limits) && Limits)
	{
		int32 I; int64 I64;
		if ((*Limits)->TryGetNumberField(TEXT("perPlayerPerMinute"), I)) { Parsed.Limits.PerPlayerPerMinute = I; }
		if ((*Limits)->TryGetNumberField(TEXT("globalPerMinute"), I)) { Parsed.Limits.GlobalPerMinute = I; }
		if ((*Limits)->TryGetNumberField(TEXT("maxToolRoundTrips"), I)) { Parsed.Limits.MaxToolRoundTrips = I; }
		if ((*Limits)->TryGetNumberField(TEXT("dailyTokenBudget"), I64)) { Parsed.Limits.DailyTokenBudget = I64; }
	}

	const TSharedPtr<FJsonObject>* Permissions = nullptr;
	if (Root->TryGetObjectField(TEXT("permissions"), Permissions) && Permissions)
	{
		(*Permissions)->TryGetStringField(TEXT("chat"), Parsed.Permissions.Chat);
		(*Permissions)->TryGetStringField(TEXT("query"), Parsed.Permissions.Query);
		const TArray<TSharedPtr<FJsonValue>>* Act = nullptr;
		if ((*Permissions)->TryGetArrayField(TEXT("act"), Act) && Act)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Act)
			{
				FString Id;
				if (Value.IsValid() && Value->TryGetString(Id)) { Parsed.Permissions.Act.Add(Id); }
			}
		}
	}

	const TSharedPtr<FJsonObject>* Privacy = nullptr;
	if (Root->TryGetObjectField(TEXT("privacy"), Privacy) && Privacy)
	{
		(*Privacy)->TryGetBoolField(TEXT("sendFactoryData"), Parsed.Privacy.bSendFactoryData);
		(*Privacy)->TryGetBoolField(TEXT("sendPlayerNames"), Parsed.Privacy.bSendPlayerNames);
		int32 Depth;
		if ((*Privacy)->TryGetNumberField(TEXT("sendChatHistoryDepth"), Depth)) { Parsed.Privacy.SendChatHistoryDepth = Depth; }
		(*Privacy)->TryGetBoolField(TEXT("logPromptsToSidecar"), Parsed.Privacy.bLogPromptsToSidecar);
	}

	const TSharedPtr<FJsonObject>* Snapshots = nullptr;
	if (Root->TryGetObjectField(TEXT("snapshots"), Snapshots) && Snapshots)
	{
		int32 I;
		if ((*Snapshots)->TryGetNumberField(TEXT("intervalMinutes"), I)) { Parsed.Snapshots.IntervalMinutes = I; }
		if ((*Snapshots)->TryGetNumberField(TEXT("keep"), I)) { Parsed.Snapshots.Keep = I; }
	}

	const TSharedPtr<FJsonObject>* Actions = nullptr;
	if (Root->TryGetObjectField(TEXT("actions"), Actions) && Actions)
	{
		int32 I;
		(*Actions)->TryGetBoolField(TEXT("enabled"), Parsed.Actions.bEnabled);
		if ((*Actions)->TryGetNumberField(TEXT("ttlSeconds"), I)) { Parsed.Actions.TtlSeconds = I; }
		if ((*Actions)->TryGetNumberField(TEXT("maxProposalItems"), I)) { Parsed.Actions.MaxProposalItems = I; }
		if ((*Actions)->TryGetNumberField(TEXT("maxPendingProposals"), I)) { Parsed.Actions.MaxPendingProposals = I; }
		if ((*Actions)->TryGetNumberField(TEXT("batchPerTick"), I)) { Parsed.Actions.BatchPerTick = I; }
		if ((*Actions)->TryGetNumberField(TEXT("undoWindow"), I)) { Parsed.Actions.UndoWindow = I; }
		(*Actions)->TryGetStringField(TEXT("costMode"), Parsed.Actions.CostMode);

		// approvalPolicy is either a mode string ("any-act" | "requester") or an explicit id array.
		FString Policy;
		const TArray<TSharedPtr<FJsonValue>>* PolicyIds = nullptr;
		if ((*Actions)->TryGetStringField(TEXT("approvalPolicy"), Policy))
		{
			Parsed.Actions.ApprovalPolicy = Policy;
		}
		else if ((*Actions)->TryGetArrayField(TEXT("approvalPolicy"), PolicyIds) && PolicyIds)
		{
			Parsed.Actions.ApprovalPolicy = TEXT("list");
			for (const TSharedPtr<FJsonValue>& Value : *PolicyIds)
			{
				FString Id;
				if (Value.IsValid() && Value->TryGetString(Id)) { Parsed.Actions.ApprovalIds.Add(Id); }
			}
		}
	}

	const TSharedPtr<FJsonObject>* Uploads = nullptr;
	if (Root->TryGetObjectField(TEXT("uploads"), Uploads) && Uploads)
	{
		int32 I;
		(*Uploads)->TryGetBoolField(TEXT("enabled"), Parsed.Uploads.bEnabled);
		if ((*Uploads)->TryGetNumberField(TEXT("maxImageBytes"), I)) { Parsed.Uploads.MaxImageBytes = I; }
		if ((*Uploads)->TryGetNumberField(TEXT("maxImagesPerMessage"), I)) { Parsed.Uploads.MaxImagesPerMessage = I; }
		if ((*Uploads)->TryGetNumberField(TEXT("maxImagesPerRequest"), I)) { Parsed.Uploads.MaxImagesPerRequest = I; }
		if ((*Uploads)->TryGetNumberField(TEXT("maxDimension"), I)) { Parsed.Uploads.MaxDimension = I; }
		if ((*Uploads)->TryGetNumberField(TEXT("maxStoredImages"), I)) { Parsed.Uploads.MaxStoredImages = I; }
		if ((*Uploads)->TryGetNumberField(TEXT("ttlSeconds"), I)) { Parsed.Uploads.TtlSeconds = I; }
	}

	const TSharedPtr<FJsonObject>* Prompts = nullptr;
	if (Root->TryGetObjectField(TEXT("prompts"), Prompts) && Prompts)
	{
		(*Prompts)->TryGetStringField(TEXT("systemPromptFile"), Parsed.Prompts.SystemPromptFile);
		(*Prompts)->TryGetStringField(TEXT("toolsFile"), Parsed.Prompts.ToolsFile);
		(*Prompts)->TryGetBoolField(TEXT("packEnabled"), Parsed.Prompts.bPackEnabled);
	}

	if (!Validate(Parsed, OutError))
	{
		return false;
	}

	OutConfig = MoveTemp(Parsed);
	return true;
}

bool FAIDAConfigLoader::LoadFromFile(const FString& FilePath, FAIDAConfig& OutConfig, FString& OutError)
{
	if (!FPaths::FileExists(FilePath))
	{
		OutError = FString::Printf(TEXT("config file not found: %s"), *FilePath);
		return false;
	}

	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *FilePath))
	{
		OutError = FString::Printf(TEXT("failed to read config file: %s"), *FilePath);
		return false;
	}

	return LoadFromString(Raw, OutConfig, OutError);
}

bool FAIDAConfigLoader::Validate(const FAIDAConfig& Config, FString& OutError)
{
	const FString& Type = Config.Provider.Type;
	if (Type != TEXT("openai-compatible") && Type != TEXT("anthropic"))
	{
		OutError = FString::Printf(TEXT("provider.type must be 'openai-compatible' or 'anthropic', got '%s'"), *Type);
		return false;
	}
	if (Config.Provider.BaseUrl.IsEmpty())
	{
		OutError = TEXT("provider.baseUrl is empty");
		return false;
	}
	if (Config.Provider.Model.IsEmpty())
	{
		OutError = TEXT("provider.model is empty");
		return false;
	}
	if (Config.Limits.MaxToolRoundTrips < 0)
	{
		OutError = TEXT("limits.maxToolRoundTrips must be >= 0");
		return false;
	}

	const FString& CostMode = Config.Actions.CostMode;
	if (CostMode != TEXT("central") && CostMode != TEXT("free"))
	{
		OutError = FString::Printf(TEXT("actions.costMode must be 'central' or 'free', got '%s'"), *CostMode);
		return false;
	}
	const FString& Approval = Config.Actions.ApprovalPolicy;
	if (Approval != TEXT("any-act") && Approval != TEXT("requester") && Approval != TEXT("list"))
	{
		OutError = FString::Printf(TEXT("actions.approvalPolicy must be 'any-act', 'requester', or an id array, got '%s'"), *Approval);
		return false;
	}
	if (Config.Actions.MaxProposalItems < 0 || Config.Actions.BatchPerTick < 1 || Config.Actions.MaxPendingProposals < 1)
	{
		OutError = TEXT("actions.maxProposalItems must be >= 0 (0 = unlimited); batchPerTick and maxPendingProposals must be >= 1");
		return false;
	}

	const FAIDAUploadsConfig& Up = Config.Uploads;
	if (Up.MaxImageBytes < 1024 || Up.MaxImagesPerMessage < 1 || Up.MaxImagesPerRequest < 1
		|| Up.MaxDimension < 64 || Up.MaxStoredImages < 1 || Up.TtlSeconds < 1)
	{
		OutError = TEXT("uploads.* limits out of range (maxImageBytes >= 1024, maxDimension >= 64, counts/ttl >= 1)");
		return false;
	}

	return true;
}
