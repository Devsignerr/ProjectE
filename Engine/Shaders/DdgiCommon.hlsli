#ifndef E_DDGI_COMMON_HLSLI
#define E_DDGI_COMMON_HLSLI

// 동적 GI — DDGI 프로브 볼륨 공용 (Phase 51). CPU 기준 식은 Renderer/DdgiMath.h, 상수 구조는 Renderer/DdgiRenderer.h FDdgiConstants — 함께 고친다.
//   레지스터는 include하는 쪽이 정한다: E_DDGI_CONSTANTS_REGISTER(상수 버퍼), E_DDGI_IRRADIANCE_REGISTER / E_DDGI_DISTANCE_REGISTER /
//   E_DDGI_PROBE_DATA_REGISTER(아틀라스 3장), E_DDGI_SAMPLER(선형 클램프 샘플러 이름 — 먼저 선언되어 있어야 함)
//   메시 패스(Mesh.hlsl/Terrain.hlsl): b9, t40~t42, IblSampler / 프로브 광선(DdgiTrace.hlsl): b0, t9~t11, RtClampSampler / 누적(DdgiBlend.hlsl): b0, t1~t3
//
// 아틀라스: 조도 타일 10x10 (안쪽 8x8 = 조도/π, RGBA16F), 거리 타일 18x18 (안쪽 16x16 = (평균, 제곱 평균) / DistanceClamp, RG16F),
//   프로브 상태 1텍셀 (xyz 재배치 오프셋 cm, w = 상태(0 없음 / 1 활성 / 2 비활성) + 4 × 갱신 횟수(상한 1023)). 프로브 g의 타일 = (g % 64, g / 64)

#define E_DDGI_IRRADIANCE_TEXELS 8
#define E_DDGI_DISTANCE_TEXELS 16
#define E_DDGI_TILES_PER_ROW 64
#define E_DDGI_MAX_VOLUMES 8
#define E_DDGI_STATE_UNINITIALIZED 0
#define E_DDGI_STATE_ACTIVE 1
#define E_DDGI_STATE_INACTIVE 2
#define E_DDGI_FLAG_RELOCATION 1u
#define E_DDGI_FLAG_CLASSIFICATION 2u
#define E_DDGI_MAX_UPDATE_COUNT 1023u

// FDdgiVolumeGpu (128바이트)
struct FDdgiVolume
{
	float3 Origin;        // 프로브 (0,0,0) 위치
	float  Intensity;
	float3 Spacing;
	float  NormalBias;
	uint   CountX;
	uint   CountY;
	uint   CountZ;
	uint   ProbeOffset;   // 아틀라스 전체 번호 시작
	float  ViewBias;
	float  FadeDistance;
	float  DistanceClamp; // 거리 정규화 (1.5 × |간격|)
	float  MaxRayDistance;
	uint   RaysPerProbe;
	uint   FixedRays;     // 재배치·분류 전용 고정 광선 수 (누적 제외)
	uint   UpdateStart;   // 이번 프레임 갱신 범위 [Start, Start + Count) (볼륨 안 번호, 순환)
	uint   UpdateCount;
	uint   RowOffset;     // 추적 타깃 행 시작
	float  Hysteresis;
	float  ChangeThreshold;
	float  MinFrontfaceDistance;
	float  BackfaceThreshold;
	uint   Flags;         // E_DDGI_FLAG_*
	uint   DebugProbes;   // 0 없음, 1 조도, 2 거리, 3 상태
	float  DebugRadius;
	float4 VolumePadding;
};

cbuffer DdgiConstants : register(E_DDGI_CONSTANTS_REGISTER)
{
	uint        DdgiVolumeCount; // 0 = 볼륨 없음 → 메시는 예전 하늘 IBL 식 그대로
	uint        DdgiDebugView;   // 1 = 간접 확산만 출력 (--debug-view gi)
	uint        DdgiReset;       // 1 = 이전 아틀라스 무효 (처음/배치 변경) — 누적이 이전 값을 읽지 않는다
	uint        DdgiTotalProbes;
	float4      DdgiRayRotation[3]; // 행 3개 (xyz)
	float4x4    DdgiDebugViewProjection; // 프로브 구 표시 (지터 포함 뷰-투영)
	float3      DdgiDebugCameraPosition;
	float       DdgiBounceIntensity; // 프로브 광선 히트의 간접 확산(이전 프로브 조도) 배율 — 다중 반사
	float2      DdgiIrradianceTexelSize; // 1 / 조도 아틀라스 크기
	float2      DdgiDistanceTexelSize;
	FDdgiVolume DdgiVolumes[E_DDGI_MAX_VOLUMES];
};

