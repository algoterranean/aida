#pragma once

#include "CoreMinimal.h"
#include "Core/AIDAConfig.h"

// Phase 5 (docs/PHASE5.md §2-§3): server-RAM home of uploaded reference images, plus the
// per-player chunked-upload assembler behind the RCO upload RPCs. Plain C++, game-header-free,
// unit-testable. Images are deliberately ephemeral — never saved, never replicated to clients.

/** One committed upload. Base64 is encoded once here and reused verbatim on every LLM request. */
struct FAIDAStoredImage
{
	FString MediaType;
	FString Base64Data;
	FString OwnerPlayerId;
	int64 CreatedUtc = 0;
	/** Decoded upload size in bytes (budget accounting; Base64Data is ~4/3 of this). */
	int32 SourceBytes = 0;
	/** True once a sent chat message cites this image — exempts it from the TTL sweep. */
	bool bReferenced = false;
};

/**
 * Bounded, TTL'd image store (proposal-store lifecycle pattern: lazy sweep, no timers of its own).
 * Budgets: per-image bytes (config), image count (config), and a fixed total-byte ceiling; adding
 * past a budget evicts oldest-unreferenced first, then oldest-referenced (old conversation turns
 * degrade to "[attached image no longer available]" rather than blocking new uploads).
 */
class FAIDAImageStore
{
public:
	/** Hard ceiling on summed SourceBytes, independent of config (a 16-image config can't ask for 3 GiB). */
	static constexpr int64 kTotalByteBudget = 32ll * 1024 * 1024;

	void Configure(const FAIDAUploadsConfig& InConfig) { Config = InConfig; }
	const FAIDAUploadsConfig& GetConfig() const { return Config; }

	/** Store committed bytes (already validated as an image). Invalid FGuid + OutError on refusal. */
	FGuid Add(const FString& MediaType, const TArray<uint8>& Bytes, const FString& OwnerPlayerId,
		int64 NowUtc, FString& OutError);

	const FAIDAStoredImage* Find(const FGuid& Id) const { return Images.Find(Id); }

	/** Cite an image from a sent message. False if missing or owned by a different player. */
	bool MarkReferenced(const FGuid& Id, const FString& OwnerPlayerId);

	/** Lazily expire unreferenced uploads past the TTL (call on the shared orchestrator cadence). */
	void Sweep(int64 NowUtc);

	int32 Num() const { return Images.Num(); }
	int64 TotalBytes() const;

private:
	FAIDAUploadsConfig Config;
	TMap<FGuid, FAIDAStoredImage> Images;

	/** Evict oldest (unreferenced first) until Count/Bytes fit the budgets. */
	void EvictFor(int32 IncomingBytes);
};

/**
 * Reassembles one in-flight chunked upload per player (docs/PHASE5.md §3). Strict in-order Seq;
 * a new Begin discards any prior session; stale sessions time out. CRC32 is checked at Commit.
 */
class FAIDAImageUploadAssembler
{
public:
	static constexpr int32 kChunkBytes = 16 * 1024;
	static constexpr int32 kMaxChunkCount = 256; // hard 4 MiB wire ceiling regardless of config
	static constexpr int64 kSessionTimeoutSec = 60;

	bool Begin(const FString& PlayerId, const FString& MediaType, int32 TotalBytes, int32 ChunkCount,
		int32 MaxImageBytes, int64 NowUtc, FString& OutError);

	/** Append the next chunk (Seq must be exactly the count received so far). */
	bool AddChunk(const FString& PlayerId, int32 Seq, const TArray<uint8>& Data, int64 NowUtc, FString& OutError);

	/** Finish: verifies byte count + CRC32 and moves the assembled bytes out. Session ends either way. */
	bool Commit(const FString& PlayerId, uint32 Crc32, TArray<uint8>& OutBytes, FString& OutMediaType,
		FString& OutError);

	void Cancel(const FString& PlayerId) { Sessions.Remove(PlayerId); }

	/** Drop sessions idle past the timeout. */
	void Sweep(int64 NowUtc);

	bool HasSession(const FString& PlayerId) const { return Sessions.Contains(PlayerId); }

private:
	struct FSession
	{
		FString MediaType;
		int32 TotalBytes = 0;
		int32 ChunkCount = 0;
		int32 NextSeq = 0;
		TArray<uint8> Bytes;
		int64 TouchedUtc = 0;
	};
	TMap<FString, FSession> Sessions;
};
