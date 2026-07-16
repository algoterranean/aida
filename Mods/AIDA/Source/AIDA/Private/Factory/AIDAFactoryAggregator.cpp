#include "Factory/AIDAFactoryAggregator.h"

namespace
{
	// DBSCAN label sentinels (real cluster ids are >= 0).
	constexpr int32 AIDA_LABEL_UNASSIGNED = -1;
	constexpr int32 AIDA_LABEL_NOISE = -2;

	/** Add a rate into a per-item accumulator (creates the entry at 0 on first sight). */
	void AccumulateRate(TMap<FString, double>& Map, const FString& Item, double Rate)
	{
		Map.FindOrAdd(Item) += Rate;
	}

	/** Stable ordering for item-rate lists so tests and cache-keys are deterministic. */
	void SortRatesByItem(TArray<FAIDAItemRate>& Rates)
	{
		Rates.Sort([](const FAIDAItemRate& A, const FAIDAItemRate& B) { return A.Item < B.Item; });
	}

	/**
	 * Net a set of machines' inputs/outputs into export (NetOutputs) and import (NetInputs) lists.
	 * An item produced more than consumed is a net export; the reverse is a net import (magnitude
	 * positive). Items within Tolerance of balanced are dropped.
	 */
	void NetMachineFlows(const TArray<FAIDAMachine>& Machines, const TArray<int32>& Indices, double Tolerance,
		TArray<FAIDAItemRate>& OutInputs, TArray<FAIDAItemRate>& OutOutputs)
	{
		TMap<FString, double> Produced;
		TMap<FString, double> Consumed;
		for (int32 Idx : Indices)
		{
			const FAIDAMachine& M = Machines[Idx];
			for (const FAIDAItemRate& R : M.Outputs) { AccumulateRate(Produced, R.Item, R.PerMinute); }
			for (const FAIDAItemRate& R : M.Inputs) { AccumulateRate(Consumed, R.Item, R.PerMinute); }
		}

		// Union of item keys across produced + consumed.
		TSet<FString> Items;
		for (const TPair<FString, double>& P : Produced) { Items.Add(P.Key); }
		for (const TPair<FString, double>& P : Consumed) { Items.Add(P.Key); }

		for (const FString& Item : Items)
		{
			const double Net = Produced.FindRef(Item) - Consumed.FindRef(Item);
			if (Net > Tolerance) { OutOutputs.Add({ Item, Net }); }
			else if (Net < -Tolerance) { OutInputs.Add({ Item, -Net }); }
		}
		SortRatesByItem(OutInputs);
		SortRatesByItem(OutOutputs);
	}
}

TArray<int32> FAIDAFactoryAggregator::ClusterMachines(const TArray<FAIDAMachine>& Machines, const FAIDAAggregatorConfig& Config)
{
	const int32 N = Machines.Num();
	TArray<int32> Labels;
	Labels.Init(AIDA_LABEL_UNASSIGNED, N);
	if (N == 0)
	{
		return Labels;
	}

	const double Eps2 = Config.ClusterEpsilon * Config.ClusterEpsilon;
	const int32 MinPts = FMath::Max(1, Config.MinClusterPoints);

	// O(n^2) neighbourhood query — fine for factory-scale counts; swap for a spatial grid if it ever bites.
	auto RegionQuery = [&Machines, N, Eps2](int32 P, TArray<int32>& Out)
	{
		Out.Reset();
		for (int32 Q = 0; Q < N; ++Q)
		{
			if (FVector::DistSquared(Machines[P].Location, Machines[Q].Location) <= Eps2)
			{
				Out.Add(Q);
			}
		}
	};

	TBitArray<> Visited(false, N);
	int32 ClusterCount = 0;
	TArray<int32> Neighbors, Seeds, QN;

	for (int32 P = 0; P < N; ++P)
	{
		if (Visited[P]) { continue; }
		Visited[P] = true;

		RegionQuery(P, Neighbors);
		if (Neighbors.Num() < MinPts)
		{
			Labels[P] = AIDA_LABEL_NOISE; // may be reclaimed as a border point later
			continue;
		}

		const int32 C = ClusterCount++;
		Labels[P] = C;

		// Seed the expansion with P's neighbours; InSeeds keeps the growing seed list duplicate-free in O(1).
		Seeds.Reset();
		TBitArray<> InSeeds(false, N);
		for (int32 R : Neighbors)
		{
			if (R != P) { InSeeds[R] = true; Seeds.Add(R); }
		}

		for (int32 i = 0; i < Seeds.Num(); ++i)
		{
			const int32 Q = Seeds[i];
			if (Labels[Q] == AIDA_LABEL_NOISE) { Labels[Q] = C; } // noise touching a cluster becomes a border

			if (!Visited[Q])
			{
				Visited[Q] = true;
				RegionQuery(Q, QN);
				if (QN.Num() >= MinPts) // Q is a core point: its neighbours join the cluster's frontier
				{
					for (int32 R : QN)
					{
						if (!InSeeds[R]) { InSeeds[R] = true; Seeds.Add(R); }
					}
				}
			}
			if (Labels[Q] == AIDA_LABEL_UNASSIGNED) { Labels[Q] = C; }
		}
	}

	// Any machine still noise is sparse enough to stand alone — give it its own singleton cluster so
	// every machine is reported somewhere.
	for (int32 P = 0; P < N; ++P)
	{
		if (Labels[P] == AIDA_LABEL_NOISE) { Labels[P] = ClusterCount++; }
	}
	return Labels;
}

