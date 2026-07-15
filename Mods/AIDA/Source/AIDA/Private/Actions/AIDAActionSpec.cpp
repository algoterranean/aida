#include "Actions/AIDAActionSpec.h"

#include "Tools/AIDAToolJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr double MetersToCm = 100.0;

	/** Read a {x,y,z} object (metres; z optional) into a vector. False if the field is missing/malformed. */
	bool ReadVectorM(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field, FVector& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Parent->TryGetObjectField(Field, Obj) || !Obj) { return false; }
		double X = 0.0, Y = 0.0, Z = 0.0;
		if (!(*Obj)->TryGetNumberField(TEXT("x"), X) || !(*Obj)->TryGetNumberField(TEXT("y"), Y)) { return false; }
		(*Obj)->TryGetNumberField(TEXT("z"), Z); // optional; 0 = ground-level intent, dry-run projects anyway
		Out = FVector(X, Y, Z);
		return true;
	}

	/** Snap any yaw to the nearest 90° and normalize to {0, 90, 180, 270}. */
	int32 SnapYaw(double YawDeg)
	{
		const int32 Snapped = static_cast<int32>(FMath::RoundToDouble(YawDeg / 90.0)) * 90;
		return ((Snapped % 360) + 360) % 360;
	}

	/** Shared version gate: only spec version 1 exists. */
	bool CheckVersion(const TSharedPtr<FJsonObject>& Spec, FString& OutError)
	{
		int32 Version = 0;
		if (!Spec->TryGetNumberField(TEXT("version"), Version) || Version != 1)
		{
			OutError = TEXT("spec version must be 1");
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> CostToJson(const FAIDACostItem& Item)
	{
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("item"), Item.Item);
		O->SetNumberField(TEXT("amount"), Item.Amount);
		return O;
	}
}

bool AIDAActionSpec::ParseBuildSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDABuildSpec& Out, FString& OutError)
{
	if (!Spec.IsValid()) { OutError = TEXT("missing spec object"); return false; }
	if (!CheckVersion(Spec, OutError)) { return false; }

	FAIDABuildSpec Parsed;
	if (!Spec->TryGetStringField(TEXT("buildable"), Parsed.Buildable) || Parsed.Buildable.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("'buildable' (display name) is required");
		return false;
	}
	Parsed.Buildable = Parsed.Buildable.TrimStartAndEnd();

	if (!ReadVectorM(Spec, TEXT("origin"), Parsed.OriginM))
	{
		OutError = TEXT("'origin' must be an object with numeric x and y (metres)");
		return false;
	}

	double Yaw = 0.0;
	Spec->TryGetNumberField(TEXT("yawDeg"), Yaw);
	Parsed.YawDeg = SnapYaw(Yaw);

	const TSharedPtr<FJsonObject>* Grid = nullptr;
	if (Spec->TryGetObjectField(TEXT("grid"), Grid) && Grid)
	{
		int32 I;
		if ((*Grid)->TryGetNumberField(TEXT("countX"), I)) { Parsed.Grid.CountX = I; }
		if ((*Grid)->TryGetNumberField(TEXT("countY"), I)) { Parsed.Grid.CountY = I; }
		(*Grid)->TryGetNumberField(TEXT("stepX"), Parsed.Grid.StepXM);
		(*Grid)->TryGetNumberField(TEXT("stepY"), Parsed.Grid.StepYM);
	}
	if (Parsed.Grid.CountX < 1 || Parsed.Grid.CountY < 1)
	{
		OutError = TEXT("grid counts must be >= 1");
		return false;
	}
	if (Parsed.Grid.StepXM < 0.0 || Parsed.Grid.StepYM < 0.0)
	{
		OutError = TEXT("grid steps must be >= 0 (0 = buildable footprint)");
		return false;
	}
	const int64 Total = static_cast<int64>(Parsed.Grid.CountX) * Parsed.Grid.CountY;
	if (Total > MaxItems)
	{
		OutError = FString::Printf(TEXT("%lld placements exceeds the per-proposal cap of %d"), Total, MaxItems);
		return false;
	}

	Out = MoveTemp(Parsed);
	return true;
}

bool AIDAActionSpec::ParseDismantleSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDADismantleSpec& Out, FString& OutError)
{
	if (!Spec.IsValid()) { OutError = TEXT("missing selector object"); return false; }
	if (!CheckVersion(Spec, OutError)) { return false; }

	FAIDADismantleSpec Parsed;
	Spec->TryGetStringField(TEXT("buildable"), Parsed.Buildable); // "" = anything
	Parsed.Buildable = Parsed.Buildable.TrimStartAndEnd();

	if (!ReadVectorM(Spec, TEXT("center"), Parsed.CenterM))
	{
		OutError = TEXT("'center' must be an object with numeric x and y (metres)");
		return false;
	}
	if (!Spec->TryGetNumberField(TEXT("radiusM"), Parsed.RadiusM) || Parsed.RadiusM <= 0.0)
	{
		OutError = TEXT("'radiusM' must be a positive number");
		return false;
	}

	int32 MaxCount = 0;
	if (Spec->TryGetNumberField(TEXT("maxCount"), MaxCount))
	{
		if (MaxCount < 1) { OutError = TEXT("'maxCount' must be >= 1"); return false; }
		Parsed.MaxCount = MaxCount;
	}
	Parsed.MaxCount = FMath::Min(Parsed.MaxCount, MaxItems);

	Out = MoveTemp(Parsed);
	return true;
}

