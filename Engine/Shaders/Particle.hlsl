#include "Common.hlsli"
#include "ParticleCommon.hlsli"

// 파티클 그리기 (조명/그림자 없음 — 메시 렌더러만 간단한 음영)
//   스프라이트: 입자마다 사각형 (정점 6개를 SV_VertexID로), 입자는 구조화 버퍼에서 SV_InstanceID로 읽는다
//   메시: 메시 정점 + 입자(SV_InstanceID). 리본: CPU가 만든 정점 그대로
//   가산: 색 * 알파를 더한다 (One/One). 반투명: 알파 블렌드. 깊이는 테스트만 한다. 색은 HDR 선형 (1보다 크면 블룸)

cbuffer ParticleFrame : register(b0)
{
	float4x4 ViewProjection;
	float3   CameraRight;
	float    FramePadding0;
	float3   CameraUp;
	float    FramePadding1;
	float3   CameraPosition;
	float    FramePadding2;
};

cbuffer ParticleDraw : register(b1)
{
	float4x4 LocalToWorld;
	int      SubImageColumns;
	int      SubImageRows;
	int      Alignment; // 0 = 카메라, 1 = 속도
	int      DrawPadding0;
	float    VelocityStretch;
	float3   DrawPadding1;
};

StructuredBuffer<FParticleData> ParticleBuffer  : register(t1);
Texture2D                       ParticleTexture : register(t0);
SamplerState                    LinearClamp     : register(s0);

struct FParticleVSOutput
{
	float4 Position : SV_Position;
	float2 UV       : TEXCOORD0;
	float4 Color    : COLOR;
};

static const float2 GCorners[6] = {
	float2(-1.0f, -1.0f), float2(-1.0f, 1.0f), float2(1.0f, 1.0f),
	float2(-1.0f, -1.0f), float2(1.0f, 1.0f), float2(1.0f, -1.0f),
};

FParticleVSOutput MakeCulled()
{
	FParticleVSOutput Output;
	Output.Position = float4(0.0f, 0.0f, -2.0f, 1.0f); // 깊이 범위 밖 (모든 정점이 같아 면적도 0)
	Output.UV       = float2(0.0f, 0.0f);
	Output.Color    = float4(0.0f, 0.0f, 0.0f, 0.0f);
	return Output;
}

FParticleVSOutput VSSprite(uint VertexId : SV_VertexID, uint InstanceId : SV_InstanceID)
{
	const FParticleData P = ParticleBuffer[InstanceId];
	if (P.Age >= P.Lifetime)
	{
		return MakeCulled();
	}
	const float2 Corner   = GCorners[VertexId % 6];
	const float3 Center   = mul(float4(P.Position, 1.0f), LocalToWorld).xyz;
	const float3 Velocity = mul(float4(P.Velocity, 0.0f), LocalToWorld).xyz;

	float3 Offset;
	const float3 ViewDir = SafeNormalize(Center - CameraPosition, float3(1, 0, 0));
	const float3 Planar  = Velocity - ViewDir * dot(Velocity, ViewDir);
	if (Alignment == 1 && dot(Planar, Planar) > 1e-4f)
	{
		// 속도 정렬: 화면에 비친 속도 방향으로 세우고 빠를수록 길게
		const float3 AxisY = normalize(Planar);
		const float3 AxisX = normalize(cross(AxisY, ViewDir));
		const float  HalfY = P.Size.y * 0.5f + length(Velocity) * VelocityStretch * 0.5f;
		Offset             = AxisX * (Corner.x * P.Size.x * 0.5f) + AxisY * (Corner.y * HalfY);
	}
	else
	{
		float SinR, CosR;
		sincos(radians(P.Rotation), SinR, CosR);
		const float2 Rotated = float2(Corner.x * CosR - Corner.y * SinR, Corner.x * SinR + Corner.y * CosR);
		Offset               = CameraRight * (Rotated.x * P.Size.x * 0.5f) + CameraUp * (Rotated.y * P.Size.y * 0.5f);
	}

	// 플립북: 칸 번호 → (열, 행)
	float2      UV     = float2(Corner.x * 0.5f + 0.5f, 0.5f - Corner.y * 0.5f);
	const uint  Cols   = uint(max(SubImageColumns, 1));
	const uint  Rows   = uint(max(SubImageRows, 1));
	const uint  Frame  = uint(max(P.SubImage, 0.0f)) % (Cols * Rows);
	UV                 = (UV + float2(Frame % Cols, Frame / Cols)) / float2(Cols, Rows);

	FParticleVSOutput Output;
	Output.Position = mul(float4(Center + Offset, 1.0f), ViewProjection);
	Output.UV       = UV;
	Output.Color    = P.Color;
	return Output;
}

struct FParticleMeshInput
{
	float3 Position : POSITION;
	float3 Normal   : NORMAL;
	float2 UV       : TEXCOORD0;
};

FParticleVSOutput VSMesh(FParticleMeshInput Input, uint InstanceId : SV_InstanceID)
{
	const FParticleData P = ParticleBuffer[InstanceId];
	if (P.Age >= P.Lifetime)
	{
		return MakeCulled();
	}
	// 내장 도형은 1m → 크기 X(cm)로 균등 배율, Z축 회전
	float SinR, CosR;
	sincos(radians(P.Rotation), SinR, CosR);
	const float  Scale  = P.Size.x / 100.0f;
	const float3 Local  = Input.Position * Scale;
	const float3 Turned = float3(Local.x * CosR - Local.y * SinR, Local.x * SinR + Local.y * CosR, Local.z);
	const float3 Normal = float3(Input.Normal.x * CosR - Input.Normal.y * SinR, Input.Normal.x * SinR + Input.Normal.y * CosR, Input.Normal.z);
	const float3 World  = mul(float4(P.Position + Turned, 1.0f), LocalToWorld).xyz;
	const float3 WorldN = SafeNormalize(mul(float4(Normal, 0.0f), LocalToWorld).xyz, float3(0, 0, 1));
	const float  Shade  = 0.35f + 0.65f * saturate(dot(WorldN, normalize(float3(0.4f, 0.3f, 1.0f))));

	FParticleVSOutput Output;
	Output.Position = mul(float4(World, 1.0f), ViewProjection);
	Output.UV       = Input.UV;
	Output.Color    = float4(P.Color.rgb * Shade, P.Color.a);
	return Output;
}

struct FParticleRibbonInput
{
	float3 Position : POSITION;
	float2 UV       : TEXCOORD0;
	float4 Color    : COLOR;
};

FParticleVSOutput VSRibbon(FParticleRibbonInput Input)
{
	FParticleVSOutput Output;
	Output.Position = mul(float4(Input.Position, 1.0f), ViewProjection);
	Output.UV       = Input.UV;
	Output.Color    = Input.Color;
	return Output;
}

float4 PSAlpha(FParticleVSOutput Input) : SV_Target
{
	const float4 Texel = ParticleTexture.Sample(LinearClamp, Input.UV);
	return float4(Texel.rgb * Input.Color.rgb, saturate(Texel.a * Input.Color.a));
}

float4 PSAdditive(FParticleVSOutput Input) : SV_Target
{
	const float4 Texel = ParticleTexture.Sample(LinearClamp, Input.UV);
	// 알파는 색에 영향 없음(가산) — TAA 반응형 마스크로 덮인 정도를 남긴다
	const float Coverage = saturate(Texel.a * Input.Color.a);
	return float4(Texel.rgb * Input.Color.rgb * Coverage, Coverage);
}
