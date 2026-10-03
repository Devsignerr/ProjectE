// DDGI 프로브 누적 (Phase 51, FDdgiRenderer — FScreenPassRootSignature 계산): 이전 장 → 이번 장 (이력 2장 핑퐁)
//   CSIrradiance (그룹 = 프로브 하나 = 10x10 타일) / CSDistance (18x18 타일) / CSProbeData (스레드 = 프로브 하나: 재배치 오프셋 + 분류)
//   갱신 대상 프로브: 추적 행의 광선(휘도·거리)과 방향을 그룹 공유 메모리에 한 번 올린 뒤 텍셀마다 가중 합 → 이전 값과 히스테리시스 섞기.
//   아니면 이전 값 복사. 테두리 텍셀은 접힌 이웃 안쪽 텍셀과 같은 값을 직접 계산한다 (DdgiMath::MapBorderTexel — 따로 복사 패스 없음)
// 바인딩: b0 DDGI 상수, t0 광선 결과 (DdgiTrace.hlsl), t1~t3 이전 아틀라스 (조도/거리/상태), u0 이번 장, s0 선형 클램프

SamplerState DdgiLinearClamp : register(s0);
#define E_DDGI_CONSTANTS_REGISTER b0
#define E_DDGI_IRRADIANCE_REGISTER t1
#define E_DDGI_DISTANCE_REGISTER t2
#define E_DDGI_PROBE_DATA_REGISTER t3
#define E_DDGI_SAMPLER DdgiLinearClamp
#include "DdgiCommon.hlsli"

#define E_DDGI_MAX_RAYS 512 // DdgiMath::MaxRaysPerProbe

Texture2D<float4>   RayData        : register(t0);
RWTexture2D<float4> OutputAtlas    : register(u0);
RWTexture2D<float2> OutputDistance : register(u1);

groupshared float4 SharedRays[E_DDGI_MAX_RAYS];       // rgb 휘도, a 거리 (뒷면 음수)
groupshared float3 SharedDirections[E_DDGI_MAX_RAYS];

// 테두리 → 안쪽 텍셀 (DdgiMath::MapBorderTexel, 타일 좌표 0..Texels+1)
uint2 DdgiMapBorderTexel(uint2 T, uint Texels)
{
	const uint Last    = Texels + 1;
	const bool bLeft   = T.x == 0;
	const bool bRight  = T.x == Last;
	const bool bTop    = T.y == 0;
	const bool bBottom = T.y == Last;
	if ((bLeft || bRight) && (bTop || bBottom))
	{
		return uint2(bLeft ? Texels : 1u, bTop ? Texels : 1u);
	}
	if (bTop || bBottom)
	{
		return uint2(Last - T.x, bTop ? 1u : Texels);
	}
	if (bLeft || bRight)
	{
		return uint2(bLeft ? 1u : Texels, Last - T.y);
	}
	return T;
}

// 히스테리시스 (DdgiMath::ComputeHysteresis): 처음 n번은 누적 평균 상한 n/(n+1)
float ComputeHysteresis(float3 Previous, float3 Current, float Base, float ChangeThreshold, bool bFirst, uint UpdateCount)
{
	if (bFirst)
	{
		return 0.0f;
	}
	Base = min(Base, (float)UpdateCount / (float)(UpdateCount + 1u));
	const float3 Delta    = abs(Current - Previous);
	const float  MaxDelta = max(Delta.x, max(Delta.y, Delta.z));
	const float  Scale    = max(max(max(Previous.x, Previous.y), Previous.z), max(max(Current.x, Current.y), Current.z));
	if (MaxDelta > ChangeThreshold * max(Scale, 1.0e-3f))
	{
		return max(Base - 0.75f, 0.0f);
	}
	return Base;
}

// 그룹(= 프로브 하나) 공통: 볼륨, 갱신 여부·행, 처음 여부, 갱신 횟수
struct FProbeInfo
{
	FDdgiVolume Volume;
	bool        bUpdated;
	uint        Row;
	bool        bFirst;
	uint        UpdateCount;
};

