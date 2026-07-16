#include "Core/AIDAImageValidation.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

bool AIDAValidateImageBytes(const TArray<uint8>& Bytes, int32 MaxDimension,
	FString& OutMediaType, FString& OutError)
{
	if (Bytes.Num() <= 0)
	{
		OutError = TEXT("empty image");
		return false;
	}

	IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const EImageFormat Format = Module.DetectImageFormat(Bytes.GetData(), Bytes.Num());

	switch (Format)
	{
	case EImageFormat::JPEG: OutMediaType = TEXT("image/jpeg"); break;
	case EImageFormat::PNG:  OutMediaType = TEXT("image/png");  break;
	case EImageFormat::BMP:  OutMediaType = TEXT("image/bmp");  break;
	default:
		OutError = TEXT("not a recognized image (jpeg/png/bmp)");
		return false;
	}

	const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Format);
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
	{
		OutError = TEXT("image failed to decode");
		return false;
	}

	const int64 Width = Wrapper->GetWidth();
	const int64 Height = Wrapper->GetHeight();
	const int64 Cap = 2ll * MaxDimension; // client normalizes to MaxDimension; allow slack, not 8K originals
	if (Width <= 0 || Height <= 0 || Width > Cap || Height > Cap)
	{
		OutError = FString::Printf(TEXT("image is %lldx%lld; the client should downscale to <= %d px"),
			Width, Height, MaxDimension);
		return false;
	}

	return true;
}
