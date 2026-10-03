#ifndef E_AERIAL_PERSPECTIVE_HLSLI
#define E_AERIAL_PERSPECTIVE_HLSLI

// 공중 원근 (Phase 49): 평평한 지면 + 지수 밀도 대기의 단일 산란(카메라 고도의 태양 투과율 상수) + 다중 산란(Ψms × 조도) 닫힌 식.
// CPU 참조 식은 Renderer/AtmosphereMath.h FAtmosphereMath::ComputeAerialPerspective — 함께 고친다 (테스트 Atmosphere_AerialPerspective*)
//   장면 크기(수 km 이하)에서는 곡률·태양 투과율 변화가 작아 이 근사로 충분하다. 단위 cm

static const float E_AERIAL_PI = 3.14159265358979f;

struct FAerialPerspectiveInputs
{
	float3 RayleighScattering; // 1/cm
	float  RayleighScaleHeight; // cm
	float3 MieScattering;
	float  MieScaleHeight;
	float3 MieExtinction;
	float  MieAnisotropy;
	float3 SunIlluminance;
	float3 SunDirection;
	float3 MultiScattering;
	float  GroundHeight;
	float  DistanceScale;
};

// 지수 밀도 exp(-(z - 지면) / H)의 광선 [0, Distance] 광학 길이
float AerialExponentialLength(float OriginZ, float DirZ, float Distance, float GroundHeight, float ScaleHeight)
{
	if (Distance <= 0.0f)
	{
		return 0.0f;
	}
	const float Falloff  = 1.0f / ScaleHeight;
	const float AtStart  = exp(-Falloff * max(OriginZ - GroundHeight, 0.0f));
	const float Exponent = Falloff * DirZ * Distance;
	if (abs(Exponent) < 1.0e-4f)
	{
		return AtStart * Distance * (1.0f - 0.5f * Exponent);
	}
	return AtStart * (1.0f - exp(-Exponent)) / (Falloff * DirZ);
}

float AerialRayleighPhase(float CosTheta)
{
	return 3.0f / (16.0f * E_AERIAL_PI) * (1.0f + CosTheta * CosTheta);
}

float AerialMiePhase(float G, float CosTheta)
{
	const float K     = 3.0f / (8.0f * E_AERIAL_PI) * (1.0f - G * G) / (2.0f + G * G);
	const float Denom = max(1.0f + G * G - 2.0f * G * CosTheta, 1.0e-6f);
	return K * (1.0f + CosTheta * CosTheta) / (Denom * sqrt(Denom));
}

// 더할 빛 (반환) + 채널별 투과율 (OutTransmittance)
float3 EvaluateAerialPerspective(FAerialPerspectiveInputs Inputs, float3 CameraPosition, float3 WorldPosition, out float3 OutTransmittance)
{
	OutTransmittance        = 1.0f;
	const float3 ToPoint    = WorldPosition - CameraPosition;
	const float  Distance   = length(ToPoint);
	if (Distance <= 1.0e-3f)
	{
		return 0.0f;
	}
	const float3 Dir            = ToPoint / Distance;
	const float  Scaled         = Distance * max(Inputs.DistanceScale, 0.0f);
	const float  RayleighLength = AerialExponentialLength(CameraPosition.z, Dir.z, Scaled, Inputs.GroundHeight, Inputs.RayleighScaleHeight);
	const float  MieLength      = AerialExponentialLength(CameraPosition.z, Dir.z, Scaled, Inputs.GroundHeight, Inputs.MieScaleHeight);
	const float3 RayleighDepth  = Inputs.RayleighScattering * RayleighLength;
	const float3 MieScatterDepth = Inputs.MieScattering * MieLength;
	const float3 OpticalDepth   = RayleighDepth + Inputs.MieExtinction * MieLength;
	OutTransmittance            = exp(-OpticalDepth);
	const float  CosTheta       = dot(Dir, Inputs.SunDirection);
	const float3 Single         = (RayleighDepth * AerialRayleighPhase(CosTheta) + MieScatterDepth * AerialMiePhase(Inputs.MieAnisotropy, CosTheta)) * Inputs.SunIlluminance;
	const float3 Multi          = (RayleighDepth + MieScatterDepth) * Inputs.MultiScattering;
	const float3 Factor         = select(OpticalDepth > 1.0e-6f, (1.0f - OutTransmittance) / max(OpticalDepth, 1.0e-6f), 1.0f);
	return (Single + Multi) * Factor;
}

#endif // E_AERIAL_PERSPECTIVE_HLSLI
