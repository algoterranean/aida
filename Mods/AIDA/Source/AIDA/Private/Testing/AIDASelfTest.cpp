#include "Testing/AIDASelfTest.h"

#include "AIDA.h"
#include "Core/AIDAOrchestrator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "TimerManager.h"

namespace
{
	// Unity-build gotcha: anonymous namespaces merge across Private/*.cpp, so these helpers carry a
	// SelfTest prefix to stay unambiguous next to the test file's similarly-named ones.
	constexpr double kSelfTestStepIntervalSeconds = 0.5;   // one step per tick keeps the world responsive
	constexpr double kSelfTestTeleportSettleSeconds = 0.75; // camera/controller settle after a reposition

	TSharedPtr<FJsonObject> SelfTestParseJson(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

	FString SelfTestCompactJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	/** A top-level JSON value rendered to a canonical comparison/capture string. */
	bool SelfTestFieldToString(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FString& Out)
	{
		const TSharedPtr<FJsonValue> Value = Obj.IsValid() ? Obj->TryGetField(Field) : nullptr;
		if (!Value.IsValid()) { return false; }
		switch (Value->Type)
		{
		case EJson::String:  Out = Value->AsString(); return true;
		case EJson::Number:  Out = FString::SanitizeFloat(Value->AsNumber()); return true;
		case EJson::Boolean: Out = Value->AsBool() ? TEXT("true") : TEXT("false"); return true;
		default:             return false; // arrays/objects: assert via contains instead
		}
	}
}

bool AIDASelfTest::ParseScript(const FString& Json, FAIDASelfTestScript& Out, FString& OutError)
{
	const TSharedPtr<FJsonObject> Root = SelfTestParseJson(Json);
	if (!Root.IsValid())
	{
		OutError = TEXT("scenarios file is not valid JSON");
		return false;
	}

	FAIDASelfTestScript Script;
	Root->TryGetNumberField(TEXT("settleSeconds"), Script.SettleSeconds);
	Root->TryGetBoolField(TEXT("quitOnDone"), Script.bQuitOnDone);

	const TArray<TSharedPtr<FJsonValue>>* Scenarios = nullptr;
	if (!Root->TryGetArrayField(TEXT("scenarios"), Scenarios) || Scenarios->Num() == 0)
	{
		OutError = TEXT("'scenarios' array is required and must be non-empty");
		return false;
	}

	for (int32 s = 0; s < Scenarios->Num(); ++s)
	{
		const TSharedPtr<FJsonObject> ScenarioObj = (*Scenarios)[s]->AsObject();
		if (!ScenarioObj.IsValid())
		{
			OutError = FString::Printf(TEXT("scenarios[%d] is not an object"), s);
			return false;
		}
		FAIDASelfTestScenario Scenario;
		ScenarioObj->TryGetStringField(TEXT("name"), Scenario.Name);
		if (Scenario.Name.IsEmpty()) { Scenario.Name = FString::Printf(TEXT("scenario-%d"), s); }

		if (const TSharedPtr<FJsonObject>* Player = nullptr;
			ScenarioObj->TryGetObjectField(TEXT("player"), Player) && Player->IsValid())
		{
			const TSharedPtr<FJsonObject>* Pos = nullptr;
			if ((*Player)->TryGetObjectField(TEXT("pos"), Pos) && Pos->IsValid())
			{
				(*Pos)->TryGetNumberField(TEXT("x"), Scenario.PlayerPosM.X);
				(*Pos)->TryGetNumberField(TEXT("y"), Scenario.PlayerPosM.Y);
				(*Pos)->TryGetNumberField(TEXT("z"), Scenario.PlayerPosM.Z);
				Scenario.bHasPlayer = true;
			}
			(*Player)->TryGetNumberField(TEXT("yawDeg"), Scenario.YawDeg);
			(*Player)->TryGetNumberField(TEXT("pitchDeg"), Scenario.PitchDeg);
		}

		const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
		if (!ScenarioObj->TryGetArrayField(TEXT("steps"), Steps) || Steps->Num() == 0)
		{
			OutError = FString::Printf(TEXT("scenarios[%d] ('%s') has no steps"), s, *Scenario.Name);
			return false;
		}
		for (int32 t = 0; t < Steps->Num(); ++t)
		{
			const TSharedPtr<FJsonObject> StepObj = (*Steps)[t]->AsObject();
			if (!StepObj.IsValid())
			{
				OutError = FString::Printf(TEXT("%s steps[%d] is not an object"), *Scenario.Name, t);
				return false;
			}
			FAIDASelfTestStep Step;
			StepObj->TryGetStringField(TEXT("tool"), Step.Tool);
			StepObj->TryGetStringField(TEXT("action"), Step.Action);
			Step.Action = Step.Action.ToLower();
			if (Step.Tool.IsEmpty() == Step.Action.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s steps[%d] must have exactly one of 'tool' or 'action'"), *Scenario.Name, t);
				return false;
			}
			if (!Step.Tool.IsEmpty())
			{
				const TSharedPtr<FJsonObject>* Args = nullptr;
				Step.ArgsJson = StepObj->TryGetObjectField(TEXT("args"), Args) && Args->IsValid()
					? SelfTestCompactJson(Args->ToSharedRef()) : TEXT("{}");
			}
			else
			{
				if (Step.Action != TEXT("approve") && Step.Action != TEXT("reject")
					&& Step.Action != TEXT("nudge") && Step.Action != TEXT("wait"))
				{
					OutError = FString::Printf(TEXT("%s steps[%d]: unknown action '%s'"), *Scenario.Name, t, *Step.Action);
					return false;
				}
				StepObj->TryGetStringField(TEXT("proposalId"), Step.ActionTarget);
				StepObj->TryGetNumberField(TEXT("seconds"), Step.WaitSeconds);
				const TSharedPtr<FJsonObject>* Delta = nullptr;
				if (StepObj->TryGetObjectField(TEXT("deltaM"), Delta) && Delta->IsValid())
				{
					(*Delta)->TryGetNumberField(TEXT("x"), Step.NudgeDeltaM.X);
					(*Delta)->TryGetNumberField(TEXT("y"), Step.NudgeDeltaM.Y);
					(*Delta)->TryGetNumberField(TEXT("z"), Step.NudgeDeltaM.Z);
				}
				StepObj->TryGetNumberField(TEXT("yawDeg"), Step.NudgeYawDeg);
			}

			if (const TSharedPtr<FJsonObject>* Expect = nullptr;
				StepObj->TryGetObjectField(TEXT("expect"), Expect) && Expect->IsValid())
			{
				bool bOk = false;
				if ((*Expect)->TryGetBoolField(TEXT("ok"), bOk)) { Step.Expect.bOk = bOk; }
				if (const TSharedPtr<FJsonObject>* Fields = nullptr;
					(*Expect)->TryGetObjectField(TEXT("fields"), Fields) && Fields->IsValid())
				{
					for (const auto& Pair : (*Fields)->Values)
					{
						FString Value;
						switch (Pair.Value->Type)
						{
						case EJson::String:  Value = Pair.Value->AsString(); break;
						case EJson::Number:  Value = FString::SanitizeFloat(Pair.Value->AsNumber()); break;
						case EJson::Boolean: Value = Pair.Value->AsBool() ? TEXT("true") : TEXT("false"); break;
						default: OutError = FString::Printf(TEXT("%s steps[%d]: expect.fields.%s must be a scalar"), *Scenario.Name, t, *Pair.Key); return false;
						}
						Step.Expect.Fields.Add(Pair.Key, Value);
					}
				}
				const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
				if ((*Expect)->TryGetArrayField(TEXT("contains"), List))
				{
					for (const TSharedPtr<FJsonValue>& V : *List) { Step.Expect.Contains.Add(V->AsString()); }
				}
				if ((*Expect)->TryGetArrayField(TEXT("notContains"), List))
				{
					for (const TSharedPtr<FJsonValue>& V : *List) { Step.Expect.NotContains.Add(V->AsString()); }
				}
			}

			if (const TSharedPtr<FJsonObject>* Save = nullptr;
				StepObj->TryGetObjectField(TEXT("save"), Save) && Save->IsValid())
			{
				for (const auto& Pair : (*Save)->Values)
				{
					Step.Save.Add(Pair.Key, Pair.Value->AsString()); // result field -> variable name
				}
			}

			Scenario.Steps.Add(MoveTemp(Step));
		}
		Script.Scenarios.Add(MoveTemp(Scenario));
	}

	Out = MoveTemp(Script);
	return true;
}

FString AIDASelfTest::Substitute(const FString& In, const TMap<FString, FString>& Vars)
{
	FString Out;
	Out.Reserve(In.Len());
	for (int32 i = 0; i < In.Len();)
	{
		if (In[i] != '$')
		{
			Out.AppendChar(In[i++]);
			continue;
		}
		int32 j = i + 1;
		while (j < In.Len() && (FChar::IsAlnum(In[j]) || In[j] == '_')) { ++j; }
		const FString Name = In.Mid(i + 1, j - i - 1);
		if (const FString* Value = Name.IsEmpty() ? nullptr : Vars.Find(Name))
		{
			Out += *Value;
		}
		else
		{
			Out += In.Mid(i, j - i); // unknown/naked $ stays literal
		}
		i = j;
	}
	return Out;
}

bool AIDASelfTest::EvaluateExpect(const FAIDASelfTestExpect& Expect, bool bIsError, const FString& Content, FString& OutFailReason)
{
	if (Expect.bOk.IsSet() && Expect.bOk.GetValue() == bIsError)
	{
		OutFailReason = FString::Printf(TEXT("expected %s but tool returned %s: %s"),
			Expect.bOk.GetValue() ? TEXT("success") : TEXT("an error"),
			bIsError ? TEXT("an error") : TEXT("success"), *Content.Left(300));
		return false;
	}

	if (Expect.Fields.Num() > 0)
	{
		const TSharedPtr<FJsonObject> Obj = SelfTestParseJson(Content);
		if (!Obj.IsValid())
		{
			OutFailReason = TEXT("expect.fields set but the result is not a JSON object");
			return false;
		}
		for (const auto& Pair : Expect.Fields)
		{
			FString Actual;
			if (!SelfTestFieldToString(Obj, Pair.Key, Actual))
			{
				OutFailReason = FString::Printf(TEXT("result has no scalar field '%s'"), *Pair.Key);
				return false;
			}
			// Numbers compare numerically so 3 == 3.0; everything else compares exactly.
			double ExpectedNum = 0.0, ActualNum = 0.0;
			const bool bNumeric = LexTryParseString(ExpectedNum, *Pair.Value) && LexTryParseString(ActualNum, *Actual);
			const bool bEqual = bNumeric ? FMath::IsNearlyEqual(ExpectedNum, ActualNum, 0.0001) : Actual == Pair.Value;
			if (!bEqual)
			{
				OutFailReason = FString::Printf(TEXT("field '%s' is '%s', expected '%s'"), *Pair.Key, *Actual, *Pair.Value);
				return false;
			}
		}
	}

	for (const FString& Needle : Expect.Contains)
	{
		if (!Content.Contains(Needle))
		{
			OutFailReason = FString::Printf(TEXT("result does not contain '%s': %s"), *Needle, *Content.Left(300));
			return false;
		}
	}
	for (const FString& Needle : Expect.NotContains)
	{
		if (Content.Contains(Needle))
		{
			OutFailReason = FString::Printf(TEXT("result contains forbidden '%s'"), *Needle);
			return false;
		}
	}
	return true;
}

void AIDASelfTest::CaptureFields(const FString& Content, const TMap<FString, FString>& Save, TMap<FString, FString>& OutVars)
{
	if (Save.Num() == 0) { return; }
	const TSharedPtr<FJsonObject> Obj = SelfTestParseJson(Content);
	for (const auto& Pair : Save)
	{
		FString Value;
		if (SelfTestFieldToString(Obj, Pair.Key, Value))
		{
			OutVars.Add(Pair.Value, Value);
		}
	}
}

FString AIDASelfTest::ResultsToJson(const TArray<FAIDASelfTestOutcome>& Outcomes)
{
	int32 Passed = 0;
	TArray<TSharedPtr<FJsonValue>> List;
	for (const FAIDASelfTestOutcome& Outcome : Outcomes)
	{
		if (Outcome.bPassed) { ++Passed; }
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("scenario"), Outcome.Scenario);
		O->SetNumberField(TEXT("step"), Outcome.StepIndex);
		O->SetStringField(TEXT("label"), Outcome.Label);
		O->SetBoolField(TEXT("passed"), Outcome.bPassed);
		if (!Outcome.Reason.IsEmpty()) { O->SetStringField(TEXT("reason"), Outcome.Reason); }
		List.Add(MakeShared<FJsonValueObject>(O));
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("passed"), Passed);
	Root->SetNumberField(TEXT("failed"), Outcomes.Num() - Passed);
	Root->SetArrayField(TEXT("outcomes"), List);
	return SelfTestCompactJson(Root);
}

