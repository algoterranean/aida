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

	//~ Alert inputs (P8 Slice 1) — why a stopped machine is stopped, read cheaply at extract time.
	bool bPaused = false;            // production paused by a player (IsProductionPaused)
	bool bInputStarved = false;      // manufacturer with a recipe and an EMPTY input inventory
	bool bOutputFull = false;        // manufacturer whose output inventory has no room left
	bool bFuelGenerator = false;     // fuel-burning generator (coal/fuel/nuclear)
	bool bHasFuel = true;            // fuel generators only: false = tank/hopper is empty
	TArray<FAIDAItemRate> Inputs;    // items/min consumed at Clock
	TArray<FAIDAItemRate> Outputs;   // items/min produced at Clock
	double PowerMW = 0.0;            // electrical load; >0 draws, <0 generates
	int32 CircuitId = 0;             // power circuit this machine is wired to (0 = none)

	//~ Logistics graph (P7 Slice 0). Splitters/mergers/pipe attachments enter Machines as
	//~ bLogisticsOnly nodes: no production, but they carry edges, join clusters, and count ports.
	bool bLogisticsOnly = false;
	int32 InPortsTotal = 0;          // directional belt+pipe ports (FCD_INPUT / PCT_CONSUMER)
	int32 InPortsConnected = 0;
	int32 OutPortsTotal = 0;         // FCD_OUTPUT / PCT_PRODUCER
	int32 OutPortsConnected = 0;
	int32 AnyPortsTotal = 0;         // direction-agnostic ports (pipe junctions are all PCT_ANY)
	int32 AnyPortsConnected = 0;
};

/**
 * A logical belt/pipe link between two snapshot nodes (machines, logistics nodes, or containers —
 * one shared id space). Chains of belt/pipe segments are collapsed to one edge; 0 = a dangling end.
 */
struct FAIDAConveyorEdge
{
	int32 FromMachine = 0;           // source node id; 0 = fed by nothing (dangling input)
	int32 ToMachine = 0;             // sink node id; 0 = feeds nothing (dangling output)
	FString Item;                    // best-effort: pipe fluid, or the source machine's main output; may be empty
	double PerMinute = 0.0;          // carrying CAPACITY (slowest segment): belts items/min, pipes m³/min
	bool bPipe = false;
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
	bool bFuseTriggered = false;     // the circuit's fuse is blown — everything on it is dark
};

/** One item kind inside a container, aggregated across its stacks. */
struct FAIDAItemCount
{
	FString Item;
	int32 Count = 0;
};

/** A storage container: where it is and what it holds (P7 Slice 3, read side). */
struct FAIDAContainerInfo
{
	int32 Id = 0;                    // stable index within a snapshot (separate space from machine ids)
	FString BuildingClass;           // "StorageContainerMk1", "StorageContainerMk2", ...
	FVector Location = FVector::ZeroVector;
	int32 SlotsUsed = 0;
	int32 SlotsTotal = 0;
	TArray<FAIDAItemCount> Contents; // per-item totals, largest count first
};

