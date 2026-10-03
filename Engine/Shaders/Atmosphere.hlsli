#ifndef E_ATMOSPHERE_HLSLI
#define E_ATMOSPHERE_HLSLI

// 물리 기반 대기 공용 (Phase 49, Hillaire 2020). CPU 참조 식은 Renderer/AtmosphereMath.h — 함께 고친다 (테스트 AtmosphereTests)
//   단위 km, 행성 중심 원점 Z-up. LUT: 투과율(t0) 256x64, 다중 산란(t1) 32x32, 하늘 뷰(t2) 192x108
//   레지스터는 포함하는 쪽이 E_ATMOSPHERE_CONSTANTS_REGISTER / E_ATMOSPHERE_LUT_SPACE로 바꾼다

#ifndef E_ATMOSPHERE_CONSTANTS_REGISTER
#define E_ATMOSPHERE_CONSTANTS_REGISTER b0
#endif
#ifndef E_ATMOSPHERE_TRANSMITTANCE_REGISTER
#define E_ATMOSPHERE_TRANSMITTANCE_REGISTER t0
#endif
#ifndef E_ATMOSPHERE_MULTISCATTER_REGISTER
#define E_ATMOSPHERE_MULTISCATTER_REGISTER t1
#endif
#ifndef E_ATMOSPHERE_SKYVIEW_REGISTER
#define E_ATMOSPHERE_SKYVIEW_REGISTER t2
#endif
#ifndef E_ATMOSPHERE_SAMPLER_REGISTER
#define E_ATMOSPHERE_SAMPLER_REGISTER s0
#endif

// ShaderTypes.h FAtmosphereConstants와 1:1
cbuffer AtmosphereConstants : register(E_ATMOSPHERE_CONSTANTS_REGISTER)
{
	float3 AtmoRayleighScattering;
	float  AtmoBottomRadius;
	float3 AtmoMieScattering;
	float  AtmoTopRadius;
	float3 AtmoMieExtinction;
	float  AtmoRayleighDensityExpScale;
	float3 AtmoMieAbsorption;
	float  AtmoMieDensityExpScale;
	float3 AtmoOzoneAbsorption;
	float  AtmoMiePhaseG;
	float3 AtmoGroundAlbedo;
	float  AtmoOzoneCenterHeight;
	float3 AtmoSunDirection;
	float  AtmoOzoneHalfWidth;
	float3 AtmoSunIlluminance;
	float  AtmoSunDiskCosHalfAngle;
	float3 AtmoCameraPosition;
	float  AtmoSunDiskLuminance;
	float3 AtmoNightSkyLuminance;
	float  AtmoStarIntensity;
	float3 AtmoSkyCameraForward;
	float  AtmoSkyTanHalfFov;
	float3 AtmoSkyCameraRight;
	float  AtmoSkyAspect;
	float3 AtmoSkyCameraUp;
	uint   AtmoOrthographic;
	float3 AtmoMoonDirection;
	float  AtmoMoonDiskLuminance;
	float3 AtmoMoonIlluminance;
	float  AtmoPadding0;
};

Texture2D<float4> AtmoTransmittanceLut   : register(E_ATMOSPHERE_TRANSMITTANCE_REGISTER);
Texture2D<float4> AtmoMultiScatteringLut : register(E_ATMOSPHERE_MULTISCATTER_REGISTER);
Texture2D<float4> AtmoSkyViewLut         : register(E_ATMOSPHERE_SKYVIEW_REGISTER);
SamplerState      AtmoLinearSampler      : register(E_ATMOSPHERE_SAMPLER_REGISTER);

static const float  E_ATMO_PI                  = 3.14159265358979f;
static const float  E_ATMO_PLANET_OFFSET       = 0.01f; // FAtmosphereMath::PlanetRadiusOffset
static const float2 E_ATMO_MULTISCATTER_SIZE   = float2(32.0f, 32.0f);
static const float2 E_ATMO_SKYVIEW_SIZE        = float2(192.0f, 108.0f);

struct FAtmoMedium
{
	float3 Scattering;
	float3 Extinction;
	float3 RayleighScattering;
	float3 MieScattering;
};

