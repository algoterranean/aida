#pragma once

#include "CoreMinimal.h"
#include "Memory/AIDAMemoryTypes.h"

/**
 * Pure filter + serializer for the Phase 3 note tools (docs/PHASE3.md §4). No game headers, so the
 * proximity/keyword/region selection unit-tests on synthetic notes; the orchestrator wires it over the
 * in-save note array. get_notes returns a bounded, LLM-friendly list.
 */
namespace AIDANotesTools
{
	/**
	 * Select notes matching the filters, ordered for the model: nearest-first when bNear (distance to
	 * From), else newest-first (by CreatedUtc). Keyword is a case-insensitive substring on the text or a
	 * tag; Region is a case-insensitive substring on the note's region. Empty filters match everything.
	 */
	TArray<const FAIDANote*> SelectNotes(const TArray<FAIDANote>& Notes, const FString& Keyword,
		const FString& Region, const FVector& From, bool bNear);

	/** get_notes: the selected notes as bounded JSON (text, author, region, tags, age; distance when near). */
	FString BuildNotesJson(const TArray<FAIDANote>& Notes, const FString& Keyword, const FString& Region,
		const FVector& From, bool bNear, int64 NowUtc);
}
