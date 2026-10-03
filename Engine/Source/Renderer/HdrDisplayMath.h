#pragma once

#include "Core/Math/Math.h"

#include <cmath>

// HDR 디스플레이 출력 (Phase 49) — CPU 참조 식. 셰이더 HdrDisplay.hlsli와 같은 식 (테스트 HdrDisplayTests)
//   톤매핑: SDR 경로는 기존 ACES 근사(Tonemap.hlsl) 그대로. HDR 경로는 같은 SDR 곡선 값 s를 무릎(Knee) 위에서만 늘려
//     [0, 1) → [0, PeakRatio)로 펼친다 (무릎 아래는 SDR과 같은 값 — 중간톤·UI 밝기 일치). PeakRatio = 최대 밝기 nits / 종이 흰색 nits
//   출력: 선형 BT.709, 1.0 = 종이 흰색. 합성 패스가 HDR10(ST.2084 PQ + BT.2020) 또는 scRGB(선형 BT.709, 1.0 = 80 nits)로 인코딩한다
namespace FHdrDisplayMath
{
	constexpr float PqMaxNits   = 10000.0f;
	constexpr float ScRgbWhiteNits = 80.0f; // scRGB 1.0
	constexpr float DefaultKnee = 0.5f;

	// SMPTE ST.2084 (PQ): 선형 (nits / 10000, 0~1) → 인코딩 (0~1)
	inline float PqEncode(float Linear)
	{
		constexpr float M1 = 2610.0f / 16384.0f;
		constexpr float M2 = 2523.0f / 4096.0f * 128.0f;
		constexpr float C1 = 3424.0f / 4096.0f;
		constexpr float C2 = 2413.0f / 4096.0f * 32.0f;
		constexpr float C3 = 2392.0f / 4096.0f * 32.0f;
		const float     Y  = std::pow(FMath::Clamp(Linear, 0.0f, 1.0f), M1);
		return std::pow((C1 + C2 * Y) / (1.0f + C3 * Y), M2);
	}
	inline float PqDecode(float Encoded)
	{
		constexpr float M1 = 2610.0f / 16384.0f;
		constexpr float M2 = 2523.0f / 4096.0f * 128.0f;
		constexpr float C1 = 3424.0f / 4096.0f;
		constexpr float C2 = 2413.0f / 4096.0f * 32.0f;
		constexpr float C3 = 2392.0f / 4096.0f * 32.0f;
		const float     E  = std::pow(FMath::Clamp(Encoded, 0.0f, 1.0f), 1.0f / M2);
		return std::pow(FMath::Max(E - C1, 0.0f) / (C2 - C3 * E), 1.0f / M1);
	}

	// BT.709 선형 → BT.2020 선형 (ITU-R BT.2087)
	inline FVector3 Rec709ToRec2020(const FVector3& C)
	{
		return FVector3(0.627403896f * C.X + 0.329283039f * C.Y + 0.043313065f * C.Z, 0.069097289f * C.X + 0.919540395f * C.Y + 0.011362316f * C.Z,
		                0.016391439f * C.X + 0.088013307f * C.Y + 0.895595255f * C.Z);
	}

	// HDR 하이라이트 펼치기: s(SDR 톤매핑 값, 0~1) → [0, PeakRatio). 무릎 아래는 그대로, 무릎에서 기울기 1로 이어진다 (C1 연속, 단조)
	inline float ExpandHighlights(float S, float PeakRatio, float Knee = DefaultKnee)
	{
		if (PeakRatio <= 1.0f || S <= Knee)
		{
			return S;
		}
		const float U     = FMath::Min(S, 1.0f) - Knee;
		const float Range = 1.0f - Knee;
		const float Gain  = 1.0f - Range / (PeakRatio - Knee); // s = 1에서 분모가 Range / (Peak - Knee)
		return Knee + U / FMath::Max(1.0f - U / Range * Gain, 1.0e-6f);
	}

	// 선형 BT.709 (1 = 종이 흰색) → HDR10 신호 (PQ, BT.2020)
	inline FVector3 EncodeHdr10(const FVector3& Linear709, float PaperWhiteNits)
	{
		const FVector3 Rec2020 = Rec709ToRec2020(Linear709) * (PaperWhiteNits / PqMaxNits);
		return FVector3(PqEncode(Rec2020.X), PqEncode(Rec2020.Y), PqEncode(Rec2020.Z));
	}
	// 선형 BT.709 (1 = 종이 흰색) → scRGB (1 = 80 nits)
	inline FVector3 EncodeScRgb(const FVector3& Linear709, float PaperWhiteNits)
	{
		return Linear709 * (PaperWhiteNits / ScRgbWhiteNits);
	}

	// sRGB 전달 함수 (UI 겹침 층 해독)
	inline float SrgbToLinear(float C)
	{
		return C <= 0.04045f ? C / 12.92f : std::pow((C + 0.055f) / 1.055f, 2.4f);
	}
} // namespace FHdrDisplayMath