FAtmoMedium SampleAtmosphereMedium(float Height)
{
	const float RayleighDensity = exp(max(Height, 0.0f) * AtmoRayleighDensityExpScale);
	const float MieDensity      = exp(max(Height, 0.0f) * AtmoMieDensityExpScale);
	const float OzoneDensity    = max(0.0f, 1.0f - abs(Height - AtmoOzoneCenterHeight) / AtmoOzoneHalfWidth);
	FAtmoMedium Medium;
	Medium.RayleighScattering = AtmoRayleighScattering * RayleighDensity;
	Medium.MieScattering      = AtmoMieScattering * MieDensity;
	Medium.Scattering         = Medium.RayleighScattering + Medium.MieScattering;
	Medium.Extinction         = Medium.RayleighScattering + AtmoMieExtinction * MieDensity + AtmoOzoneAbsorption * OzoneDensity;
	return Medium;
}

// 원점 중심 구와 가장 가까운 양의 교차 (없으면 -1)
float AtmoRaySphereNearest(float3 Origin, float3 Direction, float Radius)
{
	const float B = dot(Origin, Direction);
	const float C = dot(Origin, Origin) - Radius * Radius;
	const float Discriminant = B * B - C;
	if (Discriminant < 0.0f)
	{
		return -1.0f;
	}
	const float Root = sqrt(Discriminant);
	const float T0   = -B - Root;
	const float T1   = -B + Root;
	if (T0 < 0.0f && T1 < 0.0f)
	{
		return -1.0f;
	}
	if (T0 < 0.0f)
	{
		return max(0.0f, T1);
	}
	return max(0.0f, T0);
}

float AtmoRayleighPhase(float CosTheta)
{
	return 3.0f / (16.0f * E_ATMO_PI) * (1.0f + CosTheta * CosTheta);
}

float AtmoMiePhase(float G, float CosTheta)
{
	const float K     = 3.0f / (8.0f * E_ATMO_PI) * (1.0f - G * G) / (2.0f + G * G);
	const float Denom = max(1.0f + G * G - 2.0f * G * CosTheta, 1.0e-6f);
	return K * (1.0f + CosTheta * CosTheta) / (Denom * sqrt(Denom));
}

// ---- 투과율 LUT (Bruneton 매개화)
void AtmoTransmittanceUvToParams(float2 UV, out float ViewHeight, out float ViewZenithCos)
{
	const float H    = sqrt(max(AtmoTopRadius * AtmoTopRadius - AtmoBottomRadius * AtmoBottomRadius, 0.0f));
	const float Rho  = H * UV.y;
	ViewHeight       = sqrt(Rho * Rho + AtmoBottomRadius * AtmoBottomRadius);
	const float DMin = AtmoTopRadius - ViewHeight;
	const float DMax = Rho + H;
	const float D    = DMin + UV.x * (DMax - DMin);
	ViewZenithCos    = D == 0.0f ? 1.0f : (H * H - Rho * Rho - D * D) / (2.0f * ViewHeight * D);
	ViewZenithCos    = clamp(ViewZenithCos, -1.0f, 1.0f);
}

float2 AtmoTransmittanceParamsToUv(float ViewHeight, float ViewZenithCos)
{
	const float H            = sqrt(max(AtmoTopRadius * AtmoTopRadius - AtmoBottomRadius * AtmoBottomRadius, 0.0f));
	const float Rho          = sqrt(max(ViewHeight * ViewHeight - AtmoBottomRadius * AtmoBottomRadius, 0.0f));
	const float Discriminant = ViewHeight * ViewHeight * (ViewZenithCos * ViewZenithCos - 1.0f) + AtmoTopRadius * AtmoTopRadius;
	const float D            = max(0.0f, -ViewHeight * ViewZenithCos + sqrt(max(Discriminant, 0.0f)));
	const float DMin         = AtmoTopRadius - ViewHeight;
	const float DMax         = Rho + H;
	return float2((D - DMin) / max(DMax - DMin, 1.0e-6f), Rho / max(H, 1.0e-6f));
}

float3 AtmoGetTransmittance(float ViewHeight, float ViewZenithCos)
{
	return AtmoTransmittanceLut.SampleLevel(AtmoLinearSampler, AtmoTransmittanceParamsToUv(ViewHeight, ViewZenithCos), 0.0f).rgb;
}

