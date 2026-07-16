#include "Factory/AIDAFactoryIndex.h"

#include "AIDA.h"
#include "Factory/AIDALogisticsGraph.h"

#include "FGBuildableSubsystem.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableStorage.h"
#include "Buildables/FGBuildableGenerator.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Buildables/FGBuildablePipeline.h"
#include "Buildables/FGBuildablePipelineAttachment.h"
#include "FGFactoryConnectionComponent.h"
#include "FGPipeConnectionComponent.h"
#include "FGInventoryComponent.h"
#include "Resources/FGExtractableResourceInterface.h"
#include "FGRecipe.h"
#include "ItemAmount.h"
#include "Resources/FGItemDescriptor.h"
#include "Resources/FGResourceDescriptor.h"
#include "Resources/FGResourceNode.h"
#include "FGPowerInfoComponent.h"
#include "FGPowerCircuit.h"

namespace
{
	/** Stable, human-readable key for an item: its display name, falling back to the class name. */
	FString ExtractItemKey(TSubclassOf<UFGItemDescriptor> ItemClass)
	{
		if (!ItemClass) { return FString(); }
		const FString Name = UFGItemDescriptor::GetItemName(ItemClass).ToString();
		return Name.IsEmpty() ? GetNameSafe(ItemClass.Get()) : Name;
	}

	/**
	 * Items/min for one recipe entry at the machine's current cycle time (which already folds in the
	 * overclock potential). Fluids/gases are stored in cm³ (×1000), so normalize them to m³-equivalent.
	 */
	double PerMinuteFor(const FItemAmount& Entry, float CycleSeconds)
	{
		if (CycleSeconds <= 0.0f || !Entry.ItemClass) { return 0.0; }
		double Amount = static_cast<double>(Entry.Amount);
		const EResourceForm Form = UFGItemDescriptor::GetForm(Entry.ItemClass);
		if (Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS) { Amount /= 1000.0; }
		return Amount * 60.0 / static_cast<double>(CycleSeconds);
	}

	/** "Build_ConstructorMk1_C" -> "ConstructorMk1" for a friendlier census key. */
	FString CleanBuildableName(const AActor* Actor)
	{
		FString Name = GetNameSafe(Actor ? Actor->GetClass() : nullptr);
		Name.RemoveFromStart(TEXT("Build_"));
		Name.RemoveFromEnd(TEXT("_C"));
		return Name;
	}

	/** Count the actor's belt + pipe ports into the machine's In/Out/Any totals (P7 Slice 0). */
	void CountPorts(AActor* Actor, FAIDAMachine& Machine)
	{
		TInlineComponentArray<UFGFactoryConnectionComponent*> BeltPorts(Actor);
		for (UFGFactoryConnectionComponent* Port : BeltPorts)
		{
			if (!Port) { continue; }
			const bool bConnected = Port->IsConnected();
			switch (Port->GetDirection())
			{
			case EFactoryConnectionDirection::FCD_INPUT:  ++Machine.InPortsTotal;  Machine.InPortsConnected  += bConnected ? 1 : 0; break;
			case EFactoryConnectionDirection::FCD_OUTPUT: ++Machine.OutPortsTotal; Machine.OutPortsConnected += bConnected ? 1 : 0; break;
			case EFactoryConnectionDirection::FCD_ANY:    ++Machine.AnyPortsTotal; Machine.AnyPortsConnected += bConnected ? 1 : 0; break;
			default: break; // SNAP_ONLY poles etc. aren't logistics ports
			}
		}
		TInlineComponentArray<UFGPipeConnectionComponent*> PipePorts(Actor);
		for (UFGPipeConnectionComponent* Port : PipePorts)
		{
			if (!Port) { continue; }
			const bool bConnected = Port->IsConnected();
			switch (Port->GetPipeConnectionType())
			{
			case EPipeConnectionType::PCT_CONSUMER: ++Machine.InPortsTotal;  Machine.InPortsConnected  += bConnected ? 1 : 0; break;
			case EPipeConnectionType::PCT_PRODUCER: ++Machine.OutPortsTotal; Machine.OutPortsConnected += bConnected ? 1 : 0; break;
			case EPipeConnectionType::PCT_ANY:      ++Machine.AnyPortsTotal; Machine.AnyPortsConnected += bConnected ? 1 : 0; break;
			default: break;
			}
		}
	}

