#include "Core/AIDAImageStore.h"

#include "Misc/Base64.h"

FGuid FAIDAImageStore::Add(const FString& MediaType, const TArray<uint8>& Bytes,
	const FString& OwnerPlayerId, int64 NowUtc, FString& OutError)
{
	if (Bytes.Num() <= 0)
	{
		OutError = TEXT("empty image");
		return FGuid();
	}
	if (Bytes.Num() > Config.MaxImageBytes)
	{
		OutError = FString::Printf(TEXT("image is %d bytes; uploads.maxImageBytes is %d"),
			Bytes.Num(), Config.MaxImageBytes);
		return FGuid();
	}

	EvictFor(Bytes.Num());

	FAIDAStoredImage Img;
	Img.MediaType = MediaType;
	Img.Base64Data = FBase64::Encode(Bytes.GetData(), Bytes.Num());
	Img.OwnerPlayerId = OwnerPlayerId;
	Img.CreatedUtc = NowUtc;
	Img.SourceBytes = Bytes.Num();

	const FGuid Id = FGuid::NewGuid();
	Images.Add(Id, MoveTemp(Img));
	return Id;
}

bool FAIDAImageStore::MarkReferenced(const FGuid& Id, const FString& OwnerPlayerId)
{
	FAIDAStoredImage* Img = Images.Find(Id);
	if (!Img || Img->OwnerPlayerId != OwnerPlayerId)
	{
		return false;
	}
	Img->bReferenced = true;
	return true;
}

void FAIDAImageStore::Sweep(int64 NowUtc)
{
	for (auto It = Images.CreateIterator(); It; ++It)
	{
		if (!It->Value.bReferenced && NowUtc - It->Value.CreatedUtc > Config.TtlSeconds)
		{
			It.RemoveCurrent();
		}
	}
}

int64 FAIDAImageStore::TotalBytes() const
{
	int64 Total = 0;
	for (const auto& Pair : Images)
	{
		Total += Pair.Value.SourceBytes;
	}
	return Total;
}

void FAIDAImageStore::EvictFor(int32 IncomingBytes)
{
	// Evict oldest-unreferenced first, then oldest-referenced, until the new image fits both budgets.
	auto OverBudget = [this, IncomingBytes]()
	{
		return Images.Num() >= Config.MaxStoredImages || TotalBytes() + IncomingBytes > kTotalByteBudget;
	};

	for (const bool bAllowReferenced : { false, true })
	{
		while (OverBudget() && Images.Num() > 0)
		{
			FGuid Oldest;
			int64 OldestUtc = INT64_MAX;
			for (const auto& Pair : Images)
			{
				if ((bAllowReferenced || !Pair.Value.bReferenced) && Pair.Value.CreatedUtc < OldestUtc)
				{
					OldestUtc = Pair.Value.CreatedUtc;
					Oldest = Pair.Key;
				}
			}
			if (!Oldest.IsValid())
			{
				break; // nothing evictable in this pass
			}
			Images.Remove(Oldest);
		}
	}
}

bool FAIDAImageUploadAssembler::Begin(const FString& PlayerId, const FString& MediaType,
	int32 TotalBytes, int32 ChunkCount, int32 MaxImageBytes, int64 NowUtc, FString& OutError)
{
	if (TotalBytes <= 0 || TotalBytes > MaxImageBytes)
	{
		OutError = FString::Printf(TEXT("upload size %d out of range (max %d)"), TotalBytes, MaxImageBytes);
		return false;
	}
	if (ChunkCount <= 0 || ChunkCount > kMaxChunkCount)
	{
		OutError = FString::Printf(TEXT("chunk count %d out of range (max %d)"), ChunkCount, kMaxChunkCount);
		return false;
	}
	// The declared chunking must be able to carry the declared bytes in kChunkBytes pieces.
	if ((int64)ChunkCount * kChunkBytes < TotalBytes)
	{
		OutError = TEXT("chunk count too small for declared size");
		return false;
	}

	FSession Session;
	Session.MediaType = MediaType;
	Session.TotalBytes = TotalBytes;
	Session.ChunkCount = ChunkCount;
	Session.TouchedUtc = NowUtc;
	Session.Bytes.Reserve(TotalBytes);
	Sessions.Add(PlayerId, MoveTemp(Session)); // a new Begin discards any prior in-flight session
	return true;
}

bool FAIDAImageUploadAssembler::AddChunk(const FString& PlayerId, int32 Seq,
	const TArray<uint8>& Data, int64 NowUtc, FString& OutError)
{
	FSession* Session = Sessions.Find(PlayerId);
	if (!Session)
	{
		OutError = TEXT("no upload in progress");
		return false;
	}
	if (Seq != Session->NextSeq || Seq >= Session->ChunkCount)
	{
		OutError = FString::Printf(TEXT("out-of-order chunk %d (expected %d of %d)"),
			Seq, Session->NextSeq, Session->ChunkCount);
		Sessions.Remove(PlayerId);
		return false;
	}
	if (Data.Num() <= 0 || Data.Num() > kChunkBytes
		|| Session->Bytes.Num() + Data.Num() > Session->TotalBytes)
	{
		OutError = TEXT("chunk size invalid or exceeds declared upload size");
		Sessions.Remove(PlayerId);
		return false;
	}

	Session->Bytes.Append(Data);
	Session->NextSeq++;
	Session->TouchedUtc = NowUtc;
	return true;
}

bool FAIDAImageUploadAssembler::Commit(const FString& PlayerId, uint32 Crc32,
	TArray<uint8>& OutBytes, FString& OutMediaType, FString& OutError)
{
	FSession* Session = Sessions.Find(PlayerId);
	if (!Session)
	{
		OutError = TEXT("no upload in progress");
		return false;
	}

	// The session ends on commit regardless of outcome — move everything out first.
	FSession Done = MoveTemp(*Session);
	Sessions.Remove(PlayerId);

	if (Done.Bytes.Num() != Done.TotalBytes || Done.NextSeq != Done.ChunkCount)
	{
		OutError = FString::Printf(TEXT("incomplete upload: %d of %d bytes, %d of %d chunks"),
			Done.Bytes.Num(), Done.TotalBytes, Done.NextSeq, Done.ChunkCount);
		return false;
	}
	if (FCrc::MemCrc32(Done.Bytes.GetData(), Done.Bytes.Num()) != Crc32)
	{
		OutError = TEXT("checksum mismatch");
		return false;
	}

	OutBytes = MoveTemp(Done.Bytes);
	OutMediaType = MoveTemp(Done.MediaType);
	return true;
}

void FAIDAImageUploadAssembler::Sweep(int64 NowUtc)
{
	for (auto It = Sessions.CreateIterator(); It; ++It)
	{
		if (NowUtc - It->Value.TouchedUtc > kSessionTimeoutSec)
		{
			It.RemoveCurrent();
		}
	}
}