TArray<FAIDACluster> FAIDAFactoryAggregator::BuildClusters(const TArray<FAIDAMachine>& Machines,
	const TArray<int32>& ClusterOfMachine, const FAIDAAggregatorConfig& Config)
{
	check(Machines.Num() == ClusterOfMachine.Num());

	int32 ClusterCount = 0;
	for (int32 Label : ClusterOfMachine) { ClusterCount = FMath::Max(ClusterCount, Label + 1); }

	// Bucket machine indices by cluster id.
	TArray<TArray<int32>> Members;
	Members.SetNum(ClusterCount);
	for (int32 i = 0; i < Machines.Num(); ++i) { Members[ClusterOfMachine[i]].Add(i); }

	TArray<FAIDACluster> Clusters;
	Clusters.Reserve(ClusterCount);
	for (int32 c = 0; c < ClusterCount; ++c)
	{
		const TArray<int32>& Idx = Members[c];
		FAIDACluster Cluster;
		Cluster.Id = c;

		FVector Sum = FVector::ZeroVector;
		double EfficiencySum = 0.0;
		for (int32 i : Idx)
		{
			const FAIDAMachine& M = Machines[i];
			Cluster.MachineIds.Add(M.Id);
			Cluster.BuildingCensus.FindOrAdd(M.BuildingClass) += 1;
			Sum += M.Location;
			EfficiencySum += M.Productivity;
		}
		if (Idx.Num() > 0)
		{
			Cluster.Centroid = Sum / Idx.Num();
			Cluster.Efficiency = static_cast<float>(EfficiencySum / Idx.Num());
		}
		NetMachineFlows(Machines, Idx, Config.RateTolerance, Cluster.NetInputs, Cluster.NetOutputs);
		Clusters.Add(MoveTemp(Cluster));
	}
	return Clusters;
}

TArray<FAIDAItemBalance> FAIDAFactoryAggregator::BuildBalanceSheet(const TArray<FAIDAMachine>& Machines,
	const FAIDAAggregatorConfig& Config)
{
	TMap<FString, double> Produced;
	TMap<FString, double> Consumed;
	for (const FAIDAMachine& M : Machines)
	{
		for (const FAIDAItemRate& R : M.Outputs) { AccumulateRate(Produced, R.Item, R.PerMinute); }
		for (const FAIDAItemRate& R : M.Inputs) { AccumulateRate(Consumed, R.Item, R.PerMinute); }
	}

	TSet<FString> Items;
	for (const TPair<FString, double>& P : Produced) { Items.Add(P.Key); }
	for (const TPair<FString, double>& P : Consumed) { Items.Add(P.Key); }

	TArray<FAIDAItemBalance> Balance;
	Balance.Reserve(Items.Num());
	for (const FString& Item : Items)
	{
		FAIDAItemBalance B;
		B.Item = Item;
		B.Produced = Produced.FindRef(Item);
		B.Consumed = Consumed.FindRef(Item);
		Balance.Add(MoveTemp(B));
	}
	Balance.Sort([](const FAIDAItemBalance& A, const FAIDAItemBalance& B) { return A.Item < B.Item; });
	return Balance;
}

