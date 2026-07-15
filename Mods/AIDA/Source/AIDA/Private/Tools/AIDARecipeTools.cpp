#include "Tools/AIDARecipeTools.h"

#include "Tools/AIDAToolJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	constexpr int32 MaxRecipesListed = 12;
	constexpr int32 MaxBuildingsListed = 12;

	/** One ingredient/product line: item, per-craft amount, and per-minute rate at 100% clock. */
	TSharedRef<FJsonValue> ItemLineJson(const FAIDAItemAmount& Line, double DurationSeconds)
	{
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("item"), Line.Item);
		O->SetField(TEXT("amount"), AIDANumber(Line.Amount));
		if (DurationSeconds > 0.0)
		{
			O->SetField(TEXT("perMin"), AIDANumber(Line.Amount * 60.0 / DurationSeconds));
		}
		return MakeShared<FJsonValueObject>(O);
	}

	bool RecipeMatches(const FAIDARecipeInfo& Recipe, const FString& Filter)
	{
		if (Filter.IsEmpty()) { return true; }
		if (Recipe.RecipeName.Contains(Filter, ESearchCase::IgnoreCase)) { return true; }
		for (const FAIDAItemAmount& P : Recipe.Products)
		{
			if (P.Item.Contains(Filter, ESearchCase::IgnoreCase)) { return true; }
		}
		return false;
	}
}

FString AIDARecipeTools::BuildRecipeJson(const TArray<FAIDARecipeInfo>& Recipes, const FString& ItemFilter)
{
	TArray<const FAIDARecipeInfo*> Matched;
	for (const FAIDARecipeInfo& R : Recipes)
	{
		if (RecipeMatches(R, ItemFilter)) { Matched.Add(&R); }
	}

	const int32 Shown = FMath::Min(Matched.Num(), MaxRecipesListed);
	TArray<TSharedPtr<FJsonValue>> List;
	for (int32 i = 0; i < Shown; ++i)
	{
		const FAIDARecipeInfo& R = *Matched[i];
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("recipe"), R.RecipeName);
		O->SetField(TEXT("seconds"), AIDANumber(R.DurationSeconds));

		TArray<TSharedPtr<FJsonValue>> Ins;
		for (const FAIDAItemAmount& L : R.Ingredients) { Ins.Add(ItemLineJson(L, R.DurationSeconds)); }
		O->SetArrayField(TEXT("inputs"), Ins);

		TArray<TSharedPtr<FJsonValue>> Outs;
		for (const FAIDAItemAmount& L : R.Products) { Outs.Add(ItemLineJson(L, R.DurationSeconds)); }
		O->SetArrayField(TEXT("outputs"), Outs);

		TArray<TSharedPtr<FJsonValue>> In;
		for (const FString& B : R.ProducedIn) { In.Add(MakeShared<FJsonValueString>(B)); }
		O->SetArrayField(TEXT("producedIn"), In);

		List.Add(MakeShared<FJsonValueObject>(O));
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("matches"), Matched.Num());
	Root->SetArrayField(TEXT("recipes"), List);
	if (Matched.Num() > Shown) { Root->SetNumberField(TEXT("omitted"), Matched.Num() - Shown); }
	return AIDAToCompactJson(Root);
}

FString AIDARecipeTools::BuildBuildingJson(const TArray<FAIDABuildingInfo>& Buildings, const FString& NameFilter)
{
	const bool bHasFilter = !NameFilter.IsEmpty();

	TArray<const FAIDABuildingInfo*> Matched;
	for (const FAIDABuildingInfo& B : Buildings)
	{
		if (!bHasFilter || B.Name.Contains(NameFilter, ESearchCase::IgnoreCase)) { Matched.Add(&B); }
	}

	const int32 Shown = FMath::Min(Matched.Num(), MaxBuildingsListed);
	TArray<TSharedPtr<FJsonValue>> List;
	for (int32 i = 0; i < Shown; ++i)
	{
		const FAIDABuildingInfo& B = *Matched[i];
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), B.Name);
		if (B.bVariablePower)
		{
			O->SetField(TEXT("minPowerMW"), AIDANumber(B.MinPowerMW));
			O->SetField(TEXT("maxPowerMW"), AIDANumber(B.MaxPowerMW));
		}
		else if (B.PowerConsumptionMW > 0.0)
		{
			O->SetField(TEXT("powerMW"), AIDANumber(B.PowerConsumptionMW));
		}
		if (B.PowerProductionMW > 0.0)
		{
			O->SetField(TEXT("powerProductionMW"), AIDANumber(B.PowerProductionMW));
		}
		List.Add(MakeShared<FJsonValueObject>(O));
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("matches"), Matched.Num());
	Root->SetArrayField(TEXT("buildings"), List);
	if (Matched.Num() > Shown) { Root->SetNumberField(TEXT("omitted"), Matched.Num() - Shown); }
	return AIDAToCompactJson(Root);
}
