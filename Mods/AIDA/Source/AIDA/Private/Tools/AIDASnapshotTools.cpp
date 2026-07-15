#include "Tools/AIDASnapshotTools.h"

#include "Tools/AIDAToolJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	constexpr int32 MaxItemDeltasListed = 15;
	constexpr double ChangeTolerance = 1e-3;

	FString SinceString(int64 Seconds)
	{
		if (Seconds < 0) { return TEXT("?"); }
		if (Seconds < 60) { return TEXT("just now"); }
		if (Seconds < 3600) { return FString::Printf(TEXT("%lldm ago"), Seconds / 60); }
		if (Seconds < 86400) { return FString::Printf(TEXT("%lldh ago"), Seconds / 3600); }
		return FString::Printf(TEXT("%lldd ago"), Seconds / 86400);
	}
}

FAIDASnapshot AIDASnapshotTools::MakeSnapshot(const FAIDAFactoryAggregates& Aggregates, int64 TakenUtc, const FString& Label)
{
	FAIDASnapshot S;
	S.TakenUtc = TakenUtc;
	S.Label = Label;
	for (const FAIDAItemBalance& B : Aggregates.Balance)
	{
		S.ItemBalance.Add({ B.Item, B.Net() });
	}
	for (const FAIDAPowerReport& P : Aggregates.Power)
	{
		S.PowerConsumedMW += P.ConsumedMW;
		S.PowerCapacityMW += P.CapacityMW;
	}
	return S;
}

const FAIDASnapshot* AIDASnapshotTools::PickBaseline(const TArray<FAIDASnapshot>& Snaps, int64 Timestamp, bool bHasTimestamp)
{
	if (Snaps.Num() == 0) { return nullptr; }
	if (!bHasTimestamp) { return &Snaps.Last(); } // most recent (stored oldest-first)

	// Newest snapshot at/or-before the requested time; fall back to the earliest if all are newer.
	const FAIDASnapshot* Best = nullptr;
	for (const FAIDASnapshot& S : Snaps)
	{
		if (S.TakenUtc <= Timestamp) { Best = &S; }
	}
	return Best ? Best : &Snaps[0];
}

FString AIDASnapshotTools::BuildCompareJson(const FAIDASnapshot& Baseline, const FAIDASnapshot& Current, const FString& ItemFilter, int64 NowUtc)
{
	// Index both sides by item so we can diff the union.
	TMap<FString, double> Base;
	for (const FAIDASnapshotItem& I : Baseline.ItemBalance) { Base.Add(I.Item, I.Net); }
	TMap<FString, double> Now;
	for (const FAIDASnapshotItem& I : Current.ItemBalance) { Now.Add(I.Item, I.Net); }

	TSet<FString> Items;
	for (const auto& Pair : Base) { Items.Add(Pair.Key); }
	for (const auto& Pair : Now) { Items.Add(Pair.Key); }

	struct FDelta { FString Item; double Was; double Nw; double Change; };
	TArray<FDelta> Deltas;
	for (const FString& Item : Items)
	{
		if (!ItemFilter.IsEmpty() && !Item.Contains(ItemFilter, ESearchCase::IgnoreCase)) { continue; }
		const double Was = Base.FindRef(Item);
		const double Nw = Now.FindRef(Item);
		const double Change = Nw - Was;
		if (FMath::Abs(Change) > ChangeTolerance) { Deltas.Add({ Item, Was, Nw, Change }); }
	}
	Deltas.Sort([](const FDelta& A, const FDelta& B) { return FMath::Abs(A.Change) > FMath::Abs(B.Change); });

	const int32 Shown = FMath::Min(Deltas.Num(), MaxItemDeltasListed);
	TArray<TSharedPtr<FJsonValue>> List;
	for (int32 i = 0; i < Shown; ++i)
	{
		const FDelta& D = Deltas[i];
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("item"), D.Item);
		O->SetField(TEXT("was"), AIDANumber(D.Was));
		O->SetField(TEXT("now"), AIDANumber(D.Nw));
		O->SetField(TEXT("delta"), AIDANumber(D.Change));
		List.Add(MakeShared<FJsonValueObject>(O));
	}

	const TSharedRef<FJsonObject> Power = MakeShared<FJsonObject>();
	Power->SetField(TEXT("consumedDeltaMW"), AIDANumber(Current.PowerConsumedMW - Baseline.PowerConsumedMW));
	Power->SetField(TEXT("capacityDeltaMW"), AIDANumber(Current.PowerCapacityMW - Baseline.PowerCapacityMW));

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("since"), SinceString(NowUtc - Baseline.TakenUtc));
	if (!Baseline.Label.IsEmpty()) { Root->SetStringField(TEXT("baselineLabel"), Baseline.Label); }
	Root->SetNumberField(TEXT("changedItems"), Deltas.Num());
	Root->SetArrayField(TEXT("items"), List);
	if (Deltas.Num() > Shown) { Root->SetNumberField(TEXT("omitted"), Deltas.Num() - Shown); }
	Root->SetObjectField(TEXT("power"), Power);
	return AIDAToCompactJson(Root);
}
