#include "Common.hlsli"
// 대기 (상수 b1, LUT t0~t2, 선형 클램프 s0)
#define E_ATMOSPHERE_CONSTANTS_REGISTER b1
#include "Atmosphere.hlsli"
// 공중 원근 (안개 상수 b2 — EvaluateAerialFog만 쓴다, 볼륨/샘플러는 쓰지 않음)
#define E_FOG_CONSTANTS_REGISTER b2
#define E_FOG_VOLUME_REGISTER t9
#define E_FOG_SAMPLER_REGISTER s3
#include "Fog.hlsli"

// 볼류메트릭 구름 (Phase 49, FVolumetricCloudRenderer). CPU 참조 식은 Renderer/CloudMath.h (테스트 Cloud_*)
//   TraceCS:    저해상도(씬 ÷ r.VolumetricClouds.Divisor) 레이마칭 — 낮은 적운 층(3D 모양/세부 노이즈 + 덮임 분포) + 높은 권운(2D).
//               조명 = 태양(또는 밤에는 달) 대기 투과율 × 구름 그림자 행진(6단계) × 다중 산란 옥타브(3) × 이중 로브 위상 + 하늘빛 앰비언트.
//               공중 원근은 구름 평균 거리에서 Fog.hlsli EvaluateAerialFog로 합성 (구름 앞 대기 빛 유지). 출력 rgb = 더할 빛, a = 투과율, 거리(km)
//               픽셀 안 표본 위치(지터)와 행진 시작(인터리브드 그레이디언트 노이즈)은 프레임마다 바뀌고 ResolveCS가 누적한다
//   ResolveCS:  시간 누적 (구름 거리로 이전 화면 재투영 + 3x3 분산 클리핑, 이력 2장 핑퐁). 이력 무효면 현재 그대로
//   CubeCS:     IBL 하늘 큐브용 저해상도 큐브 (지터·누적 없음, 적은 단계)
//   PSComposite: 씬 컬러(내부 해상도)에 업샘플 합성 — 하늘이거나 구름이 기하보다 가까운 픽셀만, 프리멀티플라이드 (색 = 구름 빛 + 원래 × 투과율)

cbuffer CloudConstants : register(b0)
{
	float4x4 InvViewProjection;   // 지터 없음 (광선 방향)
	float4x4 PrevViewProjection;  // 지터 없음 (재투영)
	float4x4 ViewProjection;      // 지터 없음 (현재)
	float3   CameraPositionWorld; // cm
	float    LayerBottomRadius;   // km (행성 중심 거리)
	float3   WindOffset;          // km (누적 바람 이동)
	float    LayerTopRadius;
	float    Coverage;
	float    CloudType;
	float    Extinction;          // 1/km
	float    ShapeTile;           // km
	float    DetailTile;
	float    WeatherTile;
	float    DetailStrength;
	float    SilverLining;
	float3   Albedo;
	float    AmbientScale;
	float3   LightDirection;      // 빛 쪽 (태양, 밤에는 달)
	float    CirrusCoverage;
	float3   LightIlluminance;    // 대기 위 조도 × 하늘 밝기 배율
	float    CirrusRadius;        // km
	float2   TraceSize;
	float2   InvTraceSize;
	float2   Jitter;              // 추적 픽셀 안 표본 위치 (0~1)
	uint     MaxSteps;
	uint     FrameIndex;
	float    HistoryWeight;       // 이력 비중 (0이면 현재만)
	uint     bHistoryValid;
	float    MaxDistance;         // km
	float    CubeSize;
};

