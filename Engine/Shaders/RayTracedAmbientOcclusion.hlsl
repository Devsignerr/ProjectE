#include "RayTracingCommon.hlsli"
#include "RayTracingView.hlsli"
#include "Fullscreen.hlsli"
#include "ScreenSpace.hlsli"

// RT 앰비언트 오클루전 (RTAO, FRayTracingEffects::AddAmbientOcclusionPasses) — 근거리 간접 가림. 식은 Renderer/RayTracingMath.h "RTAO"와 같다.
//   결과 형식은 SSAO(AmbientOcclusion.hlsl)와 같다: R = 가시도(1 = 가림 없음), G = 뷰 깊이(cm) → 메인 패스 t16이 간접광(DDGI/IBL)에만 곱한다
//   PSTrace     픽셀마다 광선 RtAoRayCount개: 코사인 가중 반구 방향 = 교차 표본(Bayer 4x4 + 프레임 회전)의 Vogel 원판을 반구로 올림(Malley).
//               가장 가까운 히트 거리 t → 가림 w = (1 - t/반경)^RtAoFalloffPower (반경 밖·빗나감 0). 어느 4x4 창에도 표본 16 × 광선 수 방향이 모두 있다
//               가시도 = 1 - 평균 w. (조도 가중 — 가려진 방향의 DDGI 조도를 히트 표면 빛으로 바꾸는 방식 — 은 히트의 DDGI 조도도 접촉부에서
//               가려지지 않은 값이라 거의 어두워지지 않아 경로 추적 기준과 더 멀었다 → 쓰지 않음, 2026-10-04 Demo_GI)
//   PSFilter    5x5 텐트(가장자리 0.5) — 4 주기 패턴의 각 칸이 같은 가중이라 평평한 곳은 프레임마다 같은 값(결정적) + 깊이·법선 가중
//   PSResolve   시간 누적 (RT 그림자와 같은 규칙: 표면 움직임 재투영 + 3x3 최소/최대 자름) — G에 뷰 깊이(cm)
//   PSReference 고비용 기준 (r.RayTracing.AO.Reference N): 픽셀마다 N개 코사인 광선으로 두 번 반사까지 경로 추적한 간접 확산(그림자 광선 포함,
//               3번째 반사부터 DDGI 조도, 빗나감 = 하늘)을 프레임마다 평균에 더하고, "모은 값 / 이 픽셀 DDGI(+하늘) 값" 밝기 비를 가시도로 낸다
//               → 메인 패스가 DDGI × 비 = 직접 모은 간접광을 그린다 (비교·튜닝용, 정지 카메라 전용)
// 입력: t5 깊이, t6 법선, t7 이번 단계 입력, t8 지난 누적, t9 움직임 벡터, (기준) t10~t12 DDGI 아틀라스 + b2 DDGI 상수, t7 지난 기준 누적

Texture2D<float>  SceneDepth    : register(t5);
Texture2D<float4> SceneNormal   : register(t6);
#ifndef E_RTAO_REFERENCE
Texture2D<float4> PassInput     : register(t7);
Texture2D<float2> AoHistory     : register(t8);
Texture2D<float2> SceneVelocity : register(t9);
#endif

// DDGI (메인 패스와 같은 상수·아틀라스 — 이번 프레임 값). 볼륨이 없으면 VolumeCount 0 → 하늘 조도만
#define E_DDGI_CONSTANTS_REGISTER b2
#define E_DDGI_IRRADIANCE_REGISTER t10
#define E_DDGI_DISTANCE_REGISTER t11
#define E_DDGI_PROBE_DATA_REGISTER t12
#define E_DDGI_SAMPLER RtClampSampler
#include "DdgiCommon.hlsli"
#include "RayTracingLighting.hlsli"

// RTAO 상수: 뷰 상수(b0)의 그림자용 칸을 다시 쓴다 (RayTracingEffects.cpp AddAmbientOcclusionPasses)
#define RtAoRadius        RtMaxDistance     // 광선 길이 = 가림 반경 (cm)
#define RtAoRayCount      RtDebugMode       // 픽셀당 광선 수 (1~4)
#define RtAoFalloffPower  RtSunTanHalfAngle // 거리 감쇠 지수
#define RtAoIntensity     RtMinFilterRadius // 가시도^세기
#define RtAoReferenceRays RtDecals          // 기준 모드: 픽셀당 광선 수 (0 = 끔)
#define RtAoDivisor       RtMaxRoughness    // 해상도 나눔 (1 = 씬 해상도, 2 = 반해상도 — AO 픽셀 i는 씬 픽셀 i × 2에서 계산, SSAO와 같은 규칙)

