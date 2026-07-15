#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

/**
 * Serialize a JSON object to a compact (condensed, no-whitespace) string. Shared by the tool
 * serializers so each tools translation unit doesn't emit its own copy — a duplicate-symbol clash
 * in unity builds — and tool JSON stays uniformly formatted (and token-lean).
 */
FString AIDAToCompactJson(const TSharedRef<FJsonObject>& Object);

/**
 * A JSON number rounded to one decimal place and emitted WITHOUT floating-point noise (e.g. 0.4 not
 * 0.40000000000000002). Uses a string-backed number value so the writer prints exactly the rounded
 * form; keeps tool JSON compact and readable. Assign with FJsonObject::SetField.
 */
TSharedRef<FJsonValue> AIDANumber(double Value);
