#pragma once

#include "CoreMinimal.h"
#include "Recipes/AIDARecipeModel.h"

class UObject;

/**
 * Game-header seam for AIDA's static reference catalog (docs/PHASE2.md Slice 3b). Walks the recipe
 * manager's available recipes into plain `FAIDARecipeInfo`s, and derives the `FAIDABuildingInfo` list
 * from the building descriptors those recipes produce. Only the .cpp touches FactoryGame headers; the
 * pure lookup_recipe / lookup_building serializers work off the cached result.
 *
 * The catalog changes only as the player unlocks recipes, so it's cached behind a long TTL (build-once
 * in practice). One extraction fills BOTH lists.
 */
class FAIDARecipeCatalog
{
public:
	/** Cached recipe list, re-scanned if older than TtlSeconds. NowSeconds is world time. */
	const TArray<FAIDARecipeInfo>& GetRecipes(UObject* WorldContext, double NowSeconds, double TtlSeconds = 60.0);

	/** Cached building list (derived from the same scan). NowSeconds is world time. */
	const TArray<FAIDABuildingInfo>& GetBuildings(UObject* WorldContext, double NowSeconds, double TtlSeconds = 60.0);

	/** Force a re-scan on the next Get (e.g. after a milestone unlock). */
	void Invalidate() { bValid = false; }

	/** Extract the recipe + building catalog from the world's recipe manager right now. Clears the outputs. */
	static void ExtractInto(UObject* WorldContext, TArray<FAIDARecipeInfo>& OutRecipes, TArray<FAIDABuildingInfo>& OutBuildings);

private:
	void EnsureFresh(UObject* WorldContext, double NowSeconds, double TtlSeconds);

	TArray<FAIDARecipeInfo> CachedRecipes;
	TArray<FAIDABuildingInfo> CachedBuildings;
	double LastExtractSeconds = 0.0;
	bool bValid = false;
};
