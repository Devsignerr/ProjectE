#include "Common.hlsli"
#include "PBR.hlsli"
#include "SkinnedMesh.hlsli"
#include "Lighting.hlsli" // b5 클러스터 상수
#include "MeshInstance.hlsli" // t13/t14 인스턴스
#include "ScreenSpace.hlsli"

// 정적 메시 기본 셰이더: 금속/거칠기 PBR (glTF 2.0 텍스처 규약), 방향광 1개(캐스케이드 섀도우) + IBL
// + 점광원/스포트라이트(클러스터드: 픽셀의 클러스터 목록만 순회). 출력은 선형 HDR

struct FDirectionalLight
{
	float3 Direction; // 빛이 진행하는 방향 (정규화)
	float  Intensity;
	float3 Color;
	float  Padding0;
};

// 묶음 상수 (루트 상수): 인스턴스 번호 목록 안 시작 위치
cbuffer DrawConstants : register(b0)
{
	uint InstanceOffset;
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
	float             AmbientOcclusionEnabled; // 1 = SSAO(t16) 사용
	float4x4          UnjitteredViewProjection; // 움직임 벡터용 (지터 없음)
	float4x4          PrevViewProjection;       // 이전 프레임 (지터 없음)
	float2            JitterNdc;
	float2            ScreenSize;
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

// 점광원/스포트라이트 (LocalLightRenderer: 목록 + 클러스터별 인덱스)
StructuredBuffer<FLocalLight> LocalLights : register(t9);
StructuredBuffer<uint>        ClusterData : register(t10);
StructuredBuffer<float4x4>    LocalShadowMatrices : register(t11); // 그림자 장별 뷰-투영
Texture2DArray<float>         LocalShadowMap      : register(t12); // 그림자 타일 배열 (스포트 1장, 점광원 6장: +X,-X,+Y,-Y,+Z,-Z)

// 1 = 빛 받음, 0 = 그림자. 3x3 PCF + 법선 오프셋 (텍셀 월드 크기 = 광원 기준 깊이 × ShadowTexelFactor)
float ComputeLocalShadow(FLocalLight Light, float3 WorldPosition, float3 GeometricNormal, float3 L)
{
	if (Light.ShadowIndex < 0)
	{
		return 1.0f;
	}
	const float3 FromLight = WorldPosition - Light.Position;
	uint         Slice     = (uint)Light.ShadowIndex;
	float        Depth;
	if (Light.Type == 0)
	{
		Slice += SelectCubeFace(FromLight);
		const float3 A = abs(FromLight);
		Depth          = max(A.x, max(A.y, A.z));
	}
	else
	{
		Depth = dot(FromLight, Light.Direction);
	}

	const float  NdotL   = saturate(dot(GeometricNormal, L));
	const float  Texel   = max(Depth, 1.0f) * Light.ShadowTexelFactor;
	const float3 Offset  = GeometricNormal * Texel * LocalShadowNormalOffset * (1.0f - 0.5f * NdotL);
	const float4 ClipPos = mul(float4(WorldPosition + Offset, 1.0f), LocalShadowMatrices[Slice]);
	if (ClipPos.w <= 0.0f)
	{
		return 1.0f;
	}
	const float3 Ndc = ClipPos.xyz / ClipPos.w;
	const float2 UV  = Ndc.xy * float2(0.5f, -0.5f) + 0.5f;
	if (any(UV < 0.0f) || any(UV > 1.0f) || Ndc.z > 1.0f)
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
			Lit += LocalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(UV + float2(X, Y) * LocalShadowTexelSize, Slice), Ndc.z);
		}
	}
	return Lit / 9.0f;
}

uint GetClusterIndex(float2 PixelPosition, float3 WorldPosition)
{
	const uint  TileX     = min((uint)(PixelPosition.x / ClusterScreenSize.x * ClusterGridX), ClusterGridX - 1);
	const uint  TileY     = min((uint)(PixelPosition.y / ClusterScreenSize.y * ClusterGridY), ClusterGridY - 1);
	const float ViewDepth = mul(float4(WorldPosition, 1.0f), ClusterView).z;
	const uint  Slice     = ClusterDepthToSlice(ViewDepth, ClusterSliceScale, ClusterSliceBias, ClusterGridZ);
	return TileX + ClusterGridX * (TileY + ClusterGridY * Slice);
}