Texture2D<float4> DdgiIrradianceAtlas : register(E_DDGI_IRRADIANCE_REGISTER);
Texture2D<float2> DdgiDistanceAtlas   : register(E_DDGI_DISTANCE_REGISTER);
Texture2D<float4> DdgiProbeData       : register(E_DDGI_PROBE_DATA_REGISTER);

// ---- DdgiMath와 같은 식

// 프로브 상태 텍셀 w → 상태 / 갱신 횟수 (DdgiMath::EncodeProbeState)
uint DdgiGetProbeState(float W)
{
	return ((uint)W) & 3u;
}

uint DdgiGetProbeUpdateCount(float W)
{
	return ((uint)W) >> 2;
}

float DdgiEncodeProbeState(uint State, uint UpdateCount)
{
	return (float)(State + 4u * min(UpdateCount, E_DDGI_MAX_UPDATE_COUNT));
}

float2 DdgiOctEncode(float3 D)
{
	float2 P = D.xy / (abs(D.x) + abs(D.y) + abs(D.z));
	if (D.z < 0.0f)
	{
		P = (1.0f - abs(P.yx)) * select(P >= 0.0f, 1.0f, -1.0f);
	}
	return P;
}

float3 DdgiOctDecode(float2 P)
{
	float3      D = float3(P, 1.0f - abs(P.x) - abs(P.y));
	const float T = max(-D.z, 0.0f);
	D.xy += select(D.xy >= 0.0f, -T, T);
	return normalize(D);
}

// 구면 피보나치 (DdgiMath::SphericalFibonacci)
float3 DdgiSphericalFibonacci(uint Index, uint Count)
{
	const float I        = (float)Index;
	const float Phi      = 2.0f * 3.14159265358979f * frac(I * 0.618033988749895f);
	const float CosTheta = 1.0f - (2.0f * I + 1.0f) / (float)Count;
	const float SinTheta = sqrt(saturate(1.0f - CosTheta * CosTheta));
	return float3(cos(Phi) * SinTheta, sin(Phi) * SinTheta, CosTheta);
}

// 광선 방향 (DdgiMath::GetRayDirection): 고정 광선은 회전 없음
float3 DdgiGetRayDirection(uint Index, uint RayCount, uint FixedRays)
{
	if (Index < FixedRays)
	{
		return DdgiSphericalFibonacci(Index, FixedRays);
	}
	const float3 D = DdgiSphericalFibonacci(Index - FixedRays, RayCount - FixedRays);
	return float3(dot(DdgiRayRotation[0].xyz, D), dot(DdgiRayRotation[1].xyz, D), dot(DdgiRayRotation[2].xyz, D));
}

uint3 DdgiGetCounts(FDdgiVolume Volume)
{
	return uint3(Volume.CountX, Volume.CountY, Volume.CountZ);
}

uint3 DdgiGetProbeCoords(FDdgiVolume Volume, uint Local)
{
	return uint3(Local % Volume.CountX, (Local / Volume.CountX) % Volume.CountY, Local / (Volume.CountX * Volume.CountY));
}

uint DdgiGetProbeCount(FDdgiVolume Volume)
{
	return Volume.CountX * Volume.CountY * Volume.CountZ;
}

uint2 DdgiGetTileOrigin(uint Probe, uint TileSize)
{
	return uint2(Probe % E_DDGI_TILES_PER_ROW, Probe / E_DDGI_TILES_PER_ROW) * TileSize;
}

uint2 DdgiGetDataTexel(uint Probe)
{
	return uint2(Probe % E_DDGI_TILES_PER_ROW, Probe / E_DDGI_TILES_PER_ROW);
}

