#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Serialize a JSON object to a compact (condensed, no-whitespace) string. Shared by the tool
 * serializers so each tools translation unit doesn't emit its own copy — a duplicate-symbol clash
 * in unity builds — and tool JSON stays uniformly formatted (and token-lean).
 */
FString AIDAToCompactJson(const TSharedRef<FJsonObject>& Object);