	/**
	 * Resolve one end of a belt/pipe segment: the peer connection's owning actor is either another
	 * segment (chain continues), a snapshot node (machine/logistics node/container), or absent.
	 */
	void ResolveSegmentEnd(UFGConnectionComponent* Peer,
		const TMap<const AActor*, int32>& SegmentIdOfActor, const TMap<const AActor*, int32>& NodeIdOfActor,
		int32& OutSegment, int32& OutNode)
	{
		OutSegment = 0;
		OutNode = 0;
		const AActor* Owner = Peer ? Peer->GetOwner() : nullptr;
		if (!Owner) { return; }
		if (const int32* Seg = SegmentIdOfActor.Find(Owner)) { OutSegment = *Seg; return; }
		if (const int32* Node = NodeIdOfActor.Find(Owner)) { OutNode = *Node; }
	}
}

void FAIDAFactoryIndex::ExtractInto(UObject* WorldContext, FAIDAFactorySnapshot& Out)
{
	Out.Machines.Reset();
	Out.Edges.Reset();
	Out.Circuits.Reset();
	Out.Containers.Reset();

	AFGBuildableSubsystem* Subsystem = AFGBuildableSubsystem::Get(WorldContext);
	if (!Subsystem)
	{
		UE_LOG(LogAIDA, Warning, TEXT("[index] no AFGBuildableSubsystem for world — extraction skipped."));
		return;
	}

	const TArray<AFGBuildable*>& Buildables = Subsystem->GetAllBuildablesRef();
	int32 NextId = 1;
	int32 ExtractorCount = 0;
	int32 ExtractorWithResource = 0;
	TSet<UFGPowerCircuit*> SeenCircuits;
	TMap<const AActor*, int32> NodeIdOfActor; // every snapshot node (machine/logistics/container) for edge endpoints

	for (AFGBuildable* Buildable : Buildables)
	{
		// P7 Slice 0: splitters/mergers are plain AFGBuildables (not factories) — logistics-only nodes.
		if (Cast<AFGBuildableConveyorAttachment>(Buildable))
		{
			FAIDAMachine Node;
			Node.Id = NextId++;
			Node.BuildingClass = CleanBuildableName(Buildable);
			Node.Location = Buildable->GetActorLocation();
			Node.bLogisticsOnly = true;
			CountPorts(Buildable, Node);
			NodeIdOfActor.Add(Buildable, Node.Id);
			Out.Machines.Add(MoveTemp(Node));
			continue;
		}

		AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Buildable);
		if (!Factory) { continue; }

		// Record the circuit for the power pass regardless of whether this is a machine we model.
		UFGPowerInfoComponent* PowerInfo = Factory->GetPowerInfo();
		UFGPowerCircuit* Circuit = PowerInfo ? PowerInfo->GetPowerCircuit() : nullptr;
		if (Circuit) { SeenCircuits.Add(Circuit); }

		FAIDAMachine Machine;
		bool bIsMachine = false;

		if (AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Factory))
		{
			bIsMachine = true;
			const TSubclassOf<UFGRecipe> Recipe = Manufacturer->GetCurrentRecipe();
			if (Recipe)
			{
				Machine.Recipe = UFGRecipe::GetRecipeName(Recipe).ToString();
				const float Cycle = Manufacturer->GetProductionCycleTime();
				for (const FItemAmount& Product : UFGRecipe::GetProducts(Recipe))
				{
					if (Product.ItemClass) { Machine.Outputs.Add({ ExtractItemKey(Product.ItemClass), PerMinuteFor(Product, Cycle) }); }
				}
				for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(WorldContext, Recipe))
				{
					if (Ingredient.ItemClass) { Machine.Inputs.Add({ ExtractItemKey(Ingredient.ItemClass), PerMinuteFor(Ingredient, Cycle) }); }
				}
			}
		}
		else if (Cast<AFGBuildablePipelineAttachment>(Factory))
		{
			// Pipe junctions, pumps, valves: logistics-only nodes so pipe edges route through them.
			bIsMachine = true;
			Machine.bLogisticsOnly = true;
		}
		else if (Cast<AFGBuildableGenerator>(Factory))
		{
			// Generators carry no recipe rates (circuit stats are authoritative for power), but they
			// must exist as nodes or every fuel belt/pipe into them would read as dangling.
			bIsMachine = true;
		}
		else if (AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(Factory))
		{
			// P7 Slice 3 read side: containers go in their own snapshot list, not Machines.
			// They share the node id space so belts to/from storage resolve as edges.
			if (UFGInventoryComponent* Inventory = Storage->GetStorageInventory())
			{
				FAIDAContainerInfo Container;
				Container.Id = NextId++;
				Container.BuildingClass = CleanBuildableName(Buildable);
				Container.Location = Buildable->GetActorLocation();
				Container.SlotsTotal = Inventory->GetSizeLinear();

				TMap<FString, int32> Totals;
				for (int32 Idx = 0; Idx < Inventory->GetSizeLinear(); ++Idx)
				{
					FInventoryStack Stack;
					if (!Inventory->GetStackFromIndex(Idx, Stack) || !Stack.HasItems()) { continue; }
					++Container.SlotsUsed;
					Totals.FindOrAdd(ExtractItemKey(Stack.Item.GetItemClass())) += Stack.NumItems;
				}
				for (const TPair<FString, int32>& Pair : Totals)
				{
					Container.Contents.Add({ Pair.Key, Pair.Value });
				}
				Container.Contents.Sort([](const FAIDAItemCount& A, const FAIDAItemCount& B)
				{
					if (A.Count != B.Count) { return A.Count > B.Count; }
					return A.Item < B.Item;
				});
				NodeIdOfActor.Add(Buildable, Container.Id);
				Out.Containers.Add(MoveTemp(Container));
			}
			continue;
		}
		else if (AFGBuildableResourceExtractor* Extractor = Cast<AFGBuildableResourceExtractor>(Factory))
		{
			bIsMachine = true;
			++ExtractorCount;
			// GetResourceNode() is DEPRECATED and returns null in current versions — read the resource
			// off the extractable-resource interface instead, or every mined item reads as a false deficit.
			TSubclassOf<UFGItemDescriptor> ResourceClass;
			const TScriptInterface<IFGExtractableResourceInterface> Resource = Extractor->GetExtractableResource();
			if (Resource.GetObject())
			{
				ResourceClass = Resource->GetResourceClass();
			}
			if (ResourceClass)
			{
				++ExtractorWithResource;
				Machine.Outputs.Add({ ExtractItemKey(ResourceClass), static_cast<double>(Extractor->GetExtractionPerMinute()) });
			}
		}

		if (!bIsMachine) { continue; }

		Machine.Id = NextId++;
		Machine.BuildingClass = CleanBuildableName(Buildable);
		Machine.Location = Buildable->GetActorLocation();
		Machine.Clock = Factory->GetCurrentPotential();
		Machine.bProducing = Factory->IsProducing();
		Machine.Productivity = Factory->GetProductivity();
		if (PowerInfo)
		{
			Machine.PowerMW = PowerInfo->GetActualConsumption();
			if (Circuit) { Machine.CircuitId = Circuit->GetCircuitID(); }
		}
		CountPorts(Buildable, Machine);
		NodeIdOfActor.Add(Buildable, Machine.Id);
		Out.Machines.Add(MoveTemp(Machine));
	}

	// P7 Slice 0: belts (incl. lifts) and pipelines → flat segments → collapsed node-to-node edges.
	// Two passes: segment ids first so belt-to-belt joins resolve regardless of iteration order.
	TMap<const AActor*, int32> SegmentIdOfActor;
	int32 NextSegmentId = 1;
	for (AFGBuildable* Buildable : Buildables)
	{
		if (Cast<AFGBuildableConveyorBase>(Buildable) || Cast<AFGBuildablePipeline>(Buildable))
		{
			SegmentIdOfActor.Add(Buildable, NextSegmentId++);
		}
	}

	TArray<AIDALogisticsGraph::FSegment> Segments;
	Segments.Reserve(SegmentIdOfActor.Num());
	for (AFGBuildable* Buildable : Buildables)
	{
		if (AFGBuildableConveyorBase* Belt = Cast<AFGBuildableConveyorBase>(Buildable))
		{
			AIDALogisticsGraph::FSegment Segment;
			Segment.SegmentId = SegmentIdOfActor.FindChecked(Belt);
			Segment.PerMinute = Belt->GetSpeed() / 2.0; // mSpeed is cm/s with 120 cm spacing → items/min = speed/2
			UFGFactoryConnectionComponent* In = Belt->GetConnection0();   // 0 = input end
			UFGFactoryConnectionComponent* Out1 = Belt->GetConnection1(); // 1 = output end
			ResolveSegmentEnd(In ? In->GetConnection() : nullptr, SegmentIdOfActor, NodeIdOfActor, Segment.FromSegment, Segment.FromNode);
			ResolveSegmentEnd(Out1 ? Out1->GetConnection() : nullptr, SegmentIdOfActor, NodeIdOfActor, Segment.ToSegment, Segment.ToNode);
			Segments.Add(MoveTemp(Segment));
		}
		else if (AFGBuildablePipeline* Pipe = Cast<AFGBuildablePipeline>(Buildable))
		{
			AIDALogisticsGraph::FSegment Segment;
			Segment.SegmentId = SegmentIdOfActor.FindChecked(Pipe);
			Segment.bPipe = true;
			Segment.PerMinute = Pipe->GetFlowLimit() * 60.0; // m³/s → m³/min
			Segment.Item = ExtractItemKey(Pipe->GetFluidDescriptor());
			UFGPipeConnectionComponent* C0 = Pipe->GetPipeConnection0();
			UFGPipeConnectionComponent* C1 = Pipe->GetPipeConnection1();
			UFGPipeConnectionComponentBase* P0 = C0 ? C0->GetConnection() : nullptr;
			UFGPipeConnectionComponentBase* P1 = C1 ? C1->GetConnection() : nullptr;
			ResolveSegmentEnd(P0, SegmentIdOfActor, NodeIdOfActor, Segment.FromSegment, Segment.FromNode);
			ResolveSegmentEnd(P1, SegmentIdOfActor, NodeIdOfActor, Segment.ToSegment, Segment.ToNode);
			// Pipes have no inherent direction; orient producer → consumer when the endpoints say so.
			// (Chained pipes may still fragment when their 0/1 ends alternate — v1 accepts that.)
			const EPipeConnectionType T0 = P0 ? P0->GetPipeConnectionType() : EPipeConnectionType::PCT_ANY;
			const EPipeConnectionType T1 = P1 ? P1->GetPipeConnectionType() : EPipeConnectionType::PCT_ANY;
			if (T0 == EPipeConnectionType::PCT_CONSUMER || T1 == EPipeConnectionType::PCT_PRODUCER)
			{
				Swap(Segment.FromSegment, Segment.ToSegment);
				Swap(Segment.FromNode, Segment.ToNode);
			}
			Segments.Add(MoveTemp(Segment));
		}
	}
	Out.Edges = AIDALogisticsGraph::CollapseChains(Segments);
	AIDALogisticsGraph::AttributeItems(Out.Edges, Out.Machines);

	for (UFGPowerCircuit* Circuit : SeenCircuits)
	{
		if (!Circuit) { continue; }
		FPowerCircuitStats Stats;
		Circuit->GetStats(Stats);

		FAIDAPowerCircuitStats Report;
		Report.CircuitId = Circuit->GetCircuitID();
		Report.ProducedMW = Stats.PowerProduced;
		Report.CapacityMW = Stats.PowerProductionCapacity;
		Report.ConsumedMW = Stats.PowerConsumed;
		Report.BatteryMWh = Circuit->GetBatterySumPowerStore();
		Report.BatteryDrainSeconds = Circuit->GetTimeToBatteriesEmpty();
		Out.Circuits.Add(MoveTemp(Report));
	}

	UE_LOG(LogAIDA, Log, TEXT("[index] extracted %d machines (%d/%d extractors resolved a resource), %d containers, %d segments -> %d edges, %d power circuits."),
		Out.Machines.Num(), ExtractorWithResource, ExtractorCount, Out.Containers.Num(), Segments.Num(), Out.Edges.Num(), Out.Circuits.Num());
}

const FAIDAFactorySnapshot& FAIDAFactoryIndex::GetSnapshot(UObject* WorldContext, double NowSeconds, double TtlSeconds)
{
	if (!bValid || (NowSeconds - LastExtractSeconds) >= TtlSeconds)
	{
		ExtractInto(WorldContext, Cached);
		LastExtractSeconds = NowSeconds;
		bValid = true;
	}
	return Cached;
}