// AO 버퍼 픽셀 → 씬(깊이·법선) 픽셀
int2 AoToScenePixel(int2 Pixel)
{
	return min(Pixel * (int)RtAoDivisor, int2(RtScreenSize) - 1);
}
int2 GetAoSize()
{
	return (int2(RtScreenSize) + (int)RtAoDivisor - 1) / (int)RtAoDivisor;
}

// 이 점·법선의 간접 확산 조도/π (Mesh.hlsl EvaluateImageBasedLightingEx와 같은 DDGI + 하늘 남은 비중)
float3 EvaluateIndirectIrradiance(float3 P, float3 N, float3 V)
{
	float        Remaining = 1.0f;
	const float3 Probes    = DdgiVolumeCount != 0 ? EvaluateDdgiIrradiance(P, N, V, Remaining) : 0.0f;
	return Probes + RtIblDiffuse.SampleLevel(RtClampSampler, N, 0).rgb * (RtAmbientIntensity * Remaining);
}

float Luminance3(float3 Color)
{
	return dot(Color, float3(0.2126f, 0.7152f, 0.0722f));
}

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

// 법선 N 둘레 코사인 가중 반구 방향: 단위 원판 표본을 반구로 올린다 (Malley — 원판 균등 → 반구 코사인 가중). RayTracingMath::SampleCosineHemisphere
float3 SampleCosineHemisphere(float3 N, float2 Disk)
{
	const float3 Helper    = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
	const float3 Tangent   = normalize(cross(Helper, N));
	const float3 Bitangent = cross(N, Tangent);
	const float  Z         = sqrt(saturate(1.0f - dot(Disk, Disk)));
	return normalize(Tangent * Disk.x + Bitangent * Disk.y + N * Z);
}

// 히트 거리 → 가림 (RayTracingMath::ComputeAoOcclusion): 반경 안 가까울수록 1, 반경에서 0
float ComputeAoOcclusion(float HitDistance, float Radius, float FalloffPower)
{
	if (HitDistance < 0.0f || HitDistance >= Radius)
	{
		return 0.0f;
	}
	return pow(saturate(1.0f - HitDistance / max(Radius, 1.0e-3f)), FalloffPower);
}

#ifndef E_RTAO_REFERENCE
// 가장 가까운 히트 거리 (없으면 -1). 가림 판정만 — 머티리얼 평가 없음 (Masked는 알파 테스트)
float TraceAoDistance(RayDesc Ray)
{
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> Query;
	Query.TraceRayInline(SceneTlas, RAY_FLAG_NONE, E_RT_MASK_TYPES, Ray);
	E_RT_PROCESS_CANDIDATES(Query, Ray)
	return Query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? Query.CommittedRayT() : -1.0f;
}

// 출력: x = 가시도, y = 뷰 깊이 cm (하늘 0) — 필터가 탭마다 깊이 재구성(행렬 곱)을 하지 않게
float2 PSTrace(FFullscreenVSOutput Input) : SV_Target
{
	const int2  Pixel      = int2(Input.Position.xy);
	const int2  ScenePixel = AoToScenePixel(Pixel);
	const float Depth      = SceneDepth.Load(int3(ScenePixel, 0));
	if (Depth >= 1.0f)
	{
		return float2(1.0f, 0.0f);
	}
	const float2 UV        = (float2(ScenePixel) + 0.5f) / RtScreenSize;
	const float3 P         = ReconstructWorldPosition(UV, Depth);
	const float  ViewDepth = max(ComputeViewDepth(P), 1.0e-3f);
	const float3 N         = DecodeScreenNormal(SceneNormal.Load(int3(ScenePixel, 0)));
	const uint   Rays      = clamp(RtAoRayCount, 1u, 4u);
	const uint   Base      = GetInterleavedSampleIndex(uint2(Pixel), RtFrameIndex);
	const float3 Origin    = P + N * ComputeSurfaceBias(ViewDepth, 1.0f, RtNormalBias);

	float Occlusion = 0.0f;
	for (uint Index = 0; Index < Rays; ++Index)
	{
		RayDesc Ray;
		Ray.Origin    = Origin;
		Ray.Direction = SampleCosineHemisphere(N, GetDiskSample(Base + Index * E_RT_SAMPLE_COUNT, E_RT_SAMPLE_COUNT * Rays));
		Ray.TMin      = 0.0f;
		Ray.TMax      = RtAoRadius;
		Occlusion += ComputeAoOcclusion(TraceAoDistance(Ray), RtAoRadius, RtAoFalloffPower);
	}
	return float2(1.0f - Occlusion / (float)Rays, ViewDepth);
}

