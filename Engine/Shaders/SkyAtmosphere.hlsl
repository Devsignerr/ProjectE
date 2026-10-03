#include "Common.hlsli"
#include "Atmosphere.hlsli" // b0, t0~t2, s0

// 물리 기반 대기 LUT 계산 + 하늘 패스 + IBL 하늘 큐브 (FSkyAtmosphereRenderer, Phase 49). 식은 Atmosphere.hlsli / Renderer/AtmosphereMath.h
//   TransmittanceLutCS   (256x64)  매질이 바뀔 때만
//   MultiScatteringLutCS (32x32)   매질이 바뀔 때만 (투과율 LUT 읽음)
//   SkyViewLutCS         (192x108) 매 프레임 (카메라 고도·태양)
//   SkyCubeCS            IBL 하늘 큐브 밉 0 (하늘 뷰 LUT + 밤하늘, 태양 원반 제외 — 태양 직접광은 방향광), 구름은 t3 (없으면 투과 1)
//   VSSky/PSSky          메인 패스 하늘 (깊이 1, LESS_EQUAL): 하늘 뷰 LUT + 태양 원반(투과율) + 별 + 밤하늘

cbuffer PassConstants : register(b1)
{
	uint  PassSize;      // 출력 크기 (정사각 큐브 면 / LUT 폭)
	uint  PassHeight;
	uint  PassSampleCount;
	float PassCloudWeight; // SkyCubeCS: 구름 합성 (0 = 없음)
};

RWTexture2D<float4>      LutOutput   : register(u0);
RWTexture2DArray<float4> CubeOutput  : register(u1);
TextureCube<float4>      CloudCube   : register(t3); // SkyCubeCS: rgb 구름 산란, a 투과율 (구름 렌더러가 미리 그린 저해상도 큐브)

[numthreads(8, 8, 1)]
void TransmittanceLutCS(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= PassSize || Id.y >= PassHeight)
	{
		return;
	}
	const float2 UV = (float2(Id.xy) + 0.5f) / float2(PassSize, PassHeight);
	float        ViewHeight;
	float        ViewZenithCos;
	AtmoTransmittanceUvToParams(UV, ViewHeight, ViewZenithCos);
	const float3 Origin    = float3(0.0f, 0.0f, ViewHeight);
	const float3 Direction = float3(sqrt(max(1.0f - ViewZenithCos * ViewZenithCos, 0.0f)), 0.0f, ViewZenithCos);
	const float  TBottom   = AtmoRaySphereNearest(Origin, Direction, AtmoBottomRadius);
	const float  TTop      = AtmoRaySphereNearest(Origin, Direction, AtmoTopRadius);
	float        TMax      = TTop;
	if (TBottom > 0.0f && (TTop < 0.0f || TBottom < TTop))
	{
		TMax = TBottom;
	}
	float3 OpticalDepth = 0.0f;
	if (TMax > 0.0f)
	{
		const uint  Steps = 40; // FAtmosphereMath::TransmittanceSteps
		const float Dt    = TMax / float(Steps);
		for (uint Step = 0; Step < Steps; ++Step)
		{
			const float3 P = Origin + Direction * ((float(Step) + 0.5f) * Dt);
			OpticalDepth += SampleAtmosphereMedium(length(P) - AtmoBottomRadius).Extinction * Dt;
		}
	}
	LutOutput[Id.xy] = float4(exp(-OpticalDepth), 1.0f);
}

// 8x8 균등 구면 방향 (FAtmosphereMath::GetMultiScatteringDirection)
float3 MultiScatteringDirection(uint Index)
{
	const float I      = (float(Index % 8) + 0.5f) / 8.0f;
	const float J      = (float(Index / 8) + 0.5f) / 8.0f;
	const float Theta  = 2.0f * E_ATMO_PI * I;
	const float CosPhi = 1.0f - 2.0f * J;
	const float SinPhi = sqrt(max(1.0f - CosPhi * CosPhi, 0.0f));
	return float3(cos(Theta) * SinPhi, sin(Theta) * SinPhi, CosPhi);
}

