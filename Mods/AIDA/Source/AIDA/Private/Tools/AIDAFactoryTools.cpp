#include "Tools/AIDAFactoryTools.h"

#include "Factory/AIDAFactoryAggregator.h"
#include "Tools/AIDAToolJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// Bounds so a tool result always fits comfortably in context (docs/ARCHITECTURE.md §4).
	constexpr int32 MaxClustersInOverview = 12;
	constexpr int32 MaxDeficits = 5;
	constexpr int32 MaxItemsInBalance = 40;
	constexpr int32 MaxMachineIdsInCluster = 60;

	/** Centroid humanized to whole metres (world units are cm), as [x, y] — Z dropped for the overview. */
	TArray<TSharedPtr<FJsonValue>> CentroidMetres(const FVector& Centroid)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Centroid.X / 100.0)));
		Arr.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Centroid.Y / 100.0)));
		return Arr;
	}

	/** [{ "item": .., "perMin": .. }, ...] for a rate list. */
	TArray<TSharedPtr<FJsonValue>> RatesToJson(const TArray<FAIDAItemRate>& Rates)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FAIDAItemRate& R : Rates)
		{
			const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("item"), R.Item);
			O->SetField(TEXT("perMin"), AIDANumber(R.PerMinute));
			Arr.Add(MakeShared<FJsonValueObject>(O));
		}
		return Arr;
	}

	/** The cluster's largest net export item ("—" if it exports nothing). */
	FString PrimaryOutput(const FAIDACluster& C)
	{
		const FAIDAItemRate* Best = nullptr;
		for (const FAIDAItemRate& R : C.NetOutputs)
		{
			if (!Best || R.PerMinute > Best->PerMinute) { Best = &R; }
		}
		return Best ? Best->Item : FString(TEXT("—"));
	}
}

FString AIDAFactoryTools::BuildOverviewJson(const FAIDAFactoryAggregates& Aggregates)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	int32 TotalMachines = 0;
	for (const FAIDACluster& C : Aggregates.Clusters) { TotalMachines += C.MachineIds.Num(); }
	Root->SetNumberField(TEXT("clusters"), Aggregates.Clusters.Num());
	Root->SetNumberField(TEXT("machines"), TotalMachines);

	// Largest clusters first (by machine count), capped; note any omitted.
	TArray<const FAIDACluster*> Sorted;
	Sorted.Reserve(Aggregates.Clusters.Num());
	for (const FAIDACluster& C : Aggregates.Clusters) { Sorted.Add(&C); }
	Sorted.Sort([](const FAIDACluster& A, const FAIDACluster& B)
	{
		if (A.MachineIds.Num() != B.MachineIds.Num()) { return A.MachineIds.Num() > B.MachineIds.Num(); }
		return A.Id < B.Id;
	});

	TArray<TSharedPtr<FJsonValue>> ClusterList;
	const int32 Shown = FMath::Min(Sorted.Num(), MaxClustersInOverview);
	for (int32 i = 0; i < Shown; ++i)
	{
		const FAIDACluster& C = *Sorted[i];
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("id"), C.Id);
		O->SetNumberField(TEXT("machines"), C.MachineIds.Num());
		O->SetArrayField(TEXT("centroid_m"), CentroidMetres(C.Centroid));
		O->SetStringField(TEXT("primaryOutput"), PrimaryOutput(C));
		O->SetField(TEXT("efficiency"), AIDANumber(C.Efficiency));
		ClusterList.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("clusterList"), ClusterList);
	if (Sorted.Num() > Shown) { Root->SetNumberField(TEXT("clustersOmitted"), Sorted.Num() - Shown); }

	// Biggest deficits (most negative net) first.
	TArray<const FAIDAItemBalance*> Deficits;
	for (const FAIDAItemBalance& B : Aggregates.Balance)
	{
		if (B.IsDeficit()) { Deficits.Add(&B); }
	}
	Deficits.Sort([](const FAIDAItemBalance& A, const FAIDAItemBalance& B) { return A.Net() < B.Net(); });
	TArray<TSharedPtr<FJsonValue>> DeficitArr;
	for (int32 i = 0; i < FMath::Min(Deficits.Num(), MaxDeficits); ++i)
	{
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("item"), Deficits[i]->Item);
		O->SetField(TEXT("deficit"), AIDANumber(-Deficits[i]->Net()));
		DeficitArr.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("topDeficits"), DeficitArr);

	// Power per circuit.
	TArray<TSharedPtr<FJsonValue>> PowerArr;
	for (const FAIDAPowerReport& R : Aggregates.Power)
	{
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("circuit"), R.CircuitId);
		O->SetField(TEXT("produced_MW"), AIDANumber(R.ProducedMW));
		O->SetField(TEXT("capacity_MW"), AIDANumber(R.CapacityMW));
		O->SetField(TEXT("consumed_MW"), AIDANumber(R.ConsumedMW));
		O->SetBoolField(TEXT("overloaded"), R.IsOverloaded());
		PowerArr.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("power"), PowerArr);

	return AIDAToCompactJson(Root);
}

