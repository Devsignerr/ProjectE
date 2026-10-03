#include "Fullscreen.hlsli"

// DDGI 프로브 누적 (Phase 51, FDdgiRenderer — FScreenPassRootSignature): 아틀라스 전체를 그리는 패스 3개 (이력 2장 핑퐁)
//   PSIrradiance (조도 아틀라스) / PSDistance (거리 아틀라스) / PSProbeData (프로브 상태: 재배치 오프셋 + 분류)
//   텍셀 → 프로브 (타일) → 이번 프레임 갱신 대상이면 추적 행의 광선을 모아 새 값 → 이전 값과 히스테리시스 섞기, 아니면 이전 값 복사.
//   테두리 텍셀은 접힌 이웃 안쪽 텍셀과 같은 값을 직접 계산한다 (DdgiMath::MapBorderTexel — 따로 복사 패스 없음)
// 바인딩: b0 DDGI 상수, t0 광선 결과 (DdgiTrace.hlsl), t1~t3 이전 아틀라스 (조도/거리/상태), s0 선형 클램프

SamplerState DdgiLinearClamp : register(s0);
#define E_DDGI_CONSTANTS_REGISTER b0
#define E_DDGI_IRRADIANCE_REGISTER t1
#define E_DDGI_DISTANCE_REGISTER t2
#define E_DDGI_PROBE_DATA_REGISTER t3
#define E_DDGI_SAMPLER DdgiLinearClamp
#include "DdgiCommon.hlsli"

Texture2D<float4> RayData : register(t0);

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

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

// 텍셀 공용 해석: 프로브 번호, 볼륨, 안쪽 텍셀 (0..Texels-1), 아틀라스 안 안쪽 텍셀 위치, 갱신 여부·행, 처음 여부
struct FTexelInfo
{
	bool        bValid;
	uint        Probe;
	FDdgiVolume Volume;
	uint2       Interior;      // 0..Texels-1
	int2        InteriorPixel; // 아틀라스 좌표 (이전 값 읽기)
	bool        bUpdated;
	uint        Row;
	bool        bFirst;        // 이전 값 없음 (초기화 / 아직 한 번도 갱신 안 됨)
	uint        UpdateCount;   // 지금까지 갱신 횟수 (처음 몇 번은 누적 평균 — 수렴 가속)
};

