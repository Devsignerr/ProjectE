#include "Renderer/ColorGradingMath.h"

#include "Renderer/Image.h"

namespace ColorGradingMath
{
	namespace
	{
		constexpr float WarmScale = 0.25f; // 온도 1이면 빨강 +25%, 파랑 -25% (밝기 보존 전)
		constexpr float TintScale = 0.2f;  // 틴트 1이면 초록 -20%

		float Luminance(const FVector3& C) { return 0.2126f * C.X + 0.7152f * C.Y + 0.0722f * C.Z; }

		FVector3 Encode(const FVector3& C) { return FVector3(SrgbEncode(C.X), SrgbEncode(C.Y), SrgbEncode(C.Z)); }
		FVector3 Decode(const FVector3& C) { return FVector3(SrgbDecode(C.X), SrgbDecode(C.Y), SrgbDecode(C.Z)); }

		float LiftGammaGain(float C, float Lift, float Gamma, float Gain)
		{
			const float Lifted = Gain * (C + Lift * (1.0f - C));
			return std::pow(FMath::Max(Lifted, 0.0f), 1.0f / FMath::Max(Gamma, 1.0e-3f));
		}

		FVector3 ReadStripTexel(const FImage& Lut, uint32 Size, uint32 R, uint32 G, uint32 B)
		{
			const size_t Offset = (static_cast<size_t>(G) * Lut.Width + B * Size + R) * FImage::BytesPerPixel;
			return FVector3(Lut.Pixels[Offset] / 255.0f, Lut.Pixels[Offset + 1] / 255.0f, Lut.Pixels[Offset + 2] / 255.0f);
		}
	} // namespace

	bool IsIdentity(const FColorGradingParams& Params)
	{
		return Params.Temperature == 0.0f && Params.Tint == 0.0f && Params.Saturation == 1.0f && Params.Contrast == 1.0f &&
		       Params.Lift == FVector3(0.0f, 0.0f, 0.0f) && Params.Gamma == FVector3(1.0f, 1.0f, 1.0f) && Params.Gain == FVector3(1.0f, 1.0f, 1.0f);
	}

	FVector3 ApplyGrading(const FVector3& Linear, const FColorGradingParams& Params)
	{
		// 화이트 밸런스 (선형, 휘도 보존)
		FVector3 Color = Linear;
		if (Params.Temperature != 0.0f || Params.Tint != 0.0f)
		{
			const FVector3 Gains(1.0f + WarmScale * Params.Temperature, 1.0f - TintScale * Params.Tint, 1.0f - WarmScale * Params.Temperature);
			const float    Norm = Luminance(Gains);
			Color = FVector3(Color.X * Gains.X, Color.Y * Gains.Y, Color.Z * Gains.Z) / FMath::Max(Norm, 1.0e-4f);
		}
		// 대비 (인코딩 공간, 0.5 중심)
		if (Params.Contrast != 1.0f)
		{
			FVector3 Encoded = Encode(Color);
			Encoded          = FVector3((Encoded.X - 0.5f) * Params.Contrast + 0.5f, (Encoded.Y - 0.5f) * Params.Contrast + 0.5f,
			                            (Encoded.Z - 0.5f) * Params.Contrast + 0.5f);
			Color = Decode(Encoded);
		}
		// 채도 (선형)
		if (Params.Saturation != 1.0f)
		{
			const float Luma = Luminance(Color);
			Color = FVector3(Luma, Luma, Luma) + (Color - FVector3(Luma, Luma, Luma)) * FMath::Max(Params.Saturation, 0.0f);
		}
		// lift·gamma·gain (인코딩 공간, 채널별)
		FVector3 Encoded = Encode(Color);
		Encoded = FVector3(LiftGammaGain(Encoded.X, Params.Lift.X, Params.Gamma.X, Params.Gain.X), LiftGammaGain(Encoded.Y, Params.Lift.Y, Params.Gamma.Y, Params.Gain.Y),
		                   LiftGammaGain(Encoded.Z, Params.Lift.Z, Params.Gamma.Z, Params.Gain.Z));
		return Decode(Encoded);
	}

	bool IsStripLut(const FImage& Lut)
	{
		return Lut.IsValid() && Lut.Height >= 2 && Lut.Width == Lut.Height * Lut.Height;
	}

	FVector3 SampleStripLut(const FImage& Lut, const FVector3& Encoded)
	{
		if (!IsStripLut(Lut))
		{
			return Encoded;
		}
		const uint32 Size = Lut.Height;
		const float  Max  = static_cast<float>(Size - 1);
		const float  R = FMath::Clamp(Encoded.X, 0.0f, 1.0f) * Max, G = FMath::Clamp(Encoded.Y, 0.0f, 1.0f) * Max, B = FMath::Clamp(Encoded.Z, 0.0f, 1.0f) * Max;
		const uint32 R0 = FMath::Min(static_cast<uint32>(R), Size - 2), G0 = FMath::Min(static_cast<uint32>(G), Size - 2), B0 = FMath::Min(static_cast<uint32>(B), Size - 2);
		const float  FR = R - R0, FG = G - G0, FB = B - B0;
		FVector3     Result(0.0f, 0.0f, 0.0f);
		for (uint32 Corner = 0; Corner < 8; ++Corner)
		{
			const uint32 DR = Corner & 1u, DG = (Corner >> 1) & 1u, DB = (Corner >> 2) & 1u;
			const float  W  = (DR ? FR : 1.0f - FR) * (DG ? FG : 1.0f - FG) * (DB ? FB : 1.0f - FB);
			Result          = Result + ReadStripTexel(Lut, Size, R0 + DR, G0 + DG, B0 + DB) * W;
		}
		return Result;
	}

	void BakeLut(const FColorGradingParams& Params, const FImage* UserLut, std::vector<uint16>& OutPixels)
	{
		const bool bUserLut = UserLut != nullptr && IsStripLut(*UserLut) && Params.LutIntensity > 0.0f;
		OutPixels.resize(static_cast<size_t>(LutWidth) * LutHeight * 4);
		const float Max = static_cast<float>(LutSize - 1);
		for (uint32 B = 0; B < LutSize; ++B)
		{
			for (uint32 G = 0; G < LutSize; ++G)
			{
				for (uint32 R = 0; R < LutSize; ++R)
				{
					const FVector3 Encoded(R / Max, G / Max, B / Max);
					FVector3       Graded = Encode(ApplyGrading(Decode(Encoded), Params));
					if (bUserLut)
					{
						const FVector3 Looked = SampleStripLut(*UserLut, Graded);
						Graded                = Graded + (Looked - Graded) * FMath::Clamp(Params.LutIntensity, 0.0f, 1.0f);
					}
					const size_t Offset = (static_cast<size_t>(G) * LutWidth + B * LutSize + R) * 4;
					OutPixels[Offset + 0] = static_cast<uint16>(FMath::Clamp(Graded.X, 0.0f, 1.0f) * 65535.0f + 0.5f);
					OutPixels[Offset + 1] = static_cast<uint16>(FMath::Clamp(Graded.Y, 0.0f, 1.0f) * 65535.0f + 0.5f);
					OutPixels[Offset + 2] = static_cast<uint16>(FMath::Clamp(Graded.Z, 0.0f, 1.0f) * 65535.0f + 0.5f);
					OutPixels[Offset + 3] = 65535;
				}
			}
		}
	}
} // namespace ColorGradingMath
