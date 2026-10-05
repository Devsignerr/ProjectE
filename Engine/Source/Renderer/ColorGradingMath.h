#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"

#include <cmath>
#include <vector>

struct FImage;

// 색 보정 LUT와 비네트 순수 식 (2026-10-05, HD-2D — 테스트 ColorGrading_*). 셰이더 쪽은 Tonemap.hlsl (LUT 읽기·비네트).
//   적용 위치: 톤매핑 직후 SDR [0, 1] 선형 값(HDR 출력이면 하이라이트를 펼치기 전) → 색 보정 LUT → 비네트.
//   LUT = 32³ 칸을 1024x32 띠(파랑 = 가로 칸 번호, 빨강 = 칸 안 가로, 초록 = 세로) 한 장 — 인덱스·값 모두 sRGB 인코딩 공간
//   (선형으로 칸을 나누면 어두운 쪽이 거칠다). 셰이더는 두 칸 쌍선형 + 파랑 보간.
//   보정 순서 (선형 입력 → 선형 출력): 화이트 밸런스(선형 — 밝기 보존) → 대비(인코딩 공간, 0.5 중심) → 채도(선형, Rec.709 휘도) →
//   lift·gamma·gain(인코딩 공간, 채널별: c = gain × (c + lift × (1 - c)), c^(1/gamma)) → 0~1 → 사용자 LUT(인코딩 공간, 세기만큼 섞음)
struct FColorGradingParams
{
	float    Temperature = 0.0f; // -1(차갑게) ~ 1(따뜻하게)
	float    Tint        = 0.0f; // -1(초록) ~ 1(자홍)
	float    Saturation  = 1.0f;
	float    Contrast    = 1.0f;
	FVector3 Lift        = FVector3(0.0f, 0.0f, 0.0f);
	FVector3 Gamma       = FVector3(1.0f, 1.0f, 1.0f);
	FVector3 Gain        = FVector3(1.0f, 1.0f, 1.0f);
	float    LutIntensity = 1.0f; // 사용자 LUT 섞는 정도 (LUT가 있을 때)
};

namespace ColorGradingMath
{
	inline constexpr uint32 LutSize   = 32;                 // 한 축 칸 수
	inline constexpr uint32 LutWidth  = LutSize * LutSize;  // 1024
	inline constexpr uint32 LutHeight = LutSize;            // 32

	inline float SrgbEncode(float Linear)
	{
		const float C = FMath::Clamp(Linear, 0.0f, 1.0f);
		return C <= 0.0031308f ? C * 12.92f : 1.055f * std::pow(C, 1.0f / 2.4f) - 0.055f;
	}
	inline float SrgbDecode(float Encoded)
	{
		const float C = FMath::Clamp(Encoded, 0.0f, 1.0f);
		return C <= 0.04045f ? C / 12.92f : std::pow((C + 0.055f) / 1.055f, 2.4f);
	}

	// 사용자 LUT 없이 보정 값이 항등인가 (그럼 LUT를 만들지 않고 셰이더도 건너뛴다)
	bool IsIdentity(const FColorGradingParams& Params);

	// 선형 [0, 1] → 보정된 선형 [0, 1] (사용자 LUT 제외)
	FVector3 ApplyGrading(const FVector3& Linear, const FColorGradingParams& Params);

	// 사용자 LUT 띠 이미지(가로 = N², 세로 = N, RGBA8 — 256x16·1024x32 등)에서 인코딩 색을 3선형으로 읽는다. 형식이 아니면 입력 그대로
	FVector3 SampleStripLut(const FImage& Lut, const FVector3& Encoded);
	bool     IsStripLut(const FImage& Lut);

	// 1024x32 RGBA16 UNORM 띠 (값 = 보정된 인코딩 색 × 65535, A = 65535). UserLut가 없거나 형식이 아니면 보정만
	void BakeLut(const FColorGradingParams& Params, const FImage* UserLut, std::vector<uint16>& OutPixels);

	// 비네트 가림 정도 0~1 (Tonemap.hlsl VignetteMask와 같은 식): p = (uv - 0.5) × 2, 가로 × lerp(1, 화면비, 둥글기)
	//   (1 = 화면에서 원, 0 = 화면 모양 타원), r = |p| / |모서리| (모서리 = 1) → smoothstep(크기, 크기 + 부드러움, r)
	inline float ComputeVignetteMask(const FVector2& Uv, float Aspect, float Size, float Smoothness, float Roundness)
	{
		const float ScaleX  = 1.0f + (Aspect - 1.0f) * FMath::Clamp(Roundness, 0.0f, 1.0f);
		const float X       = (Uv.X - 0.5f) * 2.0f * ScaleX;
		const float Y       = (Uv.Y - 0.5f) * 2.0f;
		const float R       = std::sqrt(X * X + Y * Y) / std::sqrt(ScaleX * ScaleX + 1.0f);
		const float Edge0   = Size;
		const float Edge1   = Size + FMath::Max(Smoothness, 1.0e-4f);
		const float T       = FMath::Clamp((R - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}
} // namespace ColorGradingMath
