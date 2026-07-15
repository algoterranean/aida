#include "Actions/AIDAChatCommands.h"

namespace
{
	const TCHAR* kUsage = TEXT("AIDA commands: /aida undo [n] — reverse the last n AI actions (default 1).");
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

	OutError = kUsage;
	return true;
}
