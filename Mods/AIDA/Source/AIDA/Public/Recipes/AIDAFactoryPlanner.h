#pragma once

#include "CoreMinimal.h"
#include "Recipes/AIDARecipeModel.h"

/**
 * plan_factory (docs/PHASE7.md Slice 5, the deterministic half): walk the unlocked recipe catalog
 * backwards from a target item+rate to a full production plan — machine counts per step, uniform
 * clocks that hit the target exactly, per-edge transport (belt/pipe mark from the catalog's own
 * rates), per-step and total power, and raw-resource requirements.
 *
 * Pure math over the plain catalog structs — no game headers, so every number is unit-testable on
 * synthetic recipes. Layout (floors, placement) is deliberately NOT here: the model does layout,
 * this provides the numbers (footprints included as hints).
 */

/** One production step: everything needed to build and clock the machines for one item. */
struct FAIDAPlanStep
{
	FString Item;                    // the product this step exists to make
	double RatePerMin = 0.0;         // demanded output rate (items/min, fluids m³/min)
	bool bFluid = false;
	FString Recipe;                  // chosen recipe name
	TArray<FString> AlternateRecipes;// other unlocked recipes for this item (names only)
	FString Building;                // building that runs the recipe
	int32 Machines = 0;
	double Clock = 1.0;              // uniform per-machine potential (≤ 1.0; exact, not rounded)
	double PowerMW = 0.0;            // step total at Clock (base × machines × clock^exponent)
	FString Transport;               // smallest belt/pipe carrying RatePerMin ("2× ..." when lanes needed)
	double FootprintXM = 0.0;        // per-machine clearance footprint, metres (0 = unknown)
	double FootprintYM = 0.0;
};

/** An input the plan does not produce (ore, water, ...) or a surplus it emits (byproduct). */
struct FAIDAPlanResource
{
	FString Item;
	double RatePerMin = 0.0;
	bool bFluid = false;
};

/** The whole plan; Error is non-empty when planning failed (unknown item, bad rate). */
struct FAIDAFactoryPlan
{
	FString TargetItem;
	double TargetPerMin = 0.0;
	TArray<FAIDAPlanStep> Steps;        // discovery order, target first
	TArray<FAIDAPlanResource> RawInputs;
	TArray<FAIDAPlanResource> Byproducts;
	int32 TotalMachines = 0;
	double TotalPowerMW = 0.0;
	TArray<FString> Notes;              // caveats: cycles, byproduct overlap, variable power, ...
	FString Error;
};

class FAIDAFactoryPlanner
{
public:
	/**
	 * Build the plan. Recipe choice per item is deterministic: standard (non-"Alternate:") recipes
	 * beat alternates, then a recipe named exactly like the item, then lexicographic; the losers are
	 * listed in AlternateRecipes for the model to re-plan with. Byproducts are reported, not netted
	 * against demand (a Note flags overlap so the model knows raw needs are an upper bound).
	 */
	static FAIDAFactoryPlan Plan(const FString& Item, double RatePerMin,
		const TArray<FAIDARecipeInfo>& Recipes, const TArray<FAIDABuildingInfo>& Buildings);

	/** Bounded, LLM-friendly JSON for the plan (or {"error"} when planning failed). */
	static FString BuildPlanJson(const FAIDAFactoryPlan& Plan);
};