Texture3D<float4>   ShapeNoise    : register(t3);
Texture3D<float4>   DetailNoise   : register(t4);
Texture2D<float2>   WeatherMap    : register(t5);
Texture2D<float4>   HistoryColor  : register(t6);
Texture2D<float>    HistoryDepth  : register(t7);
Texture2D<float>    SceneDepthTex : register(t8);
Texture2D<float4>   CurrentColor  : register(t10);
Texture2D<float>    CurrentDepth  : register(t11);
RWTexture2D<float4> OutColor      : register(u0);
RWTexture2D<float>  OutDepth      : register(u1);
RWTexture2DArray<float4> OutCube  : register(u2);
SamplerState        WrapSampler   : register(s1);

float Remap(float Value, float OldMin, float OldMax, float NewMin, float NewMax)
{
	return NewMin + (Value - OldMin) / max(OldMax - OldMin, 1.0e-6f) * (NewMax - NewMin);
}

// FCloudMath::HeightGradient
float HeightGradient(float H, float Type)
{
	const float Bottom   = saturate(Remap(H, 0.0f, 0.1f, 0.0f, 1.0f));
	const float TopLimit = 0.3f + 0.7f * saturate(Type);
	const float Top      = saturate(Remap(H, TopLimit * 0.6f, TopLimit, 1.0f, 0.0f));
	return Bottom * Top;
}

float HenyeyGreenstein(float CosTheta, float G)
{
	const float G2 = G * G;
	return (1.0f - G2) / (4.0f * E_PI * pow(max(1.0f + G2 - 2.0f * G * CosTheta, 1.0e-6f), 1.5f));
}

float DualLobePhase(float CosTheta, float ForwardG, float BackwardG, float Blend)
{
	return lerp(HenyeyGreenstein(CosTheta, ForwardG), HenyeyGreenstein(CosTheta, BackwardG), Blend);
}

// 구름 밀도 (1/km). bCheap = 세부 노이즈 없이 (그림자 행진 뒤쪽·빈 공간 판정)
float SampleCloudDensity(float3 P, bool bCheap)
{
	const float Radius = length(P);
	const float H      = saturate((Radius - LayerBottomRadius) / (LayerTopRadius - LayerBottomRadius));
	const float3 Q     = P + WindOffset;
	const float2 Weather = WeatherMap.SampleLevel(WrapSampler, Q.xy / WeatherTile, 0.0f);
	const float  LocalCoverage = saturate(Coverage + (Weather.x - 0.5f) * 0.9f);
	if (LocalCoverage <= 0.0f)
	{
		return 0.0f;
	}
	const float  Type     = saturate(CloudType + (Weather.y - 0.5f) * 0.5f);
	const float  Gradient = HeightGradient(H, Type);
	if (Gradient <= 0.0f)
	{
		return 0.0f;
	}
	// 높이에 따라 바람 방향으로 조금 밀려 기울어진 모양
	const float4 Shape    = ShapeNoise.SampleLevel(WrapSampler, (Q + float3(WindOffset.xy * 0.15f * H, 0.0f)) / ShapeTile, 0.0f);
	const float  Fbm      = dot(Shape.gba, float3(0.625f, 0.25f, 0.125f));
	const float  ShapeVal = saturate(Remap(Shape.r, Fbm - 1.0f, 1.0f, 0.0f, 1.0f)) * Gradient;
	float        Base     = saturate(Remap(ShapeVal, 1.0f - LocalCoverage, 1.0f, 0.0f, 1.0f)) * LocalCoverage; // FCloudMath::BaseDensity
	if (Base <= 0.0f || bCheap)
	{
		return Base * Extinction;
	}
	const float3 DetailSample = DetailNoise.SampleLevel(WrapSampler, Q / DetailTile, 0.0f).rgb;
	const float  DetailFbm    = dot(DetailSample, float3(0.625f, 0.25f, 0.125f));
	const float  Detail       = lerp(DetailFbm, 1.0f - DetailFbm, saturate(H * 4.0f)); // FCloudMath::ErodeDensity
	Base                      = saturate(Remap(Base, Detail * DetailStrength, 1.0f, 0.0f, 1.0f));
	return Base * Extinction;
}