// ---------------------------------------------------------------------------------------------
// Runner (world side)
// ---------------------------------------------------------------------------------------------

bool FAIDASelfTestRunner::ShouldRun(FString& OutScriptPath)
{
	FString Path;
	if (!FParse::Value(FCommandLine::Get(), TEXT("AIDASelfTest="), Path) || Path.TrimStartAndEnd().IsEmpty())
	{
		return false;
	}
	Path = Path.TrimStartAndEnd().TrimQuotes();
	OutScriptPath = FPaths::IsRelative(Path)
		? FPaths::Combine(FPaths::ProjectDir(), TEXT("Configs"), TEXT("AIDA"), Path)
		: Path;
	return true;
}

void FAIDASelfTestRunner::Start(UAIDAOrchestrator* InOwner, const FString& ScriptPath)
{
	Owner = InOwner;
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World) { return; }

	FString Json;
	FString Error;
	if (!FFileHelper::LoadFileToString(Json, *ScriptPath))
	{
		UE_LOG(LogAIDA, Error, TEXT("[selftest] cannot read scenarios file '%s' â€” self-test disabled."), *ScriptPath);
		return;
	}
	if (!AIDASelfTest::ParseScript(Json, Script, Error))
	{
		UE_LOG(LogAIDA, Error, TEXT("[selftest] scenarios file '%s' invalid: %s â€” self-test disabled."), *ScriptPath, *Error);
		return;
	}

	UE_LOG(LogAIDA, Log, TEXT("[selftest] armed: %d scenario(s) from %s (waiting for the host player)."),
		Script.Scenarios.Num(), *ScriptPath);
	World->GetTimerManager().SetTimer(Timer, FTimerDelegate::CreateRaw(this, &FAIDASelfTestRunner::OnTimer),
		kSelfTestStepIntervalSeconds, /*bLoop*/ true);
}