// Ψms = L2차 / (1 - f_ms) (FAtmosphereMath::ComputeMultipleScattering과 같은 식, 투과율은 LUT)
[numthreads(8, 8, 1)]
void MultiScatteringLutCS(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= PassSize || Id.y >= PassHeight)
	{
		return;
	}
	float2 UV           = (float2(Id.xy) + 0.5f) / float2(PassSize, PassHeight);
	UV                  = float2(AtmoFromSubUvToUnit(UV.x, float(PassSize)), AtmoFromSubUvToUnit(UV.y, float(PassHeight)));
	const float SunCosZ = UV.x * 2.0f - 1.0f;
	const float Height  = clamp(UV.y, 0.0f, 1.0f) * (AtmoTopRadius - AtmoBottomRadius);
	const float3 Origin = float3(0.0f, 0.0f, AtmoBottomRadius + Height);
	const float3 Sun    = float3(sqrt(max(1.0f - SunCosZ * SunCosZ, 0.0f)), 0.0f, SunCosZ);
	const float  IsotropicPhase = 1.0f / (4.0f * E_ATMO_PI);
	float3       SecondOrder    = 0.0f;
	float3       MultiAsOne     = 0.0f;
	for (uint DirIndex = 0; DirIndex < 64; ++DirIndex)
	{
		const float3 Direction = MultiScatteringDirection(DirIndex);
		const float  TBottom   = AtmoRaySphereNearest(Origin, Direction, AtmoBottomRadius);
		const float  TTop      = AtmoRaySphereNearest(Origin, Direction, AtmoTopRadius);
		float        TMax      = TTop;
		if (TBottom > 0.0f && (TTop < 0.0f || TBottom < TTop))
		{
			TMax = TBottom;
		}
		if (TMax <= 0.0f)
		{
			continue;
		}
		const float Dt          = TMax / 20.0f; // FAtmosphereMath::MultiScatteringSteps
		float3      Throughput  = 1.0f;
		float3      Luminance   = 0.0f;
		float3      ScatterOnce = 0.0f;
		for (uint Step = 0; Step < 20; ++Step)
		{
			const float3      P      = Origin + Direction * ((float(Step) + 0.3f) * Dt);
			const float       Radius = length(P);
			const float3      Up     = P / Radius;
			const FAtmoMedium Medium = SampleAtmosphereMedium(Radius - AtmoBottomRadius);
			const float       SunCos = dot(Sun, Up);
			const float3      SunTransmittance = AtmoGetTransmittance(Radius, SunCos);
			const bool        bShadow = AtmoRaySphereNearest(P + Up * E_ATMO_PLANET_OFFSET, Sun, AtmoBottomRadius) >= 0.0f;
			const float3      SampleTransmittance = exp(-Medium.Extinction * Dt);
			const float3      Integral = (1.0f - SampleTransmittance) / max(Medium.Extinction, 1.0e-9f);
			const float3      S        = bShadow ? 0.0f : Medium.Scattering * SunTransmittance * IsotropicPhase;
			Luminance += Throughput * S * Integral;
			ScatterOnce += Throughput * Medium.Scattering * Integral;
			Throughput *= SampleTransmittance;
		}
		if (TBottom > 0.0f && TMax == TBottom)
		{
			const float3 P      = Origin + Direction * TBottom;
			const float3 Up     = normalize(P);
			const float  SunCos = dot(Sun, Up);
			Luminance += Throughput * AtmoGetTransmittance(length(P), SunCos) * AtmoGroundAlbedo * (max(SunCos, 0.0f) / E_ATMO_PI);
		}
		SecondOrder += Luminance;
		MultiAsOne += ScatterOnce;
	}
	SecondOrder /= 64.0f;
	MultiAsOne /= 64.0f; // 등방 위상 × 4π = 1
	LutOutput[Id.xy] = float4(SecondOrder / max(1.0f - MultiAsOne, 1.0e-3f), 1.0f);
}

