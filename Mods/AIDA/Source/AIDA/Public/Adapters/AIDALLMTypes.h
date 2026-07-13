#pragma once

#include "CoreMinimal.h"

// Provider-agnostic LLM request/response types + callbacks. Plain C++ (no UObject) so adapter
// wire-format logic stays engine-independent and unit-testable (docs/DEV.md §5).

struct FAIDAChatMessage
{
	FString Role;    // "user" | "assistant"
	FString Content;
};

struct FAIDACompletionRequest
{
	FString Model;
	FString System;          // system prompt (optional)
	int32 MaxTokens = 1024;
	TArray<FAIDAChatMessage> Messages;
};

/** Fired once on success with the full assembled text. (Streaming deltas come in a later increment.) */
using FAIDAOnComplete = TFunction<void(const FString& /*Text*/)>;

/** Fired once on failure. HttpStatus is 0 for transport-level failures. */
using FAIDAOnError = TFunction<void(int32 /*HttpStatus*/, const FString& /*Message*/)>;
