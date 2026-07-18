#pragma once

#include "CoreMinimal.h"

/**
 * The `/aida ...` chat-command surface (docs/PHASE4.md §4d). Parsed at the very top of
 * HandleChatRequest so a command NEVER reaches the LLM — undo in particular must not depend on a
 * model round-trip. Pure string parsing, unit-testable without the game.
 */
struct FAIDAChatCommand
{
	enum class EKind : uint8
	{
		None,     // "/aida" with an unknown/missing subcommand — OutError carries usage text
		Undo,     // "/aida undo [n]"
		Approve,  // "/aida approve [proposalId]" — no id = the newest pending proposal
		Reject,   // "/aida reject [proposalId]"
		Nudge,    // "/aida nudge <north|south|east|west|up|down> [metres]" — move the pending ghost
		Rotate,   // "/aida rotate [degrees]" — rotate the pending ghost (default 90)
		Task      // "/aida task add \"<prompt>\" every <N>m | list | rm/pause/resume <n>" (P8 Slice 5)
	};

	enum class ETaskOp : uint8 { List, Add, Remove, Pause, Resume };

	EKind Kind = EKind::None;
	int32 Count = 1;                          // Undo: how many actions to reverse
	FGuid ProposalId;                         // Approve/Reject: explicit target (invalid = newest pending)
	FVector NudgeDir = FVector::ZeroVector;   // Nudge: world-axis unit direction
	double NudgeDistM = 8.0;                  // Nudge: metres (default = one foundation tile)
	int32 RotateDeg = 90;                     // Rotate: yaw delta
	ETaskOp TaskOp = ETaskOp::List;           // Task: which sub-operation
	FString TaskPrompt;                       // Task add: the check to run (from the quoted string)
	int32 TaskIntervalMinutes = 10;           // Task add: requested cadence
	int32 TaskIndex = 0;                      // Task rm/pause/resume: 1-based index from `task list`
};

namespace AIDAChatCommands
{
	/**
	 * True when Text is a `/aida` command (it must then short-circuit the LLM), with the parse in
	 * Out. A recognized-but-malformed or unknown subcommand still returns true, with Kind None and
	 * a usage message in OutError. Plain chat returns false.
	 */
	bool TryParse(const FString& Text, FAIDAChatCommand& Out, FString& OutError);
}