TArray<FAIDALogisticsFlow> FAIDAFactoryAggregator::BuildLogistics(const TArray<FAIDAConveyorEdge>& Edges,
	const TMap<int32, int32>& ClusterOfMachineId)
{
	// Accumulate throughput per (fromCluster, toCluster, item); drop intra-cluster and dangling edges.
	struct FFlowKey
	{
		int32 From; int32 To; FString Item;
		bool operator==(const FFlowKey& O) const { return From == O.From && To == O.To && Item == O.Item; }
	};
	struct FFlowKeyFuncs : BaseKeyFuncs<TPair<FFlowKey, double>, FFlowKey>
	{
		static const FFlowKey& GetSetKey(const TPair<FFlowKey, double>& E) { return E.Key; }
		static bool Matches(const FFlowKey& A, const FFlowKey& B) { return A == B; }
		static uint32 GetKeyHash(const FFlowKey& K)
		{
			return HashCombine(HashCombine(GetTypeHash(K.From), GetTypeHash(K.To)), GetTypeHash(K.Item));
		}
	};

	TMap<FFlowKey, double, FDefaultSetAllocator, FFlowKeyFuncs> Totals;
	for (const FAIDAConveyorEdge& E : Edges)
	{
		const int32* From = ClusterOfMachineId.Find(E.FromMachine);
		const int32* To = ClusterOfMachineId.Find(E.ToMachine);
		if (!From || !To || *From == *To) { continue; } // dangling or internal edge
		Totals.FindOrAdd(FFlowKey{ *From, *To, E.Item }) += E.PerMinute;
	}

	TArray<FAIDALogisticsFlow> Flows;
	Flows.Reserve(Totals.Num());
	for (const TPair<FFlowKey, double>& P : Totals)
	{
		Flows.Add({ P.Key.From, P.Key.To, P.Key.Item, P.Value });
	}
	Flows.Sort([](const FAIDALogisticsFlow& A, const FAIDALogisticsFlow& B)
	{
		if (A.FromCluster != B.FromCluster) { return A.FromCluster < B.FromCluster; }
		if (A.ToCluster != B.ToCluster) { return A.ToCluster < B.ToCluster; }
		return A.Item < B.Item;
	});
	return Flows;
}

TArray<FAIDAPowerReport> FAIDAFactoryAggregator::BuildPowerReport(const TArray<FAIDAPowerCircuitStats>& Circuits)
{
	// One report per circuit, straight from the game's authoritative circuit stats (produced / capacity /
	// consumed / battery). Circuit id 0 is a real single-grid id, so we do NOT filter it out.
	TArray<FAIDAPowerReport> Reports;
	Reports.Reserve(Circuits.Num());
	for (const FAIDAPowerCircuitStats& S : Circuits)
	{
		FAIDAPowerReport R;
		R.CircuitId = S.CircuitId;
		R.ProducedMW = S.ProducedMW;
		R.CapacityMW = S.CapacityMW;
		R.ConsumedMW = S.ConsumedMW;
		R.BatteryMWh = S.BatteryMWh;
		R.BatteryDrainSeconds = S.BatteryDrainSeconds;
		Reports.Add(MoveTemp(R));
	}
	Reports.Sort([](const FAIDAPowerReport& A, const FAIDAPowerReport& B) { return A.CircuitId < B.CircuitId; });
	return Reports;
}

FAIDABottleneckResult FAIDAFactoryAggregator::FindBottleneck(const FAIDAFactorySnapshot& Snapshot,
	const FString& Item, const FAIDAAggregatorConfig& Config)
{
	FAIDABottleneckResult Result;
	Result.Item = Item;

	const TArray<FAIDAItemBalance> Balance = BuildBalanceSheet(Snapshot.Machines, Config);
	auto FindBalance = [&Balance](const FString& Key) -> const FAIDAItemBalance*
	{
		return Balance.FindByPredicate([&Key](const FAIDAItemBalance& B) { return B.Item.Equals(Key, ESearchCase::IgnoreCase); });
	};

	const FAIDAItemBalance* Target = FindBalance(Item);
	if (!Target)
	{
		Result.Kind = EAIDABottleneck::UnknownItem;
		return Result;
	}
	Result.Produced = Target->Produced;
	Result.Consumed = Target->Consumed;

	// Machines that output the item.
	TArray<const FAIDAMachine*> Producers;
	for (const FAIDAMachine& M : Snapshot.Machines)
	{
		for (const FAIDAItemRate& O : M.Outputs)
		{
			if (O.Item.Equals(Item, ESearchCase::IgnoreCase)) { Producers.Add(&M); break; }
		}
	}
	Result.ProducerCount = Producers.Num();
	if (Producers.Num() == 0)
	{
		Result.Kind = EAIDABottleneck::NoProducers;
		return Result;
	}

	double ProductivitySum = 0.0;
	TSet<FString> InputItems;
	TSet<int32> ProducerCircuits;
	for (const FAIDAMachine* M : Producers)
	{
		if (!M->bProducing) { ++Result.ProducersIdle; }
		ProductivitySum += M->Productivity;
		for (const FAIDAItemRate& In : M->Inputs) { InputItems.Add(In.Item); }
		if (M->CircuitId != 0) { ProducerCircuits.Add(M->CircuitId); }
	}
	Result.AvgProductivity = static_cast<float>(ProductivitySum / Producers.Num());

	// Upstream: producer inputs that are themselves in factory-wide deficit.
	for (const FString& In : InputItems)
	{
		if (const FAIDAItemBalance* B = FindBalance(In))
		{
			if (B->IsDeficit(Config.RateTolerance)) { Result.StarvedInputs.Add({ In, -B->Net() }); }
		}
	}
	Result.StarvedInputs.Sort([](const FAIDAItemRate& A, const FAIDAItemRate& B) { return A.PerMinute > B.PerMinute; });

	// Power: any producer circuit over capacity?
	const TArray<FAIDAPowerReport> Power = BuildPowerReport(Snapshot.Circuits);
	int32 OverloadedCircuit = -1;
	for (const FAIDAPowerReport& P : Power)
	{
		if (ProducerCircuits.Contains(P.CircuitId) && P.IsOverloaded()) { OverloadedCircuit = P.CircuitId; break; }
	}

	// Attribute the constraint — upstream deficit is the root cause, so it wins over local symptoms.
	if (Result.StarvedInputs.Num() > 0)
	{
		Result.Kind = EAIDABottleneck::Upstream;
		Result.LimitingDetail = Result.StarvedInputs[0].Item;
	}
	else if (OverloadedCircuit >= 0)
	{
		Result.Kind = EAIDABottleneck::Power;
		Result.LimitingDetail = FString::FromInt(OverloadedCircuit);
	}
	else if (Result.ProducersIdle > 0)
	{
		Result.Kind = EAIDABottleneck::OutputBackedUp;
	}
	else
	{
		Result.Kind = EAIDABottleneck::None;
	}
	return Result;
}

