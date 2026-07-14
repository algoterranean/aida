#pragma once

#include "CoreMinimal.h"

// Provider-agnostic LLM request/response types + callbacks. Plain C++ (no UObject) so adapter
// wire-format logic stays engine-independent and unit-testable (docs/DEV.md §5).

/**
 * A tool call the model wants executed. Id is the provider's opaque call id (Anthropic "toolu_...",
 * OpenAI "call_..."); the orchestrator echoes it back on the matching tool_result. ArgsJson is the
 * raw JSON arguments string (may be "" for a no-arg call — the registry treats "" as {}).
 */
struct FAIDAToolCall
{
	FString Id;
	FString Name;
	FString ArgsJson;
};

/** One tool result, sent back to the model in the next turn, paired to a prior FAIDAToolCall by id. */
struct FAIDAToolResultPart
{
	FString ToolCallId;
	FString Content;
	bool bIsError = false;
};

struct FAIDAChatMessage
{
	FString Role;    // "user" | "assistant"
	FString Content; // plain text; may be empty when only tool parts are present

	/** Assistant turn: the tool_use blocks to echo back so the model sees its own prior calls. */
	TArray<FAIDAToolCall> ToolCalls;
	/** User turn: the tool_result blocks answering a prior assistant turn's tool_use blocks. */
	TArray<FAIDAToolResultPart> ToolResults;
};

/** Wire tool definition sent to the model. InputSchemaJson is a JSON-Schema object string ("" => empty object). */
struct FAIDAToolDef
{
	FString Name;
	FString Description;
	FString InputSchemaJson;
};

struct FAIDACompletionRequest
{
	FString Model;
	FString System;          // system prompt (optional)
	int32 MaxTokens = 1024;
	TArray<FAIDAChatMessage> Messages;
	TArray<FAIDAToolDef> Tools;  // empty => no tools block is sent
};

/**
 * The terminal result of one completion. Text is the assembled visible reply; ToolCalls is populated
 * when the model wants tools run (StopReason "tool_use" / "tool_calls"). StopReason is the provider's
 * raw stop reason ("end_turn", "tool_use", "max_tokens", "stop", "tool_calls", …).
 */
struct FAIDACompletionResult
{
	FString Text;
	FString StopReason;
	TArray<FAIDAToolCall> ToolCalls;

	bool HasToolCalls() const { return ToolCalls.Num() > 0; }
};

/**
 * Fired zero or more times as the reply streams in, once per decoded text delta.
 * Always delivered on the game thread (adapters marshal from the HTTP worker thread).
 * May be unset (nullptr) when a caller only wants the final blob.
 */
using FAIDAOnChunk = TFunction<void(const FString& /*Delta*/)>;

/** Fired once on success with the full assembled text (the concatenation of all deltas). */
using FAIDAOnComplete = TFunction<void(const FString& /*Text*/)>;

/** Fired once on success with the structured result (text + tool calls + stop reason). */
using FAIDAOnCompleteResult = TFunction<void(const FAIDACompletionResult& /*Result*/)>;

/** Fired once on failure. HttpStatus is 0 for transport-level failures. */
using FAIDAOnError = TFunction<void(int32 /*HttpStatus*/, const FString& /*Message*/)>;
