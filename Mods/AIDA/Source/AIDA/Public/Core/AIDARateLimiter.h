#pragma once

#include "CoreMinimal.h"
#include "Core/AIDAConfig.h"

/**
 * Token-bucket rate limiter, per-player and global (docs/ARCHITECTURE.md §3.2 RateLimiter).
 * Plain C++ and time-injected so it's unit-testable without a running world. Bucket capacity
 * equals the per-minute allowance (so a short burst is permitted, then throttled to the rate).
 */
class FAIDARateLimiter
{
public:
	FAIDARateLimiter() = default;
	explicit FAIDARateLimiter(const FAIDALimitsConfig& Limits);

	void Configure(const FAIDALimitsConfig& Limits);

	/**
	 * Try to admit one request from PlayerId at time NowSeconds. On denial returns false and sets
	 * OutReason to a friendly, player-facing explanation. A denied request consumes no tokens.
	 */
	bool TryConsume(const FString& PlayerId, double NowSeconds, FString& OutReason);

private:
	struct FBucket
	{
		double Tokens = 0.0;
		double LastRefill = 0.0;
		bool bSeeded = false;
	};

	static bool Peek(FBucket& Bucket, double Capacity, double NowSeconds); // refills; true if >=1 token

	int32 PerPlayerPerMinute = 4;
	int32 GlobalPerMinute = 12;

	TMap<FString, FBucket> PlayerBuckets;
	FBucket GlobalBucket;
};
