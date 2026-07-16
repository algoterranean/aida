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

const FAIDAResourceNode* AIDAMapTools::FindNearestUntapped(const TArray<FAIDAResourceNode>& Nodes, const FString& ResourceFilter,
	const FString& PurityFilter, const FVector& From, bool bHasFrom)
{
	const FAIDAResourceNode* Best = nullptr;
	double BestDistSq = TNumericLimits<double>::Max();

	for (const FAIDAResourceNode& N : Nodes)
	{
		if (N.bOccupied) { continue; }
		if (!ResourceFilter.IsEmpty() && !N.Resource.Contains(ResourceFilter, ESearchCase::IgnoreCase)) { continue; }
		if (!PurityFilter.IsEmpty() && !N.Purity.Equals(PurityFilter, ESearchCase::IgnoreCase)) { continue; }

		if (!bHasFrom)
		{
			return &N; // no origin → first match (deterministic given the scan order)
		}
		const double DistSq = FVector::DistSquared(From, N.Location);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = &N;
		}
	}
	return Best;
}

FString AIDAMapTools::BuildTerrainProbeJson(const TArray<double>& HeightsM, int32 Cols, int32 Rows,
	double CenterXM, double CenterYM, double StepM)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetField(TEXT("centerX"), AIDANumber(CenterXM));
	Root->SetField(TEXT("centerY"), AIDANumber(CenterYM));
	Root->SetField(TEXT("stepM"), AIDANumber(StepM));
	Root->SetNumberField(TEXT("cols"), Cols);
	Root->SetNumberField(TEXT("rows"), Rows);

	double MinZ = TNumericLimits<double>::Max();
	double MaxZ = TNumericLimits<double>::Lowest();
	int32 Misses = 0;

	TArray<TSharedPtr<FJsonValue>> RowValues;
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		TArray<TSharedPtr<FJsonValue>> Cells;
		for (int32 Col = 0; Col < Cols; ++Col)
		{
			const int32 Index = Row * Cols + Col;
			const double Height = HeightsM.IsValidIndex(Index) ? HeightsM[Index] : AIDATerrainNoHit;
			if (Height <= AIDATerrainNoHit * 0.5)
			{
				++Misses;
				Cells.Add(MakeShared<FJsonValueNull>());
				continue;
			}
			MinZ = FMath::Min(MinZ, Height);
			MaxZ = FMath::Max(MaxZ, Height);
			Cells.Add(AIDANumber(Height));
		}
		RowValues.Add(MakeShared<FJsonValueArray>(Cells));
	}
	Root->SetArrayField(TEXT("heights"), RowValues);

	if (MinZ <= MaxZ)
	{
		Root->SetField(TEXT("minZ"), AIDANumber(MinZ));
		Root->SetField(TEXT("maxZ"), AIDANumber(MaxZ));
		Root->SetField(TEXT("spreadZ"), AIDANumber(MaxZ - MinZ));
	}
	if (Misses > 0) { Root->SetNumberField(TEXT("noHit"), Misses); }
	Root->SetStringField(TEXT("legend"),
		TEXT("ground z in metres; row 0 = north edge (-Y), rows advance south, columns run west->east (+X); null = nothing under that point"));
	return AIDAToCompactJson(Root);
}