// 전체 번호 → 볼륨 번호 (볼륨은 ProbeOffset 오름차순으로 붙어 있다)
uint DdgiFindVolume(uint Probe)
{
	uint Found = 0;
	for (uint Index = 1; Index < DdgiVolumeCount; ++Index)
	{
		if (Probe >= DdgiVolumes[Index].ProbeOffset)
		{
			Found = Index;
		}
	}
	return Found;
}

// 프로브 격자 위치 (재배치 전)
float3 DdgiGetProbeBasePosition(FDdgiVolume Volume, uint3 Coords)
{
	return Volume.Origin + float3(Coords) * Volume.Spacing;
}

// 이번 갱신 대상인가 (DdgiMath::IsProbeUpdated) + 추적 행
bool DdgiIsProbeUpdated(FDdgiVolume Volume, uint Local, out uint Row)
{
	const uint Count = DdgiGetProbeCount(Volume);
	const uint K     = (Local + Count - Volume.UpdateStart % Count) % Count;
	Row              = Volume.RowOffset + K;
	return K < Volume.UpdateCount;
}

// 체비셰프 가시성 (DdgiMath::ComputeVisibility)
float DdgiComputeVisibility(float Distance, float Mean, float MeanSquared)
{
	if (Distance <= Mean)
	{
		return 1.0f;
	}
	const float Variance = abs(MeanSquared - Mean * Mean);
	const float Delta    = Distance - Mean;
	const float Bound    = Variance / max(Variance + Delta * Delta, 1.0e-8f);
	return max(Bound * Bound * Bound, 0.0f);
}

// 프로브 가중치 (DdgiMath::ComputeProbeWeight)
float DdgiComputeProbeWeight(float3 DirectionToProbe, float3 N, float Trilinear, float Visibility)
{
	const float Wrap   = (dot(DirectionToProbe, N) + 1.0f) * 0.5f;
	float       Weight = Wrap * Wrap + 0.2f;
	Weight *= max(Visibility, 0.05f);
	Weight = max(Weight, 1.0e-6f);
	if (Weight < 0.2f)
	{
		Weight *= Weight * Weight / (0.2f * 0.2f);
	}
	return Weight * Trilinear;
}

// 경계 페이드 (DdgiMath::ComputeEdgeFade)
float DdgiComputeEdgeFade(float3 P, float3 BoxMin, float3 BoxMax, float FadeDistance)
{
	const float3 Inside = min(P - BoxMin, BoxMax - P);
	return saturate(min(Inside.x, min(Inside.y, Inside.z)) / max(FadeDistance, 1.0e-3f));
}

// 타일 안 팔면체 좌표 → 아틀라스 UV (테두리 1칸, 쌍선형)
float2 DdgiGetAtlasUV(uint Probe, float3 Direction, uint Texels, float2 TexelSize)
{
	const uint2  Origin = DdgiGetTileOrigin(Probe, Texels + 2);
	const float2 Coord  = (DdgiOctEncode(Direction) * 0.5f + 0.5f) * (float)Texels;
	return (float2(Origin) + 1.0f + Coord) * TexelSize;
}

