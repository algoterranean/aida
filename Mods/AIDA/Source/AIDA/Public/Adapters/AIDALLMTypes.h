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

/**
 * Fired zero or more times as the reply streams in, once per decoded text delta.
 * Always delivered on the game thread (adapters marshal from the HTTP worker thread).
 * May be unset (nullptr) when a caller only wants the final blob.
 */
using FAIDAOnChunk = TFunction<void(const FString& /*Delta*/)>;

/** Fired once on success with the full assembled text (the concatenation of all deltas). */
using FAIDAOnComplete = TFunction<void(const FString& /*Text*/)>;

/** Fired once on failure. HttpStatus is 0 for transport-level failures. */
using FAIDAOnError = TFunction<void(int32 /*HttpStatus*/, const FString& /*Message*/)>;
