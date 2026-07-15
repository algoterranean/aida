#pragma once

#include "CoreMinimal.h"
#include "Factory/AIDAFactoryModel.h"

/** Knobs for aggregation. Distances are in cm (Unreal world units); rates in items/min. */
struct FAIDAAggregatorConfig
{
	/** DBSCAN neighbourhood radius: machines within this distance chain into the same cluster (~50 m). */
	double ClusterEpsilon = 5000.0;
	/** DBSCAN minPts. 1 = connected-components within epsilon (no noise); >1 marks sparse machines as noise. */
	int32 MinClusterPoints = 1;
	/** Rates whose magnitude is below this are treated as zero when netting production vs consumption. */
	double RateTolerance = 1e-3;
};

/**
 * Derives LLM-friendly aggregates from a normalized factory snapshot (docs/ARCHITECTURE.md §4):
 * spatial clusters, a per-item balance sheet, inter-cluster logistics flows, and a per-circuit power
 * report. Pure and stateless — no game headers, no allocation beyond the result — so the bug-prone
 * math is unit-tested on synthetic entities. Every output is deterministically ordered.
 */
class FAIDAFactoryAggregator
{
public:
	/** Full pass: cluster the machines, then build every aggregate over the snapshot. */
	static FAIDAFactoryAggregates Aggregate(const FAIDAFactorySnapshot& Snapshot,
		const FAIDAAggregatorConfig& Config = FAIDAAggregatorConfig());

	//~ Sub-steps, exposed for unit testing. -------------------------------------------------------

	/**
	 * DBSCAN over machine positions. Returns a 0-based cluster id per machine, parallel to `Machines`
	 * (index i -> cluster of Machines[i]). With MinClusterPoints==1 this is connected-components within
	 * epsilon; sparser settings leave noise machines, which are then each given their own singleton id.
	 */
	static TArray<int32> ClusterMachines(const TArray<FAIDAMachine>& Machines, const FAIDAAggregatorConfig& Config);

	/** Build the per-cluster summaries (centroid, census, net item flows, efficiency) from a labelling. */
	static TArray<FAIDACluster> BuildClusters(const TArray<FAIDAMachine>& Machines,
		const TArray<int32>& ClusterOfMachine, const FAIDAAggregatorConfig& Config);

	/** Whole-factory per-item net production vs consumption, sorted by item key. */
	static TArray<FAIDAItemBalance> BuildBalanceSheet(const TArray<FAIDAMachine>& Machines,
		const FAIDAAggregatorConfig& Config = FAIDAAggregatorConfig());

	/** Inter-cluster flows inferred from edges (intra-cluster edges dropped), keyed by (from,to,item). */
	static TArray<FAIDALogisticsFlow> BuildLogistics(const TArray<FAIDAConveyorEdge>& Edges,
		const TMap<int32, int32>& ClusterOfMachineId);

	/** Per-circuit power report, straight from the game's authoritative circuit stats (keeps circuit id 0). */
	static TArray<FAIDAPowerReport> BuildPowerReport(const TArray<FAIDAPowerCircuitStats>& Circuits);

	/**
	 * Diagnose why one item's production is limited, without the belt/pipe graph: locate its producers,
	 * then attribute the constraint to a starved upstream input (in factory-wide deficit), an overloaded
	 * power circuit, or idle-with-inputs (output backed up). Upstream is preferred as the root cause.
	 */
	static FAIDABottleneckResult FindBottleneck(const FAIDAFactorySnapshot& Snapshot, const FString& Item,
		const FAIDAAggregatorConfig& Config = FAIDAAggregatorConfig());
};