// 빛 쪽 광학 깊이 (6단계, 뒤로 갈수록 길게)
float LightOpticalDepth(float3 P)
{
	float Depth = 0.0f;
	float Step  = (LayerTopRadius - LayerBottomRadius) * 0.06f;
	float T     = 0.0f;
	[unroll]
	for (uint Index = 0; Index < 6; ++Index)
	{
		T += Step * 0.5f;
		Depth += SampleCloudDensity(P + LightDirection * T, Index >= 3) * Step;
		T += Step * 0.5f;
		Step *= 1.8f;
	}
	return Depth;
}

// 광선 - 구 껍질 (FCloudMath::RayShellIntersection)
bool CloudShell(float3 Origin, float3 Dir, out float Start, out float End)
{
	Start               = 0.0f;
	End                 = 0.0f;
	const float CamR    = length(Origin);
	const float Mu      = dot(Origin, Dir) / CamR;
	const float B       = CamR * Mu;
	const float COuter  = CamR * CamR - LayerTopRadius * LayerTopRadius;
	const float DOuter  = B * B - COuter;
	if (DOuter < 0.0f)
	{
		return false;
	}
	const float OuterT1 = -B + sqrt(DOuter);
	if (OuterT1 <= 0.0f)
	{
		return false;
	}
	const float OuterT0 = -B - sqrt(DOuter);
	const float CInner  = CamR * CamR - LayerBottomRadius * LayerBottomRadius;
	const float DInner  = B * B - CInner;
	const bool  bInner  = DInner >= 0.0f;
	const float InnerT0 = bInner ? -B - sqrt(DInner) : 0.0f;
	const float InnerT1 = bInner ? -B + sqrt(DInner) : 0.0f;
	if (CamR < LayerBottomRadius)
	{
		const float CGround = CamR * CamR - AtmoBottomRadius * AtmoBottomRadius;
		const float DGround = B * B - CGround;
		if (DGround >= 0.0f && -B - sqrt(DGround) > 0.0f)
		{
			return false; // 지면에 막힘
		}
		Start = bInner ? max(InnerT1, 0.0f) : 0.0f;
		End   = OuterT1;
	}
	else if (CamR <= LayerTopRadius)
	{
		Start = 0.0f;
		End   = (bInner && InnerT0 > 0.0f) ? InnerT0 : OuterT1;
	}
	else
	{
		Start = max(OuterT0, 0.0f);
		End   = (bInner && InnerT0 > 0.0f) ? InnerT0 : OuterT1;
	}
	return End > Start;
}

float InterleavedGradientNoise(float2 Pixel, uint Frame)
{
	Pixel += float(Frame % 64u) * 5.588238f;
	return frac(52.9829189f * frac(0.06711056f * Pixel.x + 0.00583715f * Pixel.y));
}

// 하늘빛 앰비언트 (구름 아래쪽은 지면 쪽 하늘, 위쪽은 천정) — 하늘 뷰 LUT 두 방향
float3 CloudAmbient(float H)
{
	const float3 Up       = normalize(AtmoCameraPosition);
	const float3 Side     = normalize(cross(Up, abs(Up.x) < 0.9f ? float3(1, 0, 0) : float3(0, 1, 0)));
	const float3 Zenith   = AtmoSampleSkyView(AtmoCameraPosition, Up);
	const float3 Horizon  = AtmoSampleSkyView(AtmoCameraPosition, normalize(Side + Up * 0.1f));
	return lerp(Horizon * 0.6f, Zenith, H) * (E_PI * AmbientScale) + AtmoNightSkyLuminance * (E_PI * AmbientScale);
}

