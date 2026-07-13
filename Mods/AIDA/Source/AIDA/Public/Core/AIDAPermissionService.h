#pragma once

#include "CoreMinimal.h"
#include "Core/AIDAConfig.h"

/** Permission tiers (docs/ARCHITECTURE.md §3.2 PermissionService). */
enum class EAIDATier : uint8
{
	Chat,   // send messages / converse
	Query,  // factory & map read tools (Phase 2)
	Act     // world-modifying proposals (Phase 4)
};

/**
 * Config-driven permission checks, evaluated server-side on every request (client UI hiding is
 * cosmetic, never security — coding rule 1). "everyone" opens a tier to all players; any other
 * value restricts it to the `act` allowlist of Epic account ids.
 */
class FAIDAPermissionService
{
public:
	FAIDAPermissionService() = default;
	explicit FAIDAPermissionService(const FAIDAPermissionsConfig& Permissions);

	void Configure(const FAIDAPermissionsConfig& Permissions);

	/** True if PlayerId may use the given tier. */
	bool IsAllowed(EAIDATier Tier, const FString& PlayerId) const;

private:
	static bool Evaluate(const FString& Rule, const TArray<FString>& Allowlist, const FString& PlayerId);

	FString ChatRule = TEXT("everyone");
	FString QueryRule = TEXT("everyone");
	TArray<FString> ActAllowlist;
};
