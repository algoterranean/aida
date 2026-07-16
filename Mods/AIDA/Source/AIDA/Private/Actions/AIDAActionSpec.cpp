#include "Actions/AIDAActionSpec.h"

#include "Tools/AIDAToolJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace
{
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

	/** Shared version gate: only spec version 1 exists (build specs accept 2 via CheckBuildVersion). */
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

	/** Build-spec version gate: 1 = single buildable, 2 = composite parts (docs/PHASE7.md Slice 4). */
	bool CheckBuildVersion(const TSharedPtr<FJsonObject>& Spec, int32& OutVersion, FString& OutError)
	{
		OutVersion = 0;
		if (!Spec->TryGetNumberField(TEXT("version"), OutVersion) || (OutVersion != 1 && OutVersion != 2))
		{
			OutError = TEXT("spec version must be 1 (single buildable) or 2 (composite parts)");
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

	FAIDABuildSpec Parsed;
	if (!CheckBuildVersion(Spec, Parsed.Version, OutError)) { return false; }

	if (Parsed.Version == 2)
	{
		// Composite: shared origin/yaw + a part list. Belts/wires (the PHASE7 Slice 4 second half)
		// are not accepted yet — reject loudly rather than silently dropping them.
		if (Spec->HasField(TEXT("belts")) || Spec->HasField(TEXT("wires")))
		{
			OutError = TEXT("'belts'/'wires' are not supported yet — propose the parts now and connect them later");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* PartsArr = nullptr;
		if (!Spec->TryGetArrayField(TEXT("parts"), PartsArr) || !PartsArr || PartsArr->Num() == 0)
		{
			OutError = TEXT("version 2 requires a non-empty 'parts' array");
			return false;
		}
		constexpr int32 kMaxParts = 32;
		if (PartsArr->Num() > kMaxParts)
		{
			OutError = FString::Printf(TEXT("%d parts exceeds the per-proposal cap of %d"), PartsArr->Num(), kMaxParts);
			return false;
		}

		int64 Total = 0;
		for (int32 i = 0; i < PartsArr->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* PartObj = nullptr;
			if (!(*PartsArr)[i].IsValid() || !(*PartsArr)[i]->TryGetObject(PartObj) || !PartObj)
			{
				OutError = FString::Printf(TEXT("parts[%d] must be an object"), i);
				return false;
			}

			FAIDABuildPart Part;
			if (!(*PartObj)->TryGetStringField(TEXT("buildable"), Part.Buildable) || Part.Buildable.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("parts[%d].buildable (display name) is required"), i);
				return false;
			}
			Part.Buildable = Part.Buildable.TrimStartAndEnd();

			if ((*PartObj)->HasField(TEXT("at")) && !ReadVectorM(*PartObj, TEXT("at"), Part.OffsetM))
			{
				OutError = FString::Printf(TEXT("parts[%d].at must be an object with numeric x and y (metres, relative to the origin) — or omit it for {0,0,0}"), i);
				return false;
			}

			double PartYaw = 0.0;
			(*PartObj)->TryGetNumberField(TEXT("yawDeg"), PartYaw);
			Part.YawDeg = SnapYaw(PartYaw);

			const TSharedPtr<FJsonObject>* Grid = nullptr;
			if ((*PartObj)->TryGetObjectField(TEXT("grid"), Grid) && Grid)
			{
				int32 I;
				if ((*Grid)->TryGetNumberField(TEXT("countX"), I)) { Part.Grid.CountX = I; }
				if ((*Grid)->TryGetNumberField(TEXT("countY"), I)) { Part.Grid.CountY = I; }
				(*Grid)->TryGetNumberField(TEXT("stepX"), Part.Grid.StepXM);
				(*Grid)->TryGetNumberField(TEXT("stepY"), Part.Grid.StepYM);
			}
			if (Part.Grid.CountX < 1 || Part.Grid.CountY < 1)
			{
				OutError = FString::Printf(TEXT("parts[%d]: grid counts must be >= 1"), i);
				return false;
			}
			if (Part.Grid.StepXM < 0.0 || Part.Grid.StepYM < 0.0)
			{
				OutError = FString::Printf(TEXT("parts[%d]: grid steps must be >= 0 (0 = buildable footprint)"), i);
				return false;
			}

			Total += static_cast<int64>(Part.Grid.CountX) * Part.Grid.CountY;
			Parsed.Parts.Add(MoveTemp(Part));
		}
		if (MaxItems > 0 && Total > MaxItems)
		{
			OutError = FString::Printf(TEXT("%lld placements across parts exceeds the per-proposal cap of %d"), Total, MaxItems);
			return false;
		}

		if (Spec->HasField(TEXT("origin")))
		{
			if (!ReadVectorM(Spec, TEXT("origin"), Parsed.OriginM))
			{
				OutError = TEXT("'origin' must be an object with numeric x and y (metres) — or omit it to build at the player");
				return false;
			}
			Parsed.bHasOrigin = true;
		}
		double Yaw = 0.0;
		Spec->TryGetNumberField(TEXT("yawDeg"), Yaw);
		Parsed.YawDeg = SnapYaw(Yaw);
		Parsed.bPower = false; // v2: no auto-power — include poles as parts; wire routing lands with P7

		Out = MoveTemp(Parsed);
		return true;
	}

	if (!Spec->TryGetStringField(TEXT("buildable"), Parsed.Buildable) || Parsed.Buildable.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("'buildable' (display name) is required");
		return false;
	}
	Parsed.Buildable = Parsed.Buildable.TrimStartAndEnd();

	// origin is optional: omitted = "at the requesting player" (the tool fills it from Ctx.Location).
	// A present-but-malformed origin is still an error (a garbled position must not silently relocate).
	if (Spec->HasField(TEXT("origin")))
	{
		if (!ReadVectorM(Spec, TEXT("origin"), Parsed.OriginM))
		{
			OutError = TEXT("'origin' must be an object with numeric x and y (metres) — or omit it to build at the player");
			return false;
		}
		Parsed.bHasOrigin = true;
	}

	double Yaw = 0.0;
	Spec->TryGetNumberField(TEXT("yawDeg"), Yaw);
	Parsed.YawDeg = SnapYaw(Yaw);

	Spec->TryGetBoolField(TEXT("followTerrain"), Parsed.bFollowTerrain);
	Spec->TryGetBoolField(TEXT("power"), Parsed.bPower); // default true (docs/PHASE4-POWER.md)
	Spec->TryGetStringField(TEXT("pole"), Parsed.Pole);
	Parsed.Pole = Parsed.Pole.TrimStartAndEnd();

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
	if (MaxItems > 0 && Total > MaxItems) // MaxItems 0 = unlimited
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

	// center is optional, like a build spec's origin: omitted = "around the requesting player".
	if (Spec->HasField(TEXT("center")))
	{
		if (!ReadVectorM(Spec, TEXT("center"), Parsed.CenterM))
		{
			OutError = TEXT("'center' must be an object with numeric x and y (metres) — or omit it to search around the player");
			return false;
		}
		Parsed.bHasCenter = true;
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
	if (MaxItems > 0) { Parsed.MaxCount = FMath::Min(Parsed.MaxCount, MaxItems); } // 0 = unlimited

	Out = MoveTemp(Parsed);
	return true;
}

bool AIDAActionSpec::ParseLabelSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDALabelSpec& Out, FString& OutError)
{
	if (!Spec.IsValid()) { OutError = TEXT("missing spec object"); return false; }
	if (!CheckVersion(Spec, OutError)) { return false; }

	FAIDALabelSpec Parsed;
	Spec->TryGetStringField(TEXT("sign"), Parsed.Sign);
	Parsed.Sign = Parsed.Sign.TrimStartAndEnd();
	Spec->TryGetStringField(TEXT("item"), Parsed.ItemFilter);
	Parsed.ItemFilter = Parsed.ItemFilter.TrimStartAndEnd();

	// center is optional, like every selector: omitted = around the requester's aim/position.
	if (Spec->HasField(TEXT("center")))
	{
		if (!ReadVectorM(Spec, TEXT("center"), Parsed.CenterM))
		{
			OutError = TEXT("'center' must be an object with numeric x and y (metres) — or omit it to label containers around the player");
			return false;
		}
		Parsed.bHasCenter = true;
	}
	if (Spec->HasField(TEXT("radiusM")))
	{
		if (!Spec->TryGetNumberField(TEXT("radiusM"), Parsed.RadiusM) || Parsed.RadiusM <= 0.0)
		{
			OutError = TEXT("'radiusM' must be a positive number");
			return false;
		}
	}
	int32 MaxCount = 0;
	if (Spec->TryGetNumberField(TEXT("maxCount"), MaxCount))
	{
		if (MaxCount < 1) { OutError = TEXT("'maxCount' must be >= 1"); return false; }
		Parsed.MaxCount = MaxCount;
	}
	if (MaxItems > 0) { Parsed.MaxCount = FMath::Min(Parsed.MaxCount, MaxItems); }

	Out = MoveTemp(Parsed);
	return true;
}

FString AIDAActionSpec::SummarizeLabel(const FAIDALabelSpec& Spec, const FString& SignName, int32 Count)
{
	FString Summary = FString::Printf(TEXT("label %d container(s) with a %s each (contents as text) within %.0f m of (%.0f, %.0f)"),
		Count, *SignName, Spec.RadiusM, Spec.CenterM.X, Spec.CenterM.Y);
	if (!Spec.ItemFilter.IsEmpty())
	{
		Summary += FString::Printf(TEXT(", holding %s"), *Spec.ItemFilter);
	}
	return Summary;
}

bool AIDAActionSpec::ParsePowerSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDAPowerSpec& Out, FString& OutError)
{
	if (!Spec.IsValid()) { OutError = TEXT("missing spec object"); return false; }
	if (!CheckVersion(Spec, OutError)) { return false; }

	FAIDAPowerSpec Parsed;
	Spec->TryGetStringField(TEXT("buildable"), Parsed.Buildable);
	Parsed.Buildable = Parsed.Buildable.TrimStartAndEnd();
	Spec->TryGetStringField(TEXT("pole"), Parsed.Pole);
	Parsed.Pole = Parsed.Pole.TrimStartAndEnd();

	if (Spec->HasField(TEXT("center")))
	{
		if (!ReadVectorM(Spec, TEXT("center"), Parsed.CenterM))
		{
			OutError = TEXT("'center' must be an object with numeric x and y (metres) — or omit it to wire the machines near the player");
			return false;
		}
		Parsed.bHasCenter = true;
	}
	if (Spec->HasField(TEXT("radiusM")))
	{
		if (!Spec->TryGetNumberField(TEXT("radiusM"), Parsed.RadiusM) || Parsed.RadiusM <= 0.0)
		{
			OutError = TEXT("'radiusM' must be a positive number");
			return false;
		}
	}
	int32 MaxCount = 0;
	if (Spec->TryGetNumberField(TEXT("maxCount"), MaxCount))
	{
		if (MaxCount < 1) { OutError = TEXT("'maxCount' must be >= 1"); return false; }
		Parsed.MaxCount = MaxCount;
	}
	if (MaxItems > 0) { Parsed.MaxCount = Parsed.MaxCount > 0 ? FMath::Min(Parsed.MaxCount, MaxItems) : MaxItems; }

	Out = MoveTemp(Parsed);
	return true;
}

FAIDAPowerPlan AIDAActionSpec::PlanPowerForPoints(const TArray<FVector>& MachinesCm, int32 MachinesPerPole, double OffsetCm)
{
	FAIDAPowerPlan Plan;
	if (MachinesCm.Num() == 0)
	{
		Plan.Error = TEXT("no machines to power");
		return Plan;
	}
	const int32 PerPole = FMath::Max(1, MachinesPerPole);

	// Dominant world axis by extent: machine rows track the build grid closely enough to chunk by
	// projection, and the perpendicular offset pushes the poles out of the row line itself.
	FBox Bounds(ForceInit);
	for (const FVector& Machine : MachinesCm) { Bounds += Machine; }
	const FVector Size = Bounds.GetSize();
	const bool bAlongX = Size.X >= Size.Y;

	TArray<int32> Order;
	Order.Reserve(MachinesCm.Num());
	for (int32 i = 0; i < MachinesCm.Num(); ++i) { Order.Add(i); }
	Order.Sort([&MachinesCm, bAlongX](int32 A, int32 B)
	{
		return bAlongX ? MachinesCm[A].X < MachinesCm[B].X : MachinesCm[A].Y < MachinesCm[B].Y;
	});

	const FVector Perp = bAlongX ? FVector(0.0, 1.0, 0.0) : FVector(1.0, 0.0, 0.0);
	for (int32 Start = 0; Start < Order.Num(); Start += PerPole)
	{
		const int32 End = FMath::Min(Start + PerPole, Order.Num());
		FVector Centroid = FVector::ZeroVector;
		for (int32 i = Start; i < End; ++i) { Centroid += MachinesCm[Order[i]]; }
		Centroid /= (End - Start);

		const int32 PoleIndex = Plan.Poles.Num();
		Plan.Poles.Add(FTransform(Centroid + Perp * OffsetCm));
		for (int32 i = Start; i < End; ++i) { Plan.MachineWires.Add(FIntPoint(Order[i], PoleIndex)); }
		if (PoleIndex > 0) { Plan.ChainWires.Add(FIntPoint(PoleIndex - 1, PoleIndex)); }
	}
	return Plan;
}

bool AIDAActionSpec::ParseManifoldSpec(const TSharedPtr<FJsonObject>& Spec, int32 MaxItems, FAIDAManifoldSpec& Out, FString& OutError)
{
	if (!Spec.IsValid()) { OutError = TEXT("missing spec object"); return false; }
	if (!CheckVersion(Spec, OutError)) { return false; }

	FAIDAManifoldSpec Parsed;

	FString Kind = TEXT("belt");
	Spec->TryGetStringField(TEXT("kind"), Kind);
	Kind = Kind.TrimStartAndEnd().ToLower();
	if (Kind == TEXT("pipe")) { Parsed.bPipe = true; }
	else if (Kind != TEXT("belt")) { OutError = TEXT("'kind' must be \"belt\" or \"pipe\""); return false; }

	FString Direction = TEXT("in");
	Spec->TryGetStringField(TEXT("direction"), Direction);
	Direction = Direction.TrimStartAndEnd().ToLower();
	if (Direction == TEXT("out")) { Parsed.bOutput = true; }
	else if (Direction != TEXT("in")) { OutError = TEXT("'direction' must be \"in\" (feed inputs) or \"out\" (collect outputs)"); return false; }

	if (!Spec->TryGetStringField(TEXT("transport"), Parsed.Transport) || Parsed.Transport.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("'transport' (belt or pipe display name, e.g. \"Conveyor Belt Mk.2\") is required");
		return false;
	}
	Parsed.Transport = Parsed.Transport.TrimStartAndEnd();
	Spec->TryGetStringField(TEXT("attachment"), Parsed.Attachment);
	Parsed.Attachment = Parsed.Attachment.TrimStartAndEnd();

	const TSharedPtr<FJsonObject>* Machines = nullptr;
	if (!Spec->TryGetObjectField(TEXT("machines"), Machines) || !Machines)
	{
		OutError = TEXT("'machines' selector object is required");
		return false;
	}
	if (!(*Machines)->TryGetStringField(TEXT("buildable"), Parsed.Machines.Buildable) ||
		Parsed.Machines.Buildable.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("'machines.buildable' (machine display name) is required");
		return false;
	}
	Parsed.Machines.Buildable = Parsed.Machines.Buildable.TrimStartAndEnd();
	if ((*Machines)->HasField(TEXT("center")))
	{
		if (!ReadVectorM(*Machines, TEXT("center"), Parsed.Machines.CenterM))
		{
			OutError = TEXT("'machines.center' must be an object with numeric x and y (metres) — or omit it to use where the requester is aiming");
			return false;
		}
		Parsed.Machines.bHasCenter = true;
	}
	Parsed.Machines.RadiusM = 30.0;
	if ((*Machines)->TryGetNumberField(TEXT("radiusM"), Parsed.Machines.RadiusM) && Parsed.Machines.RadiusM <= 0.0)
	{
		OutError = TEXT("'machines.radiusM' must be a positive number");
		return false;
	}
	Parsed.Machines.MaxCount = 0; // 0 = every match in radius
	int32 MaxCount = 0;
	if ((*Machines)->TryGetNumberField(TEXT("maxCount"), MaxCount))
	{
		if (MaxCount < 0) { OutError = TEXT("'machines.maxCount' must be >= 0 (0 = all)"); return false; }
		Parsed.Machines.MaxCount = MaxCount;
	}
	if (MaxItems > 0)
	{
		Parsed.Machines.MaxCount = (Parsed.Machines.MaxCount == 0)
			? MaxItems : FMath::Min(Parsed.Machines.MaxCount, MaxItems);
	}

	if (Spec->TryGetNumberField(TEXT("standoffM"), Parsed.StandoffM) && (Parsed.StandoffM < 1.0 || Parsed.StandoffM > 20.0))
	{
		OutError = TEXT("'standoffM' must be between 1 and 20");
		return false;
	}
	int32 Port = 0;
	if (Spec->TryGetNumberField(TEXT("port"), Port))
	{
		if (Port < 0) { OutError = TEXT("'port' must be >= 0"); return false; }
		Parsed.PortIndex = Port;
	}

	Out = MoveTemp(Parsed);
	return true;
}

FAIDAManifoldPlan AIDAActionSpec::PlanManifold(const TArray<FAIDAManifoldPortPoint>& Ports, bool bOutput, bool bPipe,
	double StandoffM, double FootprintM, double MaxRunM)
{
	FAIDAManifoldPlan Plan;
	if (Ports.Num() < 1)
	{
		Plan.Error = TEXT("no machine ports to connect");
		return Plan;
	}

	// The machines must broadly face the same way (v1): the trunk is one straight line in front of
	// them. Average the XY normals and reject any port more than ~45° off the average.
	FVector AvgNormal = FVector::ZeroVector;
	for (const FAIDAManifoldPortPoint& Port : Ports)
	{
		AvgNormal += FVector(Port.NormalCm.X, Port.NormalCm.Y, 0.0).GetSafeNormal();
	}
	AvgNormal = FVector(AvgNormal.X, AvgNormal.Y, 0.0).GetSafeNormal();
	if (AvgNormal.IsNearlyZero())
	{
		Plan.Error = TEXT("machine ports face opposing directions — a manifold needs a row of machines facing the same way");
		return Plan;
	}
	for (const FAIDAManifoldPortPoint& Port : Ports)
	{
		const FVector N = FVector(Port.NormalCm.X, Port.NormalCm.Y, 0.0).GetSafeNormal();
		if (FVector::DotProduct(N, AvgNormal) < 0.7)
		{
			Plan.Error = TEXT("machine ports face different directions — a manifold needs a row of machines facing the same way (split into one manifold per row)");
			return Plan;
		}
	}

	Plan.DropDir = -AvgNormal;
	Plan.RowAxis = FVector::CrossProduct(FVector::UpVector, AvgNormal).GetSafeNormal();

	// Sort ports along the axis; attachments land at each port's projection onto the trunk line.
	TArray<int32> Order;
	Order.Reserve(Ports.Num());
	for (int32 i = 0; i < Ports.Num(); ++i) { Order.Add(i); }
	Order.Sort([&](int32 A, int32 B)
	{
		return FVector::DotProduct(Ports[A].PosCm, Plan.RowAxis) < FVector::DotProduct(Ports[B].PosCm, Plan.RowAxis);
	});

	const double FootprintCm = FootprintM * AIDAMetersToCm;
	const double MaxRunCm = MaxRunM * AIDAMetersToCm;
	const double StandoffCm = StandoffM * AIDAMetersToCm;

	// Attachment yaw: pass-through runs along the axis. Conveyor attachments flow local -X → +X, so
	// splitters take yaw = axis yaw (flow ascending, feed at index 0) and mergers flip 180° (flow
	// descending, collection at index 0 — the open end is always the first machine along the axis).
	const double AxisYaw = FMath::RadiansToDegrees(FMath::Atan2(Plan.RowAxis.Y, Plan.RowAxis.X));
	Plan.YawDeg = FMath::RoundToInt32(AxisYaw);
	if (bOutput && !bPipe) { Plan.YawDeg = ((Plan.YawDeg + 180) % 360 + 360) % 360; }

	// The trunk is ONE straight line (that's the point of a manifold): axis coordinate varies per
	// port, the normal coordinate is shared — the ports' centroid pushed out by the standoff.
	FVector Centroid = FVector::ZeroVector;
	for (const FAIDAManifoldPortPoint& Port : Ports) { Centroid += Port.PosCm; }
	Centroid /= Ports.Num();
	const double TrunkN = FVector::DotProduct(Centroid, AvgNormal) + StandoffCm;

	const FRotator Rotation(0.0, static_cast<double>(Plan.YawDeg), 0.0);
	double PrevT = 0.0;
	for (int32 i = 0; i < Order.Num(); ++i)
	{
		const FAIDAManifoldPortPoint& Port = Ports[Order[i]];
		const double T = FVector::DotProduct(Port.PosCm, Plan.RowAxis);
		if (i > 0)
		{
			const double Spacing = T - PrevT;
			if (Spacing < FootprintCm)
			{
				Plan.Error = FString::Printf(TEXT("machine ports %d and %d are %.1f m apart — closer than the %.0f m attachment footprint"),
					i - 1, i, Spacing / AIDAMetersToCm, FootprintM);
				return Plan;
			}
			if (Spacing > MaxRunCm)
			{
				Plan.Error = FString::Printf(TEXT("machine ports %d and %d are %.0f m apart — beyond the %.0f m maximum run length"),
					i - 1, i, Spacing / AIDAMetersToCm, MaxRunM);
				return Plan;
			}
		}
		PrevT = T;

		// Axis coordinate from the port, normal coordinate from the shared trunk line (axis and
		// normal are orthonormal in XY, so this reconstructs the world point directly). Z carries
		// the port height — the seam re-probes the ground under each center.
		FVector Center = Plan.RowAxis * T + AvgNormal * TrunkN;
		Center.Z = Port.PosCm.Z;
		Plan.Attachments.Emplace(Rotation, Center);
		Plan.PortOrder.Add(Order[i]);
	}

	return Plan;
}

FAIDAPowerPlan AIDAActionSpec::PlanPower(int32 CountX, int32 CountY, double StepXCm, double StepYCm, int32 YawDeg,
	const FVector& OriginCm, int32 MachinesPerPole)
{
	FAIDAPowerPlan Plan;
	if (CountX < 1 || CountY < 1 || StepXCm <= 0.0 || StepYCm <= 0.0)
	{
		Plan.Error = TEXT("degenerate grid for power planning");
		return Plan;
	}
	const int32 PerPole = FMath::Max(1, MachinesPerPole);

	// The grid's own axes (ExpandGrid's math) so the pole rows rotate with the build.
	const double YawRad = FMath::DegreesToRadians(static_cast<double>(YawDeg));
	const FVector AxisX(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.0);
	const FVector AxisY(-FMath::Sin(YawRad), FMath::Cos(YawRad), 0.0);
	const FRotator Rotation(0.0, static_cast<double>(YawDeg), 0.0);

	for (int32 IY = 0; IY < CountY; ++IY)
	{
		// Poles sit half a step OFF the row line — between this row and the next, so one lane of
		// poles serves the walkway; the last row of a multi-row grid folds back onto the previous
		// gap instead of jutting past the grid's edge — STAGGERED half a step along the row, or its
		// poles would land exactly on the previous row's (identical-overlap rejection).
		const bool bFoldBack = CountY > 1 && IY == CountY - 1;
		const double SideSign = bFoldBack ? -0.5 : 0.5;
		const double XStagger = bFoldBack ? 0.5 : 0.0;
		for (int32 IX0 = 0; IX0 < CountX; IX0 += PerPole)
		{
			const int32 IX1 = FMath::Min(IX0 + PerPole, CountX); // exclusive
			const double MidX = (IX0 + IX1 - 1) * 0.5 + XStagger;
			const FVector Center = OriginCm + AxisX * (MidX * StepXCm) + AxisY * ((IY + SideSign) * StepYCm);
			const int32 PoleIdx = Plan.Poles.Emplace(Rotation, Center);
			for (int32 IX = IX0; IX < IX1; ++IX)
			{
				Plan.MachineWires.Emplace(IY * CountX + IX, PoleIdx);
			}
		}
	}
	for (int32 i = 0; i + 1 < Plan.Poles.Num(); ++i)
	{
		Plan.ChainWires.Emplace(i, i + 1);
	}
	return Plan;
}

FString AIDAActionSpec::CompassName(const FVector& Dir)
{
	// Game convention (AIDAChatCommands): north = -Y, east = +X. 8-way, 45° sectors.
	static const TCHAR* Names[8] = { TEXT("north"), TEXT("northeast"), TEXT("east"), TEXT("southeast"),
		TEXT("south"), TEXT("southwest"), TEXT("west"), TEXT("northwest") };
	const double Deg = FMath::RadiansToDegrees(FMath::Atan2(Dir.X, -Dir.Y)); // 0 = north, 90 = east
	const int32 Sector = ((FMath::RoundToInt32(Deg / 45.0) % 8) + 8) % 8;
	return Names[Sector];
}

FString AIDAActionSpec::SummarizeManifold(const FAIDAManifoldSpec& Spec, const FString& AttachmentName, const FString& TransportName,
	int32 MachineCount, int32 RunCount, const FString& OpenEndCompass)
{
	const FString Verb = Spec.bOutput
		? FString::Printf(TEXT("collecting from %d x %s (output at the %s end)"), MachineCount, *Spec.Machines.Buildable, *OpenEndCompass)
		: FString::Printf(TEXT("feeding %d x %s (feed at the %s end)"), MachineCount, *Spec.Machines.Buildable, *OpenEndCompass);
	return FString::Printf(TEXT("manifold: %d x %s + %d x %s runs %s"),
		MachineCount, *AttachmentName, RunCount, *TransportName, *Verb);
}

TArray<FTransform> AIDAActionSpec::ExpandGrid(const FAIDABuildSpec& Spec, double DefaultStepXM, double DefaultStepYM)
{
	// Steps smaller than the footprint stack tiles on top of each other — never what a player wants
	// (live-verify: the model kept assuming a "Foundation (1 m)" tile is 1 m wide and packed 100
	// overlapping tiles). Clamp UP to the footprint; deliberate gaps (larger steps) stay allowed.
	const double StepXCm = FMath::Max(Spec.Grid.StepXM, DefaultStepXM) * AIDAMetersToCm;
	const double StepYCm = FMath::Max(Spec.Grid.StepYM, DefaultStepYM) * AIDAMetersToCm;

	// Step axes rotate with the yaw so a rotated grid stays coherent (rows follow the buildable's X).
	const double YawRad = FMath::DegreesToRadians(static_cast<double>(Spec.YawDeg));
	const FVector AxisX(FMath::Cos(YawRad), FMath::Sin(YawRad), 0.0);
	const FVector AxisY(-FMath::Sin(YawRad), FMath::Cos(YawRad), 0.0);

	const FVector OriginCm = Spec.OriginM * AIDAMetersToCm;
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

TArray<FTransform> AIDAActionSpec::ExpandParts(const FAIDABuildSpec& Spec, const TArray<FVector2D>& FootprintsM,
	TArray<int32>& OutPartIndex)
{
	OutPartIndex.Reset();
	TArray<FTransform> Out;

	// Part offsets rotate with the composite yaw so the whole arrangement turns as one rigid body.
	const double CompositeYawRad = FMath::DegreesToRadians(static_cast<double>(Spec.YawDeg));
	const FVector AxisX(FMath::Cos(CompositeYawRad), FMath::Sin(CompositeYawRad), 0.0);
	const FVector AxisY(-FMath::Sin(CompositeYawRad), FMath::Cos(CompositeYawRad), 0.0);

	for (int32 PartIdx = 0; PartIdx < Spec.Parts.Num(); ++PartIdx)
	{
		const FAIDABuildPart& Part = Spec.Parts[PartIdx];

		// Reuse the v1 grid expansion by synthesizing a single-part spec at the part's world origin.
		FAIDABuildSpec PartSpec;
		PartSpec.OriginM = Spec.OriginM
			+ AxisX * Part.OffsetM.X + AxisY * Part.OffsetM.Y + FVector(0, 0, Part.OffsetM.Z);
		PartSpec.YawDeg = ((Spec.YawDeg + Part.YawDeg) % 360 + 360) % 360;
		PartSpec.Grid = Part.Grid;

		const FVector2D Footprint = FootprintsM.IsValidIndex(PartIdx) ? FootprintsM[PartIdx] : FVector2D(8.0, 8.0);
		TArray<FTransform> Expanded = ExpandGrid(PartSpec, Footprint.X, Footprint.Y);
		for (FTransform& Placement : Expanded)
		{
			OutPartIndex.Add(PartIdx);
			Out.Add(MoveTemp(Placement));
		}
	}
	return Out;
}

FString AIDAActionSpec::SummarizeBuild(const FAIDABuildSpec& Spec)
{
	if (Spec.Parts.Num() > 0)
	{
		int32 Total = 0;
		TArray<FString> PartLines;
		for (const FAIDABuildPart& Part : Spec.Parts)
		{
			const int32 Count = Part.Grid.CountX * Part.Grid.CountY;
			Total += Count;
			if (PartLines.Num() < 4)
			{
				PartLines.Add(FString::Printf(TEXT("%d x %s"), Count, *Part.Buildable));
			}
		}
		if (Spec.Parts.Num() > 4)
		{
			PartLines.Add(FString::Printf(TEXT("+%d more part(s)"), Spec.Parts.Num() - 4));
		}
		return FString::Printf(TEXT("place a %d-part composite (%d placements: %s)"),
			Spec.Parts.Num(), Total, *FString::Join(PartLines, TEXT(", ")));
	}

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

FString AIDAActionSpec::BuildDryRunJson(const FAIDAProposal& Proposal, int32 ExpiresInSec, bool bAffordable, double PowerDrawMW,
	const FVector* OriginM)
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
	if (Proposal.bManifold)
	{
		const int32 N = Proposal.Placements.Num();
		Root->SetNumberField(TEXT("runs"), N > 0 ? (2 * N - 1) : 0);
		Root->SetStringField(TEXT("costNote"),
			TEXT("cost covers the attachments; belt/pipe runs are length-priced and charged from central storage as they build"));
	}
	if (PowerDrawMW > 0.0) { Root->SetField(TEXT("powerDrawMW"), AIDANumber(PowerDrawMW)); }
	if (OriginM)
	{
		// The resolved anchor (metres) — follow-up proposals can place parts relative to it.
		const TSharedRef<FJsonObject> Origin = MakeShared<FJsonObject>();
		Origin->SetField(TEXT("x"), AIDANumber(OriginM->X));
		Origin->SetField(TEXT("y"), AIDANumber(OriginM->Y));
		Origin->SetField(TEXT("z"), AIDANumber(OriginM->Z));
		Root->SetObjectField(TEXT("origin"), Origin);
	}
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

FString AIDAActionSpec::CostItemsToJson(const TArray<FAIDACostItem>& Items)
{
	TArray<TSharedPtr<FJsonValue>> List;
	for (const FAIDACostItem& Item : Items)
	{
		const TSharedRef<FJsonObject> O = CostToJson(Item);
		if (!Item.ClassPath.IsEmpty()) { O->SetStringField(TEXT("class"), Item.ClassPath); }
		List.Add(MakeShared<FJsonValueObject>(O));
	}
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(List, Writer);
	return Out;
}

TArray<FAIDACostItem> AIDAActionSpec::ParseCostItems(const FString& Json)
{
	TArray<FAIDACostItem> Out;
	TArray<TSharedPtr<FJsonValue>> List;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, List))
	{
		return Out;
	}
	for (const TSharedPtr<FJsonValue>& Value : List)
	{
		const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Obj.IsValid()) { continue; }
		FAIDACostItem Item;
		Obj->TryGetStringField(TEXT("item"), Item.Item);
		Obj->TryGetNumberField(TEXT("amount"), Item.Amount);
		Obj->TryGetStringField(TEXT("class"), Item.ClassPath);
		if (!Item.Item.IsEmpty() && Item.Amount > 0) { Out.Add(MoveTemp(Item)); }
	}
	return Out;
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
