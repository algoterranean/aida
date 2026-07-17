#include "UI/AIDAMarkdown.h"

namespace
{
	/**
	 * Wrap each shortest pair of `Marker` in Open/Close tags. Unbalanced trailing markers, and empty
	 * spans (adjacent markers), are left as literal text. Case-sensitive so we don't disturb tag names.
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

	/** Split a Markdown table row "| a | b |" into trimmed cells (outer pipes dropped). */
	void ParseTableCells(const FString& Row, TArray<FString>& OutCells)
	{
		FString Trimmed = Row;
		Trimmed.TrimStartAndEndInline();
		Trimmed.RemoveFromStart(TEXT("|"));
		Trimmed.RemoveFromEnd(TEXT("|"));
		Trimmed.ParseIntoArray(OutCells, TEXT("|"), /*bCullEmpty=*/false);
		for (FString& Cell : OutCells) { Cell.TrimStartAndEndInline(); }
	}

	/** A markdown separator row like |---|:--:|---| (cells are only - : and spaces, with at least one -). */
	bool IsSeparatorRow(const FString& Row)
	{
		TArray<FString> Cells;
		ParseTableCells(Row, Cells);
		if (Cells.Num() == 0) { return false; }
		for (const FString& Cell : Cells)
		{
			if (Cell.IsEmpty() || !Cell.Contains(TEXT("-"))) { return false; }
			for (const TCHAR C : Cell)
			{
				if (C != '-' && C != ':' && C != ' ') { return false; }
			}
		}
		return true;
	}

	bool IsTableRow(const FString& TrimmedLine)
	{
		return TrimmedLine.StartsWith(TEXT("|"));
	}

	/** Strip inline markdown markers so cell widths are measured on visible text. */
	FString StripInlineMarkers(const FString& In)
	{
		return In.Replace(TEXT("**"), TEXT("")).Replace(TEXT("`"), TEXT("")).Replace(TEXT("*"), TEXT(""));
	}

	/** Render a Markdown table as column-aligned monospace text; bHasHeader bolds the first data row. */
	FString RenderTable(const TArray<FString>& Rows, bool bHasHeader)
	{
		// Parse cells (plain text), skipping separator rows.
		TArray<TArray<FString>> Grid;
		for (const FString& Row : Rows)
		{
			if (IsSeparatorRow(Row)) { continue; }
			TArray<FString> Cells;
			ParseTableCells(Row, Cells);
			for (FString& Cell : Cells) { Cell = StripInlineMarkers(Cell); }
			Grid.Add(MoveTemp(Cells));
		}
		if (Grid.Num() == 0) { return FString(); }
		const int32 HeaderIndex = bHasHeader ? 0 : INDEX_NONE;

		int32 ColumnCount = 0;
		for (const TArray<FString>& Row : Grid) { ColumnCount = FMath::Max(ColumnCount, Row.Num()); }

		TArray<int32> Widths;
		Widths.Init(0, ColumnCount);
		for (const TArray<FString>& Row : Grid)
		{
			for (int32 c = 0; c < Row.Num(); ++c) { Widths[c] = FMath::Max(Widths[c], Row[c].Len()); }
		}

		// Build each row as space-padded plain cells, then wrap the whole row in a monospace style so the
		// padding aligns (the game's proportional font can't align by spaces). Header gets its own style.
		FString Out;
		for (int32 r = 0; r < Grid.Num(); ++r)
		{
			const TArray<FString>& Row = Grid[r];
			FString Cells;
			for (int32 c = 0; c < ColumnCount; ++c)
			{
				const FString Cell = Row.IsValidIndex(c) ? Row[c] : FString();
				FString Padded = Cell;
				while (Padded.Len() < Widths[c]) { Padded.AppendChar(' '); }
				Cells += Padded;
				if (c < ColumnCount - 1) { Cells += TEXT("  "); }
			}
			const TCHAR* Style = (r == HeaderIndex) ? TEXT("MonoHeader") : TEXT("Mono");
			Out += FString::Printf(TEXT("<%s>%s</>"), Style, *Cells);
			if (r < Grid.Num() - 1) { Out += TEXT("\n"); }
		}
		return Out;
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
			continue;
		}
		if (bInFence)
		{
			Out += FString::Printf(TEXT("<Code>%s</>"), *Line);
			if (i < Lines.Num() - 1) { Out += TEXT("\n"); }
			continue;
		}

		// Table detection. Two forms: a proper header row followed by a |---| separator, OR (some models
		// emit these) a separator row with no header above it. Either way, consume the run of '|' lines.
		// A bare separator must ITSELF be a '|' row: IsSeparatorRow accepts a lone "---" (outer pipes
		// are optional), but the collector below only consumes '|' lines — treating a dash-rule as a
		// table consumed NOTHING and re-visited the same line forever, growing Out until the 2 GB
		// FString limit crashed the game (live crash 2026-07-17: every streamed reply with a "---").
		const bool bHeaderThenSeparator = IsTableRow(Trimmed) && i + 1 < Lines.Num() && IsSeparatorRow(Lines[i + 1]);
		const bool bBareSeparator = IsTableRow(Trimmed) && IsSeparatorRow(Trimmed);
		if (bHeaderThenSeparator || bBareSeparator)
		{
			TArray<FString> TableRows;
			int32 j = i;
			for (; j < Lines.Num(); ++j)
			{
				FString RowTrim = Lines[j];
				RowTrim.TrimStartInline();
				if (!IsTableRow(RowTrim)) { break; }
				TableRows.Add(Lines[j]);
			}
			if (j > i) // defensive: NEVER loop without consuming at least the current line
			{
				Out += RenderTable(TableRows, /*bHasHeader=*/bHeaderThenSeparator);
				if (j < Lines.Num()) { Out += TEXT("\n"); }
				i = j - 1; // for-loop ++ lands on the first non-table line
				continue;
			}
		}

		if (Trimmed.StartsWith(TEXT("#")))
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
