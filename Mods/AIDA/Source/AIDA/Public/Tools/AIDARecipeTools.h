#pragma once

#include "CoreMinimal.h"
#include "Recipes/AIDARecipeModel.h"

/**
 * Pure serializers for the Slice 3b static-reference tools (docs/PHASE2.md): lookup_recipe and
 * lookup_building. They turn the cached catalog (from AIDARecipeService) into bounded, LLM-friendly
 * JSON — no game headers, so they unit-test on synthetic catalogs. Each per-craft amount is paired
 * with a per-minute rate (amount * 60 / duration) since that's how players reason about throughput.
 */
namespace AIDARecipeTools
{
	/**
	 * lookup_recipe: recipes that produce (or are named for) ItemFilter — case-insensitive substring on
	 * a product name or the recipe name. Empty filter lists a bounded slice of the catalog. Each recipe
	 * carries its ingredients/products (amount + per-minute), craft duration, and producing buildings.
	 */
	FString BuildRecipeJson(const TArray<FAIDARecipeInfo>& Recipes, const FString& ItemFilter);

	/**
	 * lookup_building: buildings whose name matches NameFilter (case-insensitive substring). Empty filter
	 * lists a bounded slice. Each carries its power draw (fixed, or min/max when variable) and, for
	 * generators, power production.
	 */
	FString BuildBuildingJson(const TArray<FAIDABuildingInfo>& Buildings, const FString& NameFilter);
}