FAIDAClockAdviceReport FAIDAFactoryAggregator::BuildClockAdvice(const TArray<FAIDAMachine>& Machines,
	const FAIDAAggregatorConfig& Config)
{
	FAIDAClockAdviceReport Report;

	for (const FAIDAMachine& M : Machines)
	{
		// Only power-drawing producers: generators (PowerMW < 0) and non-producing buildables don't underclock.
		if (M.PowerMW <= Config.RateTolerance || M.Outputs.Num() == 0 || M.Clock <= 0.0f) { continue; }

		if (M.Productivity <= 0.0f)
		{
			++Report.StoppedMachines;
			continue;
		}
		if (static_cast<double>(M.Productivity) > 1.0 - Config.UnderclockIdleThreshold) { continue; }

		// Match the clock to observed uptime, rounded UP to a whole percent so throughput never drops.
		const double Raw = FMath::Max(Config.MinSuggestedClock, static_cast<double>(M.Clock) * static_cast<double>(M.Productivity));
		const double Suggested = FMath::CeilToDouble(Raw * 100.0) / 100.0;
		if (Suggested >= static_cast<double>(M.Clock) - 0.005) { continue; }

		FAIDAClockAdvice Advice;
		Advice.MachineId = M.Id;
		Advice.BuildingClass = M.BuildingClass;
		Advice.Recipe = M.Recipe;
		Advice.Location = M.Location;
		Advice.Clock = M.Clock;
		Advice.Productivity = M.Productivity;
		Advice.PowerMW = M.PowerMW;
		Advice.SuggestedClock = static_cast<float>(Suggested);
		Advice.SavedMW = M.PowerMW * (1.0 - FMath::Pow(Suggested / static_cast<double>(M.Clock), Config.PowerExponent));
		Report.TotalSavableMW += Advice.SavedMW;
		Report.Advice.Add(MoveTemp(Advice));
	}

	Report.Advice.Sort([](const FAIDAClockAdvice& A, const FAIDAClockAdvice& B)
	{
		if (A.SavedMW != B.SavedMW) { return A.SavedMW > B.SavedMW; }
		return A.MachineId < B.MachineId;
	});
	return Report;
}

FAIDAFactoryAggregates FAIDAFactoryAggregator::Aggregate(const FAIDAFactorySnapshot& Snapshot,
	const FAIDAAggregatorConfig& Config)
{
	FAIDAFactoryAggregates Result;

	const TArray<int32> Labels = ClusterMachines(Snapshot.Machines, Config);
	Result.Clusters = BuildClusters(Snapshot.Machines, Labels, Config);
	Result.Balance = BuildBalanceSheet(Snapshot.Machines, Config);

	TMap<int32, int32> ClusterOfMachineId;
	ClusterOfMachineId.Reserve(Snapshot.Machines.Num());
	for (int32 i = 0; i < Snapshot.Machines.Num(); ++i)
	{
		ClusterOfMachineId.Add(Snapshot.Machines[i].Id, Labels[i]);
	}
	Result.Flows = BuildLogistics(Snapshot.Edges, ClusterOfMachineId);
	Result.Power = BuildPowerReport(Snapshot.Circuits);

	return Result;
}