float3 EvaluateLocalLights(FSurface Surface, float2 PixelPosition, float3 WorldPosition, float3 GeometricNormal)
{
	if (LocalLightCount == 0)
	{
		return 0.0f;
	}
	const uint Base  = GetClusterIndex(PixelPosition, WorldPosition) * E_CLUSTER_STRIDE;
	const uint Count = ClusterData[Base];

	float3 Color = 0.0f;
	for (uint Index = 0; Index < Count; ++Index)
	{
		const FLocalLight Light    = LocalLights[ClusterData[Base + 1 + Index]];
		const float3      ToLight  = Light.Position - WorldPosition;
		const float       Distance = length(ToLight);
		if (Distance >= Light.Radius)
		{
			continue;
		}
		const float3 L           = ToLight / max(Distance, 1.0e-4f);
		const float  Attenuation = LightDistanceAttenuation(Distance, Light.Radius) *
		                          LightConeAttenuation(dot(Light.Direction, -L), Light.ConeScale, Light.ConeOffset);
		if (Attenuation <= 0.0f)
		{
			continue;
		}
		const float3 Direct = EvaluateDirectLight(Surface, L, Light.Color * Attenuation);
		if (any(Direct > 0.0f))
		{
			Color += Direct * ComputeLocalShadow(Light, WorldPosition, GeometricNormal, L);
		}
	}
	return Color;
}

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
	// 움직임 벡터 (지터 없는 현재/이전 클립 좌표). 깊이 사전 패스와 메인 패스가 같은 정점 셰이더를 써야 깊이 같음 테스트가 맞는다
	float4 CurrentClip   : TEXCOORD1;
	float4 PreviousClip  : TEXCOORD2;
};

FPixelInput VSMain(FVertexInput Input, uint InstanceId : SV_InstanceID)
{
	FPixelInput Output;

	const FInstanceData Instance      = LoadInstance(InstanceOffset, InstanceId);
	const float4        WorldPosition = mul(float4(Input.Position, 1.0f), Instance.World);
	Output.Position      = mul(WorldPosition, ViewProjection);
	Output.CurrentClip   = mul(WorldPosition, UnjitteredViewProjection);
	Output.PreviousClip  = mul(mul(float4(Input.Position, 1.0f), Instance.PrevWorld), PrevViewProjection);
	Output.WorldPosition = WorldPosition.xyz;
	Output.WorldNormal   = normalize(mul(Input.Normal, GetNormalMatrix(Instance)));

	// 탄젠트는 표면을 따라가는 벡터이므로 World로 변환. 반사(음수 스케일)면 바이탄젠트 부호도 뒤집는다
	const float3x3 World3     = (float3x3)Instance.World;
	const float    Handedness = determinant(World3) < 0.0f ? -1.0f : 1.0f;
	Output.WorldTangent = float4(normalize(mul(Input.Tangent.xyz, World3)), Input.Tangent.w * Handedness);
	Output.UV           = Input.UV;
	Output.Color        = Input.Color;
	return Output;
}

struct FSkinnedVertexInput
{
	float3 Position : POSITION;
	float3 Normal   : NORMAL;
	float2 UV       : TEXCOORD0;
	float4 Color    : COLOR;
	float4 Tangent  : TANGENT;
	uint4  Joints   : BLENDINDICES; // 슬롯 1 스킨 스트림
	float4 Weights  : BLENDWEIGHT;
};

// 스킨 메시 (인스턴싱): 인스턴스의 BoneOffset 팔레트로 바로 월드 공간 (인스턴스 행렬 없음).
// 본 행렬은 균등 스케일 + 회전 + 이동을 가정해 법선도 같은 3x3으로 변환
FPixelInput VSSkinned(FSkinnedVertexInput Input, uint InstanceId : SV_InstanceID)
{
	FPixelInput Output;

	const FInstanceData Instance = LoadInstance(InstanceOffset, InstanceId);
	const float4x4      Skin     = ComputeSkinMatrix(Instance.BoneOffset, Input.Joints, Input.Weights);
	const float4   WorldPosition = mul(float4(Input.Position, 1.0f), Skin);
	const float3x3 Skin3         = (float3x3)Skin;
	Output.Position      = mul(WorldPosition, ViewProjection);
	Output.CurrentClip   = mul(WorldPosition, UnjitteredViewProjection);
	const float4x4 PrevSkin = ComputeSkinMatrix(Instance.PrevBoneOffset, Input.Joints, Input.Weights);
	Output.PreviousClip  = mul(mul(float4(Input.Position, 1.0f), PrevSkin), PrevViewProjection);
	Output.WorldPosition = WorldPosition.xyz;
	Output.WorldNormal   = normalize(mul(Input.Normal, Skin3));

	const float Handedness = determinant(Skin3) < 0.0f ? -1.0f : 1.0f;
	Output.WorldTangent = float4(normalize(mul(Input.Tangent.xyz, Skin3)), Input.Tangent.w * Handedness);
	Output.UV           = Input.UV;
	Output.Color        = Input.Color;
	return Output;
}

