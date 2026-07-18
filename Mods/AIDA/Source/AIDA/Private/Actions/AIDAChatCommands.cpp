#include "Actions/AIDAChatCommands.h"

namespace
{
	const TCHAR* kUsage = TEXT("AIDA commands: /aida undo [n] — reverse the last n AI actions; /aida approve [id] | /aida reject [id] — decide a pending proposal (no id = the newest one); /aida nudge <north|south|east|west|up|down> [metres] and /aida rotate [degrees] — move/turn the pending proposal's ghost before approving; /aida task add \"<check>\" every <N>m | task list | task rm|pause|resume <n> — recurring read-only checks.");

	/** Satisfactory map convention: north = -Y, east = +X, up = +Z. */
	bool ParseCardinal(const FString& Token, FVector& OutDir)
	{
		if (Token.Equals(TEXT("north"), ESearchCase::IgnoreCase)) { OutDir = FVector(0, -1, 0); return true; }
		if (Token.Equals(TEXT("south"), ESearchCase::IgnoreCase)) { OutDir = FVector(0, 1, 0); return true; }
		if (Token.Equals(TEXT("east"), ESearchCase::IgnoreCase))  { OutDir = FVector(1, 0, 0); return true; }
		if (Token.Equals(TEXT("west"), ESearchCase::IgnoreCase))  { OutDir = FVector(-1, 0, 0); return true; }
		if (Token.Equals(TEXT("up"), ESearchCase::IgnoreCase))    { OutDir = FVector(0, 0, 1); return true; }
		if (Token.Equals(TEXT("down"), ESearchCase::IgnoreCase))  { OutDir = FVector(0, 0, -1); return true; }
		return false;
	}
}