void FAIDASelfTestRunner::Shutdown()
{
	if (Owner)
	{
		if (UWorld* World = Owner->GetWorld())
		{
			World->GetTimerManager().ClearTimer(Timer);
		}
	}
	Owner = nullptr;
}

void FAIDASelfTestRunner::OnTimer()
{
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	if (!World) { return; }
	const double Now = World->GetTimeSeconds();

	if (!bReady)
	{
		APlayerController* PC = World->GetFirstPlayerController();
		if (!PC || !PC->GetPawn()) { return; } // save still loading / menu â€” keep waiting
		if (ResumeAt <= 0.0)
		{
			ResumeAt = Now + FMath::Max(0.0, Script.SettleSeconds);
			UE_LOG(LogAIDA, Log, TEXT("[selftest] player ready â€” settling %.0fs before scenario 1."), Script.SettleSeconds);
			return;
		}
		if (Now < ResumeAt) { return; }
		bReady = true;
		ResumeAt = 0.0;
	}

	if (Now < ResumeAt) { return; } // wait step / post-teleport settle
	RunNextStep();
}

bool FAIDASelfTestRunner::PositionPlayer(const FAIDASelfTestScenario& Scenario)
{
	UWorld* World = Owner->GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!PC || !Pawn) { return false; }

	const FRotator Facing(0.0, Scenario.YawDeg, 0.0);
	Pawn->TeleportTo(Scenario.PlayerPosM * 100.0, Facing, /*bIsATest*/ false, /*bNoCheck*/ true);
	PC->SetControlRotation(FRotator(Scenario.PitchDeg, Scenario.YawDeg, 0.0));
	return true;
}

