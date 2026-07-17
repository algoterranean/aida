# AIDA Self-Test Harness — map-level scenarios in the packaged game

Automated integration tests that run **inside the real packaged game** against a curated test save:
position the host player at specific spots, drive the exact tool dispatch the model uses, assert on
the JSON results, write a results file, quit. Tool-level only — deterministic, no LLM tokens.

## Why this shape

- **PIE is out**: SML doesn't load mods in the editor on this setup (`ENABLE_MOD_LOADING_IN_EDITOR=0`;
  forcing it crashes the editor in SML's session-settings stack — tried and reverted).
- **UE Automation in the packaged game is out**: the game cooks as Shipping, which strips
  `WITH_AUTOMATION_TESTS`.
- So the mod carries its own scenario runner (`Source/AIDA/*/Testing/AIDASelfTest.*`), armed only by
  a launch flag — completely inert in normal play.

## Running

```powershell
./tools/selftest.ps1                       # smoke scenarios, waits for results, prints PASS/FAIL
./tools/selftest.ps1 -Scenarios tools/selftest/my-scenarios.json -TimeoutMinutes 20
```

The script launches Satisfactory (Steam must be running) with:

```
FactoryGameSteam.exe -AIDASelfTest=<abs path to scenarios.json> -AIDASelfTestOut=<abs results path>
```

**Load the test save when the menu appears** (if your game version honors an auto-load argument,
pass it via `-ExtraArgs`). The runner waits for the host player to exist, settles
`settleSeconds`, then executes scenarios one step per half-second. Progress streams to
`%LOCALAPPDATA%\FactoryGame\Saved\Logs\FactoryGame.log` as `[selftest]` lines; the results JSON
lands at the `-AIDASelfTestOut` path (default `<repo>/.debug/selftest-results.json` via the script,
or `<game>/FactoryGame/Saved/AIDA/selftest-results.json` without it). `quitOnDone` (default true)
exits the game afterwards.

## The test save

Scenarios reference concrete places in a save you curate once: park machines/containers/belts at
known coordinates, save, and write scenarios against those coordinates (positions are in METRES,
matching every AIDA tool). Keep the save disposable — approve steps really build, and nothing saves
back unless you do so manually. A useful layout: a flat platform for build proposals, a row of
machines for manifolds, a deliberately broken belt for diagnostics, rough terrain for the
blocked-placement advisory.

## Scenario file

```jsonc
{
  "settleSeconds": 8,          // wait after the player exists before scenario 1
  "quitOnDone": true,
  "scenarios": [
    {
      "name": "manifold-a-pending-line",
      "player": {                       // optional: teleport + face the host player first
        "pos": { "x": 454, "y": 1202, "z": 197 },   // metres
        "yawDeg": 0, "pitchDeg": -40                 // aim matters for omitted-origin proposals
      },
      "steps": [
        {
          "tool": "propose_build",                   // any registered AIDA tool
          "args": { "spec": { "version": 1, "buildable": "Refinery",
                              "grid": { "countX": 3, "countY": 1 } } },
          "expect": {                                // all clauses must hold
            "ok": true,                              // tool must not return an error
            "fields": { "count": 3 },                // top-level JSON equality (numbers numeric)
            "contains": ["proposalId"],              // raw-JSON substrings
            "notContains": ["error"]
          },
          "save": { "proposalId": "machines" }       // capture result fields into variables
        },
        { "tool": "propose_manifold",
          "args": { "spec": { "version": 1, "kind": "pipe", "direction": "in",
                              "transport": "Pipeline" },
                    "forProposalId": "$machines" },  // $var substitution anywhere in args
          "expect": { "ok": true }, "save": { "proposalId": "combined" } },
        { "action": "nudge", "deltaM": { "x": 8 }, "yawDeg": 90 },
        { "action": "approve", "proposalId": "$combined" },
        { "action": "wait", "seconds": 10 },          // let the time-sliced executor finish
        { "tool": "get_proposal_status", "args": { "proposalId": "$combined" },
          "expect": { "contains": ["executed"] } }
      ]
    }
  ]
}
```

Steps are either a **tool call** (`tool` + `args`, with `expect`/`save`) or an **action**:
`approve` / `reject` (explicit `proposalId` or newest pending), `nudge` (`deltaM` metres +
`yawDeg`), `wait` (`seconds`). Variables are per-scenario; leftover pending proposals are retired
automatically between scenarios so they stay independent.

## Notes

- The runner's requester mirrors the listen host (empty player id), so `actions` permissions behave
  exactly as they do when the host plays.
- Assertions run against the same JSON the model would receive — if a scenario passes, the model
  sees the same thing.
- The pure pieces (parse / substitution / expectation evaluation) are unit-tested in the editor
  commandlet suite (`AIDA.SelfTest.Script`).
- Agent-level scenarios (scripted chat line → assert on the model's tool-call trace) are a later
  layer; the hooks (`[tools]` log lines) already exist.
