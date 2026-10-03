#include "RayTracingCommon.hlsli"
#include "RayTracingView.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// RT 방향광 그림자 (Phase 50, FRayTracingEffects). 사전 패스 깊이·법선 → 그림자 마스크 (메인 패스 t24, 불투명 표면만 — 반투명/안개는 섀도맵)
//   PSTrace   픽셀마다 광선 하나: 태양 원반 안 방향 = 교차 표본(Bayer 4x4 + 프레임 회전, RayTracingMath::GetInterleavedSampleIndex)의
//             Vogel 원판 표본 → (가시도, 차폐물 거리, 뷰 깊이 m). 어느 4x4 창에도 16개 표본이 모두 있어 공간 필터 결과가 프레임마다 같다 (결정적)
//   PSFilter  공간 필터: 반그림자 반경(평균 차폐물 거리 × tan(태양 반각) → 픽셀, 최소 = 패턴 크기) 5x5 격자, 깊이·법선 가중
//   PSResolve 시간 누적: 표면 움직임으로 이력 재투영 + 이번 필터 결과 3x3 최소/최대로 이력 자름 (TAA 지터 계단 평균, 고스팅 방지)
// 입력: t5 깊이, t6 법선, t9 움직임 벡터, t10 이번 단계 입력(추적/필터 결과), t11 지난 누적

Texture2D<float>  SceneDepth    : register(t5);
Texture2D<float4> SceneNormal   : register(t6);
Texture2D<float2> SceneVelocity : register(t9);
Texture2D<float4> PassInput     : register(t10);
Texture2D<float>  ShadowHistory : register(t11);

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

cbuffer RayTracingLighting : register(b1) // RayTracingLighting.hlsli와 같은 레이아웃 (그림자는 방향만 쓴다)
{
	float3 RtLightDirection;
	float  RtLightEnabled;
	float3 RtLightRadiance;
	float  RtAmbientIntensity;
	uint   RtLocalLightCount;
	uint   RtCaptureCount;
	uint   RtMaxHitLocalLights;
	uint   RtHitShadows;
	float  RtSunTanHalfAngleLighting;
	float3 RtLightingPadding;
};

float4 PSTrace(FFullscreenVSOutput Input) : SV_Target
{
	const int2  Pixel = int2(Input.Position.xy);
	const float Depth = SceneDepth.Load(int3(Pixel, 0));
	if (Depth >= 1.0f)
	{
		return float4(1.0f, 0.0f, 0.0f, 0.0f); // 하늘: 빛 받음 (메인 패스는 읽지 않음)
	}
	const float2 UV        = (float2(Pixel) + 0.5f) / RtScreenSize;
	const float3 P         = ReconstructWorldPosition(UV, Depth);
	const float  ViewDepth = max(ComputeViewDepth(P), 1.0e-3f);
	const float3 N         = DecodeScreenNormal(SceneNormal.Load(int3(Pixel, 0)));
	const float3 L         = -RtLightDirection;
	const float  NdotL     = dot(N, L);
	if (NdotL <= 0.0f)
	{
		return float4(0.0f, 0.0f, ViewDepth * 0.01f, 0.0f); // 빛 반대편 (래스터 조명도 N·L = 0)
	}
	const uint   Sample    = GetInterleavedSampleIndex(uint2(Pixel), RtFrameIndex);
	const float3 Direction = SampleConeDirection(L, RtSunTanHalfAngle, GetDiskSample(Sample, E_RT_SAMPLE_COUNT));

	RayDesc Ray;
	Ray.Origin    = P + N * ComputeSurfaceBias(ViewDepth, NdotL, RtNormalBias);
	Ray.Direction = Direction;
	Ray.TMin      = 0.0f;
	Ray.TMax      = RtMaxDistance;
	const float Blocker = TraceOcclusion(Ray, E_RT_MASK_SHADOW_CASTER);
	return float4(Blocker >= 0.0f ? 0.0f : 1.0f, min(max(Blocker, 0.0f), 60000.0f), ViewDepth * 0.01f, 0.0f);
}