void FAIDASelfTestRunner::RecordOutcome(const FString& Label, bool bPassed, const FString& Reason)
{
	FAIDASelfTestOutcome Outcome;
	Outcome.Scenario = Script.Scenarios.IsValidIndex(ScenarioIdx) ? Script.Scenarios[ScenarioIdx].Name : TEXT("?");
	Outcome.StepIndex = StepIdx - 1;
	Outcome.Label = Label;
	Outcome.bPassed = bPassed;
	Outcome.Reason = Reason;
	if (bPassed)
	{
		UE_LOG(LogAIDA, Log, TEXT("[selftest] PASS %s/%d %s"), *Outcome.Scenario, Outcome.StepIndex, *Label);
	}
	else
	{
		UE_LOG(LogAIDA, Warning, TEXT("[selftest] FAIL %s/%d %s: %s"), *Outcome.Scenario, Outcome.StepIndex, *Label, *Reason);
	}
	Outcomes.Add(MoveTemp(Outcome));
}

void FAIDASelfTestRunner::RunNextStep()
{
	if (!Script.Scenarios.IsValidIndex(ScenarioIdx))
	{
		FinishAll();
		return;
	}
	const FAIDASelfTestScenario& Scenario = Script.Scenarios[ScenarioIdx];
	UWorld* World = Owner->GetWorld();

	if (!bScenarioPositioned)
	{
		bScenarioPositioned = true;
		UE_LOG(LogAIDA, Log, TEXT("[selftest] --- scenario '%s' (%d step(s)) ---"), *Scenario.Name, Scenario.Steps.Num());
		if (Scenario.bHasPlayer && PositionPlayer(Scenario))
		{
			ResumeAt = World->GetTimeSeconds() + kSelfTestTeleportSettleSeconds; // let the camera catch up (aim traces)
			return;
		}
	}

	if (!Scenario.Steps.IsValidIndex(StepIdx))
	{
		FinishScenario();
		return;
	}
	const FAIDASelfTestStep& Step = Scenario.Steps[StepIdx++];

	// The requester mirrors the listen host: empty PlayerId matches the host's empty net id (the
	// act-allowlist convention), and the tool context carries the pawn's live location.
	FAIDARequester Requester;
	Requester.Author = TEXT("SelfTest");
	FAIDAToolContext Ctx;
	Ctx.Author = TEXT("SelfTest");
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (const APawn* Pawn = PC->GetPawn())
		{
			Ctx.Location = Pawn->GetActorLocation();
			Ctx.bHasLocation = true;
		}
	}

	if (!Step.Action.IsEmpty())
	{
		if (Step.Action == TEXT("wait"))
		{
			ResumeAt = World->GetTimeSeconds() + FMath::Max(0.0, Step.WaitSeconds);
			RecordOutcome(FString::Printf(TEXT("wait %.1fs"), Step.WaitSeconds), true, FString());
			return;
		}

		if (Step.Action == TEXT("nudge"))
		{
			Owner->HandleProposalAdjust(Requester, Step.NudgeDeltaM * 100.0, Step.NudgeYawDeg, /*bQuietSuccess*/ true);
			RecordOutcome(TEXT("nudge"), true, FString());
			return;
		}

		// approve / reject: explicit id (or $variable), else the newest pending proposal.
		FGuid Target;
		const FString TargetStr = AIDASelfTest::Substitute(Step.ActionTarget, Vars).TrimStartAndEnd();
		if (!TargetStr.IsEmpty() && !FGuid::Parse(TargetStr, Target))
		{
			RecordOutcome(Step.Action, false, FString::Printf(TEXT("'%s' is not a proposal id"), *TargetStr));
			return;
		}
		if (!Target.IsValid())
		{
			int64 NewestUtc = -1;
			for (const FAIDAProposal& Proposal : Owner->Actions.Store().All())
			{
				if (Proposal.State == EAIDAProposalState::Pending && Proposal.ProposedUtc > NewestUtc)
				{
					NewestUtc = Proposal.ProposedUtc;
					Target = Proposal.Id;
				}
			}
		}
		if (!Target.IsValid())
		{
			RecordOutcome(Step.Action, false, TEXT("no pending proposal to decide"));
			return;
		}
		Owner->HandleProposalDecision(Requester, Target, Step.Action == TEXT("approve"));
		RecordOutcome(Step.Action, true, FString());
		return;
	}

	// Tool call through the SAME dispatch the model uses.
	const FString Args = AIDASelfTest::Substitute(Step.ArgsJson, Vars);
	const FAIDAToolResult Result = Owner->Tools.Dispatch(Step.Tool, Args, Ctx);
	UE_LOG(LogAIDA, Log, TEXT("[selftest] %s(%s) -> %s%s"), *Step.Tool, *Args.Left(200),
		Result.bIsError ? TEXT("ERROR: ") : TEXT(""), *Result.Content.Left(300));

	FString Reason;
	const bool bPassed = AIDASelfTest::EvaluateExpect(Step.Expect, Result.bIsError, Result.Content, Reason);
	RecordOutcome(Step.Tool, bPassed, Reason);
	AIDASelfTest::CaptureFields(Result.Content, Step.Save, Vars);
}

