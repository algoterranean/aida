#include "Tools/AIDAMapTools.h"

#include "Tools/AIDAToolJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	constexpr int32 MaxFreeNodesListed = 40;

	/** One (resource, purity) bucket's running tally. */
	struct FNodeTally
	{
		int32 Total = 0;
		int32 Free = 0;
	};
}

FString AIDAMapTools::BuildResourceNodesJson(const TArray<FAIDAResourceNode>& Nodes, const FString& ResourceFilter, bool bUntappedOnly)
{
	const bool bHasFilter = !ResourceFilter.IsEmpty();

	// Group by "Resource|Purity"; keep insertion of a stable, sorted key list for deterministic output.
	TMap<FString, FNodeTally> Tallies;
	TMap<FString, TPair<FString, FString>> KeyParts; // key -> (resource, purity) for emit
	int32 TotalMatched = 0;
	int32 TotalFree = 0;

	TArray<const FAIDAResourceNode*> FreeNodes;

	for (const FAIDAResourceNode& N : Nodes)
	{
		if (bHasFilter && !N.Resource.Contains(ResourceFilter, ESearchCase::IgnoreCase)) { continue; }
		if (bUntappedOnly && N.bOccupied) { continue; }

		++TotalMatched;
		const FString Key = N.Resource + TEXT("|") + N.Purity;
		FNodeTally& T = Tallies.FindOrAdd(Key);
		KeyParts.FindOrAdd(Key) = TPair<FString, FString>(N.Resource, N.Purity);
		++T.Total;
		if (!N.bOccupied) { ++T.Free; ++TotalFree; FreeNodes.Add(&N); }
	}

	TArray<FString> Keys;
	Tallies.GetKeys(Keys);
	Keys.Sort();

	TArray<TSharedPtr<FJsonValue>> Summary;
	for (const FString& Key : Keys)
	{
		const FNodeTally& T = Tallies[Key];
		const TPair<FString, FString>& Parts = KeyParts[Key];
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("resource"), Parts.Key);
		O->SetStringField(TEXT("purity"), Parts.Value);
		O->SetNumberField(TEXT("total"), T.Total);
		O->SetNumberField(TEXT("free"), T.Free);
		Summary.Add(MakeShared<FJsonValueObject>(O));
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("nodes"), TotalMatched);
	Root->SetNumberField(TEXT("free"), TotalFree);
	Root->SetArrayField(TEXT("byResource"), Summary);

	// When the caller specifically wants untapped nodes, list them (bounded) with grid coordinates.
	if (bUntappedOnly)
	{
		TArray<TSharedPtr<FJsonValue>> FreeList;
		const int32 Shown = FMath::Min(FreeNodes.Num(), MaxFreeNodesListed);
		for (int32 i = 0; i < Shown; ++i)
		{
			const FAIDAResourceNode& N = *FreeNodes[i];
			const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("resource"), N.Resource);
			O->SetStringField(TEXT("purity"), N.Purity);
			if (!N.Region.IsEmpty()) { O->SetStringField(TEXT("region"), N.Region); }
			TArray<TSharedPtr<FJsonValue>> GridArr;
			GridArr.Add(MakeShared<FJsonValueNumber>(N.Grid.X));
			GridArr.Add(MakeShared<FJsonValueNumber>(N.Grid.Y));
			O->SetArrayField(TEXT("grid"), GridArr);
			FreeList.Add(MakeShared<FJsonValueObject>(O));
		}
		Root->SetArrayField(TEXT("freeNodes"), FreeList);
		if (FreeNodes.Num() > Shown) { Root->SetNumberField(TEXT("freeNodesOmitted"), FreeNodes.Num() - Shown); }
	}

	return AIDAToCompactJson(Root);
}
