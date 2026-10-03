#ifndef E_RAY_TRACING_LIGHTING_HLSLI
#define E_RAY_TRACING_LIGHTING_HLSLI

#include "RayTracingCommon.hlsli"
#include "PBR.hlsli"
#define E_CLUSTER_CONSTANTS_REGISTER b7 // FLocalLight 구조/감쇠 식만 쓴다 (클러스터 상수는 읽지 않음 — 바인딩 안 함)
#include "Lighting.hlsli"

// 레이 트레이싱 히트 조명 (Phase 50 — 반사 히트, Phase 51 DDGI 프로브 광선이 재사용).
//   방향광 + RT 그림자 광선(태양 중심 방향 하나, 결정적), 로컬 라이트(클러스터 없이 목록 앞 MaxHitLocalLights개 — 카메라 가까운 순, 그림자 없음,
//   면광원은 가운데 대표점 + 면 코사인 근사, IES/쿠키 포함 — Phase 52),
//   IBL 확산(하늘 조도) + 반사(캡처 → 하늘 프리필터, Mesh.hlsl SampleSpecularEnvironment와 같은 식 — SSR 없음), 발광.
// 바인딩: b1 조명 상수, t3 로컬 라이트 목록, t4 반사 캡처 목록, t13~t15 IBL(확산/반사/BRDF), t16 캡처 큐브 배열

cbuffer RayTracingLighting : register(b1)
{
	float3 RtLightDirection;   // 빛 진행 방향 (정규화)
	float  RtLightEnabled;     // 0 = 방향광 없음
	float3 RtLightRadiance;    // 색 × 강도
	float  RtAmbientIntensity; // 하늘 IBL 배율 (Mesh.hlsl AmbientIntensity)
	uint   RtLocalLightCount;
	uint   RtCaptureCount;
	uint   RtMaxHitLocalLights;
	uint   RtHitShadows;       // 1 = 히트 방향광에 그림자 광선
	float  RtHitSunTanHalfAngle; // 예약 (히트 그림자는 태양 중심 한 방향)
	float3 RtLightingPadding;
};

struct FReflectionCaptureGpu // Mesh.hlsl과 같은 구조 (ShaderTypes.h FReflectionCaptureGpuData)
{
	float3 Position;
	uint   Shape;
	float3 BoxExtent;
	float  Radius;
	float  FadeDistance;
	float  Intensity;
	uint   Slot;
	float  Padding;
};

StructuredBuffer<FLocalLight>           RtLocalLights        : register(t3);
StructuredBuffer<FReflectionCaptureGpu> RtCaptures           : register(t4);
TextureCube<float4>                     RtIblDiffuse         : register(t13);
TextureCube<float4>                     RtIblSpecular        : register(t14);
Texture2D<float2>                       RtIblBrdf            : register(t15);
TextureCubeArray<float4>                RtCaptureAtlas       : register(t16);

float RtCaptureInfluence(FReflectionCaptureGpu Capture, float3 P)
{
	if (Capture.Shape == 0)
	{
		return saturate((Capture.Radius - distance(P, Capture.Position)) / Capture.FadeDistance);
	}
	const float3 D = Capture.BoxExtent - abs(P - Capture.Position);
	return saturate(min(D.x, min(D.y, D.z)) / Capture.FadeDistance);
}

float3 RtParallaxCorrect(FReflectionCaptureGpu Capture, float3 P, float3 R)
{
	const float3 BoxMin = Capture.Position - Capture.BoxExtent;
	const float3 BoxMax = Capture.Position + Capture.BoxExtent;
	const float3 Planes = select(R > 0.0f, BoxMax, BoxMin);
	const float3 T      = select(abs(R) > 1.0e-6f, (Planes - P) / R, 1.0e30f);
	const float  TExit  = max(min(T.x, min(T.y, T.z)), 0.0f);
	return normalize(P + R * TExit - Capture.Position);
}

float GetSkyMipCount()
{
	uint Width, Height, MipCount;
	RtIblSpecular.GetDimensions(0, Width, Height, MipCount);
	return (float)MipCount;
}

// 하늘 (빗나간 광선): 반사 방향 프리필터 하늘 × 하늘 배율 — Mesh.hlsl 하늘 반사와 같은 밉 대응 (거칠기 × (밉 수 - 1))
float3 SampleSkyRadiance(float3 Direction, float Roughness)
{
	return RtIblSpecular.SampleLevel(RtClampSampler, Direction, Roughness * (GetSkyMipCount() - 1.0f)).rgb * RtAmbientIntensity;
}

// 반사 환경 (캡처 → 하늘): Mesh.hlsl SampleSpecularEnvironment에서 SSR을 뺀 것
float3 SampleHitSpecularEnvironment(float3 R, float Roughness, float3 WorldPosition)
{
	const float Lod       = Roughness * (GetSkyMipCount() - 1.0f);
	float3      Color     = 0.0f;
	float       Remaining = 1.0f;
	for (uint Index = 0; Index < RtCaptureCount && Remaining > 0.01f; ++Index)
	{
		const FReflectionCaptureGpu Capture = RtCaptures[Index];
		const float                 Weight  = RtCaptureInfluence(Capture, WorldPosition);
		if (Weight <= 0.0f)
		{
			continue;
		}
		const float3 Dir = Capture.Shape == 1 ? RtParallaxCorrect(Capture, WorldPosition, R) : R;
		Color += RtCaptureAtlas.SampleLevel(RtClampSampler, float4(Dir, (float)Capture.Slot), Lod).rgb * (Capture.Intensity * Weight * Remaining);
		Remaining *= 1.0f - Weight;
	}
	return Color + RtIblSpecular.SampleLevel(RtClampSampler, R, Lod).rgb * (RtAmbientIntensity * Remaining);
}