float AoDepthWeight(float TapDepth, float CenterDepth)
{
	return exp(-abs(TapDepth - CenterDepth) / max(CenterDepth * 0.02f, 0.5f)); // 뷰 깊이 cm, 상대 2%
}

// 5x5 텐트 (가장자리 0.5): 주기 4 교차 패턴의 칸마다 가중 합이 같다 → 평평한 면은 프레임 회전과 무관하게 같은 값 + 깊이·법선 가중.
// 추적 해상도 그대로 (반해상도면 메인 패스가 깊이 가중 4탭으로 올린다 — 필터에서 씬 해상도로 올리는 방식은 정지 화면 시간 표준편차가
// 오히려 커서(평균 0.12 → 0.17) 쓰지 않음, 2026-10-04)
float2 PSFilter(FFullscreenVSOutput Input) : SV_Target
{
	const int2   Pixel  = int2(Input.Position.xy);
	const int2   MaxPix = GetAoSize() - 1;
	const float2 Center = PassInput.Load(int3(Pixel, 0)).xy;
	if (Center.y <= 0.0f)
	{
		return float2(1.0f, 0.0f); // 하늘
	}
	const float  CenterDepth  = Center.y;
	const float3 CenterNormal = DecodeScreenNormal(SceneNormal.Load(int3(AoToScenePixel(Pixel), 0)));

	float Sum       = 0.0f;
	float WeightSum = 0.0f;
	[unroll] for (int Y = -2; Y <= 2; ++Y)
	{
		[unroll] for (int X = -2; X <= 2; ++X)
		{
			const int2   Tap       = clamp(Pixel + int2(X, Y), 0, MaxPix);
			const float2 Value     = PassInput.Load(int3(Tap, 0)).xy;
			const float3 TapNormal = DecodeScreenNormal(SceneNormal.Load(int3(AoToScenePixel(Tap), 0)));
			const float  Tent      = (abs(X) == 2 ? 0.5f : 1.0f) * (abs(Y) == 2 ? 0.5f : 1.0f);
			// 하늘 탭(깊이 0)은 깊이 가중이 0에 가깝다
			const float  W         = Tent * AoDepthWeight(Value.y, CenterDepth) * pow(saturate(dot(TapNormal, CenterNormal)), 8.0f);
			Sum += Value.x * W;
			WeightSum += W;
		}
	}
	const float Visibility = WeightSum > 1.0e-6f ? Sum / WeightSum : Center.x;
	return float2(pow(saturate(Visibility), max(RtAoIntensity, 0.0f)), CenterDepth);
}

float2 PSResolve(FFullscreenVSOutput Input) : SV_Target
{
	const int2   Pixel   = int2(Input.Position.xy);
	const int2   MaxPix  = GetAoSize() - 1;
	const float2 Current = PassInput.Load(int3(Pixel, 0)).xy;
	if (RtHistoryValid == 0 || Current.y <= 0.0f)
	{
		return Current;
	}
	float MinValue = Current.x;
	float MaxValue = Current.x;
	[unroll] for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll] for (int X = -1; X <= 1; ++X)
		{
			const float Value = PassInput.Load(int3(clamp(Pixel + int2(X, Y), 0, MaxPix), 0)).x;
			MinValue          = min(MinValue, Value);
			MaxValue          = max(MaxValue, Value);
		}
	}
	const float2 PrevUV = Input.UV - SceneVelocity.Load(int3(AoToScenePixel(Pixel), 0));
	if (any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
	{
		return Current;
	}
	const float History = clamp(AoHistory.SampleLevel(RtClampSampler, PrevUV, 0.0f).x, MinValue, MaxValue);
	return float2(lerp(History, Current.x, RtHistoryWeight), Current.y);
}

#else
// ---- 고비용 기준 (r.RayTracing.AO.Reference): 경로 추적 (반사 E_RTAO_REFERENCE_BOUNCES번) ÷ DDGI
//   1차 표면에서 코사인 광선 → 히트마다 직접광(방향광·로컬 라이트 모두 그림자 광선) + 발광, 다음 코사인 광선으로 이어 가다 마지막 정점에서만
//   DDGI 조도 × 알베도 (프로브 캐시는 E_RTAO_REFERENCE_BOUNCES번째 반사부터). 빗나감 = 하늘(프리필터 밉 0).
//   첫 히트에서 바로 DDGI를 쓰면 접촉부 히트(구 아랫면 등)가 가려지지 않은 프로브 조도로 밝게 빛나 기준이 접촉 가림을 과소평가했다
//   (2026-10-04 Demo_GI — 흰 물체끼리는 반사가 커서 몇 번 더 따라가야 한다). 히트 표면은 확산 + 직접 반사만 (간접 반사 생략)
#ifndef E_RTAO_REFERENCE_BOUNCES
#define E_RTAO_REFERENCE_BOUNCES 4
#endif
Texture2D<float4> ReferenceHistory : register(t7); // rgb = 지금까지 모은 간접 확산 조도/π 평균, a = 모은 프레임 수