// 광선 하나 적분: rgb = 더할 빛 (공중 원근 포함), a = 투과율, OutDistance = 투과율 가중 평균 거리(km, 없으면 MaxDistance)
float4 IntegrateClouds(float3 Origin, float3 Dir, uint Steps, float Offset, out float OutDistance)
{
	OutDistance = MaxDistance;
	float3 Scatter       = 0.0f;
	float  Transmittance = 1.0f;
	float  Start;
	float  End;
	float  WeightedDistance = 0.0f;
	float  WeightSum        = 0.0f;
	if (CloudShell(Origin, Dir, Start, End) && Start < MaxDistance)
	{
		End                   = min(End, MaxDistance);
		const float  Length   = End - Start;
		const uint   Count    = max(uint(lerp(float(Steps) * 0.5f, float(Steps), saturate(Length / 20.0f))), 8u);
		const float  Dt       = Length / float(Count);
		const float  CosTheta = dot(Dir, LightDirection);
		const float3 Ambient  = CloudAmbient(0.5f);
		[loop]
		for (uint Step = 0; Step < Count; ++Step)
		{
			const float  T       = Start + (float(Step) + Offset) * Dt;
			const float3 P       = Origin + Dir * T;
			const float  Density = SampleCloudDensity(P, false);
			if (Density <= 0.0f)
			{
				continue;
			}
			const float Radius = length(P);
			const float H      = saturate((Radius - LayerBottomRadius) / (LayerTopRadius - LayerBottomRadius));
			// 빛: 대기 투과율 (구름 고도) × 구름 그림자 + 다중 산란 옥타브 (FCloudMath::MultiScatteringOctaves, a=b=c=0.5)
			const float3 Up             = P / Radius;
			const float3 SunAtmosphere  = AtmoGetTransmittance(Radius, dot(LightDirection, Up)) * LightIlluminance;
			const float  LightDepth     = LightOpticalDepth(P);
			float        Octaves        = 0.0f;
			float        A              = 1.0f;
			[unroll]
			for (uint Octave = 0; Octave < 3; ++Octave)
			{
				Octaves += A * exp(-LightDepth * A) * DualLobePhase(CosTheta, SilverLining * A, -0.2f * A, 0.25f);
				A *= 0.5f;
			}
			const float  Powder   = 1.0f - exp(-2.0f * Density * Dt); // 가장자리 어둡기 (비어-파우더)
			const float3 Lighting = SunAtmosphere * Octaves * lerp(0.6f, 1.0f, Powder) + lerp(Ambient * 0.5f, Ambient, H);
			const float  SampleT  = exp(-Density * Dt);
			Scatter += Transmittance * Albedo * Lighting * (1.0f - SampleT); // 산란 = 소멸 × 반사율 (구간 해석 적분)
			WeightedDistance += T * Transmittance * (1.0f - SampleT);
			WeightSum += Transmittance * (1.0f - SampleT);
			Transmittance *= SampleT;
			if (Transmittance < 0.01f)
			{
				Transmittance = 0.0f;
				break;
			}
		}
		if (WeightSum > 1.0e-4f)
		{
			OutDistance = WeightedDistance / WeightSum;
		}
	}

	// 권운 (높은 2D 층): 적운 뒤(위)
	if (CirrusCoverage > 0.0f)
	{
		const float B  = dot(Origin, Dir);
		const float C  = dot(Origin, Origin) - CirrusRadius * CirrusRadius;
		const float D  = B * B - C;
		if (D >= 0.0f)
		{
			const float T = -B + sqrt(D);
			if (T > 0.0f && T < MaxDistance * 3.0f)
			{
				const float3 P       = Origin + Dir * T;
				const float2 UV      = (P.xy + WindOffset.xy * 2.0f) / (WeatherTile * 0.35f);
				const float  Noise   = WeatherMap.SampleLevel(WrapSampler, UV, 0.0f).x * 0.6f + WeatherMap.SampleLevel(WrapSampler, UV * 3.7f, 0.0f).y * 0.4f;
				const float  Opacity = saturate(Remap(Noise, 1.0f - CirrusCoverage, 1.0f, 0.0f, 1.0f)) * 0.35f;
				const float3 Up      = P / length(P);
				const float3 Light   = AtmoGetTransmittance(length(P), dot(LightDirection, Up)) * LightIlluminance *
				                     HenyeyGreenstein(dot(Dir, LightDirection), 0.6f) + CloudAmbient(1.0f) * 0.5f;
				Scatter += Transmittance * Albedo * Light * Opacity;
				if (WeightSum <= 1.0e-4f && Opacity > 0.0f)
				{
					OutDistance = T;
				}
				Transmittance *= 1.0f - Opacity;
			}
		}
	}

	// 공중 원근: 구름 앞 대기 (구름이 가린 하늘에는 이미 대기 빛이 있었다 — 가린 만큼 앞쪽 대기 빛을 더함)
	if (Transmittance < 1.0f)
	{
		const float3 CloudWorld = CameraPositionWorld + Dir * (OutDistance * 100000.0f);
		const float4 Aerial     = FogAerialEnabled != 0 ? EvaluateAerialFog(CloudWorld) : float4(0.0f, 0.0f, 0.0f, 1.0f);
		Scatter                 = Scatter * Aerial.a + Aerial.rgb * (1.0f - Transmittance);
	}
	return float4(Scatter, Transmittance);
}

