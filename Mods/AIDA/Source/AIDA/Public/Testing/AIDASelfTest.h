#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

class UAIDAOrchestrator;
class UWorld;

/**
 * Packaged-game scenario harness (docs/SELFTEST.md). PIE can't run SML subsystems on this setup and
 * Shipping strips WITH_AUTOMATION_TESTS, so map-level testing happens INSIDE the real game: launch
 * with `-AIDASelfTest=<scenarios.json>`, load the curated test save, and the runner positions the
 * host player, drives the SAME tool dispatch the model uses, asserts on the JSON results, writes a
 * results file, and (optionally) quits. Tool-level only — deterministic, no LLM tokens.
 *
 * The script model + parse/substitute/evaluate are pure (unit-tested in the editor commandlet);
 * only FAIDASelfTestRunner touches the world.
 */

/** Assertions applied to one step's result. All present clauses must hold. */
struct FAIDASelfTestExpect
{
	TOptional<bool> bOk;                 // expected "did the tool succeed" (error results fail ok:true)
	TMap<FString, FString> Fields;       // top-level JSON field -> expected value (numbers compared numerically)
	TArray<FString> Contains;            // raw-JSON substrings that must appear
	TArray<FString> NotContains;         // raw-JSON substrings that must NOT appear
};

/** One step: a tool call, or a control action (approve/reject/nudge/wait). */
struct FAIDASelfTestStep
{
	// Tool call (Tool non-empty) — ArgsJson may reference captured variables as $name.
	FString Tool;
	FString ArgsJson;

	// Control action (Action non-empty): "approve" | "reject" | "nudge" | "wait".
	FString Action;
	FString ActionTarget;                // approve/reject: proposal id or $variable; "" = newest pending
	FVector NudgeDeltaM = FVector::ZeroVector;
	int32 NudgeYawDeg = 0;
	double WaitSeconds = 0.0;

	FAIDASelfTestExpect Expect;          // tool calls only (actions have no JSON result)
	TMap<FString, FString> Save;         // result field -> variable name (tool calls only)
};

/** One scenario: an optional player placement, then a step sequence. */
struct FAIDASelfTestScenario
{
	FString Name;
	bool bHasPlayer = false;
	FVector PlayerPosM = FVector::ZeroVector; // metres
	double YawDeg = 0.0;
	double PitchDeg = 0.0;
	TArray<FAIDASelfTestStep> Steps;
};

struct FAIDASelfTestScript
{
	double SettleSeconds = 5.0;          // wait after the player is ready before scenario 1
	bool bQuitOnDone = true;             // RequestExit once results are written
	TArray<FAIDASelfTestScenario> Scenarios;
};

/** One step's outcome for the results file. */
struct FAIDASelfTestOutcome
{
	FString Scenario;
	int32 StepIndex = 0;
	FString Label;                       // tool or action name
	bool bPassed = false;
	FString Reason;                      // failure reason ("" on pass)
};

namespace AIDASelfTest
{
	/** Parse a scenarios JSON document. False + OutError on malformed input. */
	bool ParseScript(const FString& Json, FAIDASelfTestScript& Out, FString& OutError);

	/** Replace $name tokens (letters/digits/underscore names) with captured variables; unknown names stay literal. */
	FString Substitute(const FString& In, const TMap<FString, FString>& Vars);

	/** Apply an expectation to a tool result. True = pass; false + OutFailReason otherwise. */
	bool EvaluateExpect(const FAIDASelfTestExpect& Expect, bool bIsError, const FString& Content, FString& OutFailReason);

	/** Capture top-level result fields into variables per the step's Save map (missing fields are skipped). */
	void CaptureFields(const FString& Content, const TMap<FString, FString>& Save, TMap<FString, FString>& OutVars);

	/** Render outcomes as the results-file JSON ({passed, failed, outcomes:[...]}). */
	FString ResultsToJson(const TArray<FAIDASelfTestOutcome>& Outcomes);
}

/**
 * The world-side driver, owned by the orchestrator (server only). Started on world begin play when
 * the launch flag is present; polls until the host player exists, settles, then advances one step
 * per timer tick (teleports settle for a beat before their scenario's first call). Pending
 * proposals left behind by a scenario are auto-rejected so scenarios stay independent.
 */
class FAIDASelfTestRunner
{
public:
	/** True when `-AIDASelfTest=<path>` is on the command line. Relative paths resolve under Configs/AIDA/. */
	static bool ShouldRun(FString& OutScriptPath);

	/** Load the script and start the readiness poll. No-op (with a log) if the script fails to parse. */
	void Start(UAIDAOrchestrator* InOwner, const FString& ScriptPath);

	/** Clear timers (orchestrator Deinitialize). */
	void Shutdown();

private:
	void OnTimer();
	void RunNextStep();
	void RecordOutcome(const FString& Label, bool bPassed, const FString& Reason);
	void FinishScenario();
	void FinishAll();

	/** Teleport + orient the host player for the current scenario (returns false when no player yet). */
	bool PositionPlayer(const FAIDASelfTestScenario& Scenario);

	UAIDAOrchestrator* Owner = nullptr;
	FAIDASelfTestScript Script;
	bool bReady = false;                 // player found + settle elapsed
	int32 ScenarioIdx = 0;
	int32 StepIdx = 0;
	bool bScenarioPositioned = false;
	double ResumeAt = 0.0;               // world seconds gate (settle / wait steps / post-teleport)
	TMap<FString, FString> Vars;
	TArray<FAIDASelfTestOutcome> Outcomes;
	FTimerHandle Timer;
};
