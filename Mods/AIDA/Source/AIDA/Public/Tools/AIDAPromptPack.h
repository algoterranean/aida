#pragma once

#include "CoreMinimal.h"

struct FAIDARecipeInfo;
struct FAIDABuildingInfo;

/**
 * Pure formatter for the generated game data pack (docs/PROMPT.md §2): renders the recipe/building
 * catalog into the compact reference text appended to the system prompt, so the model can do rate,
 * power, and layout math without lookup round-trips. No game headers — unit-tested on synthetic
 * catalogs. Output is name-sorted per section so the bytes are stable across requests (prompt-cache
 * friendly, same discipline as FAIDAToolRegistry::GetSpecs).
 */
namespace AIDAPromptPack
{
	/** Render the full pack. Inputs are copied and sorted; safe to pass the cached catalog. */
	FString Build(const TArray<FAIDARecipeInfo>& Recipes, const TArray<FAIDABuildingInfo>& Buildings);
}
