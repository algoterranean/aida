#include "Core/AIDAImageNormalize.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Modules/ModuleManager.h"

bool AIDANormalizeImageBytes(const TArray<uint8>& FileBytes, int32 MaxDimension,
	TArray<uint8>& OutJpeg, FString& OutError)
{
	if (FileBytes.Num() <= 0)
	{
		OutError = TEXT("empty file");
		return false;
	}

	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const EImageFormat Format = Module.DetectImageFormat(FileBytes.GetData(), FileBytes.Num());
	if (Format != EImageFormat::JPEG && Format != EImageFormat::PNG && Format != EImageFormat::BMP)
	{
		OutError = TEXT("unsupported image format (use jpeg/png/bmp)");
		return false;
	}

	const TSharedPtr<IImageWrapper> Decoder = Module.CreateImageWrapper(Format);
	TArray64<uint8> RawBGRA;
	if (!Decoder.IsValid() || !Decoder->SetCompressed(FileBytes.GetData(), FileBytes.Num())
		|| !Decoder->GetRaw(ERGBFormat::BGRA, 8, RawBGRA))
	{
		OutError = TEXT("image failed to decode");
		return false;
	}

	int32 Width = static_cast<int32>(Decoder->GetWidth());
	int32 Height = static_cast<int32>(Decoder->GetHeight());
	if (Width <= 0 || Height <= 0)
	{
		OutError = TEXT("image has invalid dimensions");
		return false;
	}

	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(Width * Height);
	FMemory::Memcpy(Pixels.GetData(), RawBGRA.GetData(), (int64)Width * Height * sizeof(FColor));

	// Downscale so the long edge fits MaxDimension (provider sweet spot; also bounds upload bytes).
	const int32 LongEdge = FMath::Max(Width, Height);
	if (LongEdge > MaxDimension)
	{
		const float Scale = static_cast<float>(MaxDimension) / LongEdge;
		const int32 NewWidth = FMath::Max(1, FMath::RoundToInt(Width * Scale));
		const int32 NewHeight = FMath::Max(1, FMath::RoundToInt(Height * Scale));
		TArray<FColor> Resized;
		FImageUtils::ImageResize(Width, Height, Pixels, NewWidth, NewHeight, Resized, /*bLinearSpace*/ false);
		Pixels = MoveTemp(Resized);
		Width = NewWidth;
		Height = NewHeight;
	}

	const TSharedPtr<IImageWrapper> Encoder = Module.CreateImageWrapper(EImageFormat::JPEG);
	if (!Encoder.IsValid()
		|| !Encoder->SetRaw(Pixels.GetData(), (int64)Width * Height * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
	{
		OutError = TEXT("image failed to re-encode");
		return false;
	}

	const TArray64<uint8> Compressed = Encoder->GetCompressed(85);
	if (Compressed.Num() <= 0)
	{
		OutError = TEXT("image failed to re-encode");
		return false;
	}
	OutJpeg = TArray<uint8>(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
	return true;
}
