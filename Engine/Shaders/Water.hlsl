#include "Common.hlsli"
// 안개 + 공중 원근 (FogRenderer 상수 b2 + 볼류메트릭 결과 t8, 선형 클램프 s3)
#define E_FOG_CONSTANTS_REGISTER b2
#define E_FOG_VOLUME_REGISTER t8
#define E_FOG_SAMPLER_REGISTER s3
#include "Fog.hlsli"
// 방향광 캐스케이드 그림자 상수 b3
#define E_SHADOW_CONSTANTS_REGISTER b3
#include "ShadowCommon.hlsli"
#include "PBR.hlsli"
// 클러스터 로컬 라이트 (점광원/스포트/면광원 — LocalLightRenderer, 메시 패스와 같은 식): b4 클러스터 상수, t11~t14, 공간 3 표 (LTC·IES·쿠키)
#define E_CLUSTER_CONSTANTS_REGISTER b4
#include "Lighting.hlsli"

// 소규모 물 (Phase 49, FWaterRenderer). CPU 참조 식은 Renderer/WaterMath.h (테스트 Water_*)
//   반투명 위치(안개 적용 뒤, 반투명 메시 전)의 전용 패스. 수면 = 물 상자 윗면 사각형(요 회전)
//   굴절: 씬 컬러 복사본(t0)을 노멀 오프셋으로 읽되, 오프셋 자리의 장면이 수면보다 앞(물 위 물체)이면 오프셋 없이 → 물 위 물체가 새지 않는다
//   깊이 색: 바닥까지 물속 거리로 채널별 흡수 T = exp(-흡수 × m) → 바닥 × T + 산란 색 × 조명 × (1 - T)
//   반사: 화면 공간 추적(결정적 — 지터 없음, 씬 컬러 복사본) → 반사 캡처 → 하늘 IBL 프리필터. 프레넬(슐릭, F0 0.02)로 섞는다
//   로컬 라이트: 클러스터 점광원/스포트/면광원의 반사광만 (LocalLighting.hlsli EvaluateLocalLights — 메시 패스와 같은 식·그림자, 알베도 0)
//   거품: 물 두께가 FoamDistance보다 얇은 가장자리 × 거품 노이즈(노멀 텍스처 B)
//   출력: 씬 컬러(알파 = TAA 반응형 마스크 0.2 — 잔물결이 움직인다) + 움직임 벡터(수면 기준 — 바닥 깊이로 재투영하지 않게)
//   깊이: 씬 깊이(t1)와 셰이더에서 비교 (깊이 버퍼에는 쓰지 않음)
//   PSUnderwater: 카메라가 물 상자 안이면 전체 화면 — 카메라 → 장면(또는 상자 출구)까지 물속 거리로 흡수/산란 (스칼라 투과율, 프리멀티플라이드)

cbuffer WaterFrame : register(b0)
{
	float4x4 ViewProjection;           // 지터 포함 (래스터)
	float4x4 UnjitteredViewProjection;
	float4x4 PrevViewProjection;       // 지터 없음
	float4x4 InvViewProjection;        // 지터 포함 투영의 역 (깊이 → 월드)
	float3   CameraPosition;
	float    Time;                     // 초
	float3   SunDirection;             // 태양 쪽 (빛 진행 반대)
	float    AmbientIntensity;
	float3   SunColor;                 // 색 × 강도 (대기 투과율 포함)
	uint     ReflectionCaptureCount;
	float2   ScreenSize;
	float2   InvScreenSize;
	uint     bScreenReflections;
	float    ReactiveMask;
	float2   FramePadding;
};