FString AIDAFactoryTools::BuildItemBalanceJson(const FAIDAFactoryAggregates& Aggregates, const FString& ItemFilter)
{
	if (!ItemFilter.IsEmpty())
	{
		for (const FAIDAItemBalance& B : Aggregates.Balance)
		{
			if (B.Item.Equals(ItemFilter, ESearchCase::IgnoreCase))
			{
				const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("item"), B.Item);
				O->SetField(TEXT("produced"), AIDANumber(B.Produced));
				O->SetField(TEXT("consumed"), AIDANumber(B.Consumed));
				O->SetField(TEXT("net"), AIDANumber(B.Net()));
				O->SetBoolField(TEXT("deficit"), B.IsDeficit());
				return AIDAToCompactJson(O);
			}
		}
		const TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
		Err->SetStringField(TEXT("error"), FString::Printf(TEXT("no item '%s' in the factory balance"), *ItemFilter));
		return AIDAToCompactJson(Err);
	}

	// Whole-factory: biggest deficits first, then the rest, bounded.
	TArray<const FAIDAItemBalance*> Items;
	Items.Reserve(Aggregates.Balance.Num());
	for (const FAIDAItemBalance& B : Aggregates.Balance) { Items.Add(&B); }
	Items.Sort([](const FAIDAItemBalance& A, const FAIDAItemBalance& B) { return A.Net() < B.Net(); });

	TArray<TSharedPtr<FJsonValue>> Arr;
	const int32 Shown = FMath::Min(Items.Num(), MaxItemsInBalance);
	for (int32 i = 0; i < Shown; ++i)
	{
		const FAIDAItemBalance& B = *Items[i];
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("item"), B.Item);
		O->SetField(TEXT("produced"), AIDANumber(B.Produced));
		O->SetField(TEXT("consumed"), AIDANumber(B.Consumed));
		O->SetField(TEXT("net"), AIDANumber(B.Net()));
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("items"), Arr);
	if (Items.Num() > Shown) { Root->SetNumberField(TEXT("itemsOmitted"), Items.Num() - Shown); }
	return AIDAToCompactJson(Root);
}

