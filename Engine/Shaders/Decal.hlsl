#include "Common.hlsli"
#include "ScreenSpace.hlsli"

// 박스 투영 데칼 → DBuffer (FDecalRenderer, 식은 Renderer/DecalMath.h)
//   상자 뒷면을 깊이 테스트 없이 그려(카메라가 상자 안이어도 덮임) 픽셀마다 씬 깊이로 월드 위치를 복원 → 데칼 로컬 → UV.
//   출력 (블렌드 EBlendMode::Remaining: rgb = 값·a 누적, a = 남은 표면 비중):
//     SV_Target0 DBufferA (sRGB): 베이스색, SV_Target1 DBufferB: 월드 법선 * 0.5 + 0.5, SV_Target2 DBufferC: R 거칠기, G 금속

cbuffer DecalConstants : register(b0)
{
	float4x4 DecalToWorld;      // 단위 상자 → 월드
	float4x4 WorldToDecal;      // 월드 → 단위 상자
	float4x4 ViewProjection;    // 지터 포함 (씬 깊이와 같은 래스터)
	float4x4 InvViewProjection; // 지터 포함 투영의 역 (깊이 → 월드)
	float4   BaseColorFactor;
	float3   DecalNormal;       // 표면 쪽 법선 = 로컬 +Z (월드, 정규화)
	float    Opacity;
	float3   DecalTangent;      // 텍스처 +U = 로컬 +Y (월드, 정규화)
	float    RoughnessFactor;
	float3   DecalBitangent;    // 텍스처 위 = 로컬 +X (월드, 정규화)
	float    MetallicFactor;
	float3   CameraPosition;
	float    NormalScale;
	float2   ScreenSize;
	float    FadeStartDistance;
	float    FadeEndDistance;
	uint     AffectFlags;       // 1 = 베이스색, 2 = 노멀, 4 = 거칠기/금속
	float    FootprintScale;    // 픽셀 하나의 월드 크기 = (원근: 뷰 거리 ×) 이 값
	uint     bOrthographic;
	float    TexelsPerUnit;     // 베이스 텍스처 텍셀 / cm (밉 선택)
};

Texture2D<float>  SceneDepth        : register(t0);
Texture2D<float4> SceneNormal       : register(t1);
Texture2D<float4> BaseColorTexture  : register(t2); // sRGB
Texture2D<float4> MetalRoughTexture : register(t3); // G = 거칠기, B = 금속
Texture2D<float4> NormalTexture     : register(t4); // XY
SamplerState      LinearWrap        : register(s3);

static const float3 GCubeCorners[8] = {
	float3(-0.5f, -0.5f, -0.5f), float3(0.5f, -0.5f, -0.5f), float3(0.5f, 0.5f, -0.5f), float3(-0.5f, 0.5f, -0.5f),
	float3(-0.5f, -0.5f, 0.5f),  float3(0.5f, -0.5f, 0.5f),  float3(0.5f, 0.5f, 0.5f),  float3(-0.5f, 0.5f, 0.5f),
};
static const uint GCubeIndices[36] = {
	0, 2, 1, 0, 3, 2, // -Z
	4, 5, 6, 4, 6, 7, // +Z
	0, 1, 5, 0, 5, 4, // -Y
	3, 6, 2, 3, 7, 6, // +Y
	0, 4, 7, 0, 7, 3, // -X
	1, 2, 6, 1, 6, 5, // +X
};

float4 VSMain(uint VertexId : SV_VertexID) : SV_Position
{
	return mul(mul(float4(GCubeCorners[GCubeIndices[VertexId]], 1.0f), DecalToWorld), ViewProjection);
}

struct FDecalOutput
{
	float4 BaseColor : SV_Target0;
	float4 Normal    : SV_Target1;
	float4 Material  : SV_Target2;
};

FDecalOutput PSMain(float4 Position : SV_Position)
{
	const int2  Pixel = int2(Position.xy);
	const float Depth = SceneDepth.Load(int3(Pixel, 0));
	if (Depth >= 1.0f)
	{
		discard; // 하늘
	}
	const float2 UV    = (float2(Pixel) + 0.5f) / ScreenSize;
	const float4 World = mul(float4(UV.x * 2.0f - 1.0f, 1.0f - UV.y * 2.0f, Depth, 1.0f), InvViewProjection);
	const float3 P     = World.xyz / World.w;
	const float3 Local = mul(float4(P, 1.0f), WorldToDecal).xyz;
	if (any(abs(Local) > 0.5f))
	{
		discard;
	}

	// 페이드: 표면 각도(옆면 늘어짐/뒷면), 투영 깊이 끝, 카메라 거리
	const float3 GeometryN     = DecodeScreenNormal(SceneNormal.Load(int3(Pixel, 0)));
	const float  AngleFade     = saturate((dot(GeometryN, DecalNormal) - 0.25f) / 0.25f);
	const float  DepthFade     = saturate((0.5f - abs(Local.z)) / 0.1f);
	const float  Distance      = length(P - CameraPosition);
	const float  DistanceFade  = FadeEndDistance > FadeStartDistance ? saturate((FadeEndDistance - Distance) / (FadeEndDistance - FadeStartDistance)) : 1.0f;

	// 밉: 픽셀 크기(월드) × 텍셀 밀도 (재구성 위치는 깊이 경계에서 도함수가 튀므로 직접 계산)
	const float Footprint = FootprintScale * (bOrthographic != 0 ? 1.0f : max(Distance, 1.0f));
	const float Lod       = log2(max(Footprint * TexelsPerUnit, 1.0f));

	const float2 DecalUV   = float2(Local.y + 0.5f, 0.5f - Local.x);
	const float4 BaseColor = BaseColorTexture.SampleLevel(LinearWrap, DecalUV, Lod) * BaseColorFactor;
	const float  Alpha     = saturate(BaseColor.a * Opacity * AngleFade * DepthFade * DistanceFade);
	if (Alpha <= 1.0e-3f)
	{
		discard;
	}

	float3 TangentNormal;
	TangentNormal.xy = NormalTexture.SampleLevel(LinearWrap, DecalUV, Lod).xy * 2.0f - 1.0f;
	TangentNormal.z  = sqrt(saturate(1.0f - dot(TangentNormal.xy, TangentNormal.xy)));
	TangentNormal.xy *= NormalScale;
	const float3 N  = normalize(DecalTangent * TangentNormal.x + DecalBitangent * TangentNormal.y + DecalNormal * TangentNormal.z);
	const float4 MR = MetalRoughTexture.SampleLevel(LinearWrap, DecalUV, Lod);

	FDecalOutput Output;
	Output.BaseColor = float4(BaseColor.rgb, (AffectFlags & 1u) != 0 ? Alpha : 0.0f);
	Output.Normal    = float4(N * 0.5f + 0.5f, (AffectFlags & 2u) != 0 ? Alpha : 0.0f);
	Output.Material  = float4(saturate(MR.g * RoughnessFactor), saturate(MR.b * MetallicFactor), 0.0f, (AffectFlags & 4u) != 0 ? Alpha : 0.0f);
	return Output;
}