cbuffer WaterBody : register(b1)
{
	float3 BodyCenter;
	float  BodyCosYaw;
	float3 BodyHalfSize;
	float  BodySinYaw;
	float3 ScatterColor;
	float  NormalStrength;
	float3 Absorption;     // 1/m
	float  WaveScale;      // cm
	float2 FlowDirection;  // 월드 XY (정규화)
	float  FlowSpeed;      // cm/s
	float  WaveSpeed;      // cm/s
	float  FoamIntensity;
	float  FoamDistance;   // cm
	float  RefractionStrength;
	float  ReflectionIntensity;
	float  Roughness;
	float3 BodyPadding;
};

Texture2D<float4>      SceneColorCopy : register(t0);
Texture2D<float>       SceneDepth     : register(t1);
Texture2D<float4>      WaveNormals    : register(t2); // rg = 노멀 xy (UNORM), b = 거품 노이즈, a = 높이
TextureCube<float4>    IblDiffuse     : register(t4);
TextureCube<float4>    IblSpecular    : register(t5);
Texture2D<float2>      IblBrdf        : register(t6);
Texture2DArray<float>  ShadowMap      : register(t7);
struct FReflectionCaptureGpu
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
StructuredBuffer<FReflectionCaptureGpu> ReflectionCaptures     : register(t9);
TextureCubeArray<float4>                ReflectionCaptureAtlas : register(t10);

SamplerState           LinearClamp  : register(s0);
SamplerState           LinearWrap   : register(s1);
SamplerComparisonState ShadowSampler : register(s2);

#define E_LOCAL_LIGHTS_REGISTER t11
#define E_CLUSTER_DATA_REGISTER t12
#define E_LOCAL_SHADOW_MATRICES_REGISTER t13
#define E_LOCAL_SHADOW_MAP_REGISTER t14
#define E_LIGHT_SAMPLER_CLAMP LinearClamp
#define E_LIGHT_SAMPLER_WRAP LinearWrap
#include "LocalLighting.hlsli"

static const float WaterF0 = 0.02f; // FWaterMath::WaterF0
static const float WaterLocalLightMinRoughness = 0.08f; // 로컬 라이트 하이라이트 거칠기 하한

struct FWaterVSOutput
{
	float4 Position      : SV_Position;
	float3 WorldPosition : TEXCOORD0;
};

FWaterVSOutput VSWater(uint VertexId : SV_VertexID)
{
	// 윗면 사각형 두 삼각형 (CW 앞면 = 위에서 볼 때 — 양면이라 상관없음)
	const float2 Corners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
	const float2 Local      = Corners[VertexId] * BodyHalfSize.xy;
	const float2 Rotated    = float2(Local.x * BodyCosYaw - Local.y * BodySinYaw, Local.x * BodySinYaw + Local.y * BodyCosYaw);
	FWaterVSOutput Out;
	Out.WorldPosition = float3(BodyCenter.xy + Rotated, BodyCenter.z + BodyHalfSize.z);
	Out.Position      = mul(float4(Out.WorldPosition, 1.0f), ViewProjection);
	return Out;
}

float3 WorldFromDepth(float2 UV, float Depth)
{
	const float2 Ndc   = float2(UV.x * 2.0f - 1.0f, 1.0f - UV.y * 2.0f);
	const float4 World = mul(float4(Ndc, Depth, 1.0f), InvViewProjection);
	return World.xyz / World.w;
}

// FWaterMath::FresnelSchlick
float WaterFresnel(float CosTheta)
{
	const float C  = 1.0f - saturate(CosTheta);
	const float C2 = C * C;
	return WaterF0 + (1.0f - WaterF0) * C2 * C2 * C;
}