FString AIDAFactoryTools::BuildClusterJson(const FAIDAFactoryAggregates& Aggregates, int32 ClusterId)
{
	const FAIDACluster* Cluster = Aggregates.Clusters.FindByPredicate(
		[ClusterId](const FAIDACluster& C) { return C.Id == ClusterId; });
	if (!Cluster)
	{
		const TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
		Err->SetStringField(TEXT("error"), FString::Printf(TEXT("no cluster with id %d"), ClusterId));
		return AIDAToCompactJson(Err);
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("id"), Cluster->Id);
	Root->SetNumberField(TEXT("machines"), Cluster->MachineIds.Num());
	Root->SetArrayField(TEXT("centroid_m"), CentroidMetres(Cluster->Centroid));
	Root->SetField(TEXT("efficiency"), AIDANumber(Cluster->Efficiency));

	// Census, ordered by building name for stability.
	TArray<FString> Buildings;
	Cluster->BuildingCensus.GetKeys(Buildings);
	Buildings.Sort();
	const TSharedRef<FJsonObject> Census = MakeShared<FJsonObject>();
	for (const FString& B : Buildings) { Census->SetNumberField(B, Cluster->BuildingCensus[B]); }
	Root->SetObjectField(TEXT("census"), Census);

	Root->SetArrayField(TEXT("netInputs"), RatesToJson(Cluster->NetInputs));
	Root->SetArrayField(TEXT("netOutputs"), RatesToJson(Cluster->NetOutputs));

	TArray<TSharedPtr<FJsonValue>> Ids;
	const int32 Shown = FMath::Min(Cluster->MachineIds.Num(), MaxMachineIdsInCluster);
	for (int32 i = 0; i < Shown; ++i) { Ids.Add(MakeShared<FJsonValueNumber>(Cluster->MachineIds[i])); }
	Root->SetArrayField(TEXT("machineIds"), Ids);
	if (Cluster->MachineIds.Num() > Shown) { Root->SetNumberField(TEXT("machineIdsOmitted"), Cluster->MachineIds.Num() - Shown); }

	return AIDAToCompactJson(Root);
}

FString AIDAFactoryTools::BuildBottleneckJson(const FAIDABottleneckResult& Result)
{
	const TCHAR* Status = TEXT("ok");
	FString Explanation;
	switch (Result.Kind)
	{
	case EAIDABottleneck::None:
		Status = TEXT("ok");
		Explanation = FString::Printf(TEXT("Producers of %s are keeping up; no clear bottleneck."), *Result.Item);
		break;
	case EAIDABottleneck::UnknownItem:
		Status = TEXT("unknown_item");
		Explanation = FString::Printf(TEXT("Nothing in the factory produces or consumes %s."), *Result.Item);
		break;
	case EAIDABottleneck::NoProducers:
		Status = TEXT("no_producers");
		Explanation = FString::Printf(TEXT("%s is consumed but nothing produces it."), *Result.Item);
		break;
	case EAIDABottleneck::Upstream:
		Status = TEXT("starved_upstream");
		Explanation = FString::Printf(TEXT("%s is limited upstream: %s is in deficit."), *Result.Item, *Result.LimitingDetail);
		break;
	case EAIDABottleneck::Power:
		Status = TEXT("power_limited");
		Explanation = FString::Printf(TEXT("%s's machines sit on overloaded power circuit %s."), *Result.Item, *Result.LimitingDetail);
		break;
	case EAIDABottleneck::OutputBackedUp:
		Status = TEXT("output_backed_up");
		Explanation = FString::Printf(TEXT("%s's machines are idle despite available inputs and power — output is backing up or demand-limited."), *Result.Item);
		break;
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("item"), Result.Item);
	Root->SetStringField(TEXT("status"), Status);
	Root->SetStringField(TEXT("explanation"), Explanation);
	Root->SetField(TEXT("produced"), AIDANumber(Result.Produced));
	Root->SetField(TEXT("consumed"), AIDANumber(Result.Consumed));
	Root->SetField(TEXT("net"), AIDANumber(Result.Net()));
	Root->SetNumberField(TEXT("producers"), Result.ProducerCount);
	Root->SetNumberField(TEXT("idle"), Result.ProducersIdle);
	Root->SetField(TEXT("avgProductivity"), AIDANumber(Result.AvgProductivity));
	if (!Result.LimitingDetail.IsEmpty()) { Root->SetStringField(TEXT("limiting"), Result.LimitingDetail); }
	if (Result.StarvedInputs.Num() > 0) { Root->SetArrayField(TEXT("starvedInputs"), RatesToJson(Result.StarvedInputs)); }

	return AIDAToCompactJson(Root);
}