// ---- 다중 산란 LUT (서브 UV)
float AtmoFromUnitToSubUv(float U, float Resolution) { return (U + 0.5f / Resolution) * (Resolution / (Resolution + 1.0f)); }
float AtmoFromSubUvToUnit(float U, float Resolution) { return (U - 0.5f / Resolution) * (Resolution / (Resolution - 1.0f)); }

float3 AtmoGetMultipleScattering(float ViewHeight, float SunZenithCos)
{
	float2 UV = saturate(float2(SunZenithCos * 0.5f + 0.5f, (ViewHeight - AtmoBottomRadius) / (AtmoTopRadius - AtmoBottomRadius)));
	UV        = float2(AtmoFromUnitToSubUv(UV.x, E_ATMO_MULTISCATTER_SIZE.x), AtmoFromUnitToSubUv(UV.y, E_ATMO_MULTISCATTER_SIZE.y));
	return AtmoMultiScatteringLut.SampleLevel(AtmoLinearSampler, UV, 0.0f).rgb;
}

// ---- 하늘 뷰 LUT (Hillaire 매개화)
void AtmoSkyViewUvToParams(float ViewHeight, float2 UV, out float ViewZenithCos, out float LightViewCos)
{
	const float VHorizon           = sqrt(max(ViewHeight * ViewHeight - AtmoBottomRadius * AtmoBottomRadius, 0.0f));
	const float CosBeta            = VHorizon / ViewHeight;
	const float Beta               = acos(clamp(CosBeta, -1.0f, 1.0f));
	const float ZenithHorizonAngle = E_ATMO_PI - Beta;
	float       ViewZenithAngle;
	if (UV.y < 0.5f)
	{
		float Coord = 2.0f * UV.y;
		Coord       = 1.0f - Coord;
		Coord *= Coord;
		Coord           = 1.0f - Coord;
		ViewZenithAngle = ZenithHorizonAngle * Coord;
	}
	else
	{
		float Coord = UV.y * 2.0f - 1.0f;
		Coord *= Coord;
		ViewZenithAngle = ZenithHorizonAngle + Beta * Coord;
	}
	ViewZenithCos     = cos(ViewZenithAngle);
	const float Coord = UV.x * UV.x;
	LightViewCos      = -(Coord * 2.0f - 1.0f);
}

float2 AtmoSkyViewParamsToUv(float ViewHeight, float ViewZenithCos, float LightViewCos, bool bIntersectGround)
{
	const float VHorizon           = sqrt(max(ViewHeight * ViewHeight - AtmoBottomRadius * AtmoBottomRadius, 0.0f));
	const float CosBeta            = VHorizon / ViewHeight;
	const float Beta               = acos(clamp(CosBeta, -1.0f, 1.0f));
	const float ZenithHorizonAngle = E_ATMO_PI - Beta;
	float2      UV;
	if (!bIntersectGround)
	{
		float Coord = acos(clamp(ViewZenithCos, -1.0f, 1.0f)) / ZenithHorizonAngle;
		Coord       = 1.0f - Coord;
		Coord       = sqrt(max(Coord, 0.0f));
		Coord       = 1.0f - Coord;
		UV.y        = Coord * 0.5f;
	}
	else
	{
		float Coord = (acos(clamp(ViewZenithCos, -1.0f, 1.0f)) - ZenithHorizonAngle) / max(Beta, 1.0e-6f);
		Coord       = sqrt(max(Coord, 0.0f));
		UV.y        = Coord * 0.5f + 0.5f;
	}
	UV.x = sqrt(max(-LightViewCos * 0.5f + 0.5f, 0.0f));
	return UV;
}

// 대기 좌표 위치에서 본 월드 방향 Dir의 하늘 휘도 (하늘 뷰 LUT, 카메라 고도 기준 LUT라 위치는 카메라 근처여야 한다)
float3 AtmoSampleSkyView(float3 Position, float3 Dir)
{
	const float  ViewHeight    = length(Position);
	const float3 Up            = Position / ViewHeight;
	const float  ViewZenithCos = dot(Dir, Up);
	const float3 SideView      = Dir - Up * ViewZenithCos;
	const float3 SideSun       = AtmoSunDirection - Up * dot(AtmoSunDirection, Up);
	const float  SideLength    = length(SideView) * length(SideSun);
	const float  LightViewCos  = SideLength > 1.0e-6f ? dot(SideView, SideSun) / SideLength : 1.0f;
	const bool   bGround       = AtmoRaySphereNearest(Position, Dir, AtmoBottomRadius) >= 0.0f;
	float2       UV            = AtmoSkyViewParamsToUv(ViewHeight, ViewZenithCos, LightViewCos, bGround);
	UV                         = float2(AtmoFromUnitToSubUv(UV.x, E_ATMO_SKYVIEW_SIZE.x), AtmoFromUnitToSubUv(UV.y, E_ATMO_SKYVIEW_SIZE.y));
	return AtmoSkyViewLut.SampleLevel(AtmoLinearSampler, UV, 0.0f).rgb;
}

