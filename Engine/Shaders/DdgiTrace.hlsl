#include "RayTracingCommon.hlsli"
#include "Fullscreen.hlsli"

// DDGI 프로브 광선 추적 (Phase 51, FDdgiRenderer — 루트는 FRayTracingPassRoot, 패턴은 RayTracedReflections.hlsl).
//   타깃 = "광선 × 갱신 프로브" 2D (가로 = 광선 번호, 세로 = 이번 프레임 갱신 행, RGBA32F): rgb = 휘도, a = 히트 거리(cm)
//   (뒷면 히트 = -거리 × 0.2 — 재배치·분류·누수 방지, 빗나감 = 큰 값). 광선 원점 = 프로브 격자 위치 + 재배치 오프셋(이전 아틀라스)
//   히트 조명 = RayTracingLighting.hlsli EvaluateHitLighting (방향광 + RT 그림자 광선, 로컬 라이트, 반사 IBL/캡처, 발광) +
//   간접 확산 = 이전 프레임 프로브 조도(E_RT_HIT_DIFFUSE_IRRADIANCE → 무한 반사 근사), 빗나감 = 하늘 프리필터 밉 0 (SampleSkyRadiance)
// 바인딩: b0 DDGI 상수, b1 히트 조명 상수, t9~t11 이전 아틀라스 (조도/거리/상태), 나머지는 FRayTracingPassRoot 그대로

#define E_DDGI_CONSTANTS_REGISTER b0
#define E_DDGI_IRRADIANCE_REGISTER t9
#define E_DDGI_DISTANCE_REGISTER t10
#define E_DDGI_PROBE_DATA_REGISTER t11
#define E_DDGI_SAMPLER RtClampSampler
#include "DdgiCommon.hlsli"

// 히트 간접 확산 = 이전 프로브 조도 (볼륨 밖 = 하늘 조도 × 하늘 배율). 정의는 RayTracingLighting.hlsli 뒤
//   볼륨 안인데 프로브 값이 아직 없으면(처음·벽 속) 0 — 실내 히트가 하늘 조도로 빛나 처음 몇십 프레임 밝게 떠 있다 천천히 가라앉는 것을 막는다
float3 DdgiHitIrradiance(float3 Position, float3 N, float3 V);
#define E_RT_HIT_DIFFUSE_IRRADIANCE(Hit, Surface) DdgiHitIrradiance(Hit.Position, Surface.N, Surface.V)
#include "RayTracingLighting.hlsli"

float3 DdgiHitIrradiance(float3 Position, float3 N, float3 V)
{
	float        Remaining = 1.0f;
	const float3 Probes    = DdgiReset != 0 ? 0.0f : EvaluateDdgiIrradiance(Position, N, V, Remaining);
	return Probes * DdgiBounceIntensity + RtIblDiffuse.SampleLevel(RtClampSampler, N, 0).rgb * (RtAmbientIntensity * DdgiComputeUncovered(Position));
}

FFullscreenVSOutput VSMain(uint VertexId : SV_VertexID)
{
	return FullscreenVS(VertexId);
}

float4 PSTrace(FFullscreenVSOutput Input) : SV_Target
{
	const uint2 Pixel = uint2(Input.Position.xy);
	// 이 행의 볼륨 (행은 볼륨 순서대로 이어 붙어 있다)
	uint VolumeIndex = 0;
	for (uint Index = 1; Index < DdgiVolumeCount; ++Index)
	{
		if (Pixel.y >= DdgiVolumes[Index].RowOffset)
		{
			VolumeIndex = Index;
		}
	}
	const FDdgiVolume Volume = DdgiVolumes[VolumeIndex];
	if (Pixel.x >= Volume.RaysPerProbe || Pixel.y - Volume.RowOffset >= Volume.UpdateCount)
	{
		return 0.0f;
	}
	const uint  ProbeCount = DdgiGetProbeCount(Volume);
	const uint  Local      = (Volume.UpdateStart + (Pixel.y - Volume.RowOffset)) % ProbeCount;
	const uint  Probe      = Volume.ProbeOffset + Local;
	const float3 Offset    = DdgiReset != 0 ? 0.0f : DdgiProbeData.Load(int3(DdgiGetDataTexel(Probe), 0)).xyz;
	const float3 Direction = DdgiGetRayDirection(Pixel.x, Volume.RaysPerProbe, Volume.FixedRays);

	RayDesc Ray;
	Ray.Origin    = DdgiGetProbeBasePosition(Volume, DdgiGetProbeCoords(Volume, Local)) + Offset;
	Ray.Direction = Direction;
	Ray.TMin      = 0.0f;
	Ray.TMax      = Volume.MaxRayDistance;
	const FRayHit Hit = TraceClosestHit(Ray, E_RT_MASK_TYPES);
	if (!Hit.bHit)
	{
		return float4(SampleSkyRadiance(Direction, 0.0f), 1.0e27f);
	}
	// 뒷면 (양면 머티리얼은 앞면으로 본다): 프로브가 벽 속/뒤에 있다
	const bool bTwoSided = (RtInstances[Hit.Instance].Flags & E_RT_INFO_TWO_SIDED) != 0;
	if (!Hit.bFrontFace && !bTwoSided)
	{
		return float4(0.0f, 0.0f, 0.0f, -Hit.T * 0.2f);
	}
	// 텍스처 LOD 원뿔: 광선 사이 각 간격 ≈ √(4π / 광선 수)
	const float       ConeWidth = Hit.T * sqrt(4.0f * 3.14159265f / (float)Volume.RaysPerProbe);
	const FHitSurface Surface   = LoadHitSurface(Hit, ConeWidth, -Direction);
	return float4(EvaluateHitLighting(Surface, -Direction), Hit.T);
}