float3 TraceDirection(float2 UV)
{
	const float2 Ndc   = float2(UV.x * 2.0f - 1.0f, 1.0f - UV.y * 2.0f);
	const float4 World = mul(float4(Ndc, 1.0f, 1.0f), InvViewProjection);
	return normalize(World.xyz / World.w - CameraPositionWorld);
}

[numthreads(8, 8, 1)]
void TraceCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(float2(Id.xy) >= TraceSize))
	{
		return;
	}
	const float2 UV     = (float2(Id.xy) + Jitter) * InvTraceSize;
	const float3 Dir    = TraceDirection(UV);
	const float  Offset = HistoryWeight > 0.0f ? InterleavedGradientNoise(float2(Id.xy), FrameIndex) : 0.5f;
	float        Distance;
	const float4 Result = IntegrateClouds(AtmoCameraPosition, Dir, MaxSteps, Offset, Distance);
	OutColor[Id.xy]     = Result;
	OutDepth[Id.xy]     = Distance;
}

// 시간 누적: 현재(t10/t11) + 이전 이력(t6/t7, 구름 거리로 재투영) → 새 이력(u0/u1)
[numthreads(8, 8, 1)]
void ResolveCS(uint3 Id : SV_DispatchThreadID)
{
	if (any(float2(Id.xy) >= TraceSize))
	{
		return;
	}
	const int2   Pixel    = int2(Id.xy);
	const int2   MaxPixel = int2(TraceSize) - 1;
	const float4 Current  = CurrentColor.Load(int3(Pixel, 0));
	const float  Distance = CurrentDepth.Load(int3(Pixel, 0));
	if (bHistoryValid == 0 || HistoryWeight <= 0.0f)
	{
		OutColor[Id.xy] = Current;
		OutDepth[Id.xy] = Distance;
		return;
	}
	// 3x3 평균·분산 (현재 표본) → 이력 클리핑
	float4 Mean  = 0.0f;
	float4 Mean2 = 0.0f;
	[unroll]
	for (int Y = -1; Y <= 1; ++Y)
	{
		[unroll]
		for (int X = -1; X <= 1; ++X)
		{
			const float4 Sample = CurrentColor.Load(int3(clamp(Pixel + int2(X, Y), 0, MaxPixel), 0));
			Mean += Sample;
			Mean2 += Sample * Sample;
		}
	}
	Mean /= 9.0f;
	Mean2 /= 9.0f;
	const float4 Sigma = sqrt(max(Mean2 - Mean * Mean, 0.0f)) * 1.5f;

	const float2 UV       = (float2(Id.xy) + 0.5f) * InvTraceSize;
	const float3 Dir      = TraceDirection(UV);
	const float3 World    = CameraPositionWorld + Dir * (Distance * 100000.0f);
	const float4 Prev     = mul(float4(World, 1.0f), PrevViewProjection);
	const float2 PrevUV   = float2(Prev.x / Prev.w * 0.5f + 0.5f, 0.5f - Prev.y / Prev.w * 0.5f);
	if (Prev.w <= 0.0f || any(PrevUV < 0.0f) || any(PrevUV > 1.0f))
	{
		OutColor[Id.xy] = Current;
		OutDepth[Id.xy] = Distance;
		return;
	}
	const float4 History  = clamp(HistoryColor.SampleLevel(AtmoLinearSampler, PrevUV, 0.0f), Mean - Sigma, Mean + Sigma);
	const float  HistoryD = HistoryDepth.SampleLevel(AtmoLinearSampler, PrevUV, 0.0f);
	OutColor[Id.xy]       = lerp(Current, History, HistoryWeight);
	OutDepth[Id.xy]       = lerp(Distance, HistoryD, HistoryWeight * 0.5f);
}