// 두 장 패닝 잔물결 노멀 (월드, 수면 위쪽 기준) + 거품 노이즈
float3 SampleWaveNormal(float2 WorldXY, out float FoamNoise)
{
	const float2 Flow = FlowDirection * (FlowSpeed * Time);
	const float2 UvA  = (WorldXY - Flow) / WaveScale + float2(0.11f, 0.07f) * (WaveSpeed * Time / WaveScale);
	const float2 UvB  = (WorldXY - Flow * 1.2f) / (WaveScale * 0.57f) + float2(-0.09f, 0.13f) * (WaveSpeed * Time / WaveScale);
	const float4 A    = WaveNormals.Sample(LinearWrap, UvA);
	const float4 B    = WaveNormals.Sample(LinearWrap, float2(-UvB.y, UvB.x)); // 90도 돌려 반복 무늬를 깬다
	const float2 Na   = A.rg * 2.0f - 1.0f;
	const float2 Nb   = float2(-(B.g * 2.0f - 1.0f), B.r * 2.0f - 1.0f);
	FoamNoise         = WaveNormals.Sample(LinearWrap, (WorldXY - Flow) / (WaveScale * 0.35f)).b;
	// 멀수록 잔물결을 줄인다 (밉 평균 + 반짝임 억제 — 지터마다 깜빡이지 않게)
	const float DistanceFade = lerp(1.0f, 0.35f, saturate(distance(float3(WorldXY, 0.0f), float3(CameraPosition.xy, 0.0f)) / 6000.0f));
	return normalize(float3((Na + Nb) * (NormalStrength * DistanceFade), 1.0f));
}

float WaterShadow(float3 WorldPosition)
{
	if (ShadowEnabled < 0.5f)
	{
		return 1.0f;
	}
	const uint Cascade = SelectCascadeByDepth(dot(WorldPosition - CameraPosition, ShadowCameraForward));
	if (Cascade >= CascadeCount)
	{
		return 1.0f;
	}
	const float4 ClipPos = mul(float4(WorldPosition + float3(0, 0, CascadeTexelWorld[Cascade] * ShadowNormalOffset), 1.0f), CascadeViewProjection[Cascade]);
	const float2 UV      = ClipPos.xy * float2(0.5f, -0.5f) + 0.5f;
	if (any(UV < 0.0f) || any(UV > 1.0f) || ClipPos.z > 1.0f)
	{
		return 1.0f;
	}
	float Lit = 0.0f;
	[unroll]
	for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll]
		for (int X = -1; X <= 1; ++X)
		{
			Lit += ShadowMap.SampleCmpLevelZero(ShadowSampler, float3(UV + float2(X, Y) * ShadowTexelSize, Cascade), ClipPos.z);
		}
	}
	return Lit / 9.0f;
}

// 반사 환경: 캡처(우선순위 순 남은 비중) → 하늘 프리필터 (Mesh.hlsl SampleSpecularEnvironment와 같은 규칙, SSR 제외)
float3 SampleReflectionEnvironment(float3 R, float3 WorldPosition)
{
	uint Width, Height, MipCount;
	IblSpecular.GetDimensions(0, Width, Height, MipCount);
	const float Lod       = Roughness * float(MipCount - 1);
	float3      Color     = 0.0f;
	float       Remaining = 1.0f;
	for (uint Index = 0; Index < ReflectionCaptureCount && Remaining > 0.01f; ++Index)
	{
		const FReflectionCaptureGpu Capture = ReflectionCaptures[Index];
		float Weight;
		float3 Dir = R;
		if (Capture.Shape == 0)
		{
			Weight = saturate((Capture.Radius - distance(WorldPosition, Capture.Position)) / Capture.FadeDistance);
		}
		else
		{
			const float3 D = Capture.BoxExtent - abs(WorldPosition - Capture.Position);
			Weight         = saturate(min(D.x, min(D.y, D.z)) / Capture.FadeDistance);
			const float3 BoxMin = Capture.Position - Capture.BoxExtent;
			const float3 BoxMax = Capture.Position + Capture.BoxExtent;
			const float3 Planes = select(R > 0.0f, BoxMax, BoxMin);
			const float3 T      = select(abs(R) > 1.0e-6f, (Planes - WorldPosition) / R, 1.0e30f);
			Dir                 = normalize(WorldPosition + R * max(min(T.x, min(T.y, T.z)), 0.0f) - Capture.Position);
		}
		if (Weight <= 0.0f)
		{
			continue;
		}
		Color += ReflectionCaptureAtlas.SampleLevel(LinearClamp, float4(Dir, float(Capture.Slot)), Lod).rgb * (Capture.Intensity * Weight * Remaining);
		Remaining *= 1.0f - Weight;
	}
	return Color + IblSpecular.SampleLevel(LinearClamp, R, Lod).rgb * (AmbientIntensity * Remaining);
}