float FilterDepthWeight(float TapDepth, float CenterDepth)
{
	return exp(-abs(TapDepth - CenterDepth) / max(CenterDepth * 0.02f, 0.01f)); // 뷰 깊이 m, 상대 2%
}

float PSFilter(FFullscreenVSOutput Input) : SV_Target
{
	const int2   Pixel  = int2(Input.Position.xy);
	const int2   MaxPix = int2(RtScreenSize) - 1;
	const float4 Center = PassInput.Load(int3(Pixel, 0));
	if (SceneDepth.Load(int3(Pixel, 0)) >= 1.0f)
	{
		return 1.0f;
	}
	const float3 CenterNormal = DecodeScreenNormal(SceneNormal.Load(int3(Pixel, 0)));

	// 반그림자 반경: 5x5 안 가린 표본의 평균 차폐물 거리 (RayTracingMath::ComputePenumbraRadiusPixels)
	float BlockerSum   = 0.0f;
	float BlockerCount = 0.0f;
	[unroll] for (int Y = -2; Y <= 2; ++Y)
	{
		[unroll] for (int X = -2; X <= 2; ++X)
		{
			const float4 Tap = PassInput.Load(int3(clamp(Pixel + int2(X, Y), 0, MaxPix), 0));
			if (Tap.x < 0.5f && Tap.y > 0.0f)
			{
				BlockerSum += Tap.y;
				BlockerCount += 1.0f;
			}
		}
	}
	const float ViewDepthCm   = Center.z * 100.0f;
	const float PixelsPerUnit = RtOrthographic != 0 ? RtProjectionScale : RtProjectionScale / max(ViewDepthCm, 1.0e-3f);
	const float Penumbra      = BlockerCount > 0.0f ? (BlockerSum / BlockerCount) * RtSunTanHalfAngle * PixelsPerUnit : 0.0f;
	const float Radius        = clamp(Penumbra, RtMinFilterRadius, RtMaxFilterRadius);
	const float Step          = Radius / 2.0f;

	float Sum       = 0.0f;
	float WeightSum = 0.0f;
	[unroll] for (int Y2 = -2; Y2 <= 2; ++Y2)
	{
		[unroll] for (int X2 = -2; X2 <= 2; ++X2)
		{
			const int2   Tap      = clamp(Pixel + int2(round(float2(X2, Y2) * Step)), 0, MaxPix);
			const float4 Value    = PassInput.Load(int3(Tap, 0));
			const float3 Normal   = DecodeScreenNormal(SceneNormal.Load(int3(Tap, 0)));
			const float  Gaussian = exp(-0.5f * (float)(X2 * X2 + Y2 * Y2) / 4.0f);
			const float  W        = Gaussian * FilterDepthWeight(Value.z, Center.z) * pow(saturate(dot(Normal, CenterNormal)), 8.0f) + 1.0e-6f;
			Sum += Value.x * W;
			WeightSum += W;
		}
	}
	return Sum / WeightSum;
}

float PSResolve(FFullscreenVSOutput Input) : SV_Target
{
	const int2  Pixel   = int2(Input.Position.xy);
	const int2  MaxPix  = int2(RtScreenSize) - 1;
	const float Current = PassInput.Load(int3(Pixel, 0)).x;
	if (RtHistoryValid == 0 || SceneDepth.Load(int3(Pixel, 0)) >= 1.0f)
	{
		return Current;
	}
	float MinValue = Current;
	float MaxValue = Current;
	[unroll] for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll] for (int X = -1; X <= 1; ++X)
		{
			const float Value = PassInput.Load(int3(clamp(Pixel + int2(X, Y), 0, MaxPix), 0)).x;
			MinValue          = min(MinValue, Value);
			MaxValue          = max(MaxValue, Value);
		}
	}
	const float2 PrevUV = Input.UV - SceneVelocity.Load(int3(Pixel, 0));
	if (any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
	{
		return Current;
	}
	const float History = clamp(ShadowHistory.SampleLevel(RtClampSampler, PrevUV, 0.0f), MinValue, MaxValue);
	return lerp(History, Current, RtHistoryWeight);
}