bool AIDAChatCommands::TryParse(const FString& Text, FAIDAChatCommand& Out, FString& OutError)
{
	Out = FAIDAChatCommand();
	OutError.Reset();

	FString Trimmed = Text.TrimStartAndEnd();
	if (!Trimmed.StartsWith(TEXT("/aida"), ESearchCase::IgnoreCase))
	{
		return false; // plain chat
	}

	TArray<FString> Tokens;
	Trimmed.ParseIntoArrayWS(Tokens);
	// Tokens[0] == "/aida"; require it to be exactly that (not "/aidafoo").
	if (!Tokens[0].Equals(TEXT("/aida"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	if (Tokens.Num() >= 2 && Tokens[1].Equals(TEXT("undo"), ESearchCase::IgnoreCase))
	{
		Out.Kind = FAIDAChatCommand::EKind::Undo;
		Out.Count = 1;
		if (Tokens.Num() >= 3)
		{
			const int32 Count = FCString::Atoi(*Tokens[2]);
			if (Count < 1)
			{
				Out.Kind = FAIDAChatCommand::EKind::None;
				OutError = TEXT("usage: /aida undo [n] — n must be a positive number.");
				return true;
			}
			Out.Count = Count;
		}
		return true;
	}

	if (Tokens.Num() >= 2 &&
		(Tokens[1].Equals(TEXT("approve"), ESearchCase::IgnoreCase) || Tokens[1].Equals(TEXT("reject"), ESearchCase::IgnoreCase)))
	{
		Out.Kind = Tokens[1].Equals(TEXT("approve"), ESearchCase::IgnoreCase)
			? FAIDAChatCommand::EKind::Approve : FAIDAChatCommand::EKind::Reject;
		if (Tokens.Num() >= 3 && !FGuid::Parse(Tokens[2], Out.ProposalId))
		{
			Out.Kind = FAIDAChatCommand::EKind::None;
			OutError = TEXT("usage: /aida approve [proposalId] — omit the id to decide the newest pending proposal.");
		}
		return true;
	}

	if (Tokens.Num() >= 2 && Tokens[1].Equals(TEXT("nudge"), ESearchCase::IgnoreCase))
	{
		FVector Dir;
		if (Tokens.Num() < 3 || !ParseCardinal(Tokens[2], Dir))
		{
			OutError = TEXT("usage: /aida nudge <north|south|east|west|up|down> [metres] — default 8 m (one tile).");
			return true;
		}
		Out.Kind = FAIDAChatCommand::EKind::Nudge;
		Out.NudgeDir = Dir;
		if (Tokens.Num() >= 4)
		{
			const double Dist = FCString::Atod(*Tokens[3]);
			if (Dist <= 0.0)
			{
				Out.Kind = FAIDAChatCommand::EKind::None;
				OutError = TEXT("usage: /aida nudge <direction> [metres] — metres must be positive.");
				return true;
			}
			Out.NudgeDistM = Dist;
		}
		return true;
	}

	if (Tokens.Num() >= 2 && Tokens[1].Equals(TEXT("task"), ESearchCase::IgnoreCase))
	{
		Out.Kind = FAIDAChatCommand::EKind::Task;
		const FString Op = Tokens.Num() >= 3 ? Tokens[2] : TEXT("list");
		if (Op.Equals(TEXT("list"), ESearchCase::IgnoreCase))
		{
			Out.TaskOp = FAIDAChatCommand::ETaskOp::List;
			return true;
		}
		if (Op.Equals(TEXT("add"), ESearchCase::IgnoreCase))
		{
			// The prompt is the QUOTED span — whitespace tokens can't carry it.
			const int32 OpenQuote = Trimmed.Find(TEXT("\""));
			const int32 CloseQuote = OpenQuote != INDEX_NONE
				? Trimmed.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenQuote + 1) : INDEX_NONE;
			if (OpenQuote == INDEX_NONE || CloseQuote == INDEX_NONE || CloseQuote - OpenQuote < 2)
			{
				Out.Kind = FAIDAChatCommand::EKind::None;
				OutError = TEXT("usage: /aida task add \"<what to check>\" every <minutes>m — the check text must be in quotes.");
				return true;
			}
			Out.TaskOp = FAIDAChatCommand::ETaskOp::Add;
			Out.TaskPrompt = Trimmed.Mid(OpenQuote + 1, CloseQuote - OpenQuote - 1).TrimStartAndEnd();

			// Cadence: `every 10m` / `every 2h` / `every 30` (minutes) after the closing quote.
			TArray<FString> Tail;
			Trimmed.Mid(CloseQuote + 1).TrimStartAndEnd().ParseIntoArrayWS(Tail);
			if (Tail.Num() >= 2 && Tail[0].Equals(TEXT("every"), ESearchCase::IgnoreCase))
			{
				FString Interval = Tail[1];
				int32 Multiplier = 1;
				if (Interval.EndsWith(TEXT("h"), ESearchCase::IgnoreCase)) { Multiplier = 60; Interval.LeftChopInline(1); }
				else if (Interval.EndsWith(TEXT("m"), ESearchCase::IgnoreCase)) { Interval.LeftChopInline(1); }
				const int32 Minutes = FCString::Atoi(*Interval) * Multiplier;
				if (Minutes < 1)
				{
					Out.Kind = FAIDAChatCommand::EKind::None;
					OutError = TEXT("usage: /aida task add \"<check>\" every <N>m|<N>h — the interval must be positive.");
					return true;
				}
				Out.TaskIntervalMinutes = Minutes;
			}
			return true;
		}
		if (Op.Equals(TEXT("rm"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("remove"), ESearchCase::IgnoreCase)
			|| Op.Equals(TEXT("pause"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("resume"), ESearchCase::IgnoreCase))
		{
			Out.TaskOp = Op.Equals(TEXT("pause"), ESearchCase::IgnoreCase) ? FAIDAChatCommand::ETaskOp::Pause
				: (Op.Equals(TEXT("resume"), ESearchCase::IgnoreCase) ? FAIDAChatCommand::ETaskOp::Resume
					: FAIDAChatCommand::ETaskOp::Remove);
			const int32 Index = Tokens.Num() >= 4 ? FCString::Atoi(*Tokens[3]) : 0;
			if (Index < 1)
			{
				Out.Kind = FAIDAChatCommand::EKind::None;
				OutError = TEXT("usage: /aida task rm|pause|resume <n> — n is the task number from /aida task list.");
				return true;
			}
			Out.TaskIndex = Index;
			return true;
		}
		Out.Kind = FAIDAChatCommand::EKind::None;
		OutError = TEXT("usage: /aida task add \"<check>\" every <N>m | /aida task list | /aida task rm|pause|resume <n>.");
		return true;
	}

	if (Tokens.Num() >= 2 && Tokens[1].Equals(TEXT("rotate"), ESearchCase::IgnoreCase))
	{
		Out.Kind = FAIDAChatCommand::EKind::Rotate;
		if (Tokens.Num() >= 3)
		{
			const int32 Deg = FCString::Atoi(*Tokens[2]);
			if (Deg == 0)
			{
				Out.Kind = FAIDAChatCommand::EKind::None;
				OutError = TEXT("usage: /aida rotate [degrees] — a non-zero angle (default 90).");
				return true;
			}
			Out.RotateDeg = Deg;
		}
		return true;
	}

	OutError = kUsage;
	return true;
}