// 화면 공간 반사 추적 (결정적): 월드 광선을 제곱 간격으로 나가며 씬 깊이와 비교 → 이분 보정. 신뢰도(a) = 화면 가장자리·거리 페이드
float4 TraceScreenReflection(float3 Origin, float3 R)
{
	if (bScreenReflections == 0)
	{
		return 0.0f;
	}
	const uint  Steps       = 40;
	const float MaxDistance = 3000.0f; // cm
	float       PrevT       = 0.0f;
	[loop]
	for (uint Step = 1; Step <= Steps; ++Step)
	{
		const float  Fraction = float(Step) / float(Steps);
		const float  T        = MaxDistance * Fraction * sqrt(Fraction); // 가까이 촘촘히 (지수 1.5)
		const float3 P        = Origin + R * T;
		const float4 Clip     = mul(float4(P, 1.0f), ViewProjection);
		if (Clip.w <= 1.0e-3f)
		{
			break;
		}
		const float2 UV = float2(Clip.x / Clip.w * 0.5f + 0.5f, 0.5f - Clip.y / Clip.w * 0.5f);
		if (any(UV < 0.0f) || any(UV > 1.0f))
		{
			break;
		}
		const float  Depth     = SceneDepth.SampleLevel(LinearClamp, UV, 0.0f);
		const float3 Scene     = WorldFromDepth(UV, Depth);
		const float  RayDist   = distance(P, CameraPosition);
		const float  SceneDist = distance(Scene, CameraPosition);
		const float  Thickness = 15.0f + T * 0.05f;
		if (Depth < 1.0f && RayDist > SceneDist && RayDist - SceneDist < Thickness)
		{
			// 이분 보정
			float Low  = PrevT;
			float High = T;
			[unroll]
			for (uint Refine = 0; Refine < 5; ++Refine)
			{
				const float  Mid      = 0.5f * (Low + High);
				const float3 Pm       = Origin + R * Mid;
				const float4 ClipM    = mul(float4(Pm, 1.0f), ViewProjection);
				const float2 UVm      = float2(ClipM.x / ClipM.w * 0.5f + 0.5f, 0.5f - ClipM.y / ClipM.w * 0.5f);
				const float3 SceneM   = WorldFromDepth(UVm, SceneDepth.SampleLevel(LinearClamp, UVm, 0.0f));
				if (distance(Pm, CameraPosition) > distance(SceneM, CameraPosition))
				{
					High = Mid;
				}
				else
				{
					Low = Mid;
				}
			}
			const float3 Hit   = Origin + R * High;
			const float4 ClipH = mul(float4(Hit, 1.0f), ViewProjection);
			const float2 HitUV = float2(ClipH.x / ClipH.w * 0.5f + 0.5f, 0.5f - ClipH.y / ClipH.w * 0.5f);
			const float2 Edge  = saturate(min(HitUV, 1.0f - HitUV) / 0.08f);
			const float  Fade  = Edge.x * Edge.y * saturate(1.0f - Fraction * Fraction);
			return float4(SceneColorCopy.SampleLevel(LinearClamp, HitUV, 0.0f).rgb, Fade);
		}
		PrevT = T;
	}
	return 0.0f;
}