// SSAO (반해상도 R = 가시도, G = 뷰 깊이, AmbientOcclusion.hlsl): 4탭 깊이 가중 업샘플. 간접광에만 곱한다
Texture2D<float2> ScreenAmbientOcclusion : register(t16);

float SampleScreenAmbientOcclusion(float2 PixelPosition, float3 WorldPosition)
{
	if (AmbientOcclusionEnabled < 0.5f)
	{
		return 1.0f;
	}
	uint Width, Height;
	ScreenAmbientOcclusion.GetDimensions(Width, Height);
	const float  ViewDepth = mul(float4(WorldPosition, 1.0f), ClusterView).z;
	// 반해상도 픽셀 i는 전체 해상도 픽셀 2i에서 계산됐다 → 전체 위치 x의 반해상도 좌표 = (x - 0.5) / 2
	const float2 HalfPos = (PixelPosition - 0.5f) * 0.5f;
	const int2   Base    = int2(floor(HalfPos));
	const float2 F       = HalfPos - float2(Base);
	const int2   MaxPixel = int2(Width, Height) - 1;

	float Sum    = 0.0f;
	float Weight = 0.0f;
	float Nearest = 1.0f;
	float NearestDelta = 1.0e30f;
	[unroll]
	for (int Tap = 0; Tap < 4; ++Tap)
	{
		const int2   Offset = int2(Tap & 1, Tap >> 1);
		const float2 Sample = ScreenAmbientOcclusion.Load(int3(clamp(Base + Offset, int2(0, 0), MaxPixel), 0));
		const float  Bilinear = (Offset.x == 1 ? F.x : 1.0f - F.x) * (Offset.y == 1 ? F.y : 1.0f - F.y);
		const float  Delta    = abs(Sample.y - ViewDepth) / max(ViewDepth, 1.0e-3f);
		const float  W        = Bilinear * exp(-Delta * 40.0f) + 1.0e-5f;
		Sum += Sample.x * W;
		Weight += W;
		if (Delta < NearestDelta)
		{
			NearestDelta = Delta;
			Nearest      = Sample.x;
		}
	}
	return Weight > 1.0e-3f ? Sum / Weight : Nearest;
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
	Surface.Occlusion = lerp(1.0f, AO, OcclusionStrength) * SampleScreenAmbientOcclusion(Input.Position.xy, Input.WorldPosition); // IBL만 사용

	const float3 L        = -DirectionalLight.Direction; // 표면 → 광원
	const float3 Radiance = DirectionalLight.Color * DirectionalLight.Intensity;

	const float Shadow = ComputeShadow(Input.WorldPosition, normalize(Input.WorldNormal), L);

	float3 Color = EvaluateDirectLight(Surface, L, Radiance) * Shadow;
	Color += EvaluateLocalLights(Surface, Input.Position.xy, Input.WorldPosition, normalize(Input.WorldNormal));
	Color += EvaluateImageBasedLighting(Surface);
	Color += Emissive;

	if (VisualizeCascades != 0)
	{
		Color *= CascadeDebugColor(Input.WorldPosition);
	}

	return float4(Color, 0.0f); // 알파 = TAA 반응형 마스크 (불투명 0, 파티클이 덮은 만큼 쌓인다)
}

// 깊이 사전 패스 (FSceneRenderer): 깊이 + 화면 공간 법선(기하 법선, 팔면체) + 움직임 벡터. 머티리얼 텍스처를 읽지 않는다
struct FPrepassOutput
{
	float4 Normal   : SV_Target0; // R10G10B10A2_UNORM (ScreenSpace.hlsli EncodeScreenNormal)
	float2 Velocity : SV_Target1; // R16G16_FLOAT, UV 단위 현재 - 이전
};

FPrepassOutput PSPrepass(FPixelInput Input)
{
	FPrepassOutput Output;
	Output.Normal   = EncodeScreenNormal(normalize(Input.WorldNormal));
	Output.Velocity = ComputeVelocity(Input.CurrentClip, Input.PreviousClip);
	return Output;
}