// PCG 해시 (기준은 수렴만 보면 되므로 잡음 허용)
uint ReferenceHash(uint Value)
{
	uint H = Value * 747796405u + 2891336453u;
	H      = ((H >> ((H >> 28u) + 4u)) ^ H) * 277803737u;
	return (H >> 22u) ^ H;
}
float2 ReferenceRandom2(uint Seed)
{
	const uint A = ReferenceHash(Seed);
	const uint B = ReferenceHash(A ^ 0x9E3779B9u);
	return float2((float)(A >> 8u), (float)(B >> 8u)) / 16777216.0f;
}
float3 ReferenceCosineDirection(float3 N, float2 U)
{
	const float Radius = sqrt(U.x);
	const float Angle  = U.y * 6.28318531f;
	return SampleCosineHemisphere(N, Radius * float2(cos(Angle), sin(Angle)));
}

bool ReferenceVisible(float3 Origin, float3 Direction, float Distance)
{
	RayDesc Ray;
	Ray.Origin    = Origin;
	Ray.Direction = Direction;
	Ray.TMin      = 0.0f;
	Ray.TMax      = Distance;
	return TraceOcclusion(Ray, E_RT_MASK_SHADOW_CASTER) < 0.0f;
}

// 히트 표면 직접광 (그림자 광선 포함) + 발광 — EvaluateHitLighting의 직접광 부분과 같은 식, 로컬 라이트도 그림자
float3 ReferenceDirect(FHitSurface Hit, float3 View)
{
	FSurface Surface;
	Surface.Albedo    = Hit.Albedo;
	Surface.Metallic  = Hit.Metallic;
	Surface.Roughness = Hit.Roughness;
	Surface.N         = Hit.Normal;
	Surface.V         = View;
	Surface.Occlusion = Hit.Occlusion;
	const float3 Origin = OffsetRayOrigin(Hit.Position, Hit.GeometricNormal);
	float3       Color  = Hit.Emissive;
	if (RtLightEnabled > 0.0f)
	{
		const float3 L = -RtLightDirection;
		if (dot(Hit.GeometricNormal, L) > 0.0f && ReferenceVisible(Origin, L, 1.0e6f))
		{
			Color += EvaluateDirectLight(Surface, L, RtLightRadiance);
		}
	}
	const uint LightCount = min(RtLocalLightCount, 16u);
	for (uint Index = 0; Index < LightCount; ++Index)
	{
		const FLocalLight Light = RtLocalLights[Index];
		float3            L;
		float3            Radiance;
		float             Distance;
		if (IsAreaLight(Light))
		{
			const float Atten = AreaLightApproxAttenuation(Light, Hit.Position, L);
			Distance          = length(Light.Position - Hit.Position);
			Radiance          = Light.Color * Atten * RtLightProfile(Light, ToLightLocal(Light, -L));
		}
		else
		{
			const float3 ToLight = Light.Position - Hit.Position;
			Distance             = length(ToLight);
			if (Distance >= Light.Radius)
			{
				continue;
			}
			L        = ToLight / max(Distance, 1.0e-4f);
			Radiance = Light.Color * LightDistanceAttenuation(Distance, Light.Radius) *
			           LightConeAttenuation(dot(Light.Direction, -L), Light.ConeScale, Light.ConeOffset);
			if (Light.IesTexture >= 0 || Light.CookieTexture >= 0)
			{
				Radiance *= RtLightProfile(Light, ToLightLocal(Light, -L));
			}
		}
		if (all(Radiance <= 0.0f) || dot(Hit.GeometricNormal, L) <= 0.0f)
		{
			continue;
		}
		if (ReferenceVisible(Origin, L, max(Distance - 1.0f, 0.0f)))
		{
			Color += EvaluateDirectLight(Surface, L, Radiance);
		}
	}
	return Color;
}