// GGX 태양 반사광 (작은 하이라이트)
float SunSpecular(float3 N, float3 V, float3 L, float SurfaceRoughness)
{
	const float3 H     = normalize(V + L);
	const float  NdotH = saturate(dot(N, H));
	const float  NdotL = saturate(dot(N, L));
	const float  NdotV = max(saturate(dot(N, V)), 1.0e-4f);
	const float  A     = SurfaceRoughness * SurfaceRoughness;
	const float  A2    = A * A;
	const float  D     = A2 / max(E_PI * pow(NdotH * NdotH * (A2 - 1.0f) + 1.0f, 2.0f), 1.0e-6f);
	const float  K     = A * 0.5f;
	const float  G     = NdotL / (NdotL * (1.0f - K) + K) * NdotV / (NdotV * (1.0f - K) + K);
	return D * G / max(4.0f * NdotV, 1.0e-4f) * WaterFresnel(saturate(dot(H, V)));
}

struct FWaterOutput
{
	float4 Color    : SV_Target0;
	float2 Velocity : SV_Target1;
};

FWaterOutput PSWater(FWaterVSOutput Input, bool bFrontFace : SV_IsFrontFace)
{
	const int2  Pixel      = int2(Input.Position.xy);
	const float SceneZ     = SceneDepth.Load(int3(Pixel, 0));
	if (Input.Position.z > SceneZ)
	{
		discard; // 장면이 수면보다 앞 (셰이더 깊이 테스트 — 깊이 버퍼는 읽기 전용)
	}
	const float2 ScreenUV  = Input.Position.xy * InvScreenSize;
	const float3 P         = Input.WorldPosition;
	const float3 ToCamera  = CameraPosition - P;
	const float3 V         = normalize(ToCamera);
	const bool   bAbove    = CameraPosition.z >= P.z;

	float        FoamNoise;
	float3       N = SampleWaveNormal(P.xy, FoamNoise);
	const float3 Up = bAbove ? float3(0, 0, 1) : float3(0, 0, -1);
	if (!bAbove)
	{
		N = -N;
	}

	// 물 두께 (수면 → 이 픽셀 뒤 장면, 시선 방향) — 거품·굴절 배율
	const float3 BehindDirect = WorldFromDepth(ScreenUV, SceneZ);
	const float  Thickness0   = SceneZ >= 1.0f ? 1.0e5f : distance(BehindDirect, P);

	// 굴절: 노멀 xy 오프셋 (얕을수록 약하게, FWaterMath::RefractionScale), 오프셋 자리가 수면 앞이면 원래 자리
	float2 RefractUV = ScreenUV + N.xy * (RefractionStrength * saturate(Thickness0 / 100.0f));
	float  RefractZ  = SceneDepth.SampleLevel(LinearClamp, RefractUV, 0.0f);
	if (RefractZ < Input.Position.z || any(RefractUV < 0.0f) || any(RefractUV > 1.0f))
	{
		RefractUV = ScreenUV;
		RefractZ  = SceneZ;
	}
	const float3 Refracted = SceneColorCopy.SampleLevel(LinearClamp, RefractUV, 0.0f).rgb;
	const float3 Behind    = WorldFromDepth(RefractUV, RefractZ);
	const float  Thickness = RefractZ >= 1.0f ? 1.0e5f : distance(Behind, P);

	// 물속 산란 조명: 위에서 들어오는 태양 + 하늘 조도 (위쪽 IBL 확산)
	const float  SunUp      = saturate(SunDirection.z);
	const float  Shadow     = WaterShadow(P);
	const float3 SkyIrr     = IblDiffuse.SampleLevel(LinearClamp, float3(0, 0, 1), 0.0f).rgb * AmbientIntensity;
	const float3 InLight    = SunColor * (SunUp * Shadow / E_PI) + SkyIrr;
	const float3 T          = exp(-Absorption * (Thickness * 0.01f)); // FWaterMath::Transmittance (cm → m)

	// 굴절된 뒤 장면 색(SceneColorCopy)은 안개 적용 뒤 값이라 수면 안개를 다시 얹으면 두 번 낀다(얕은 물이 회색 띠로 뜸 — 2026-10-04
	// Alley 배수로). 그 몫(Direct, 가중치 DirectWeight)은 안개 합성에서 빼고 나머지(산란·반사·하이라이트·거품)에만 수면 안개를 건다
	float3 Color;
	float3 Direct       = 0.0f;
	float3 DirectWeight = 0.0f;
	if (bAbove)
	{
		// 반사: 화면 공간 → 캡처 → 하늘
		// 반사 방향은 잔물결을 절반만 (프레넬·하이라이트는 전체 노멀) — 화면 공간 추적이 이웃 픽셀마다 크게 튀지 않게
		const float3 Nr         = normalize(lerp(Up, N, 0.5f));
		const float3 R          = reflect(-V, Nr);
		const float3 Rup        = float3(R.xy, abs(R.z)); // 잔물결로 아래로 꺾인 반사는 수평으로
		const float3 Environment = SampleReflectionEnvironment(Rup, P);
		const float4 Ssr        = TraceScreenReflection(P + Up * 2.0f, Rup);
		const float3 Reflection = lerp(Environment, Ssr.rgb, Ssr.a) * ReflectionIntensity;
		const float  F          = WaterFresnel(dot(N, V));
		// lerp(Refracted * T + 산란 * (1 - T), Reflection, F)를 굴절 몫과 나머지로 나눔
		DirectWeight = T * (1.0f - F);
		Direct       = Refracted * DirectWeight;
		Color        = ScatterColor * InLight * ((1.0f - T) * (1.0f - F)) + Reflection * F;
		Color += SunColor * (SunSpecular(N, V, SunDirection, max(Roughness, 0.02f)) * saturate(dot(N, SunDirection)) * Shadow);
		// 로컬 라이트 반사광 (점광원/스포트/면광원 — 메시 패스와 같은 클러스터 평가, 그림자 포함). 확산 없음(알베도 0) = 반사만.
		// 점 광원의 거울 하이라이트가 잔물결마다 한 픽셀로 반짝이지 않게 거칠기 하한을 둔다
		FSurface LocalSurface;
		LocalSurface.Albedo    = 0.0f;
		LocalSurface.Metallic  = 0.0f;
		LocalSurface.Roughness = max(Roughness, WaterLocalLightMinRoughness);
		LocalSurface.N         = N;
		LocalSurface.V         = V;
		LocalSurface.Occlusion = 1.0f;
		Color += EvaluateLocalLights(LocalSurface, Input.Position.xy, P, Up);
	}
	else
	{
		// 수면 아래에서 위로: 스넬 창(임계각 48.6도) 안은 위 세상 굴절, 밖은 전반사(물 색)
		const float CosTheta = saturate(dot(N, V));
		const float Window   = saturate((CosTheta - 0.66f) / 0.05f);
		const float3 WaterColor = ScatterColor * InLight;
		Color = lerp(WaterColor, Refracted * (1.0f - WaterFresnel(CosTheta)), Window);
	}

	// 가장자리 거품 (FWaterMath::EdgeFoam × 노이즈)
	if (FoamDistance > 0.0f && FoamIntensity > 0.0f)
	{
		const float Edge = saturate(1.0f - Thickness0 / FoamDistance);
		const float Foam = saturate(Edge * Edge * FoamIntensity * smoothstep(0.35f, 0.75f, FoamNoise + Edge * 0.3f));
		const float3 FoamLit = (SunColor * (saturate(dot(Up, SunDirection)) * Shadow) + SkyIrr * E_PI) * (0.8f / E_PI);
		Color = lerp(Color, FoamLit, Foam);
		Direct *= 1.0f - Foam;
		DirectWeight *= 1.0f - Foam;
	}

	// 안개 + 공중 원근 (수면 위치)
	if (bAbove)
	{
		const float4 Fog = EvaluateFog(P);
		Color            = Direct + Color * Fog.a + Fog.rgb * (1.0f - DirectWeight);
	}

	FWaterOutput Output;
	Output.Color = float4(Color, ReactiveMask);
	// 움직임 벡터 = 현재 UV - 이전 UV (지터 없음, 수면 위치)
	const float4 Current  = mul(float4(P, 1.0f), UnjitteredViewProjection);
	const float4 Previous = mul(float4(P, 1.0f), PrevViewProjection);
	const float2 CurUV    = float2(Current.x / Current.w * 0.5f + 0.5f, 0.5f - Current.y / Current.w * 0.5f);
	const float2 PrevUV   = float2(Previous.x / Previous.w * 0.5f + 0.5f, 0.5f - Previous.y / Previous.w * 0.5f);
	Output.Velocity       = CurUV - PrevUV;
	return Output;
}

