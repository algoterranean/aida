#pragma once

#include "CoreMinimal.h"

/**
 * Plain-data factory model for Phase 2 "Eyes" (docs/ARCHITECTURE.md §4, docs/PHASE2.md).
 *
 * Two halves:
 *   1. A normalized SNAPSHOT the extractor fills by walking the game's buildable subsystems
 *      (Slice 2, the only game-header seam) — machines, belt/pipe edges, power circuits.
 *   2. AGGREGATES the pure `FAIDAFactoryAggregator` derives from the snapshot (Slice 1) — spatial
 *      clusters, a per-item balance sheet, inter-cluster logistics flows, and a per-circuit power report.
 *
 * Everything here is plain C++ (no UObject/reflection): the aggregation math stays unit-testable on
 * synthetic entities with no live game, and the tool handlers serialize these to bounded JSON. Item
 * keys are opaque strings (the extractor's stable per-item key); rates are items/min unless noted.
 */

/** A flow of one item at a rate, in items/min (fluids already converted from cm³/min to items-equivalent). */
struct FAIDAItemRate
{
	FString Item;
	double PerMinute = 0.0;
};

/**
 * One production/extraction/generation building. Rates are the CURRENT potential throughput (clock
 * already applied), not observed averages — matches the game's per-cycle math (docs/PHASE2.md).
 */
struct FAIDAMachine
{
	int32 Id = 0;                    // stable index within a snapshot
	FString BuildingClass;           // "Constructor", "Assembler", "Smelter", "MinerMk2", ...
	FString Recipe;                  // recipe display key; empty for extractors/generators/storage
	FVector Location = FVector::ZeroVector;
	float Clock = 1.0f;              // overclock potential, 1.0 = 100%
	bool bProducing = true;          // currently running (not starved/paused/unpowered)
	float Productivity = 1.0f;       // [0,1] recent uptime fraction (game's GetProductivity)
	TArray<FAIDAItemRate> Inputs;    // items/min consumed at Clock
	TArray<FAIDAItemRate> Outputs;   // items/min produced at Clock
	double PowerMW = 0.0;            // electrical load; >0 draws, <0 generates
	int32 CircuitId = 0;             // power circuit this machine is wired to (0 = none)
};

/** A belt/pipe carrying one item from one machine's output to another's input. */
struct FAIDAConveyorEdge
{
	int32 FromMachine = 0;
	int32 ToMachine = 0;
	FString Item;
	double PerMinute = 0.0;          // throughput the link carries (belt/pipe rate)
};

/** Per-circuit electrical stats as read from the game's power circuit (not derived). */
struct FAIDAPowerCircuitStats
{
	int32 CircuitId = 0;
	double ProducedMW = 0.0;         // current generation on the circuit
	double CapacityMW = 0.0;         // max generation capacity
	double ConsumedMW = 0.0;         // current draw (authoritative, from the circuit)
	double BatteryMWh = 0.0;         // stored battery energy
	double BatteryDrainSeconds = -1.0; // time-to-empty; <0 = not draining / no batteries
};

/** The normalized snapshot the extractor produces; the aggregator's sole input. */
struct FAIDAFactorySnapshot
{
	TArray<FAIDAMachine> Machines;
	TArray<FAIDAConveyorEdge> Edges;
	TArray<FAIDAPowerCircuitStats> Circuits;
};

// ----------------------------------------------------------------------------------------------------
// Aggregates (derived by FAIDAFactoryAggregator)
// ----------------------------------------------------------------------------------------------------

/** Net production vs consumption of one item across a scope (whole factory, or one cluster). */
struct FAIDAItemBalance
{
	FString Item;
	double Produced = 0.0;
	double Consumed = 0.0;

	double Net() const { return Produced - Consumed; }
	/** In deficit when consumption outruns production by more than a small tolerance. */
	bool IsDeficit(double Tolerance = 1e-3) const { return Net() < -Tolerance; }
};

/** A spatial group of machines (DBSCAN-style) with its census, net item flows, and an efficiency estimate. */
struct FAIDACluster
{
	int32 Id = 0;
	FVector Centroid = FVector::ZeroVector;
	TArray<int32> MachineIds;
	TMap<FString, int32> BuildingCensus;  // BuildingClass -> count
	TArray<FAIDAItemRate> NetInputs;      // items the cluster net-consumes (imports), magnitude positive
	TArray<FAIDAItemRate> NetOutputs;     // items the cluster net-produces (exports)
	float Efficiency = 1.0f;              // mean machine productivity [0,1]
};

/** An inferred inter-cluster flow of one item along the belt/pipe graph. */
struct FAIDALogisticsFlow
{
	int32 FromCluster = 0;
	int32 ToCluster = 0;
	FString Item;
	double PerMinute = 0.0;
};

/** Per-circuit power picture: capacity vs draw vs battery. Consumption is summed from machine loads. */
struct FAIDAPowerReport
{
	int32 CircuitId = 0;
	double ProducedMW = 0.0;
	double CapacityMW = 0.0;
	double ConsumedMW = 0.0;
	double BatteryMWh = 0.0;
	double BatteryDrainSeconds = -1.0;

	double Headroom() const { return CapacityMW - ConsumedMW; }
	/** Overloaded when draw exceeds generation capacity (batteries covering the gap). */
	bool IsOverloaded(double Tolerance = 1e-3) const { return ConsumedMW > CapacityMW + Tolerance; }
};

/** Everything the aggregator derives from one snapshot. */
struct FAIDAFactoryAggregates
{
	TArray<FAIDACluster> Clusters;
	TArray<FAIDAItemBalance> Balance;
	TArray<FAIDALogisticsFlow> Flows;
	TArray<FAIDAPowerReport> Power;
};

/** Why is `Item` production limited — a graph-free heuristic over balance, machine state, and power. */
enum class EAIDABottleneck : uint8
{
	None,          // producers running fine; supply meets demand
	UnknownItem,   // nothing produces or consumes the item
	NoProducers,   // the item is consumed but nothing produces it
	Upstream,      // a producer input is itself in factory-wide deficit (see LimitingDetail)
	Power,         // producers sit on an overloaded circuit (LimitingDetail = circuit id)
	OutputBackedUp // producers are idle though inputs and power are fine (output full / demand-limited)
};

struct FAIDABottleneckResult
{
	FString Item;
	EAIDABottleneck Kind = EAIDABottleneck::None;
	FString LimitingDetail;             // the starved upstream item, or the overloaded circuit id, as text
	double Produced = 0.0;
	double Consumed = 0.0;
	int32 ProducerCount = 0;
	int32 ProducersIdle = 0;            // producers not currently producing
	float AvgProductivity = 1.0f;       // mean [0,1] across producers
	TArray<FAIDAItemRate> StarvedInputs; // producer inputs in factory-wide deficit, magnitude positive

	double Net() const { return Produced - Consumed; }
};