// 경로 하나의 휘도 (Origin에서 Direction으로): 정점마다 직접광, 마지막 정점은 DDGI 조도 × 알베도
float3 ReferencePath(float3 Origin, float3 Direction, uint Seed)
{
	float3 Radiance   = 0.0f;
	float3 Throughput = 1.0f;
	[loop]
	for (uint Bounce = 0; Bounce < E_RTAO_REFERENCE_BOUNCES; ++Bounce)
	{
		RayDesc Ray;
		Ray.Origin    = Origin;
		Ray.Direction = Direction;
		Ray.TMin      = 0.0f;
		Ray.TMax      = RtAoRadius;
		const FRayHit Hit = TraceClosestHit(Ray, E_RT_MASK_TYPES);
		if (!Hit.bHit)
		{
			Radiance += Throughput * SampleSkyRadiance(Direction, 0.0f);
			break;
		}
		if (!Hit.bFrontFace && (RtInstances[Hit.Instance].Flags & E_RT_INFO_TWO_SIDED) == 0)
		{
			break; // 뒷면 (닫힌 물체 안) = 빛 없음
		}
		const FHitSurface Surface = LoadHitSurface(Hit, Hit.T * 0.1f, -Direction);
		Radiance += Throughput * ReferenceDirect(Surface, -Direction);
		const float3 Diffuse = Surface.Albedo * (1.0f - Surface.Metallic);
		if (Bounce + 1 == E_RTAO_REFERENCE_BOUNCES)
		{
			Radiance += Throughput * EvaluateIndirectIrradiance(Surface.Position, Surface.Normal, -Direction) * Diffuse;
			break;
		}
		// 다음 정점: 코사인 표본이라 휘도 × 알베도 (π와 pdf가 약분)
		Throughput *= Diffuse;
		Origin    = OffsetRayOrigin(Surface.Position, Surface.GeometricNormal);
		Direction = ReferenceCosineDirection(Surface.GeometricNormal, ReferenceRandom2(Seed ^ (0x68E31DA4u * (Bounce + 1u))));
	}
	return Radiance;
}

struct FReferenceOutput
{
	float4 Accumulated : SV_Target0; // 다음 프레임 기준 누적
	float2 Visibility  : SV_Target1; // 메인 패스 t16 (가시도 = 모은 값 / DDGI 값, 뷰 깊이 cm)
};

FReferenceOutput PSReference(FFullscreenVSOutput Input)
{
	FReferenceOutput Output;
	const int2  Pixel = int2(Input.Position.xy);
	const float Depth = SceneDepth.Load(int3(Pixel, 0));
	if (Depth >= 1.0f)
	{
		Output.Accumulated = 0.0f;
		Output.Visibility  = float2(1.0f, 0.0f);
		return Output;
	}
	const float2 UV        = (float2(Pixel) + 0.5f) / RtScreenSize;
	const float3 P         = ReconstructWorldPosition(UV, Depth);
	const float  ViewDepth = max(ComputeViewDepth(P), 1.0e-3f);
	const float3 N         = DecodeScreenNormal(SceneNormal.Load(int3(Pixel, 0)));
	const float3 V         = -ComputeViewDirection(P);
	const float3 Origin    = P + N * ComputeSurfaceBias(ViewDepth, 1.0f, RtNormalBias);
	const float4 Previous  = RtHistoryValid != 0 ? ReferenceHistory.Load(int3(Pixel, 0)) : 0.0f;
	const uint   Rays      = max(RtAoReferenceRays, 1u);
	const uint   PixelSeed = ReferenceHash((uint)Pixel.x * 73856093u ^ (uint)Pixel.y * 19349663u);

	float3 Sum = 0.0f;
	for (uint Index = 0; Index < Rays; ++Index)
	{
		const uint   Seed      = ReferenceHash(PixelSeed ^ ReferenceHash((uint)Previous.a * Rays + Index));
		const float3 Direction = ReferenceCosineDirection(N, ReferenceRandom2(Seed));
		Sum += ReferencePath(Origin, Direction, Seed);
	}
	const float  Frames   = Previous.a + 1.0f;
	const float3 Gathered = Previous.rgb + (Sum / (float)Rays - Previous.rgb) / Frames;
	Output.Accumulated    = float4(Gathered, Frames);

	// 메인 패스와 같은 DDGI 간접 확산 (Mesh.hlsl EvaluateImageBasedLightingEx — 하늘은 하늘 조도 × 남은 비중)
	Output.Visibility = float2(Luminance3(Gathered) / max(Luminance3(EvaluateIndirectIrradiance(P, N, V)), 1.0e-5f), ViewDepth);
	return Output;
}
#endif