FProbeInfo ResolveProbe(uint Probe)
{
	FProbeInfo Info;
	Info.Volume       = DdgiVolumes[DdgiFindVolume(Probe)];
	Info.bUpdated     = DdgiIsProbeUpdated(Info.Volume, Probe - Info.Volume.ProbeOffset, Info.Row);
	const float StateW = DdgiProbeData.Load(int3(DdgiGetDataTexel(Probe), 0)).w;
	Info.bFirst       = DdgiReset != 0 || DdgiGetProbeState(StateW) == E_DDGI_STATE_UNINITIALIZED;
	Info.UpdateCount  = Info.bFirst ? 0u : DdgiGetProbeUpdateCount(StateW);
	return Info;
}

// 추적 행 광선 → 그룹 공유 메모리 (모든 스레드가 함께, 뒤에 그룹 동기화)
void LoadRays(FProbeInfo Info, uint ThreadIndex, uint ThreadCount)
{
	for (uint Ray = ThreadIndex; Ray < Info.Volume.RaysPerProbe; Ray += ThreadCount)
	{
		SharedRays[Ray]       = RayData.Load(int3(Ray, Info.Row, 0));
		SharedDirections[Ray] = DdgiGetRayDirection(Ray, Info.Volume.RaysPerProbe, Info.Volume.FixedRays);
	}
}

[numthreads(10, 10, 1)]
void CSIrradiance(uint3 GroupId : SV_GroupID, uint3 ThreadId : SV_GroupThreadID, uint ThreadIndex : SV_GroupIndex)
{
	const uint Probe = GroupId.x;
	if (Probe >= DdgiTotalProbes || DdgiVolumeCount == 0)
	{
		return; // 그룹 전체가 같은 판정 (동기화 전에 같이 빠진다)
	}
	const FProbeInfo Info     = ResolveProbe(Probe);
	const uint2      Origin   = DdgiGetTileOrigin(Probe, E_DDGI_IRRADIANCE_TEXELS + 2);
	const uint2      Mapped   = DdgiMapBorderTexel(ThreadId.xy, E_DDGI_IRRADIANCE_TEXELS);
	const float3     Previous = Info.bFirst ? 0.0f : DdgiIrradianceAtlas.Load(int3(Origin + Mapped, 0)).rgb;
	if (!Info.bUpdated)
	{
		OutputAtlas[Origin + ThreadId.xy] = float4(Previous, 1.0f);
		return;
	}
	LoadRays(Info, ThreadIndex, 100);
	GroupMemoryBarrierWithGroupSync();

	// 코사인 가중 평균 휘도 = 조도 / π (뒷면 히트는 뺀다 — 벽 속 방향이 조도를 깎지 않게)
	const float3 Direction = DdgiOctDecode((float2(Mapped - 1) + 0.5f) / (float)E_DDGI_IRRADIANCE_TEXELS * 2.0f - 1.0f);
	float3       Sum       = 0.0f;
	float        WeightSum = 0.0f;
	for (uint Ray = Info.Volume.FixedRays; Ray < Info.Volume.RaysPerProbe; ++Ray)
	{
		const float4 Sample = SharedRays[Ray];
		if (Sample.a < 0.0f)
		{
			continue;
		}
		const float Weight = max(dot(Direction, SharedDirections[Ray]), 0.0f);
		Sum += Sample.rgb * Weight;
		WeightSum += Weight;
	}
	float3 Result = Previous;
	if (WeightSum > 1.0e-5f) // 이 방향 반구에 앞면 히트·하늘이 하나도 없으면(벽 속) 이전 값
	{
		const float3 Current    = Sum / WeightSum;
		const float  Hysteresis = ComputeHysteresis(Previous, Current, Info.Volume.Hysteresis, Info.Volume.ChangeThreshold, Info.bFirst, Info.UpdateCount);
		Result                  = Current + (Previous - Current) * Hysteresis;
	}
	OutputAtlas[Origin + ThreadId.xy] = float4(Result, 1.0f);
}