/** The normalized snapshot the extractor produces; the aggregator's sole input. */
struct FAIDAFactorySnapshot
{
	TArray<FAIDAMachine> Machines;
	TArray<FAIDAConveyorEdge> Edges;
	TArray<FAIDAPowerCircuitStats> Circuits;
	TArray<FAIDAContainerInfo> Containers;
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

/** What a find_disconnected finding points at (P7 Slice 1). */
enum class EAIDADisconnectKind : uint8
{
	DanglingNode,   // splitter/merger/pipe attachment with nothing on one whole side
	DanglingEdge,   // belt/pipe run whose far end attaches to nothing
	OpenPorts       // machine with unconnected input/output ports
};

struct FAIDADisconnectedFinding
{
	EAIDADisconnectKind Kind = EAIDADisconnectKind::OpenPorts;
	int32 NodeId = 0;                // the node itself, or a dangling edge's anchored end
	FString BuildingClass;
	FVector Location = FVector::ZeroVector;
	bool bProducing = true;
	int32 InOpen = 0;                // unconnected port counts (node/machine findings)
	int32 OutOpen = 0;
	int32 AnyOpen = 0;
	bool bPipe = false;              // edge findings
	double PerMinute = 0.0;          // edge findings: the dangling run's capacity
	FString Detail;                  // one-line human hint
};

struct FAIDADisconnectedReport
{
	TArray<FAIDADisconnectedFinding> Findings; // definite problems first
	int32 NodesChecked = 0;                    // logistics nodes examined
	int32 MachinesChecked = 0;                 // producing machines examined
	int32 EdgesChecked = 0;
};

/** What a get_alerts finding is (P8 Slice 1) — something wrong RIGHT NOW, with where and why. */
enum class EAIDAAlertKind : uint8
{
	FuseTripped,     // circuit fuse blown — everything on it is dark
	GeneratorNoFuel, // fuel generator with an empty tank/hopper
	MachineStarved,  // producer stopped with an empty input inventory
	MachineBlocked,  // producer stopped with a full output inventory
	MachineStopped,  // producer stopped with no visible inventory cause
	MachinePaused,   // production paused by a player
	DanglingEdge     // belt/pipe to/from nothing (find_disconnected's edge findings)
};

struct FAIDAAlert
{
	EAIDAAlertKind Kind = EAIDAAlertKind::MachineStopped;
	int32 NodeId = 0;                // machine/node id (0 for circuit alerts)
	int32 CircuitId = 0;             // fuse alerts; also set on machine alerts when known
	FString BuildingClass;           // empty for circuit alerts
	FVector Location = FVector::ZeroVector; // fuse alerts: centroid of the circuit's machines
	bool bPipe = false;              // dangling-edge alerts
	FString Detail;                  // one-line cause
};

/** get_alerts output: everything wrong right now (fuses, fuel, stalls, pauses, dangling ends). */
struct FAIDAAlertsReport
{
	TArray<FAIDAAlert> Alerts;
	int32 CircuitsChecked = 0;
	int32 MachinesChecked = 0;          // producing machines examined (logistics nodes excluded)
	int32 GeneratorsChecked = 0;        // fuel generators examined
	int32 EdgesChecked = 0;
	/** Stalled machines NOT listed individually because their circuit's fuse alert explains them. */
	int32 StalledOnTrippedCircuits = 0;
};

/** One suspiciously slow link (P7 Slice 1): a belt/pipe slower than the traffic around it. */
struct FAIDABeltMismatch
{
	int32 FromNode = 0;
	int32 ToNode = 0;
	FString FromClass;
	FString ToClass;
	FVector Location = FVector::ZeroVector; // midpoint of the endpoints we know
	double EdgePerMin = 0.0;
	double UpstreamPerMin = 0.0;            // fastest link into FromNode (0 = none)
	double DownstreamPerMin = 0.0;          // fastest link out of ToNode (0 = none)
	double ProducerPerMin = 0.0;            // source machine's total output rate (0 = n/a)
	bool bPipe = false;
	FString Detail;
};

/** One machine's underclock recommendation (P7 Slice 1): it idles enough that a lower clock saves power. */
struct FAIDAClockAdvice
{
	int32 MachineId = 0;
	FString BuildingClass;
	FString Recipe;
	FVector Location = FVector::ZeroVector;
	float Clock = 1.0f;              // current potential, 1.0 = 100%
	float Productivity = 1.0f;       // recent uptime fraction [0,1]
	double PowerMW = 0.0;            // current draw at Clock
	float SuggestedClock = 1.0f;     // recommended potential (rounded up to a whole percent)
	double SavedMW = 0.0;            // estimated draw reduction at the suggested clock
};

/** get_clock_advice output: per-machine recommendations plus the headline totals. */
struct FAIDAClockAdviceReport
{
	TArray<FAIDAClockAdvice> Advice; // biggest saving first
	double TotalSavableMW = 0.0;     // sum of Advice[].SavedMW
	int32 StoppedMachines = 0;       // power-drawing producers at zero productivity (a bottleneck, not a clock problem)
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
