#include "Core/AIDARateLimiter.h"

FAIDARateLimiter::FAIDARateLimiter(const FAIDALimitsConfig& Limits)
{
	Configure(Limits);
}

void FAIDARateLimiter::Configure(const FAIDALimitsConfig& Limits)
{
	PerPlayerPerMinute = FMath::Max(0, Limits.PerPlayerPerMinute);
	GlobalPerMinute = FMath::Max(0, Limits.GlobalPerMinute);
}

bool FAIDARateLimiter::Peek(FBucket& Bucket, double Capacity, double NowSeconds)
{
	if (!Bucket.bSeeded)
	{
		// Start full so the first requests after boot aren't throttled.
		Bucket.Tokens = Capacity;
		Bucket.LastRefill = NowSeconds;
		Bucket.bSeeded = true;
	}
	else
	{
		const double Elapsed = FMath::Max(0.0, NowSeconds - Bucket.LastRefill);
		Bucket.Tokens = FMath::Min(Capacity, Bucket.Tokens + Elapsed * (Capacity / 60.0));
		Bucket.LastRefill = NowSeconds;
	}
	return Bucket.Tokens >= 1.0;
}

bool FAIDARateLimiter::TryConsume(const FString& PlayerId, double NowSeconds, FString& OutReason)
{
	// 0 = feature disabled → unlimited.
	const bool bPlayerLimited = PerPlayerPerMinute > 0;
	const bool bGlobalLimited = GlobalPerMinute > 0;

	FBucket& Player = PlayerBuckets.FindOrAdd(PlayerId);
	const bool bPlayerOk = !bPlayerLimited || Peek(Player, PerPlayerPerMinute, NowSeconds);
	const bool bGlobalOk = !bGlobalLimited || Peek(GlobalBucket, GlobalPerMinute, NowSeconds);

	if (!bPlayerOk)
	{
		OutReason = FString::Printf(
			TEXT("You're sending messages too fast (limit %d/min). Give AIDA a moment."), PerPlayerPerMinute);
		return false;
	}
	if (!bGlobalOk)
	{
		OutReason = FString::Printf(
			TEXT("AIDA is busy across the server (limit %d/min). Try again shortly."), GlobalPerMinute);
		return false;
	}

	// Both admit — now actually spend a token from each active bucket.
	if (bPlayerLimited) { Player.Tokens -= 1.0; }
	if (bGlobalLimited) { GlobalBucket.Tokens -= 1.0; }
	return true;
}
