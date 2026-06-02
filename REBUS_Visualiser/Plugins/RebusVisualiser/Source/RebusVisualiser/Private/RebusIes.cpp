// Copyright REBUS Industries.
#include "RebusIes.h"
#include "RebusVisualiserLog.h"
#include "Engine/TextureLightProfile.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "PixelFormat.h"

#if __has_include("IESConverter.h")
#include "IESConverter.h"
#define REBUS_HAS_IES_CONVERTER 1
#else
#define REBUS_HAS_IES_CONVERTER 0
#endif

namespace RebusIes
{
	UTextureLightProfile* BuildLightProfile(UObject* Outer, const TArray<uint8>& IesBytes)
	{
		if (IesBytes.Num() == 0)
		{
			return nullptr;
		}

#if REBUS_HAS_IES_CONVERTER
		FIESConverter Converter(IesBytes.GetData(), IesBytes.Num());
		if (!Converter.IsValid())
		{
			UE_LOG(LogRebusVisualiser, Warning, TEXT("IES parse failed (%d bytes)."), IesBytes.Num());
			return nullptr;
		}

		const int32 Width = Converter.GetWidth();
		const int32 Height = Converter.GetHeight();
		const TArray<uint8>& Raw = Converter.GetRawData(); // RGBA16F texels

		UTextureLightProfile* Profile = NewObject<UTextureLightProfile>(
			Outer ? Outer : (UObject*)GetTransientPackage(), NAME_None, RF_Transient);

#if WITH_EDITORONLY_DATA
		// Editor / -game with editor data: use the Source path so the texture cooks/streams
		// exactly like an imported .ies.
		Profile->Source.Init(Width, Height, 1, 1, TSF_RGBA16F, Raw.GetData());
		Profile->Brightness = Converter.GetBrightness();
		Profile->TextureMultiplier = Converter.GetMultiplier();
		Profile->AddressX = TA_Clamp;
		Profile->AddressY = TA_Clamp;
		Profile->CompressionSettings = TC_HDR;
		Profile->MipGenSettings = TMGS_NoMipmaps;
		Profile->UpdateResource();
#else
		// Cooked/runtime path: populate platform data directly (no Source available).
		Profile->Brightness = Converter.GetBrightness();
		Profile->TextureMultiplier = Converter.GetMultiplier();
		Profile->SetPlatformData(new FTexturePlatformData());
		FTexturePlatformData* PlatformData = Profile->GetPlatformData();
		PlatformData->SizeX = Width;
		PlatformData->SizeY = Height;
		PlatformData->PixelFormat = PF_FloatRGBA;

		FTexture2DMipMap* Mip = new FTexture2DMipMap();
		PlatformData->Mips.Add(Mip);
		Mip->SizeX = Width;
		Mip->SizeY = Height;
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		void* Dest = Mip->BulkData.Realloc(Raw.Num());
		FMemory::Memcpy(Dest, Raw.GetData(), Raw.Num());
		Mip->BulkData.Unlock();
		Profile->UpdateResource();
#endif
		return Profile;
#else
		UE_LOG(LogRebusVisualiser, Warning,
			TEXT("IESConverter.h not available in this engine build; cannot load IES at runtime. "
				 "Falling back to the synthesized cone."));
		return nullptr;
#endif
	}
}
