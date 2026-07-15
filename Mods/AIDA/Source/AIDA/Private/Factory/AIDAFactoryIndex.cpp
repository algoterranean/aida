#include "Factory/AIDAFactoryIndex.h"

#include "AIDA.h"

#include "FGBuildableSubsystem.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableResourceExtractor.h"
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
}

void FAIDAFactoryIndex::ExtractInto(UObject* WorldContext, FAIDAFactorySnapshot& Out)
{
	Out.Machines.Reset();
	Out.Edges.Reset();
	Out.Circuits.Reset();

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

	for (AFGBuildable* Buildable : Buildables)
	{
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
		Out.Machines.Add(MoveTemp(Machine));
	}

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

	UE_LOG(LogAIDA, Log, TEXT("[index] extracted %d machines (%d/%d extractors resolved a resource), %d power circuits (edges deferred)."),
		Out.Machines.Num(), ExtractorWithResource, ExtractorCount, Out.Circuits.Num());
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
