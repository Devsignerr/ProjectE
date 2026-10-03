#include "RayTracingLighting.hlsli"
#include "RayTracingView.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// RT 반사 추적 (Phase 50, FRayTracingEffects — SsrTrace.hlsl를 대체하는 같은 출력). 사전 패스 깊이·법선(+데칼)으로 1차 표면을 되살려
//   거울 방향 광선 하나를 TLAS로 추적(인라인 RayQuery, 뒷면 컬링 — 래스터와 같은 면, Masked 알파 테스트) → 히트 표면 머티리얼 평가 + 조명
//   (RayTracingLighting.hlsli: 방향광 + RT 그림자 광선, 로컬 라이트, IBL/캡처). 빗나가면 하늘 프리필터(거칠기 밉).
//   출력은 SsrTrace와 같다: Color = (반사 색, 신뢰도 1), Motion = (반사 움직임 xy = 가상 점 재투영, 흐림 반경 z) → SsrResolve.hlsl PSBlur/PSResolve가
//   거칠기 원뿔 흐림(결정적) + 반사 움직임 누적을 그대로 한다 (시간 안정성 규칙: 확률 반사 없음, 거울 방향 한 번)
//   거칠기 > RtMaxRoughness는 추적하지 않고 신뢰도 0 → 메인 패스가 캡처/하늘 (Mesh.hlsl 거칠기 페이드와 같은 한계)

Texture2D<float>  SceneDepth    : register(t5);
Texture2D<float4> SceneNormal   : register(t6);
Texture2D<float4> DecalNormal   : register(t7);
Texture2D<float4> DecalMaterial : register(t8);

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

struct FReflectionOutput
{
	float4 Color  : SV_Target0; // rgb = 반사 색 (HDR), a = 신뢰도
	float4 Motion : SV_Target1; // xy = 반사 움직임 (현재 UV − 이전 UV), z = 흐림 반경 (픽셀)
};

// SsrTrace.hlsl / ReflectionMath::ComputeSpecularConeTangent와 같은 식
float ComputeConeTangent(float Roughness)
{
	const float Alpha = Roughness * Roughness;
	if (Alpha < 1.0e-3f)
	{
		return 0.0f;
	}
	const float Power    = max(2.0f / (Alpha * Alpha) - 2.0f, 0.0f);
	const float CosAngle = pow(0.244f, 1.0f / (Power + 1.0f));
	return sqrt(max(1.0f - CosAngle * CosAngle, 0.0f)) / max(CosAngle, 1.0e-4f);
}

FReflectionOutput PSTrace(FFullscreenVSOutput Input)
{
	FReflectionOutput Output;
	Output.Color  = 0.0f;
	Output.Motion = 0.0f;
	const int2  Pixel = int2(Input.Position.xy);
	const float Depth = SceneDepth.Load(int3(Pixel, 0));
	if (Depth >= 1.0f)
	{
		return Output;
	}
	const float4 NormalData     = SceneNormal.Load(int3(Pixel, 0));
	const float3 GeometryNormal = DecodeScreenNormal(NormalData);
	float        Roughness      = DecodeScreenRoughness(NormalData);
	float3       N              = GeometryNormal;
	if (RtDecals != 0)
	{
		ApplyScreenDecals(DecalNormal, DecalMaterial, Pixel, N, Roughness);
	}
	if (Roughness > RtMaxRoughness)
	{
		return Output; // 메인 패스가 어차피 0으로 페이드
	}

	const float2 UV              = (float2(Pixel) + 0.5f) / RtScreenSize;
	const float3 P               = ReconstructWorldPosition(UV, Depth);
	const float3 V               = ComputeViewDirection(P); // 카메라 → 점
	const float3 R               = reflect(V, N);
	const float  NdotR           = dot(GeometryNormal, R);
	if (NdotR <= 0.0f)
	{
		return Output; // 노멀 맵이 면 아래로 꺾은 반사 → 캡처/하늘
	}
	const float ViewDepth       = max(ComputeViewDepth(P), 1.0e-3f);
	const float SurfaceDistance = RtOrthographic != 0 ? ViewDepth : length(P - RtCameraPosition);

	RayDesc Ray;
	Ray.Origin    = P + GeometryNormal * ComputeSurfaceBias(ViewDepth, NdotR, RtNormalBias);
	Ray.Direction = R;
	Ray.TMin      = 0.0f;
	Ray.TMax      = RtMaxDistance;
	const FRayHit Hit = TraceClosestHitCullBack(Ray, E_RT_MASK_TYPES);

	const float ConeTangent = ComputeConeTangent(Roughness);
	const float PixelAngle  = 1.0f / max(RtProjectionScale, 1.0e-3f);
	float3      Color;
	float       HitDistance;
	if (Hit.bHit)
	{
		const float       ConeWidth = SurfaceDistance * PixelAngle + Hit.T * (PixelAngle + 2.0f * ConeTangent); // RayTracingMath::ComputeReflectionConeWidth
		const FHitSurface Surface   = LoadHitSurface(Hit, ConeWidth, -R);
		Color       = EvaluateHitLighting(Surface, -R);
		HitDistance = Hit.T;
	}
	else
	{
		Color       = SampleSkyRadiance(R, Roughness);
		HitDistance = RtMaxDistance; // 무한히 먼 상 (움직임·흐림 반경)
	}

	// 반사 움직임: 시선 방향으로 표면 뒤 교차 거리만큼 간 가상 점의 이전 화면 위치 (SsrTrace와 같은 뜻, 평면 거울 정확)
	const float3 Virtual = P + V * HitDistance;
	const float2 PrevUV  = ProjectToUV(Virtual, RtPrevViewProjection);
	if (all(PrevUV >= -0.5f))
	{
		Output.Motion.xy = UV - PrevUV;
	}
	// 거칠기 흐림 반경 (SsrTrace / ReflectionMath::ComputeSsrBlurRadiusPixels와 같은 식, 반사된 상의 깊이로 픽셀/거리)
	if (ConeTangent > 0.0f)
	{
		const float VirtualDepth  = ViewDepth * (1.0f + HitDistance / max(SurfaceDistance, 1.0e-3f));
		const float PixelsPerUnit = RtOrthographic != 0 ? RtProjectionScale : RtProjectionScale / max(VirtualDepth, 1.0e-3f);
		Output.Motion.z           = min(HitDistance * ConeTangent * PixelsPerUnit, RtMaxBlurRadius);
	}
	// 흐려질 픽셀(거친 면)의 HDR 점(태양 하이라이트 등)은 휘도를 눌러 둔다 — 원판 흐림(SsrResolve PSBlur) 표본 위치마다 점이 찍혀 점박이가 된다.
	// 거울(반경 0)은 그대로
	Color                    = max(Color, 0.0f);
	const float Luminance    = dot(Color, float3(0.2126f, 0.7152f, 0.0722f));
	const float MaxLuminance = lerp(1.0e4f, 16.0f, saturate(Output.Motion.z / 4.0f));
	Output.Color             = float4(Luminance > MaxLuminance ? Color * (MaxLuminance / Luminance) : Color, 1.0f);
	return Output;
}