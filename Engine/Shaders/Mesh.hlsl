#include "Common.hlsli"

// 정적 메시 기본 셰이더: 베이스 컬러 텍스처 + Blinn-Phong 방향광

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
	float3            AmbientColor;
	float             Padding1;
};

cbuffer Material : register(b2)
{
	float4 BaseColorTint;
	float3 SpecularColor;
	float  Shininess;
	float  SpecularStrength;
	float3 Padding2;
};

Texture2D    BaseColorTexture : register(t0);
SamplerState LinearSampler    : register(s0);

struct FVertexInput
{
	float3 Position : POSITION;
	float3 Normal   : NORMAL;
	float2 UV       : TEXCOORD0;
	float4 Color    : COLOR;
};

struct FPixelInput
{
	float4 Position      : SV_Position;
	float3 WorldPosition : POSITION0;
	float3 WorldNormal   : NORMAL;
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
	Output.UV            = Input.UV;
	Output.Color         = Input.Color;
	return Output;
}

// Blinn-Phong: 확산(Lambert) + 하프 벡터 스페큘러
float3 ShadeBlinnPhong(float3 N, float3 V, float3 L, float3 LightRadiance, float3 Albedo)
{
	const float NdotL = saturate(dot(N, L));
	if (NdotL <= 0.0f)
	{
		return 0.0f;
	}

	const float3 H     = normalize(L + V);
	const float  NdotH = saturate(dot(N, H));

	const float3 Diffuse  = Albedo * NdotL;
	const float3 Specular = SpecularColor * SpecularStrength * pow(NdotH, Shininess) * NdotL;
	return (Diffuse + Specular) * LightRadiance;
}

float4 PSMain(FPixelInput Input) : SV_Target
{
	const float4 BaseColor = BaseColorTexture.Sample(LinearSampler, Input.UV) * Input.Color * BaseColorTint;

	const float3 N = normalize(Input.WorldNormal);
	const float3 V = normalize(CameraPosition - Input.WorldPosition);
	const float3 L = -DirectionalLight.Direction; // 표면 → 광원

	const float3 Radiance = DirectionalLight.Color * DirectionalLight.Intensity;
	const float3 Lit      = ShadeBlinnPhong(N, V, L, Radiance, BaseColor.rgb) + AmbientColor * BaseColor.rgb;

	return float4(Lit, BaseColor.a);
}
