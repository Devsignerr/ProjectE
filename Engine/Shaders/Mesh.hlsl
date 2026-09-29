#include "Common.hlsli"
#include "PBR.hlsli"

// 정적 메시 기본 셰이더: 금속/거칠기 PBR (glTF 2.0 텍스처 규약), 방향광 1개 + 간이 환경광. 출력은 선형 HDR

struct FDirectionalLight
{
	float3 Direction; // 빛이 진행하는 방향 (정규화)
	float  Intensity;
	float3 Color;
	float  Padding0;
};

cbuffer PerObject : register(b0)
{
	float4x4 World;
	float4x4 WorldInverseTranspose;
};

cbuffer PerFrame : register(b1)
{
	float4x4          ViewProjection;
	float3            CameraPosition;
	float             Padding0;
	FDirectionalLight DirectionalLight;
	float3            SkyColor;
	float             AmbientIntensity;
	float3            GroundColor;
	float             Padding1;
};

cbuffer Material : register(b2)
{
	float4 BaseColorFactor;
	float3 EmissiveFactor;
	float  MetallicFactor;
	float  RoughnessFactor;
	float  NormalScale;
	float  OcclusionStrength;
	float  AlphaCutoff;
};

// 머티리얼 텍스처 테이블 (EMaterialTextureSlot 순서)
Texture2D    BaseColorTexture         : register(t0); // sRGB
Texture2D    MetallicRoughnessTexture : register(t1); // 선형, G=거칠기 B=금속
Texture2D    NormalTexture            : register(t2); // 선형, 탄젠트 공간
Texture2D    OcclusionTexture         : register(t3); // 선형, R
Texture2D    EmissiveTexture          : register(t4); // sRGB
SamplerState LinearSampler            : register(s0);

struct FVertexInput
{
	float3 Position : POSITION;
	float3 Normal   : NORMAL;
	float2 UV       : TEXCOORD0;
	float4 Color    : COLOR;
	float4 Tangent  : TANGENT; // xyz = +U, w = 바이탄젠트 부호 (B = cross(N, T) * w = 텍스처 위쪽)
};

struct FPixelInput
{
	float4 Position      : SV_Position;
	float3 WorldPosition : POSITION0;
	float3 WorldNormal   : NORMAL;
	float4 WorldTangent  : TANGENT;
	float2 UV            : TEXCOORD0;
	float4 Color         : COLOR;
};

FPixelInput VSMain(FVertexInput Input)
{
	FPixelInput Output;

	const float4 WorldPosition = mul(float4(Input.Position, 1.0f), World);
	Output.Position      = mul(WorldPosition, ViewProjection);
	Output.WorldPosition = WorldPosition.xyz;
	Output.WorldNormal   = normalize(mul(Input.Normal, (float3x3)WorldInverseTranspose));

	// 탄젠트는 표면을 따라가는 벡터이므로 World로 변환. 반사(음수 스케일)면 바이탄젠트 부호도 뒤집는다
	const float3x3 World3     = (float3x3)World;
	const float    Handedness = determinant(World3) < 0.0f ? -1.0f : 1.0f;
	Output.WorldTangent = float4(normalize(mul(Input.Tangent.xyz, World3)), Input.Tangent.w * Handedness);
	Output.UV           = Input.UV;
	Output.Color        = Input.Color;
	return Output;
}

float3 GetShadingNormal(FPixelInput Input)
{
	const float3 N = normalize(Input.WorldNormal);
	// 보간으로 틀어진 탄젠트를 법선에 다시 직교화
	const float3 T = normalize(Input.WorldTangent.xyz - N * dot(N, Input.WorldTangent.xyz));
	const float3 B = cross(N, T) * Input.WorldTangent.w;

	float3 TangentNormal = NormalTexture.Sample(LinearSampler, Input.UV).xyz * 2.0f - 1.0f;
	TangentNormal.xy *= NormalScale;
	return normalize(T * TangentNormal.x + B * TangentNormal.y + N * TangentNormal.z);
}

float4 PSMain(FPixelInput Input) : SV_Target
{
	const float4 BaseColor = BaseColorTexture.Sample(LinearSampler, Input.UV) * Input.Color * BaseColorFactor;
	const float4 MR        = MetallicRoughnessTexture.Sample(LinearSampler, Input.UV);
	const float  AO        = OcclusionTexture.Sample(LinearSampler, Input.UV).r;
	const float3 Emissive  = EmissiveTexture.Sample(LinearSampler, Input.UV).rgb * EmissiveFactor;

	FSurface Surface;
	Surface.Albedo    = BaseColor.rgb;
	Surface.Metallic  = saturate(MR.b * MetallicFactor);
	Surface.Roughness = clamp(MR.g * RoughnessFactor, 0.045f, 1.0f); // 너무 작은 거칠기는 하이라이트 에일리어싱
	Surface.N         = GetShadingNormal(Input);
	Surface.V         = normalize(CameraPosition - Input.WorldPosition);
	Surface.Occlusion = lerp(1.0f, AO, OcclusionStrength);

	const float3 L        = -DirectionalLight.Direction; // 표면 → 광원
	const float3 Radiance = DirectionalLight.Color * DirectionalLight.Intensity;

	float3 Color = EvaluateDirectLight(Surface, L, Radiance);
	Color += EvaluateAmbient(Surface, SkyColor, GroundColor, AmbientIntensity);
	Color += Emissive;

	return float4(Color, BaseColor.a);
}
