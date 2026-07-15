#include "UI/AIDAMarkdown.h"

namespace
{
	/**
	 * Wrap each shortest pair of `Marker` in Open/Close tags. Unbalanced trailing markers are left as
	 * literal text. Case-sensitive so we don't disturb tag names.
	 */
	FString WrapPairs(const FString& In, const FString& Marker, const TCHAR* Open, const TCHAR* Close)
	{
		const int32 MLen = Marker.Len();
		FString Out;
		FString Rest = In;
		while (true)
		{
			const int32 Start = Rest.Find(Marker, ESearchCase::CaseSensitive);
			if (Start == INDEX_NONE)
			{
				Out += Rest;
				break;
			}
			Out += Rest.Mid(0, Start);
			const FString AfterOpen = Rest.Mid(Start + MLen);
			const int32 End = AfterOpen.Find(Marker, ESearchCase::CaseSensitive);
			if (End == INDEX_NONE)
			{
				Out += Marker; // unbalanced — leave the opening marker literal
				Out += AfterOpen;
				break;
			}
			if (End == 0)
			{
				Out += Marker; // empty span (adjacent markers) — treat the first marker as literal
				Rest = AfterOpen;
				continue;
			}
			Out += Open;
			Out += AfterOpen.Mid(0, End);
			Out += Close;
			Rest = AfterOpen.Mid(End + MLen);
		}
		return Out;
	}

	/** Inline spans: code first (so ** / * inside code is left alone), then bold, then italic. */
	FString FormatInline(const FString& Line)
	{
		FString S = WrapPairs(Line, TEXT("`"), TEXT("<Code>"), TEXT("</>"));
		S = WrapPairs(S, TEXT("**"), TEXT("<Bold>"), TEXT("</>"));
		S = WrapPairs(S, TEXT("*"), TEXT("<Italic>"), TEXT("</>"));
		return S;
	}
}

FString AIDAMarkdownToRichText(const FString& Markdown)
{
	TArray<FString> Lines;
	Markdown.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

	FString Out;
	bool bInFence = false;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString& Line = Lines[i];
		FString Trimmed = Line;
		Trimmed.TrimStartInline();

		if (Trimmed.StartsWith(TEXT("```")))
		{
			bInFence = !bInFence; // toggle; the fence line itself is dropped
		}
		else if (bInFence)
		{
			Out += FString::Printf(TEXT("<Code>%s</>"), *Line);
			if (i < Lines.Num() - 1) { Out += TEXT("\n"); }
			continue;
		}
		else if (Trimmed.StartsWith(TEXT("#")))
		{
			FString Header = Trimmed;
			while (Header.StartsWith(TEXT("#"))) { Header.RightChopInline(1); }
			Header.TrimStartInline();
			Out += FString::Printf(TEXT("<Header>%s</>"), *FormatInline(Header));
		}
		else if (Trimmed.StartsWith(TEXT("- ")) || Trimmed.StartsWith(TEXT("* ")))
		{
			Out += FString::Printf(TEXT("  • %s"), *FormatInline(Trimmed.RightChop(2)));
		}
		else
		{
			Out += FormatInline(Line);
		}

		if (i < Lines.Num() - 1) { Out += TEXT("\n"); }
	}
	return Out;
}