[numthreads(18, 18, 1)]
void CSDistance(uint3 GroupId : SV_GroupID, uint3 ThreadId : SV_GroupThreadID, uint ThreadIndex : SV_GroupIndex)
{
	const uint Probe = GroupId.x;
	if (Probe >= DdgiTotalProbes || DdgiVolumeCount == 0)
	{
		return;
	}
	const FProbeInfo Info     = ResolveProbe(Probe);
	const uint2      Origin   = DdgiGetTileOrigin(Probe, E_DDGI_DISTANCE_TEXELS + 2);
	const uint2      Mapped   = DdgiMapBorderTexel(ThreadId.xy, E_DDGI_DISTANCE_TEXELS);
	const float2     Previous = Info.bFirst ? 0.0f : DdgiDistanceAtlas.Load(int3(Origin + Mapped, 0));
	if (!Info.bUpdated)
	{
		OutputDistance[Origin + ThreadId.xy] = Previous;
		return;
	}
	LoadRays(Info, ThreadIndex, 324);
	GroupMemoryBarrierWithGroupSync();

	// 거리 모멘트: cos^50 가중 (평균 거리, 제곱 평균) — 뒷면 히트는 줄인 거리 그대로(누수 방지), 정규화 = DistanceClamp
	const float3 Direction = DdgiOctDecode((float2(Mapped - 1) + 0.5f) / (float)E_DDGI_DISTANCE_TEXELS * 2.0f - 1.0f);
	float2       Sum       = 0.0f;
	float        WeightSum = 0.0f;
	for (uint Ray = Info.Volume.FixedRays; Ray < Info.Volume.RaysPerProbe; ++Ray)
	{
		const float Cosine = dot(Direction, SharedDirections[Ray]);
		if (Cosine <= 0.85f)
		{
			continue; // cos^50 < 3e-4 (DdgiMath::DistanceMinCosine) — 대부분의 광선을 건너뛴다
		}
		const float Weight   = pow(Cosine, 50.0f);
		const float Distance = min(abs(SharedRays[Ray].a), Info.Volume.DistanceClamp) / Info.Volume.DistanceClamp;
		Sum += float2(Distance, Distance * Distance) * Weight;
		WeightSum += Weight;
	}
	float2 Result = Previous;
	if (WeightSum > 1.0e-6f)
	{
		const float2 Current    = Sum / WeightSum;
		const float  Hysteresis = Info.bFirst ? 0.0f : min(Info.Volume.Hysteresis, (float)Info.UpdateCount / (float)(Info.UpdateCount + 1u));
		Result                  = Current + (Previous - Current) * Hysteresis;
	}
	OutputDistance[Origin + ThreadId.xy] = Result;
}

