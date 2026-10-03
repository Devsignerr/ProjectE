#ifndef E_HDR_DISPLAY_HLSLI
#define E_HDR_DISPLAY_HLSLI

// HDR 디스플레이 출력 공용 (Phase 49). CPU 참조 식은 Renderer/HdrDisplayMath.h — 함께 고친다 (테스트 Hdr_*)

static const float E_HDR_PQ_MAX_NITS    = 10000.0f;
static const float E_HDR_SCRGB_WHITE    = 80.0f;
static const float E_HDR_DEFAULT_KNEE   = 0.5f;

// SMPTE ST.2084 (PQ): 선형 (nits / 10000) → 인코딩
float3 HdrPqEncode(float3 Linear)
{
	const float M1 = 2610.0f / 16384.0f;
	const float M2 = 2523.0f / 4096.0f * 128.0f;
	const float C1 = 3424.0f / 4096.0f;
	const float C2 = 2413.0f / 4096.0f * 32.0f;
	const float C3 = 2392.0f / 4096.0f * 32.0f;
	const float3 Y = pow(saturate(Linear), M1);
	return pow((C1 + C2 * Y) / (1.0f + C3 * Y), M2);
}

float3 HdrRec709ToRec2020(float3 C)
{
	return float3(dot(C, float3(0.627403896f, 0.329283039f, 0.043313065f)), dot(C, float3(0.069097289f, 0.919540395f, 0.011362316f)),
	              dot(C, float3(0.016391439f, 0.088013307f, 0.895595255f)));
}

// SDR 톤매핑 값 s(0~1)의 하이라이트를 [0, PeakRatio)로 펼친다 (FHdrDisplayMath::ExpandHighlights)
float HdrExpandHighlights(float S, float PeakRatio, float Knee)
{
	if (PeakRatio <= 1.0f || S <= Knee)
	{
		return S;
	}
	const float U     = min(S, 1.0f) - Knee;
	const float Range = 1.0f - Knee;
	const float Gain  = 1.0f - Range / (PeakRatio - Knee);
	return Knee + U / max(1.0f - U / Range * Gain, 1.0e-6f);
}

float3 HdrSrgbToLinear(float3 C)
{
	return select(C <= 0.04045f, C / 12.92f, pow((C + 0.055f) / 1.055f, 2.4f));
}

float3 HdrLinearToSrgb(float3 C)
{
	return select(C <= 0.0031308f, C * 12.92f, 1.055f * pow(C, 1.0f / 2.4f) - 0.055f);
}

#endif // E_HDR_DISPLAY_HLSLI