void FAIDASelfTestRunner::FinishScenario()
{
	// Scenarios stay independent: whatever pending proposals a scenario left behind are retired
	// quietly (approved/executed work stays â€” the test save is disposable by design).
	TArray<FGuid> Pending;
	for (const FAIDAProposal& Proposal : Owner->Actions.Store().All())
	{
		if (Proposal.State == EAIDAProposalState::Pending) { Pending.Add(Proposal.Id); }
	}
	for (const FGuid& Id : Pending)
	{
		Owner->SupersedeProposal(Id);
	}
	if (Pending.Num() > 0)
	{
		UE_LOG(LogAIDA, Log, TEXT("[selftest] scenario cleanup: %d pending proposal(s) retired."), Pending.Num());
	}

	++ScenarioIdx;
	StepIdx = 0;
	bScenarioPositioned = false;
	Vars.Reset(); // variables are per-scenario â€” cross-scenario coupling defeats independence
}

void FAIDASelfTestRunner::FinishAll()
{
	if (UWorld* World = Owner ? Owner->GetWorld() : nullptr)
	{
		World->GetTimerManager().ClearTimer(Timer);
	}

	int32 Passed = 0;
	for (const FAIDASelfTestOutcome& Outcome : Outcomes) { if (Outcome.bPassed) { ++Passed; } }

	FString OutPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("AIDASelfTestOut="), OutPath) || OutPath.TrimStartAndEnd().IsEmpty())
	{
		OutPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AIDA"), TEXT("selftest-results.json"));
	}
	FFileHelper::SaveStringToFile(AIDASelfTest::ResultsToJson(Outcomes), *OutPath.TrimQuotes());

	UE_LOG(LogAIDA, Log, TEXT("[selftest] DONE: %d passed, %d failed -> %s"),
		Passed, Outcomes.Num() - Passed, *OutPath);

	if (Script.bQuitOnDone)
	{
		UE_LOG(LogAIDA, Log, TEXT("[selftest] quitOnDone â€” exiting."));
		FPlatformMisc::RequestExit(/*Force*/ false);
	}
}
