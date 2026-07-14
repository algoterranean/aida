#include "Tools/AIDAFactoryTools.h"

#include "Factory/AIDAFactoryAggregator.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// Bounds so a tool result always fits comfortably in context (docs/ARCHITECTURE.md §4).
	constexpr int32 MaxClustersInOverview = 12;
	constexpr int32 MaxDeficits = 5;
	constexpr int32 MaxItemsInBalance = 40;
	constexpr int32 MaxMachineIdsInCluster = 60;

	/** Round to one decimal place — plenty for rates/power and keeps token counts down. */
	double Round1(double V) { return FMath::RoundToDouble(V * 10.0) / 10.0; }

	FString ToCompactJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

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
			O->SetNumberField(TEXT("perMin"), Round1(R.PerMinute));
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
		O->SetNumberField(TEXT("efficiency"), Round1(C.Efficiency));
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
		O->SetNumberField(TEXT("deficit"), Round1(-Deficits[i]->Net()));
		DeficitArr.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("topDeficits"), DeficitArr);

	// Power per circuit.
	TArray<TSharedPtr<FJsonValue>> PowerArr;
	for (const FAIDAPowerReport& R : Aggregates.Power)
	{
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("circuit"), R.CircuitId);
		O->SetNumberField(TEXT("produced_MW"), Round1(R.ProducedMW));
		O->SetNumberField(TEXT("capacity_MW"), Round1(R.CapacityMW));
		O->SetNumberField(TEXT("consumed_MW"), Round1(R.ConsumedMW));
		O->SetBoolField(TEXT("overloaded"), R.IsOverloaded());
		PowerArr.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("power"), PowerArr);

	return ToCompactJson(Root);
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
				O->SetNumberField(TEXT("produced"), Round1(B.Produced));
				O->SetNumberField(TEXT("consumed"), Round1(B.Consumed));
				O->SetNumberField(TEXT("net"), Round1(B.Net()));
				O->SetBoolField(TEXT("deficit"), B.IsDeficit());
				return ToCompactJson(O);
			}
		}
		const TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
		Err->SetStringField(TEXT("error"), FString::Printf(TEXT("no item '%s' in the factory balance"), *ItemFilter));
		return ToCompactJson(Err);
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
		O->SetNumberField(TEXT("produced"), Round1(B.Produced));
		O->SetNumberField(TEXT("consumed"), Round1(B.Consumed));
		O->SetNumberField(TEXT("net"), Round1(B.Net()));
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("items"), Arr);
	if (Items.Num() > Shown) { Root->SetNumberField(TEXT("itemsOmitted"), Items.Num() - Shown); }
	return ToCompactJson(Root);
}

FString AIDAFactoryTools::BuildClusterJson(const FAIDAFactoryAggregates& Aggregates, int32 ClusterId)
{
	const FAIDACluster* Cluster = Aggregates.Clusters.FindByPredicate(
		[ClusterId](const FAIDACluster& C) { return C.Id == ClusterId; });
	if (!Cluster)
	{
		const TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
		Err->SetStringField(TEXT("error"), FString::Printf(TEXT("no cluster with id %d"), ClusterId));
		return ToCompactJson(Err);
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("id"), Cluster->Id);
	Root->SetNumberField(TEXT("machines"), Cluster->MachineIds.Num());
	Root->SetArrayField(TEXT("centroid_m"), CentroidMetres(Cluster->Centroid));
	Root->SetNumberField(TEXT("efficiency"), Round1(Cluster->Efficiency));

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

	return ToCompactJson(Root);
}
