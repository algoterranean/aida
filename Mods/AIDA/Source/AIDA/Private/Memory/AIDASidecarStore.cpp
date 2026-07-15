#include "Memory/AIDASidecarStore.h"

#include "AIDA.h"
#include "Tools/AIDAToolJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

FString FAIDASidecarStore::SnapshotToJson(const FAIDASnapshot& Snapshot)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("t"), static_cast<double>(Snapshot.TakenUtc));
	if (!Snapshot.Label.IsEmpty()) { Root->SetStringField(TEXT("label"), Snapshot.Label); }
	Root->SetField(TEXT("powerConsumedMW"), AIDANumber(Snapshot.PowerConsumedMW));
	Root->SetField(TEXT("powerCapacityMW"), AIDANumber(Snapshot.PowerCapacityMW));

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAIDASnapshotItem& I : Snapshot.ItemBalance)
	{
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("item"), I.Item);
		O->SetField(TEXT("net"), AIDANumber(I.Net));
		Items.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("items"), Items);
	return AIDAToCompactJson(Root);
}

bool FAIDASidecarStore::SnapshotFromJson(const FString& Line, FAIDASnapshot& OutSnapshot)
{
	if (Line.TrimStartAndEnd().IsEmpty()) { return false; }

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) { return false; }

	OutSnapshot = FAIDASnapshot();
	OutSnapshot.TakenUtc = static_cast<int64>(Root->GetNumberField(TEXT("t")));
	Root->TryGetStringField(TEXT("label"), OutSnapshot.Label);
	Root->TryGetNumberField(TEXT("powerConsumedMW"), OutSnapshot.PowerConsumedMW);
	Root->TryGetNumberField(TEXT("powerCapacityMW"), OutSnapshot.PowerCapacityMW);

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (Root->TryGetArrayField(TEXT("items"), Items) && Items)
	{
		for (const TSharedPtr<FJsonValue>& V : *Items)
		{
			const TSharedPtr<FJsonObject> O = V.IsValid() ? V->AsObject() : nullptr;
			if (!O.IsValid()) { continue; }
			FAIDASnapshotItem Item;
			O->TryGetStringField(TEXT("item"), Item.Item);
			O->TryGetNumberField(TEXT("net"), Item.Net);
			OutSnapshot.ItemBalance.Add(MoveTemp(Item));
		}
	}
	return true;
}

void FAIDASidecarStore::AppendWithRingBuffer(TArray<FString>& Lines, const FString& NewLine, int32 KeepMax)
{
	Lines.Add(NewLine);
	if (KeepMax > 0 && Lines.Num() > KeepMax)
	{
		Lines.RemoveAt(0, Lines.Num() - KeepMax, EAllowShrinking::No);
	}
}

void FAIDASidecarStore::Init(const FString& SessionId)
{
	if (SessionId.IsEmpty())
	{
		BaseDir.Reset();
		return;
	}
	BaseDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Configs"), TEXT("AIDA"), TEXT("data"), SessionId);
}

FString FAIDASidecarStore::SnapshotsPath() const
{
	return FPaths::Combine(BaseDir, TEXT("snapshots.jsonl"));
}

bool FAIDASidecarStore::AppendSnapshot(const FAIDASnapshot& Snapshot, int32 KeepMax)
{
	if (!IsReady())
	{
		UE_LOG(LogAIDA, Warning, TEXT("[memory] sidecar not initialized; dropping snapshot."));
		return false;
	}

	IPlatformFile& FS = FPlatformFileManager::Get().GetPlatformFile();
	if (!FS.CreateDirectoryTree(*BaseDir))
	{
		UE_LOG(LogAIDA, Warning, TEXT("[memory] could not create sidecar dir '%s'."), *BaseDir);
		return false;
	}

	const FString Path = SnapshotsPath();
	TArray<FString> Lines;
	FFileHelper::LoadFileToStringArray(Lines, *Path); // ok if absent (Lines stays empty)
	AppendWithRingBuffer(Lines, SnapshotToJson(Snapshot), KeepMax);

	if (!FFileHelper::SaveStringArrayToFile(Lines, *Path))
	{
		UE_LOG(LogAIDA, Warning, TEXT("[memory] failed writing snapshots to '%s'."), *Path);
		return false;
	}
	return true;
}

TArray<FAIDASnapshot> FAIDASidecarStore::LoadSnapshots() const
{
	TArray<FAIDASnapshot> Out;
	if (!IsReady()) { return Out; }

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *SnapshotsPath())) { return Out; }

	for (const FString& Line : Lines)
	{
		FAIDASnapshot Snap;
		if (SnapshotFromJson(Line, Snap)) { Out.Add(MoveTemp(Snap)); }
	}
	return Out;
}
