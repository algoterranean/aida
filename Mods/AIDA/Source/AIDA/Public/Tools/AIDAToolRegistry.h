#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Provider-agnostic tool registry (docs/ARCHITECTURE.md §3.2, docs/PHASE2.md Slice 0). Plain C++
// (no UObject) so it stays engine-independent and unit-testable, like the LLM adapter types. The
// LLM adapters translate FAIDAToolSpec -> each provider's wire "tools" schema; the orchestrator's
// tool loop dispatches a model tool-call by name back through this registry.

/** Permission tier a tool requires — mirrors PermissionService tiers (chat < query < act). */
enum class EAIDAToolTier : uint8
{
	Chat,
	Query,
	Act
};

/** Who/what is invoking a tool, so handlers can attribute actions and scope data by player. */
struct FAIDAToolContext
{
	FString Author;    // display name of the requesting player (or "AIDA"/debug)
	FString PlayerId;  // stable id; permission checks happen upstream in the orchestrator

	// The requesting player's world location, when resolvable (e.g. for "nearest node" tools). Handlers
	// must treat bHasLocation=false as "unknown" and fall back to a deterministic choice.
	FVector Location = FVector::ZeroVector;
	bool bHasLocation = false;
};

/** Outcome of a tool call. Content is the JSON (or text) the model receives as a tool_result. */
struct FAIDAToolResult
{
	bool bIsError = false;
	FString Content;

	static FAIDAToolResult Ok(const FString& InContent) { return FAIDAToolResult{ false, InContent }; }
	static FAIDAToolResult Error(const FString& InMessage) { return FAIDAToolResult{ true, InMessage }; }
};

/** Tool implementation: receives parsed args (never null) + context, returns a result. */
using FAIDAToolHandler = TFunction<FAIDAToolResult(const TSharedRef<FJsonObject>& /*Args*/, const FAIDAToolContext& /*Context*/)>;

/** Declarative tool definition: name, description, JSON-Schema params, tier, handler. */
struct FAIDAToolSpec
{
	FString Name;
	FString Description;
	FString ParametersJsonSchema; // JSON Schema (an object schema) as a string; "" => no-arg tool
	EAIDAToolTier Tier = EAIDAToolTier::Query;
	FAIDAToolHandler Handler;
};

/**
 * Registry of tools exposed to the model. Registration + lookup + dispatch only — it deliberately
 * does NOT check permission tiers or rate limits; the orchestrator gates those before dispatch.
 */
class FAIDAToolRegistry
{
public:
	/** Register (or replace) a tool by name. Ignored with a warning if Name or Handler is empty. */
	void Register(FAIDAToolSpec Spec);

	bool Contains(const FString& Name) const { return Tools.Contains(Name); }
	const FAIDAToolSpec* Find(const FString& Name) const { return Tools.Find(Name); }
	int32 Num() const { return Tools.Num(); }

	/** All specs, ordered by name — a stable order keeps the request prefix cache-friendly. */
	void GetSpecs(TArray<const FAIDAToolSpec*>& OutSpecs) const;

	/**
	 * Dispatch a model tool-call. Parses ArgsJson (empty/whitespace => an empty object, so no-arg
	 * tools work), then runs the handler. Returns an error result if the tool is unknown or the
	 * arguments are not a JSON object. Tier is metadata for the orchestrator; it is not enforced here.
	 */
	FAIDAToolResult Dispatch(const FString& Name, const FString& ArgsJson, const FAIDAToolContext& Context) const;

private:
	TMap<FString, FAIDAToolSpec> Tools;
};