// 프로브 상태: 고정 광선(회전 없음)으로 재배치 + 분류 (RTXGI ProbeRelocation/Classification 방식, cm)
[numthreads(64, 1, 1)]
void CSProbeData(uint3 DispatchId : SV_DispatchThreadID)
{
	const uint Probe = DispatchId.x;
	if (Probe >= DdgiTotalProbes || DdgiVolumeCount == 0)
	{
		return;
	}
	const uint2       Pixel        = DdgiGetDataTexel(Probe);
	const float4      PreviousData = DdgiReset != 0 ? 0.0f : DdgiProbeData.Load(int3(Pixel, 0));
	const FDdgiVolume Volume       = DdgiVolumes[DdgiFindVolume(Probe)];
	uint              Row;
	if (!DdgiIsProbeUpdated(Volume, Probe - Volume.ProbeOffset, Row))
	{
		OutputAtlas[Pixel] = PreviousData;
		return;
	}
	float3     Offset      = PreviousData.xyz;
	const uint UpdateCount = (DdgiReset != 0 || DdgiGetProbeState(PreviousData.w) == E_DDGI_STATE_UNINITIALIZED ? 0u : DdgiGetProbeUpdateCount(PreviousData.w)) + 1u;
	if (Volume.FixedRays == 0)
	{
		OutputAtlas[Pixel] = float4(Offset, DdgiEncodeProbeState(E_DDGI_STATE_ACTIVE, UpdateCount));
		return;
	}

	uint   Backfaces            = 0;
	float  ClosestBackface      = 1.0e27f;
	float3 ClosestBackfaceDir   = 0.0f;
	float  ClosestFrontface     = 1.0e27f;
	float3 ClosestFrontfaceDir  = 0.0f;
	float  FarthestFrontface    = 0.0f;
	float3 FarthestFrontfaceDir = 0.0f;
	for (uint Ray = 0; Ray < Volume.FixedRays; ++Ray)
	{
		const float  Distance  = RayData.Load(int3(Ray, Row, 0)).a;
		const float3 Direction = DdgiGetRayDirection(Ray, Volume.RaysPerProbe, Volume.FixedRays);
		if (Distance < 0.0f)
		{
			++Backfaces;
			const float Actual = -Distance * 5.0f; // 저장할 때 0.2배
			if (Actual < ClosestBackface)
			{
				ClosestBackface    = Actual;
				ClosestBackfaceDir = Direction;
			}
			continue;
		}
		if (Distance < ClosestFrontface)
		{
			ClosestFrontface    = Distance;
			ClosestFrontfaceDir = Direction;
		}
		if (Distance > FarthestFrontface)
		{
			FarthestFrontface    = Distance;
			FarthestFrontfaceDir = Direction;
		}
	}
	const float BackfaceRatio = (float)Backfaces / (float)Volume.FixedRays;

	if ((Volume.Flags & E_DDGI_FLAG_RELOCATION) != 0)
	{
		float3 FullOffset = Offset;
		if (BackfaceRatio > Volume.BackfaceThreshold)
		{
			// 벽 속: 가장 가까운 뒷면 너머(벽 밖)로
			FullOffset = Offset + ClosestBackfaceDir * (ClosestBackface + Volume.MinFrontfaceDistance * 0.5f);
		}
		else if (ClosestFrontface < Volume.MinFrontfaceDistance)
		{
			// 벽에 너무 가까움: 가장 먼 앞면 쪽으로 (반대 방향일 때만 — 좁은 틈에서 왕복 방지)
			if (dot(ClosestFrontfaceDir, FarthestFrontfaceDir) <= 0.0f)
			{
				FullOffset = Offset + FarthestFrontfaceDir * min(FarthestFrontface, Volume.MinFrontfaceDistance);
			}
		}
		else if (ClosestFrontface > Volume.MinFrontfaceDistance + 1.0f && dot(Offset, Offset) > 1.0e-4f)
		{
			// 여유가 있으면 격자 자리로 되돌아간다
			const float MoveBack = min(ClosestFrontface - Volume.MinFrontfaceDistance, length(Offset));
			FullOffset           = Offset - normalize(Offset) * MoveBack;
		}
		const float3 Normalized = abs(FullOffset / Volume.Spacing);
		if (max(Normalized.x, max(Normalized.y, Normalized.z)) <= 0.45f)
		{
			Offset = FullOffset;
		}
	}
	else
	{
		Offset = 0.0f;
	}
	uint State = E_DDGI_STATE_ACTIVE;
	if ((Volume.Flags & E_DDGI_FLAG_CLASSIFICATION) != 0 && BackfaceRatio > Volume.BackfaceThreshold)
	{
		State = E_DDGI_STATE_INACTIVE;
	}
	OutputAtlas[Pixel] = float4(Offset, DdgiEncodeProbeState(State, UpdateCount));
}
