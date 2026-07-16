#pragma once

#include "CoreMinimal.h"

// Phase 5 client-side attachment normalization (docs/PHASE5.md §1): decode any supported image
// file, downscale so the long edge fits MaxDimension, and re-encode as JPEG. Runs on the client
// before upload so the wire payload is small and uniform; the server re-validates independently.

/**
 * Normalize raw image-file bytes to an upload-ready JPEG. Returns false with OutError when the
 * bytes are not a decodable jpeg/png/bmp. Images already within MaxDimension are re-encoded
 * without resizing (normalization is also what bounds the payload, so no passthrough).
 */
bool AIDANormalizeImageBytes(const TArray<uint8>& FileBytes, int32 MaxDimension,
	TArray<uint8>& OutJpeg, FString& OutError);
