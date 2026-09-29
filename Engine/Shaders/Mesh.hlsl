#include "Common.hlsli"
#include "PBR.hlsli"

// 정적 메시 기본 셰이더: 금속/거칠기 PBR (glTF 2.0 텍스처 규약), 방향광 1개(캐스케이드 섀도우) + 간이 환경광. 출력은 선형 HDR

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

// 방향광 캐스케이드 섀도우 (ShadowRenderer.h FShadowConstants와 1:1)
cbuffer ShadowConstants : register(b3)
{
	float4x4 CascadeViewProjection[4];
	float4   CascadeSplits;     // 뷰 공간 far 거리
	float4   CascadeTexelWorld; // 캐스케이드별 월드 텍셀 크기
	float3   ShadowCameraForward;
	float    ShadowEnabled;
	float    ShadowTexelSize;   // 1 / 해상도
	float    ShadowNormalOffset;
	uint     CascadeCount;
	uint     VisualizeCascades;
};

Texture2DArray<float>  ShadowMap     : register(t8);
SamplerComparisonState ShadowSampler : register(s2);

// 확산 맵은 irradiance / PI를 저장한다. 금속 반사에는 거칠기별 프리필터와 BRDF LUT를 사용한다.
TextureCube<float4> IblDiffuse : register(t5);
TextureCube<float4> IblSpecular : register(t6);
Texture2D<float2> IblBrdf : register(t7);
SamplerState IblSampler : register(s1);

float3 EvaluateImageBasedLighting(FSurface Surface)
{
	const float NdotV = max(saturate(dot(Surface.N, Surface.V)), 1.0e-4f);
	const float3 F0 = GetF0(Surface);
	const float3 F = F0 + (max(1.0f - Surface.Roughness, F0) - F0) * pow(1.0f - NdotV, 5.0f);
	const float3 Diffuse = IblDiffuse.SampleLevel(IblSampler, Surface.N, 0).rgb * Surface.Albedo;
	uint Width, Height, MipCount;
	IblSpecular.GetDimensions(0, Width, Height, MipCount);
	const float3 R = reflect(-Surface.V, Surface.N);
	const float3 Prefiltered = IblSpecular.SampleLevel(IblSampler, R, Surface.Roughness * (MipCount - 1)).rgb;
	const float2 Brdf = IblBrdf.SampleLevel(IblSampler, float2(NdotV, Surface.Roughness), 0);
	const float3 Specular = Prefiltered * (F0 * Brdf.x + Brdf.y);
	return ((1.0f - F) * (1.0f - Surface.Metallic) * Diffuse + Specular) * Surface.Occlusion * AmbientIntensity;
}

uint SelectCascade(float3 WorldPosition)
{
	const float ViewDepth = dot(WorldPosition - CameraPosition, ShadowCameraForward);
	[unroll]
	for (uint Index = 0; Index < 4; ++Index)
	{
		if (Index < CascadeCount && ViewDepth <= CascadeSplits[Index])
		{
			return Index;
		}
	}
	return CascadeCount; // 그림자 거리 밖
}

// 1 = 완전히 빛 받음, 0 = 그림자. 3x3 PCF + 법선 오프셋, 마지막 캐스케이드 끝에서 페이드
float ComputeShadow(float3 WorldPosition, float3 GeometricNormal, float3 L)
{
	if (ShadowEnabled < 0.5f)
	{
		return 1.0f;
	}
	const uint Cascade = SelectCascade(WorldPosition);
	if (Cascade >= CascadeCount)
	{
		return 1.0f;
	}

	// 빛에 비스듬한 면일수록 더 밀어 자기 그림자(acne)를 줄인다
	const float  NdotL    = saturate(dot(GeometricNormal, L));
	const float3 Offset   = GeometricNormal * CascadeTexelWorld[Cascade] * ShadowNormalOffset * (1.0f - 0.5f * NdotL);
	const float4 ClipPos  = mul(float4(WorldPosition + Offset, 1.0f), CascadeViewProjection[Cascade]);
	const float2 UV       = ClipPos.xy * float2(0.5f, -0.5f) + 0.5f;
	const float  Depth    = ClipPos.z;
	if (any(UV < 0.0f) || any(UV > 1.0f) || Depth > 1.0f)
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
			Lit += ShadowMap.SampleCmpLevelZero(ShadowSampler, float3(UV + float2(X, Y) * ShadowTexelSize, Cascade), Depth);
		}
	}
	Lit /= 9.0f;

	// 그림자 거리 끝 10%에서 부드럽게 사라지게
	const float ViewDepth = dot(WorldPosition - CameraPosition, ShadowCameraForward);
	const float FadeStart = CascadeSplits[CascadeCount - 1] * 0.9f;
	const float Fade      = saturate((ViewDepth - FadeStart) / max(CascadeSplits[CascadeCount - 1] - FadeStart, 1.0e-3f));
	return lerp(Lit, 1.0f, Fade);
}

float3 CascadeDebugColor(float3 WorldPosition)
{
	const uint Cascade = SelectCascade(WorldPosition);
	const float3 Colors[5] = { float3(1.0f, 0.3f, 0.3f), float3(0.3f, 1.0f, 0.3f), float3(0.3f, 0.3f, 1.0f),
	                           float3(1.0f, 1.0f, 0.3f), float3(1.0f, 1.0f, 1.0f) };
	return Colors[min(Cascade, 4u)];
}

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

	// XY만 사용하고 Z는 재구성 (BC5 노멀 맵은 RG만 저장)
	float3 TangentNormal;
	TangentNormal.xy = NormalTexture.Sample(LinearSampler, Input.UV).xy * 2.0f - 1.0f;
	TangentNormal.z  = sqrt(saturate(1.0f - dot(TangentNormal.xy, TangentNormal.xy)));
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

	const float Shadow = ComputeShadow(Input.WorldPosition, normalize(Input.WorldNormal), L);

	float3 Color = EvaluateDirectLight(Surface, L, Radiance) * Shadow;
	Color += EvaluateImageBasedLighting(Surface);
	Color += Emissive;

	if (VisualizeCascades != 0)
	{
		Color *= CascadeDebugColor(Input.WorldPosition);
	}

	return float4(Color, BaseColor.a);
}
