#include "Core/Testing/TestFramework.h"
#include "Renderer/ColorGradingMath.h"
#include "Renderer/Image.h"

#include <cmath>
#include <vector>

namespace
{
	FVector3 LutTexel(const std::vector<uint16>& Pixels, uint32 R, uint32 G, uint32 B)
	{
		const size_t Offset = (static_cast<size_t>(G) * ColorGradingMath::LutWidth + B * ColorGradingMath::LutSize + R) * 4;
		return FVector3(Pixels[Offset] / 65535.0f, Pixels[Offset + 1] / 65535.0f, Pixels[Offset + 2] / 65535.0f);
	}
} // namespace

// 기본값 = 항등 (LUT 칸 값 = 칸 좌표), 채도 0 = 회색, 온도 + = 빨강↑ 파랑↓ (밝기 유지), lift·gain
E_TEST(ColorGrading_LutAndOperators)
{
	FColorGradingParams Identity;
	E_EXPECT_TRUE(ColorGradingMath::IsIdentity(Identity));
	std::vector<uint16> Pixels;
	ColorGradingMath::BakeLut(Identity, nullptr, Pixels);
	E_EXPECT_TRUE(Pixels.size() == static_cast<size_t>(ColorGradingMath::LutWidth) * ColorGradingMath::LutHeight * 4);
	for (const uint32 Index : { 0u, 7u, 16u, 31u })
	{
		const FVector3 Texel = LutTexel(Pixels, Index, 31u - Index, Index / 2u);
		E_EXPECT_NEAR(Texel.X, Index / 31.0f, 2.0e-4f);
		E_EXPECT_NEAR(Texel.Y, (31u - Index) / 31.0f, 2.0e-4f);
		E_EXPECT_NEAR(Texel.Z, (Index / 2u) / 31.0f, 2.0e-4f);
	}

	FColorGradingParams Gray;
	Gray.Saturation = 0.0f;
	const FVector3 G = ColorGradingMath::ApplyGrading(FVector3(0.8f, 0.2f, 0.1f), Gray);
	E_EXPECT_NEAR(G.X, G.Y, 1.0e-5f);
	E_EXPECT_NEAR(G.Y, G.Z, 1.0e-5f);

	FColorGradingParams Warm;
	Warm.Temperature = 1.0f;
	const FVector3 Neutral(0.4f, 0.4f, 0.4f);
	const FVector3 W = ColorGradingMath::ApplyGrading(Neutral, Warm);
	E_EXPECT_TRUE(W.X > W.Y && W.Y > W.Z);
	E_EXPECT_NEAR(0.2126f * W.X + 0.7152f * W.Y + 0.0722f * W.Z, 0.4f, 2.0e-3f); // 휘도 유지

	FColorGradingParams Lifted;
	Lifted.Lift = FVector3(0.2f, 0.0f, 0.0f);
	E_EXPECT_TRUE(ColorGradingMath::ApplyGrading(FVector3(0.0f, 0.0f, 0.0f), Lifted).X > 0.01f); // 검정이 올라감
	E_EXPECT_NEAR(ColorGradingMath::ApplyGrading(FVector3(1.0f, 1.0f, 1.0f), Lifted).X, 1.0f, 1.0e-5f); // 흰색 그대로

	FColorGradingParams Contrast;
	Contrast.Contrast = 1.5f;
	const float Mid = ColorGradingMath::SrgbDecode(0.5f);
	E_EXPECT_NEAR(ColorGradingMath::ApplyGrading(FVector3(Mid, Mid, Mid), Contrast).X, Mid, 1.0e-4f); // 인코딩 0.5 고정
	E_EXPECT_TRUE(ColorGradingMath::ApplyGrading(FVector3(0.05f, 0.05f, 0.05f), Contrast).X < 0.05f);
}

// 사용자 LUT 띠(가로 = N², 세로 = N): 항등 띠는 그대로, 반전 띠는 반전, 세기 0.5는 절반
E_TEST(ColorGrading_UserStripLut)
{
	constexpr uint32 N = 16;
	FImage Strip;
	Strip.Width  = N * N;
	Strip.Height = N;
	Strip.Pixels.resize(static_cast<size_t>(Strip.Width) * Strip.Height * 4);
	FImage Inverted = Strip;
	for (uint32 G = 0; G < N; ++G)
	{
		for (uint32 B = 0; B < N; ++B)
		{
			for (uint32 R = 0; R < N; ++R)
			{
				const size_t Offset = (static_cast<size_t>(G) * Strip.Width + B * N + R) * 4;
				const uint8  Rv = static_cast<uint8>(R * 255 / (N - 1)), Gv = static_cast<uint8>(G * 255 / (N - 1)), Bv = static_cast<uint8>(B * 255 / (N - 1));
				Strip.Pixels[Offset + 0] = Rv;    Strip.Pixels[Offset + 1] = Gv;    Strip.Pixels[Offset + 2] = Bv;    Strip.Pixels[Offset + 3] = 255;
				Inverted.Pixels[Offset + 0] = 255 - Rv; Inverted.Pixels[Offset + 1] = 255 - Gv; Inverted.Pixels[Offset + 2] = 255 - Bv; Inverted.Pixels[Offset + 3] = 255;
			}
		}
	}
	E_EXPECT_TRUE(ColorGradingMath::IsStripLut(Strip));
	const FVector3 Same = ColorGradingMath::SampleStripLut(Strip, FVector3(0.3f, 0.6f, 0.9f));
	E_EXPECT_NEAR(Same.X, 0.3f, 0.01f);
	E_EXPECT_NEAR(Same.Y, 0.6f, 0.01f);
	E_EXPECT_NEAR(Same.Z, 0.9f, 0.01f);

	FColorGradingParams Params;
	Params.LutIntensity = 0.5f;
	std::vector<uint16> Pixels;
	ColorGradingMath::BakeLut(Params, &Inverted, Pixels);
	const FVector3 Half = LutTexel(Pixels, 31, 0, 31); // 인코딩 (1, 0, 1) → 반전 (0, 1, 0) 절반 → (0.5, 0.5, 0.5)
	E_EXPECT_NEAR(Half.X, 0.5f, 0.01f);
	E_EXPECT_NEAR(Half.Y, 0.5f, 0.01f);
	E_EXPECT_NEAR(Half.Z, 0.5f, 0.01f);
}

// 비네트: 가운데 0, 모서리(크기 + 부드러움 ≤ 1) 1, 둥글기 1이면 화면에서 원 (가로·세로 같은 화면 거리면 같은 값)
E_TEST(ColorGrading_VignetteMask)
{
	const float Aspect = 16.0f / 9.0f;
	E_EXPECT_NEAR(ColorGradingMath::ComputeVignetteMask(FVector2(0.5f, 0.5f), Aspect, 0.4f, 0.5f, 1.0f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(ColorGradingMath::ComputeVignetteMask(FVector2(0.0f, 0.0f), Aspect, 0.4f, 0.5f, 1.0f), 1.0f, 1.0e-6f);
	const float Horizontal = ColorGradingMath::ComputeVignetteMask(FVector2(0.5f + 0.2f / Aspect, 0.5f), Aspect, 0.1f, 0.5f, 1.0f);
	const float Vertical   = ColorGradingMath::ComputeVignetteMask(FVector2(0.5f, 0.7f), Aspect, 0.1f, 0.5f, 1.0f);
	E_EXPECT_NEAR(Horizontal, Vertical, 1.0e-5f);
}