// ---- 물속 카메라 (전체 화면, 카메라가 이 물 상자 안일 때만)
struct FUnderwaterVSOutput
{
	float4 Position : SV_Position;
	float2 UV       : TEXCOORD0;
};

FUnderwaterVSOutput VSUnderwater(uint VertexId : SV_VertexID)
{
	FUnderwaterVSOutput Out;
	Out.UV       = float2((VertexId << 1) & 2, VertexId & 2);
	Out.Position = float4(Out.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	return Out;
}

// 상자(요 회전) 안 시선 구간 길이 (FWaterMath::UnderwaterPathLength)
float UnderwaterPathLength(float3 Origin, float3 Dir, float SceneDistance)
{
	const float3 Rel   = Origin - BodyCenter;
	const float3 LocalO = float3(Rel.x * BodyCosYaw + Rel.y * BodySinYaw, -Rel.x * BodySinYaw + Rel.y * BodyCosYaw, Rel.z);
	const float3 LocalD = float3(Dir.x * BodyCosYaw + Dir.y * BodySinYaw, -Dir.x * BodySinYaw + Dir.y * BodyCosYaw, Dir.z);
	const float3 SafeD  = select(abs(LocalD) > 1.0e-8f, LocalD, 1.0e-8f);
	const float3 T0     = (-BodyHalfSize - LocalO) / SafeD;
	const float3 T1     = (BodyHalfSize - LocalO) / SafeD;
	const float3 TMin   = min(T0, T1);
	const float3 TMax   = max(T0, T1);
	const float  Near   = max(max(TMin.x, max(TMin.y, TMin.z)), 0.0f);
	const float  Far    = min(TMax.x, min(TMax.y, TMax.z));
	return Far > Near ? max(min(Far, SceneDistance) - Near, 0.0f) : 0.0f;
}

float4 PSUnderwater(FUnderwaterVSOutput Input) : SV_Target0
{
	const float  Depth  = SceneDepth.Load(int3(int2(Input.Position.xy), 0));
	const float3 Far    = WorldFromDepth(Input.UV, Depth >= 1.0f ? 1.0f : Depth);
	const float3 Dir    = normalize(Far - CameraPosition);
	const float  Scene  = Depth >= 1.0f ? 1.0e7f : distance(Far, CameraPosition);
	const float  Length = UnderwaterPathLength(CameraPosition, Dir, Scene);
	const float3 T      = exp(-Absorption * (Length * 0.01f));
	const float3 SkyIrr = IblDiffuse.SampleLevel(LinearClamp, float3(0, 0, 1), 0.0f).rgb * AmbientIntensity;
	const float3 InLight = SunColor * (saturate(SunDirection.z) / E_PI) + SkyIrr;
	// 프리멀티플라이드: 색 = 산란 + 원래 × (1 - a), 투과율은 휘도 가중 스칼라 (합성 경로)
	const float  TScalar = dot(T, float3(0.2126f, 0.7152f, 0.0722f));
	return float4(ScatterColor * InLight * (1.0f - T), 1.0f - TScalar);
}