// IES × 쿠키 (AreaLight.hlsli EvaluateLightProfile과 같은 식 — 바인드리스 BindlessTextures)
float3 RtLightProfile(FLocalLight Light, float3 Local)
{
	float3 Result = 1.0f;
	if (Light.IesTexture >= 0)
	{
		Result *= BindlessTextures[NonUniformResourceIndex(Light.IesTexture)].SampleLevel(RtClampSampler, ComputeIesUV(Local), 0).r;
	}
	if (Light.CookieTexture >= 0)
	{
		bool         bValid;
		const float2 UV = ComputeCookieUV(Light, Local, bValid);
		Result *= bValid ? BindlessTextures[NonUniformResourceIndex(Light.CookieTexture)].SampleLevel(RtWrapSampler, UV, 0).rgb : 0.0f;
	}
	return Result;
}

// 히트 표면 조명. View = 표면 → 광선 원점. 방향광 그림자 광선은 히트 위치에서 (기하 법선 오프셋)
float3 EvaluateHitLighting(FHitSurface Hit, float3 View)
{
	FSurface Surface;
	Surface.Albedo    = Hit.Albedo;
	Surface.Metallic  = Hit.Metallic;
	Surface.Roughness = Hit.Roughness;
	Surface.N         = Hit.Normal;
	Surface.V         = View;
	Surface.Occlusion = Hit.Occlusion;

	float3 Color = Hit.Emissive;
	// 방향광
	if (RtLightEnabled > 0.0f)
	{
		const float3 L      = -RtLightDirection;
		const float3 Direct = EvaluateDirectLight(Surface, L, RtLightRadiance);
		if (any(Direct > 0.0f))
		{
			float Visibility = 1.0f;
			if (RtHitShadows != 0 && dot(Hit.GeometricNormal, L) > 0.0f)
			{
				RayDesc Ray;
				Ray.Origin    = OffsetRayOrigin(Hit.Position, Hit.GeometricNormal);
				Ray.Direction = L;
				Ray.TMin      = 0.0f;
				Ray.TMax      = 1.0e6f;
				Visibility    = TraceOcclusion(Ray, E_RT_MASK_SHADOW_CASTER) >= 0.0f ? 0.0f : 1.0f;
			}
			else if (RtHitShadows != 0)
			{
				Visibility = 0.0f; // 기하적으로 빛 반대편
			}
			Color += Direct * Visibility;
		}
	}
	// 로컬 라이트 (목록은 카메라 가까운 순 — 앞 MaxHitLocalLights개만, 그림자 없음)
	const uint LightCount = min(RtLocalLightCount, RtMaxHitLocalLights);
	for (uint Index = 0; Index < LightCount; ++Index)
	{
		const FLocalLight Light    = RtLocalLights[Index];
		if (IsAreaLight(Light))
		{
			// 면광원 (Phase 52): 가운데 대표점 + 면 코사인 근사 (LTC 없음 — 비용 상한), IES/쿠키는 바인드리스로 같은 식
			float3      AreaL;
			const float AreaAtten = AreaLightApproxAttenuation(Light, Hit.Position, AreaL);
			if (AreaAtten > 0.0f)
			{
				Color += EvaluateDirectLight(Surface, AreaL, Light.Color * AreaAtten * RtLightProfile(Light, ToLightLocal(Light, -AreaL)));
			}
			continue;
		}
		const float3      ToLight  = Light.Position - Hit.Position;
		const float       Distance = length(ToLight);
		if (Distance >= Light.Radius)
		{
			continue;
		}
		const float3 L           = ToLight / max(Distance, 1.0e-4f);
		const float  Attenuation = LightDistanceAttenuation(Distance, Light.Radius) * LightConeAttenuation(dot(Light.Direction, -L), Light.ConeScale, Light.ConeOffset);
		if (Attenuation > 0.0f)
		{
			float3 Radiance = Light.Color * Attenuation;
			if (Light.IesTexture >= 0 || Light.CookieTexture >= 0)
			{
				Radiance *= RtLightProfile(Light, ToLightLocal(Light, -L));
			}
			Color += EvaluateDirectLight(Surface, L, Radiance);
		}
	}
	// IBL (Mesh.hlsl EvaluateImageBasedLightingEx와 같은 식, SSR 없음)
	const float  NdotV       = max(saturate(dot(Surface.N, Surface.V)), 1.0e-4f);
	const float3 F0          = GetF0(Surface);
	const float3 F           = F0 + (max(1.0f - Surface.Roughness, F0) - F0) * pow(1.0f - NdotV, 5.0f);
	const float3 Diffuse     = RtIblDiffuse.SampleLevel(RtClampSampler, Surface.N, 0).rgb * Surface.Albedo;
	const float3 R           = reflect(-Surface.V, Surface.N);
	const float3 Prefiltered = SampleHitSpecularEnvironment(R, Surface.Roughness, Hit.Position);
	const float2 Brdf        = RtIblBrdf.SampleLevel(RtClampSampler, float2(NdotV, Surface.Roughness), 0);
	Color += ((1.0f - F) * (1.0f - Surface.Metallic) * Diffuse * RtAmbientIntensity + Prefiltered * (F0 * Brdf.x + Brdf.y)) * Surface.Occlusion;
	return Color;
}

#endif // E_RAY_TRACING_LIGHTING_HLSLI