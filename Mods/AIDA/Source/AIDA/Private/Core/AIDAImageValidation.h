#pragma once

#include "CoreMinimal.h"

// Phase 5: server-side re-validation of committed image uploads (docs/PHASE5.md §1). The server
// never trusts the client's claimed media type — the bytes must decode as a real image within the
// dimension budget. Kept out of AIDAImageStore so the store stays module-free and unit-testable.

/**
 * Decode-validate uploaded bytes via IImageWrapperModule. On success fills the TRUE media type
 * (from the detected format, not the client's claim) and returns true. MaxDimension is the
 * config's client downscale target; anything beyond 2× it is rejected as un-normalized.
 */
bool AIDAValidateImageBytes(const TArray<uint8>& Bytes, int32 MaxDimension,
	FString& OutMediaType, FString& OutError);