[numthreads(8, 8, 1)]
void SkyViewLutCS(uint3 Id : SV_DispatchThreadID)
{
	if (Id.x >= PassSize || Id.y >= PassHeight)
	{
		return;
	}
	float2 UV = (float2(Id.xy) + 0.5f) / float2(PassSize, PassHeight);
	UV        = float2(AtmoFromSubUvToUnit(UV.x, float(PassSize)), AtmoFromSubUvToUnit(UV.y, float(PassHeight)));
	const float  ViewHeight = length(AtmoCameraPosition);
	float        ViewZenithCos;
	float        LightViewCos;
	AtmoSkyViewUvToParams(ViewHeight, saturate(UV), ViewZenithCos, LightViewCos);
	// 지역 좌표: 위 = +Z, 태양은 XZ 평면 (방위 기준)
	const float3 Up       = AtmoCameraPosition / ViewHeight;
	const float  SunCosZ  = dot(AtmoSunDirection, Up);
	const float3 Sun      = normalize(float3(sqrt(max(1.0f - SunCosZ * SunCosZ, 0.0f)), 0.0f, SunCosZ));
	const float  ViewSinZ = sqrt(max(1.0f - ViewZenithCos * ViewZenithCos, 0.0f));
	const float3 Dir      = float3(ViewSinZ * LightViewCos, ViewSinZ * sqrt(max(1.0f - LightViewCos * LightViewCos, 0.0f)), ViewZenithCos);
	float3       Transmittance;
	const float3 Luminance = AtmoIntegrateScatteredLuminance(float3(0.0f, 0.0f, ViewHeight), Dir, Sun, PassSampleCount, Transmittance);
	LutOutput[Id.xy]       = float4(Luminance * AtmoSunIlluminance, 1.0f);
}

// IBL 큐브 면 방향 (Ibl.hlsl FaceDirection과 같은 규약 — D3D 큐브 면 +X, -X, +Y, -Y, +Z, -Z)
float3 SkyCubeFaceDirection(uint Face, float2 UV)
{
	const float U = UV.x;
	const float V = UV.y;
	if (Face == 0) return normalize(float3(1, -V, -U));
	if (Face == 1) return normalize(float3(-1, -V, U));
	if (Face == 2) return normalize(float3(U, 1, V));
	if (Face == 3) return normalize(float3(U, -1, -V));
	if (Face == 4) return normalize(float3(U, -V, 1));
	return normalize(float3(-U, -V, -1));
}

[numthreads(8, 8, 1)]
void SkyCubeCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(Id.xy >= PassSize) || Id.z >= 6)
	{
		return;
	}
	const float3 Dir   = SkyCubeFaceDirection(Id.z, (float2(Id.xy) + 0.5f) / float(PassSize) * 2.0f - 1.0f);
	float3       Color = AtmoSkyLuminance(Dir);
	if (PassCloudWeight > 0.0f)
	{
		const float4 Cloud = CloudCube.SampleLevel(AtmoLinearSampler, Dir, 0.0f);
		Color              = Color * lerp(1.0f, Cloud.a, PassCloudWeight) + Cloud.rgb * PassCloudWeight;
	}
	CubeOutput[Id] = float4(Color, 1.0f);
}

// ---- 하늘 패스 (메인 패스 안, 하늘 상자 대신)
struct FSkyVSOutput
{
	float4 Position : SV_Position;
	float2 Ndc      : TEXCOORD0;
};

FSkyVSOutput VSSky(uint Id : SV_VertexID)
{
	FSkyVSOutput Out;
	Out.Ndc      = float2((Id << 1) & 2, Id & 2) * 2.0f - 1.0f;
	Out.Position = float4(Out.Ndc, 1.0f, 1.0f);
	return Out;
}

float StarHash(float3 P)
{
	P = frac(P * 0.3183099f + 0.1f);
	P *= 17.0f;
	return frac(P.x * P.y * P.z * (P.x + P.y + P.z));
}

