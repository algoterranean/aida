#include "Actions/AIDAChatCommands.h"

namespace
{
	const TCHAR* kUsage = TEXT("AIDA commands: /aida undo [n] — reverse the last n AI actions; /aida approve [id] | /aida reject [id] — decide a pending proposal (no id = the newest one); /aida nudge <north|south|east|west|up|down> [metres] and /aida rotate [degrees] — move/turn the pending proposal's ghost before approving.");

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