TArray<FTransform> AIDAActionSpec::ExpandGrid(const FAIDABuildSpec& Spec, double DefaultStepXM, double DefaultStepYM)
{
	const double StepXCm = (Spec.Grid.StepXM > 0.0 ? Spec.Grid.StepXM : DefaultStepXM) * MetersToCm;
	const double StepYCm = (Spec.Grid.StepYM > 0.0 ? Spec.Grid.StepYM : DefaultStepYM) * MetersToCm;

	// Step axes rotate with the yaw so a rotated grid stays coherent (rows follow the buildable's X).
	const double YawRad = FMath::DegreesToRadians(static_cast<double>(Spec.YawDeg));
	const FVector AxisX(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.0);
	const FVector AxisY(-FMath::Sin(YawRad), FMath::Cos(YawRad), 0.0);

	const FVector OriginCm = Spec.OriginM * MetersToCm;
	const FRotator Rotation(0.0, static_cast<double>(Spec.YawDeg), 0.0);

	TArray<FTransform> Out;
	Out.Reserve(Spec.Grid.CountX * Spec.Grid.CountY);
	for (int32 IY = 0; IY < Spec.Grid.CountY; ++IY)     // row-major from the origin
	{
		for (int32 IX = 0; IX < Spec.Grid.CountX; ++IX)
		{
			const FVector Pos = OriginCm + AxisX * (StepXCm * IX) + AxisY * (StepYCm * IY);
			Out.Emplace(Rotation, Pos);
		}
	}
	return Out;
}

FString AIDAActionSpec::SummarizeBuild(const FAIDABuildSpec& Spec)
{
	const int32 Total = Spec.Grid.CountX * Spec.Grid.CountY;
	if (Total == 1)
	{
		return FString::Printf(TEXT("place 1 x %s"), *Spec.Buildable);
	}
	return FString::Printf(TEXT("place %d x %s in a %dx%d grid"), Total, *Spec.Buildable, Spec.Grid.CountX, Spec.Grid.CountY);
}

FString AIDAActionSpec::SummarizeDismantle(const FAIDADismantleSpec& Spec)
{
	const FString What = Spec.Buildable.IsEmpty() ? TEXT("building") : Spec.Buildable;
	return FString::Printf(TEXT("dismantle up to %d x %s within %.0f m of (%.0f, %.0f)"),
		Spec.MaxCount, *What, Spec.RadiusM, Spec.CenterM.X, Spec.CenterM.Y);
}

FString AIDAActionSpec::StateToString(EAIDAProposalState State)
{
	switch (State)
	{
	case EAIDAProposalState::Pending:   return TEXT("pending");
	case EAIDAProposalState::Approved:  return TEXT("approved");
	case EAIDAProposalState::Executing: return TEXT("executing");
	case EAIDAProposalState::Executed:  return TEXT("executed");
	case EAIDAProposalState::Failed:    return TEXT("failed");
	case EAIDAProposalState::Rejected:  return TEXT("rejected");
	case EAIDAProposalState::Expired:   return TEXT("expired");
	case EAIDAProposalState::Undone:    return TEXT("undone");
	}
	return TEXT("unknown");
}

FString AIDAActionSpec::BuildDryRunJson(const FAIDAProposal& Proposal, int32 ExpiresInSec, bool bAffordable, double PowerDrawMW)
{
	TArray<TSharedPtr<FJsonValue>> Cost;
	for (const FAIDACostItem& Item : Proposal.Cost)
	{
		Cost.Add(MakeShared<FJsonValueObject>(CostToJson(Item)));
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("proposalId"), Proposal.Id.ToString(EGuidFormats::DigitsWithHyphens));
	Root->SetStringField(TEXT("summary"), Proposal.Summary);
	Root->SetNumberField(TEXT("count"), Proposal.bDismantle ? Proposal.TargetCount : Proposal.Placements.Num());
	Root->SetArrayField(Proposal.bDismantle ? TEXT("refund") : TEXT("cost"), Cost);
	if (!Proposal.bDismantle) { Root->SetBoolField(TEXT("affordable"), bAffordable); }
	if (PowerDrawMW > 0.0) { Root->SetField(TEXT("powerDrawMW"), AIDANumber(PowerDrawMW)); }
	Root->SetStringField(TEXT("status"), TEXT("awaiting approval"));
	Root->SetNumberField(TEXT("expiresInSec"), ExpiresInSec);
	return AIDAToCompactJson(Root);
}