// 산란 적분 (하늘 뷰 LUT): 광선 Origin → 대기 끝/지면, 단일 산란(태양 투과율 LUT + 지면 그림자) + 다중 산란 LUT. 조도 1 기준
float3 AtmoIntegrateScatteredLuminance(float3 Origin, float3 Direction, float3 SunDirection, uint SampleCount, out float3 OutTransmittance)
{
	OutTransmittance    = 1.0f;
	const float TBottom = AtmoRaySphereNearest(Origin, Direction, AtmoBottomRadius);
	const float TTop    = AtmoRaySphereNearest(Origin, Direction, AtmoTopRadius);
	float       TMax    = TTop;
	if (TBottom > 0.0f && (TTop < 0.0f || TBottom < TTop))
	{
		TMax = TBottom;
	}
	if (TMax <= 0.0f)
	{
		return 0.0f;
	}
	const float  CosTheta      = dot(SunDirection, Direction);
	const float  MiePhaseValue = AtmoMiePhase(AtmoMiePhaseG, CosTheta);
	const float  RayPhaseValue = AtmoRayleighPhase(CosTheta);
	const float  Dt            = TMax / float(SampleCount);
	float3       Luminance     = 0.0f;
	float3       Throughput    = 1.0f;
	for (uint Step = 0; Step < SampleCount; ++Step)
	{
		const float3      P      = Origin + Direction * ((float(Step) + 0.3f) * Dt);
		const float       Radius = length(P);
		const float3      Up     = P / Radius;
		const FAtmoMedium Medium = SampleAtmosphereMedium(Radius - AtmoBottomRadius);
		const float       SunCos = dot(SunDirection, Up);
		const float3      SunTransmittance = AtmoGetTransmittance(Radius, SunCos);
		const float       EarthShadow      = AtmoRaySphereNearest(P + Up * E_ATMO_PLANET_OFFSET, SunDirection, AtmoBottomRadius) >= 0.0f ? 0.0f : 1.0f;
		const float3      MultiScattering  = AtmoGetMultipleScattering(Radius, SunCos);
		const float3      S = EarthShadow * SunTransmittance * (Medium.RayleighScattering * RayPhaseValue + Medium.MieScattering * MiePhaseValue) +
		                 MultiScattering * Medium.Scattering;
		const float3 SampleTransmittance = exp(-Medium.Extinction * Dt);
		const float3 Integral            = (1.0f - SampleTransmittance) / max(Medium.Extinction, 1.0e-9f);
		Luminance += Throughput * S * Integral;
		Throughput *= SampleTransmittance;
	}
	// 지면에 닿는 광선: 램버트 지면 (태양 투과율 × 기울기 × 반사율 / π) — 지평선 아래가 검은 띠가 되지 않게, 환경광의 땅 반사
	if (TBottom > 0.0f && TMax == TBottom)
	{
		const float3 P      = Origin + Direction * TBottom;
		const float3 Up     = normalize(P);
		const float  SunCos = dot(SunDirection, Up);
		Luminance += Throughput * AtmoGetTransmittance(length(P), SunCos) * AtmoGroundAlbedo * (saturate(SunCos) / E_ATMO_PI);
	}
	OutTransmittance = Throughput;
	return Luminance;
}

// 하늘 패스·하늘 큐브 공용: 방향 Dir의 하늘 휘도 (태양 원반 제외, 밤하늘 최소 밝기 포함)
float3 AtmoSkyLuminance(float3 Dir)
{
	return AtmoSampleSkyView(AtmoCameraPosition, Dir) + AtmoNightSkyLuminance;
}

#endif // E_ATMOSPHERE_HLSLI
