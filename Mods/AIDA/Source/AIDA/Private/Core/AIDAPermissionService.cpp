#include "Core/AIDAPermissionService.h"

FAIDAPermissionService::FAIDAPermissionService(const FAIDAPermissionsConfig& Permissions)
{
	Configure(Permissions);
}

void FAIDAPermissionService::Configure(const FAIDAPermissionsConfig& Permissions)
{
	ChatRule = Permissions.Chat;
	QueryRule = Permissions.Query;
	ActAllowlist = Permissions.Act;
}

bool FAIDAPermissionService::Evaluate(const FString& Rule, const TArray<FString>& Allowlist, const FString& PlayerId)
{
	if (Rule.Equals(TEXT("everyone"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	// Any non-"everyone" value restricts the tier to the act allowlist.
	return !PlayerId.IsEmpty() && Allowlist.Contains(PlayerId);
}

bool FAIDAPermissionService::IsAllowed(EAIDATier Tier, const FString& PlayerId) const
{
	switch (Tier)
	{
	case EAIDATier::Chat:
		return Evaluate(ChatRule, ActAllowlist, PlayerId);
	case EAIDATier::Query:
		return Evaluate(QueryRule, ActAllowlist, PlayerId);
	case EAIDATier::Act:
	{
		// The act tier is an explicit allowlist of Epic account IDs, with one opt-in wildcard: a literal
		// "everyone" entry opens it to ALL players — including an unidentified host, whose net id is
		// empty on a listen-server/offline session. Without the wildcard, the id must be listed
		// (empty ids never match a specific-id allowlist).
		const bool bEveryone = ActAllowlist.ContainsByPredicate(
			[](const FString& Entry) { return Entry.Equals(TEXT("everyone"), ESearchCase::IgnoreCase); });
		return bEveryone || (!PlayerId.IsEmpty() && ActAllowlist.Contains(PlayerId));
	}
	default:
		return false;
	}
}