// 볼륨 하나의 조도/π (가중 평균, 세기·페이드 미적용). 반환 w = 쓸 수 있는 프로브 가중치 합 (0이면 볼륨 정보 없음)
float4 DdgiSampleVolume(FDdgiVolume Volume, float3 P, float3 N, float3 V)
{
	const float3 Biased = P + N * Volume.NormalBias + V * Volume.ViewBias;
	const uint3  Counts = DdgiGetCounts(Volume);
	const float3 Coord  = (Biased - Volume.Origin) / Volume.Spacing;
	const float3 Base   = clamp(floor(Coord), 0.0f, float3(Counts) - 2.0f); // DdgiMath::FindBaseProbe
	const float3 Alpha  = saturate(Coord - Base);

	float3 Irradiance = 0.0f;
	float  WeightSum  = 0.0f;
	[unroll]
	for (uint Corner = 0; Corner < 8; ++Corner)
	{
		const uint3  Offset = uint3(Corner & 1, (Corner >> 1) & 1, Corner >> 2);
		const uint3  Cell   = uint3(Base) + Offset;
		const uint   Probe  = Volume.ProbeOffset + Cell.x + Counts.x * (Cell.y + Counts.y * Cell.z);
		const float4 Data   = DdgiProbeData.Load(int3(DdgiGetDataTexel(Probe), 0));
		if (DdgiGetProbeState(Data.w) != E_DDGI_STATE_ACTIVE)
		{
			continue; // 아직 없음 / 비활성 (벽 속)
		}
		const float3 ProbePosition = DdgiGetProbeBasePosition(Volume, Cell) + Data.xyz;
		const float3 ToProbe       = ProbePosition - Biased;
		const float  Distance      = max(length(ToProbe), 1.0e-3f);
		const float3 Direction     = ToProbe / Distance;
		const float3 Trilinear3    = lerp(1.0f - Alpha, Alpha, float3(Offset));
		const float2 Moments       = DdgiDistanceAtlas.SampleLevel(E_DDGI_SAMPLER, DdgiGetAtlasUV(Probe, -Direction, E_DDGI_DISTANCE_TEXELS, DdgiDistanceTexelSize), 0);
		const float  Visibility    = DdgiComputeVisibility(Distance / Volume.DistanceClamp, Moments.x, Moments.y);
		const float  Weight        = DdgiComputeProbeWeight(Direction, N, Trilinear3.x * Trilinear3.y * Trilinear3.z, Visibility);
		Irradiance += DdgiIrradianceAtlas.SampleLevel(E_DDGI_SAMPLER, DdgiGetAtlasUV(Probe, N, E_DDGI_IRRADIANCE_TEXELS, DdgiIrradianceTexelSize), 0).rgb * Weight;
		WeightSum += Weight;
	}
	return float4(WeightSum > 1.0e-6f ? Irradiance / WeightSum : 0.0f, WeightSum);
}

// 볼륨 상자들이 덮지 않는 비중 (= Π(1 - 페이드)) — 프로브 광선 히트가 하늘 IBL 조도를 쓰는 비중 (프로브 값이 아직 없어도 볼륨 안은 하늘을 쓰지 않는다)
float DdgiComputeUncovered(float3 P)
{
	float Remaining = 1.0f;
	for (uint Index = 0; Index < DdgiVolumeCount; ++Index)
	{
		const FDdgiVolume Volume = DdgiVolumes[Index];
		const float3      BoxMin = Volume.Origin - Volume.Spacing * 0.5f;
		const float3      BoxMax = Volume.Origin + (float3(DdgiGetCounts(Volume)) - 0.5f) * Volume.Spacing;
		Remaining *= 1.0f - DdgiComputeEdgeFade(P, BoxMin, BoxMax, Volume.FadeDistance);
	}
	return Remaining;
}

// 모든 볼륨의 간접 확산 조도/π (세기·페이드 적용, 우선순위 순으로 남은 비중을 채움). OutRemaining = 하늘 IBL 조도로 채울 비중
float3 EvaluateDdgiIrradiance(float3 P, float3 N, float3 V, out float OutRemaining)
{
	float3 Color     = 0.0f;
	float  Remaining = 1.0f;
	for (uint Index = 0; Index < DdgiVolumeCount && Remaining > 0.001f; ++Index)
	{
		const FDdgiVolume Volume = DdgiVolumes[Index];
		// 셰이딩·페이드 상자 = 격자 바깥 프로브에서 반 칸씩 더 (= 컴포넌트 상자, DdgiMath::FProbeGrid::GetBoxMin/Max)
		const float3      BoxMin = Volume.Origin - Volume.Spacing * 0.5f;
		const float3      BoxMax = Volume.Origin + (float3(DdgiGetCounts(Volume)) - 0.5f) * Volume.Spacing;
		const float       Fade   = DdgiComputeEdgeFade(P, BoxMin, BoxMax, Volume.FadeDistance);
		if (Fade <= 0.0f)
		{
			continue;
		}
		const float4 Sample = DdgiSampleVolume(Volume, P, N, V);
		if (Sample.w <= 1.0e-6f)
		{
			continue; // 주변 프로브가 모두 비활성/아직 없음 → 다음 볼륨 또는 하늘
		}
		Color += Sample.rgb * (Volume.Intensity * Fade * Remaining);
		Remaining *= 1.0f - Fade;
	}
	OutRemaining = Remaining;
	return Color;
}

#endif // E_DDGI_COMMON_HLSLI