// 별: 방향을 큐브 격자 칸으로 나눠 칸마다 하나 (월드 고정 — 카메라를 돌려도 하늘에 붙어 있다), 칸 안 거리로 부드럽게
float3 EvaluateStars(float3 Dir)
{
	const float3 A    = abs(Dir);
	const float  Max  = max(A.x, max(A.y, A.z));
	const float3 Cube = Dir / Max * 256.0f;
	const float3 Cell = floor(Cube);
	const float  H    = StarHash(Cell);
	if (H < 0.985f)
	{
		return 0.0f;
	}
	const float3 Center = Cell + 0.5f + (float3(StarHash(Cell + 11.0f), StarHash(Cell + 23.0f), StarHash(Cell + 37.0f)) - 0.5f) * 0.6f;
	const float  Dist   = length(Cube - Center);
	const float  Shape  = saturate(1.0f - Dist * 2.2f);
	const float  Tint   = StarHash(Cell + 5.0f);
	return lerp(float3(0.8f, 0.85f, 1.0f), float3(1.0f, 0.85f, 0.7f), Tint) * (Shape * Shape) * ((H - 0.985f) / 0.015f) * 0.3f;
}

float4 PSSky(FSkyVSOutput In) : SV_Target0
{
	float3 Dir = AtmoSkyCameraForward;
	if (AtmoOrthographic == 0)
	{
		Dir = normalize(AtmoSkyCameraForward + AtmoSkyCameraRight * In.Ndc.x * AtmoSkyTanHalfFov * AtmoSkyAspect + AtmoSkyCameraUp * In.Ndc.y * AtmoSkyTanHalfFov);
	}
	float3 Color = AtmoSkyLuminance(Dir);

	const float  ViewHeight    = length(AtmoCameraPosition);
	const float3 Up            = AtmoCameraPosition / ViewHeight;
	const bool   bGround       = AtmoRaySphereNearest(AtmoCameraPosition, Dir, AtmoBottomRadius) >= 0.0f;
	if (!bGround)
	{
		// 태양 원반: 대기 투과율을 곱한 원반 휘도 + 가장자리 어둡기, 원반 경계는 각도로 부드럽게
		const float CosAngle = dot(Dir, AtmoSunDirection);
		if (CosAngle > AtmoSunDiskCosHalfAngle - 2.0e-5f && AtmoSunDiskLuminance > 0.0f)
		{
			const float3 Transmittance = AtmoGetTransmittance(ViewHeight, dot(Dir, Up));
			const float  HalfAngle     = acos(clamp(AtmoSunDiskCosHalfAngle, -1.0f, 1.0f));
			const float  Angle         = acos(clamp(CosAngle, -1.0f, 1.0f));
			const float  Edge          = saturate((HalfAngle - Angle) / max(HalfAngle * 0.15f, 1.0e-5f));
			const float  Mu            = sqrt(saturate(1.0f - (Angle * Angle) / max(HalfAngle * HalfAngle, 1.0e-10f)));
			const float  Limb          = 0.4f + 0.6f * Mu; // 가장자리 어두워짐 (간이)
			Color += AtmoSunIlluminance * Transmittance * (AtmoSunDiskLuminance * Limb * Edge);
		}
		// 달 원반 (밤에만 — 달빛 조도가 0이 아닐 때, 태양 반대편)
		const float MoonCos = dot(Dir, AtmoMoonDirection);
		if (MoonCos > AtmoSunDiskCosHalfAngle - 2.0e-5f && AtmoMoonDiskLuminance > 0.0f)
		{
			const float3 Transmittance = AtmoGetTransmittance(ViewHeight, dot(Dir, Up));
			const float  HalfAngle     = acos(clamp(AtmoSunDiskCosHalfAngle, -1.0f, 1.0f));
			const float  Angle         = acos(clamp(MoonCos, -1.0f, 1.0f));
			const float  Edge          = saturate((HalfAngle - Angle) / max(HalfAngle * 0.15f, 1.0e-5f));
			Color += AtmoMoonIlluminance * Transmittance * (AtmoMoonDiskLuminance * Edge);
		}
		// 별: 태양이 지평선 아래로 내려갈수록 (박명 뒤)
		const float Night = saturate((-dot(AtmoSunDirection, Up) - 0.02f) / 0.15f);
		if (Night > 0.0f && AtmoStarIntensity > 0.0f)
		{
			const float3 Transmittance = AtmoGetTransmittance(ViewHeight, dot(Dir, Up));
			Color += EvaluateStars(Dir) * Transmittance * (AtmoStarIntensity * Night);
		}
	}
	return float4(Color, 0.0f); // 알파 0 = TAA 반응형 마스크 없음 (하늘 상자와 같음)
}