FString AIDAActionSpec::BuildErrorJson(const FString& Error, const TArray<FAIDAPlacementFailure>& FirstFailures, int32 MaxShown)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("error"), Error);

	const int32 Shown = FMath::Min(FirstFailures.Num(), MaxShown);
	if (Shown > 0)
	{
		TArray<TSharedPtr<FJsonValue>> List;
		for (int32 i = 0; i < Shown; ++i)
		{
			const FAIDAPlacementFailure& F = FirstFailures[i];
			const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("index"), F.Index);
			const TSharedRef<FJsonObject> At = MakeShared<FJsonObject>();
			At->SetField(TEXT("x"), AIDANumber(F.AtM.X));
			At->SetField(TEXT("y"), AIDANumber(F.AtM.Y));
			O->SetObjectField(TEXT("at"), At);
			O->SetStringField(TEXT("reason"), F.Reason);
			List.Add(MakeShared<FJsonValueObject>(O));
		}
		Root->SetArrayField(TEXT("firstFailures"), List);
		if (FirstFailures.Num() > Shown) { Root->SetNumberField(TEXT("omitted"), FirstFailures.Num() - Shown); }
	}
	return AIDAToCompactJson(Root);
}

FString AIDAActionSpec::BuildStatusJson(const TArray<FAIDAProposal>& Proposals, const FGuid& Filter, int64 NowUtc, int32 TtlSeconds)
{
	TArray<TSharedPtr<FJsonValue>> List;
	for (const FAIDAProposal& P : Proposals)
	{
		if (Filter.IsValid() && P.Id != Filter) { continue; }

		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("proposalId"), P.Id.ToString(EGuidFormats::DigitsWithHyphens));
		O->SetStringField(TEXT("summary"), P.Summary);
		O->SetStringField(TEXT("state"), StateToString(P.State));
		O->SetStringField(TEXT("requester"), P.RequesterName);
		if (P.State == EAIDAProposalState::Pending)
		{
			const int64 Left = (P.ProposedUtc + TtlSeconds) - NowUtc;
			O->SetNumberField(TEXT("expiresInSec"), FMath::Max<int64>(0, Left));
		}
		List.Add(MakeShared<FJsonValueObject>(O));
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("count"), List.Num());
	Root->SetArrayField(TEXT("proposals"), List);
	return AIDAToCompactJson(Root);
}

FString AIDAActionSpec::EncodeEntityId(const FAIDAEntityId& Entity)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("t"), Entity.Type);
	Root->SetStringField(TEXT("class"), Entity.ClassPath);
	if (Entity.Type == TEXT("actor") && !Entity.RecipePath.IsEmpty())
	{
		Root->SetStringField(TEXT("recipe"), Entity.RecipePath);
	}
	if (Entity.Type == TEXT("lw"))
	{
		Root->SetNumberField(TEXT("idx"), Entity.Index);
	}
	// Positions stay full-precision world units (cm): these are re-resolve keys, not model-facing text.
	TArray<TSharedPtr<FJsonValue>> Pos;
	Pos.Add(MakeShared<FJsonValueNumber>(Entity.Pos.X));
	Pos.Add(MakeShared<FJsonValueNumber>(Entity.Pos.Y));
	Pos.Add(MakeShared<FJsonValueNumber>(Entity.Pos.Z));
	Root->SetArrayField(TEXT("pos"), Pos);
	Root->SetNumberField(TEXT("yaw"), Entity.YawDeg);
	return AIDAToCompactJson(Root);
}

bool AIDAActionSpec::DecodeEntityId(const FString& Encoded, FAIDAEntityId& Out)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Encoded);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) { return false; }

	FAIDAEntityId Parsed;
	if (!Root->TryGetStringField(TEXT("t"), Parsed.Type) ||
		(Parsed.Type != TEXT("lw") && Parsed.Type != TEXT("actor")))
	{
		return false;
	}
	if (!Root->TryGetStringField(TEXT("class"), Parsed.ClassPath) || Parsed.ClassPath.IsEmpty()) { return false; }
	Root->TryGetStringField(TEXT("recipe"), Parsed.RecipePath);

	if (Parsed.Type == TEXT("lw"))
	{
		int32 Idx = INDEX_NONE;
		if (!Root->TryGetNumberField(TEXT("idx"), Idx)) { return false; }
		Parsed.Index = Idx;
	}

	const TArray<TSharedPtr<FJsonValue>>* Pos = nullptr;
	if (!Root->TryGetArrayField(TEXT("pos"), Pos) || !Pos || Pos->Num() != 3) { return false; }
	for (int32 i = 0; i < 3; ++i)
	{
		double V = 0.0;
		if (!(*Pos)[i].IsValid() || !(*Pos)[i]->TryGetNumber(V)) { return false; }
		Parsed.Pos[i] = V;
	}

	int32 Yaw = 0;
	Root->TryGetNumberField(TEXT("yaw"), Yaw);
	Parsed.YawDeg = Yaw;

	Out = MoveTemp(Parsed);
	return true;
}
