#include "Tools/AIDANotesTools.h"

#include "Tools/AIDAToolJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	constexpr int32 MaxNotesListed = 20;

	bool NoteMatches(const FAIDANote& N, const FString& Keyword, const FString& Region)
	{
		if (!Region.IsEmpty() && !N.Region.Contains(Region, ESearchCase::IgnoreCase)) { return false; }
		if (!Keyword.IsEmpty())
		{
			if (N.Text.Contains(Keyword, ESearchCase::IgnoreCase)) { return true; }
			for (const FString& Tag : N.Tags)
			{
				if (Tag.Contains(Keyword, ESearchCase::IgnoreCase)) { return true; }
			}
			return false;
		}
		return true;
	}

	/** Humanize a positive age in seconds ("3d", "5h", "12m", "just now"). */
	FString AgeString(int64 Seconds)
	{
		if (Seconds < 60) { return TEXT("just now"); }
		if (Seconds < 3600) { return FString::Printf(TEXT("%lldm"), Seconds / 60); }
		if (Seconds < 86400) { return FString::Printf(TEXT("%lldh"), Seconds / 3600); }
		return FString::Printf(TEXT("%lldd"), Seconds / 86400);
	}
}

TArray<const FAIDANote*> AIDANotesTools::SelectNotes(const TArray<FAIDANote>& Notes, const FString& Keyword,
	const FString& Region, const FVector& From, bool bNear)
{
	TArray<const FAIDANote*> Selected;
	for (const FAIDANote& N : Notes)
	{
		if (NoteMatches(N, Keyword, Region)) { Selected.Add(&N); }
	}

	if (bNear)
	{
		Selected.Sort([&From](const FAIDANote& A, const FAIDANote& B)
		{
			return FVector::DistSquared(From, A.Location) < FVector::DistSquared(From, B.Location);
		});
	}
	else
	{
		Selected.Sort([](const FAIDANote& A, const FAIDANote& B) { return A.CreatedUtc > B.CreatedUtc; });
	}
	return Selected;
}

FString AIDANotesTools::BuildNotesJson(const TArray<FAIDANote>& Notes, const FString& Keyword,
	const FString& Region, const FVector& From, bool bNear, int64 NowUtc)
{
	const TArray<const FAIDANote*> Selected = AIDANotesTools::SelectNotes(Notes, Keyword, Region, From, bNear);
	const int32 Shown = FMath::Min(Selected.Num(), MaxNotesListed);

	TArray<TSharedPtr<FJsonValue>> List;
	for (int32 i = 0; i < Shown; ++i)
	{
		const FAIDANote& N = *Selected[i];
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("text"), N.Text);
		if (!N.Author.IsEmpty()) { O->SetStringField(TEXT("author"), N.Author); }
		if (!N.Region.IsEmpty()) { O->SetStringField(TEXT("region"), N.Region); }
		if (N.CreatedUtc > 0 && NowUtc >= N.CreatedUtc) { O->SetStringField(TEXT("age"), AgeString(NowUtc - N.CreatedUtc)); }
		if (bNear) { O->SetField(TEXT("distanceM"), AIDANumber(FVector::Dist(From, N.Location) / 100.0)); }
		if (N.Tags.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Tags;
			for (const FString& T : N.Tags) { Tags.Add(MakeShared<FJsonValueString>(T)); }
			O->SetArrayField(TEXT("tags"), Tags);
		}
		List.Add(MakeShared<FJsonValueObject>(O));
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("matches"), Selected.Num());
	Root->SetArrayField(TEXT("notes"), List);
	if (Selected.Num() > Shown) { Root->SetNumberField(TEXT("omitted"), Selected.Num() - Shown); }
	return AIDAToCompactJson(Root);
}