FTexelInfo ResolveTexel(uint2 Pixel, uint Texels)
{
	FTexelInfo Info = (FTexelInfo)0;
	const uint TileSize = Texels + 2;
	const uint2 Tile    = Pixel / TileSize;
	Info.Probe          = Tile.x + Tile.y * E_DDGI_TILES_PER_ROW;
	Info.bValid         = Tile.x < E_DDGI_TILES_PER_ROW && Info.Probe < DdgiTotalProbes && DdgiVolumeCount != 0;
	if (!Info.bValid)
	{
		return Info;
	}
	const uint2 Mapped = DdgiMapBorderTexel(Pixel - Tile * TileSize, Texels);
	Info.Interior      = Mapped - 1;
	Info.InteriorPixel = int2(Tile * TileSize + Mapped);
	Info.Volume        = DdgiVolumes[DdgiFindVolume(Info.Probe)];
	Info.bUpdated      = DdgiIsProbeUpdated(Info.Volume, Info.Probe - Info.Volume.ProbeOffset, Info.Row);
	const float StateW = DdgiProbeData.Load(int3(DdgiGetDataTexel(Info.Probe), 0)).w;
	Info.bFirst        = DdgiReset != 0 || DdgiGetProbeState(StateW) == E_DDGI_STATE_UNINITIALIZED;
	Info.UpdateCount   = Info.bFirst ? 0u : DdgiGetProbeUpdateCount(StateW);
	return Info;
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

float4 PSIrradiance(FFullscreenVSOutput Input) : SV_Target
{
	const FTexelInfo Info = ResolveTexel(uint2(Input.Position.xy), E_DDGI_IRRADIANCE_TEXELS);
	if (!Info.bValid)
	{
		return 0.0f;
	}
	const float3 Previous = Info.bFirst ? 0.0f : DdgiIrradianceAtlas.Load(int3(Info.InteriorPixel, 0)).rgb;
	if (!Info.bUpdated)
	{
		return float4(Previous, 1.0f);
	}
	// 코사인 가중 평균 휘도 = 조도 / π (뒷면 히트는 뺀다 — 벽 속 방향이 조도를 깎지 않게)
	const float3 Direction = DdgiOctDecode((float2(Info.Interior) + 0.5f) / (float)E_DDGI_IRRADIANCE_TEXELS * 2.0f - 1.0f);
	float3       Sum       = 0.0f;
	float        WeightSum = 0.0f;
	for (uint Ray = Info.Volume.FixedRays; Ray < Info.Volume.RaysPerProbe; ++Ray)
	{
		const float4 Sample = RayData.Load(int3(Ray, Info.Row, 0));
		if (Sample.a < 0.0f)
		{
			continue;
		}
		const float Weight = max(dot(Direction, DdgiGetRayDirection(Ray, Info.Volume.RaysPerProbe, Info.Volume.FixedRays)), 0.0f);
		Sum += Sample.rgb * Weight;
		WeightSum += Weight;
	}
	if (WeightSum <= 1.0e-5f)
	{
		return float4(Previous, 1.0f); // 이 방향 반구에 앞면 히트·하늘이 하나도 없음 (벽 속)
	}
	const float3 Current    = Sum / WeightSum;
	const float  Hysteresis = ComputeHysteresis(Previous, Current, Info.Volume.Hysteresis, Info.Volume.ChangeThreshold, Info.bFirst, Info.UpdateCount);
	return float4(Current + (Previous - Current) * Hysteresis, 1.0f);
}

float2 PSDistance(FFullscreenVSOutput Input) : SV_Target
{
	const FTexelInfo Info = ResolveTexel(uint2(Input.Position.xy), E_DDGI_DISTANCE_TEXELS);
	if (!Info.bValid)
	{
		return 0.0f;
	}
	const float2 Previous = Info.bFirst ? 0.0f : DdgiDistanceAtlas.Load(int3(Info.InteriorPixel, 0));
	if (!Info.bUpdated)
	{
		return Previous;
	}
	// 거리 모멘트: cos^50 가중 (평균 거리, 제곱 평균) — 뒷면 히트는 줄인 거리 그대로(누수 방지), 정규화 = DistanceClamp
	const float3 Direction = DdgiOctDecode((float2(Info.Interior) + 0.5f) / (float)E_DDGI_DISTANCE_TEXELS * 2.0f - 1.0f);
	float2       Sum       = 0.0f;
	float        WeightSum = 0.0f;
	for (uint Ray = Info.Volume.FixedRays; Ray < Info.Volume.RaysPerProbe; ++Ray)
	{
		const float Cosine = max(dot(Direction, DdgiGetRayDirection(Ray, Info.Volume.RaysPerProbe, Info.Volume.FixedRays)), 0.0f);
		if (Cosine <= 0.0f)
		{
			continue;
		}
		const float Weight   = pow(Cosine, 50.0f);
		const float Distance = min(abs(RayData.Load(int3(Ray, Info.Row, 0)).a), Info.Volume.DistanceClamp) / Info.Volume.DistanceClamp;
		Sum += float2(Distance, Distance * Distance) * Weight;
		WeightSum += Weight;
	}
	if (WeightSum <= 1.0e-6f)
	{
		return Previous;
	}
	const float2 Current    = Sum / WeightSum;
	const float  Hysteresis = Info.bFirst ? 0.0f : min(Info.Volume.Hysteresis, (float)Info.UpdateCount / (float)(Info.UpdateCount + 1u));
	return Current + (Previous - Current) * Hysteresis;
}

// 프로브 상태: 고정 광선(회전 없음)으로 재배치 + 분류 (RTXGI ProbeRelocation/Classification 방식, cm)
float4 PSProbeData(FFullscreenVSOutput Input) : SV_Target
{
	const uint2 Pixel = uint2(Input.Position.xy);
	const uint  Probe = Pixel.x + Pixel.y * E_DDGI_TILES_PER_ROW;
	if (Pixel.x >= E_DDGI_TILES_PER_ROW || Probe >= DdgiTotalProbes || DdgiVolumeCount == 0)
	{
		return 0.0f;
	}
	const float4      PreviousData = DdgiReset != 0 ? 0.0f : DdgiProbeData.Load(int3(Pixel, 0));
	const FDdgiVolume Volume       = DdgiVolumes[DdgiFindVolume(Probe)];
	uint              Row;
	if (!DdgiIsProbeUpdated(Volume, Probe - Volume.ProbeOffset, Row))
	{
		return PreviousData;
	}
	float3     Offset      = PreviousData.xyz;
	const uint UpdateCount = (DdgiReset != 0 || DdgiGetProbeState(PreviousData.w) == E_DDGI_STATE_UNINITIALIZED ? 0u : DdgiGetProbeUpdateCount(PreviousData.w)) + 1u;
	if (Volume.FixedRays == 0)
	{
		return float4(Offset, DdgiEncodeProbeState(E_DDGI_STATE_ACTIVE, UpdateCount));
	}

	uint   Backfaces           = 0;
	float  ClosestBackface     = 1.0e27f;
	float3 ClosestBackfaceDir  = 0.0f;
	float  ClosestFrontface    = 1.0e27f;
	float3 ClosestFrontfaceDir = 0.0f;
	float  FarthestFrontface   = 0.0f;
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
	return float4(Offset, DdgiEncodeProbeState(State, UpdateCount));
}
