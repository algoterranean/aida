#pragma once

#include "CoreMinimal.h"

/**
 * Plain-data model for AIDA's static reference catalog (docs/PHASE2.md Slice 3b): recipes and the
 * production buildings that run them. Extracted once from the game's recipe manager (the game seam is
 * AIDARecipeService) into POD structs so the lookup_recipe / lookup_building serializers stay
 * game-header-free and unit-testable. Amounts are already humanized: solids in items, fluids in m³.
 */

/** One ingredient or product line: an item and its per-craft amount (fluids converted to m³). */
struct FAIDAItemAmount
{
	FString Item;
	double Amount = 0.0;
};

/** A crafting recipe: what it consumes/produces, how long a craft takes, and which buildings run it. */
struct FAIDARecipeInfo
{
	FString RecipeName;
	double DurationSeconds = 0.0;     // one manufacturing cycle at 100% clock
	TArray<FString> ProducedIn;       // display names of buildings that can run it (constructor, assembler…)
	TArray<FAIDAItemAmount> Ingredients;
	TArray<FAIDAItemAmount> Products;
};

/** One fuel a generator burns: the item and its burn rate at 100% clock (solids items/min, fluids m³/min). */
struct FAIDAFuelInfo
{
	FString Item;
	double BurnPerMin = 0.0;
};

/** A production building's static power profile (from its build-menu descriptor). */
struct FAIDABuildingInfo
{
	FString Name;
	double PowerConsumptionMW = 0.0;  // fixed draw, or 0 when variable
	bool bVariablePower = false;
	double MinPowerMW = 0.0;          // meaningful when bVariablePower
	double MaxPowerMW = 0.0;
	double PowerProductionMW = 0.0;   // > 0 for generators

	//~ Prompt-pack fields (docs/PROMPT.md §2) — all extracted from the buildable CDO; 0 = unknown/N.A.
	double FootprintXM = 0.0;         // clearance-box footprint, metres
	double FootprintYM = 0.0;
	double HeightM = 0.0;
	/** Power draw = base × (clock/100)^exponent; > 0 only for powered factory buildings. */
	double PowerExponent = 0.0;
	double BeltItemsPerMin = 0.0;     // > 0 for conveyor belts/lifts
	double PipeM3PerMin = 0.0;        // > 0 for pipelines
	double ExtractPerMinNormal = 0.0; // > 0 for extractors: rate on a NORMAL node (×0.5 impure, ×2 pure)
	TArray<FAIDAFuelInfo> Fuels;      // non-empty for fuel generators
	double SupplementalM3PerMin = 0.0;// water intake at 100% for generators that need it
};