// IBL 하늘 큐브용 구름 큐브 (SkyAtmosphere.hlsl SkyCubeFaceDirection과 같은 면 규약)
[numthreads(8, 8, 1)]
void CubeCS(uint3 Id : SV_DispatchThreadID)
{
	const uint Size = uint(CubeSize);
	if (any(Id.xy >= Size) || Id.z >= 6)
	{
		return;
	}
	const float2 Q = (float2(Id.xy) + 0.5f) / float(Size) * 2.0f - 1.0f;
	float3       Dir;
	if (Id.z == 0) Dir = float3(1, -Q.y, -Q.x);
	else if (Id.z == 1) Dir = float3(-1, -Q.y, Q.x);
	else if (Id.z == 2) Dir = float3(Q.x, 1, Q.y);
	else if (Id.z == 3) Dir = float3(Q.x, -1, -Q.y);
	else if (Id.z == 4) Dir = float3(Q.x, -Q.y, 1);
	else Dir = float3(-Q.x, -Q.y, -1);
	float Distance;
	OutCube[Id] = IntegrateClouds(AtmoCameraPosition, normalize(Dir), 24, 0.5f, Distance);
}

// ---- 합성 (전체 화면, 씬 컬러 내부 해상도)
struct FCompositeVSOutput
{
	float4 Position : SV_Position;
	float2 UV       : TEXCOORD0;
};

FCompositeVSOutput VSComposite(uint VertexId : SV_VertexID)
{
	FCompositeVSOutput Out;
	Out.UV       = float2((VertexId << 1) & 2, VertexId & 2);
	Out.Position = float4(Out.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	return Out;
}

float4 PSComposite(FCompositeVSOutput Input) : SV_Target0
{
	const float  Depth = SceneDepthTex.Load(int3(int2(Input.Position.xy), 0));
	const float4 Cloud = HistoryColor.SampleLevel(AtmoLinearSampler, Input.UV, 0.0f);
	if (Depth < 1.0f)
	{
		// 기하 앞의 구름만 (먼 산 등): 기하 거리 > 구름 거리
		const float  CloudKm  = HistoryDepth.SampleLevel(AtmoLinearSampler, Input.UV, 0.0f);
		const float2 Ndc      = float2(Input.UV.x * 2.0f - 1.0f, 1.0f - Input.UV.y * 2.0f);
		const float4 World    = mul(float4(Ndc, Depth, 1.0f), InvViewProjection);
		const float  SurfaceKm = distance(World.xyz / World.w, CameraPositionWorld) * 1.0e-5f;
		if (SurfaceKm < CloudKm)
		{
			return float4(0.0f, 0.0f, 0.0f, 0.0f);
		}
	}
	return float4(Cloud.rgb, 1.0f - Cloud.a); // 프리멀티플라이드: 색 = 구름 빛 + 원래 × 투과율
}
