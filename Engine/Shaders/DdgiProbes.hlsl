// DDGI 프로브 구 디버그 표시 (Phase 51, FDdgiRenderer::AddProbeDebugPass — FScreenPassRootSignature, 씬 컬러 + 깊이 테스트·쓰기).
//   인스턴스 = 아틀라스 전체 프로브 번호, 정점 = SV_VertexID로 만든 UV 구 (16 x 8 사각형 = 768 정점). 볼륨 DebugProbes:
//   1 조도 (구 법선 방향 프로브 조도/π = 흰 표면의 확산 반사 휘도), 2 거리 (평균 / DistanceClamp 회색), 3 상태 (초록 활성 / 빨강 비활성 / 파랑 아직 없음)
// 바인딩: b0 DDGI 상수, t1~t3 이번 프레임 아틀라스 (조도/거리/상태), s0 선형 클램프

SamplerState DdgiLinearClamp : register(s0);
#define E_DDGI_CONSTANTS_REGISTER b0
#define E_DDGI_IRRADIANCE_REGISTER t1
#define E_DDGI_DISTANCE_REGISTER t2
#define E_DDGI_PROBE_DATA_REGISTER t3
#define E_DDGI_SAMPLER DdgiLinearClamp
#include "DdgiCommon.hlsli"

#define E_DDGI_SPHERE_SLICES 16
#define E_DDGI_SPHERE_STACKS 8

struct FProbeVSOutput
{
	float4               Position : SV_Position;
	float3               Normal   : NORMAL;
	nointerpolation uint Probe    : TEXCOORD0;
	nointerpolation uint Mode     : TEXCOORD1;
};

FProbeVSOutput VSProbe(uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
{
	FProbeVSOutput Output = (FProbeVSOutput)0;
	const uint        Probe  = InstanceId;
	const FDdgiVolume Volume = DdgiVolumes[DdgiFindVolume(Probe)];
	Output.Probe = Probe;
	Output.Mode  = Volume.DebugProbes;
	if (Volume.DebugProbes == 0 || Probe >= DdgiTotalProbes)
	{
		Output.Position = float4(0.0f, 0.0f, -1.0f, 1.0f); // 잘라냄
		return Output;
	}
	// 사각형 (Slice, Stack)의 삼각형 2개 → 모서리
	const uint   Quad     = VertexId / 6;
	const uint   Corner   = VertexId % 6;
	const uint2  Offsets[6] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(1, 0), uint2(1, 1), uint2(0, 1) };
	const uint2  Grid     = uint2(Quad % E_DDGI_SPHERE_SLICES, Quad / E_DDGI_SPHERE_SLICES) + Offsets[Corner];
	const float  Phi      = (float)Grid.x / E_DDGI_SPHERE_SLICES * 6.28318531f;
	const float  Theta    = (float)Grid.y / E_DDGI_SPHERE_STACKS * 3.14159265f;
	const float3 Normal   = float3(sin(Theta) * cos(Phi), sin(Theta) * sin(Phi), cos(Theta));
	const float4 Data     = DdgiProbeData.Load(int3(DdgiGetDataTexel(Probe), 0));
	const float3 Center   = DdgiGetProbeBasePosition(Volume, DdgiGetProbeCoords(Volume, Probe - Volume.ProbeOffset)) + Data.xyz;
	Output.Position = mul(float4(Center + Normal * Volume.DebugRadius, 1.0f), DdgiDebugViewProjection);
	Output.Normal   = Normal;
	return Output;
}

float4 PSProbe(FProbeVSOutput Input) : SV_Target
{
	const float3 N     = normalize(Input.Normal);
	const uint   State = DdgiGetProbeState(DdgiProbeData.Load(int3(DdgiGetDataTexel(Input.Probe), 0)).w);
	float3       Color;
	if (Input.Mode == 3)
	{
		Color = State == E_DDGI_STATE_UNINITIALIZED ? float3(0.1f, 0.2f, 1.0f) : (State == E_DDGI_STATE_ACTIVE ? float3(0.1f, 1.0f, 0.2f) : float3(1.0f, 0.1f, 0.1f));
	}
	else if (Input.Mode == 2)
	{
		const FDdgiVolume Volume = DdgiVolumes[DdgiFindVolume(Input.Probe)];
		Color = DdgiDistanceAtlas.SampleLevel(DdgiLinearClamp, DdgiGetAtlasUV(Input.Probe, N, E_DDGI_DISTANCE_TEXELS, DdgiDistanceTexelSize), 0).x * Volume.Intensity;
		Color = saturate(Color);
	}
	else
	{
		Color = DdgiIrradianceAtlas.SampleLevel(DdgiLinearClamp, DdgiGetAtlasUV(Input.Probe, N, E_DDGI_IRRADIANCE_TEXELS, DdgiIrradianceTexelSize), 0).rgb;
		if (State == E_DDGI_STATE_INACTIVE)
		{
			Color = Color * 0.25f + float3(0.5f, 0.0f, 0.0f); // 비활성 프로브는 붉게
		}
	}
	return float4(Color, 0.0f); // 알파 = TAA 반응형 마스크 (불투명 0)
}
