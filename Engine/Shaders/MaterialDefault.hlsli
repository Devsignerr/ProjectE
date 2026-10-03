#ifndef E_MATERIAL_DEFAULT_HLSLI
#define E_MATERIAL_DEFAULT_HLSLI

#include "MaterialCommon.hlsli"

// 고정 PBR 머티리얼 (.emat에 Graph 없음 — glTF 2.0 금속/거칠기, Phase 36 이전과 같은 식).
// 그래프 머티리얼의 EvaluateMaterial과 같은 인터페이스라 메시 패스는 머티리얼 종류를 모른다.
// 식을 바꾸면 기존 화면이 바뀐다 (Phase 49 사이드 기준: 그래프 도입 전과 비트 단위 동일).

#ifdef E_MATERIAL_CUSTOM_RESOURCES
// 레이 트레이싱/바인드리스 (Phase 50, RayTracingCommon.hlsli): 호출 쪽이 E_MATERIAL_DEFAULT_CONSTANTS(FRayTracingMaterialGpu 필드 이름을 가진 값 —
//   BaseColorFactor/EmissiveFactor/Metallic/Roughness/NormalScale/OcclusionStrength/AlphaCutoff)와 E_MATERIAL_TEXTURE(0~4)를 정의한다.
//   아래 이름은 이 파일 끝에서 #undef (함수 본문은 래스터와 같은 글자 그대로)
#define BaseColorFactor          (E_MATERIAL_DEFAULT_CONSTANTS.BaseColorFactor)
#define EmissiveFactor           (E_MATERIAL_DEFAULT_CONSTANTS.EmissiveFactor)
#define MetallicFactor           (E_MATERIAL_DEFAULT_CONSTANTS.Metallic)
#define RoughnessFactor          (E_MATERIAL_DEFAULT_CONSTANTS.Roughness)
#define NormalScale              (E_MATERIAL_DEFAULT_CONSTANTS.NormalScale)
#define OcclusionStrength        (E_MATERIAL_DEFAULT_CONSTANTS.OcclusionStrength)
#define BaseColorTexture         E_MATERIAL_TEXTURE(0)
#define MetallicRoughnessTexture E_MATERIAL_TEXTURE(1)
#define NormalTexture            E_MATERIAL_TEXTURE(2)
#define OcclusionTexture         E_MATERIAL_TEXTURE(3)
#define EmissiveTexture          E_MATERIAL_TEXTURE(4)
#define E_MATERIAL_ALPHA_CUTOFF  (E_MATERIAL_DEFAULT_CONSTANTS.AlphaCutoff)
#else
cbuffer Material : register(b2)
{
	float4 BaseColorFactor;
	float3 EmissiveFactor;
	float  MetallicFactor;
	float  RoughnessFactor;
	float  NormalScale;
	float  OcclusionStrength;
	float  AlphaCutoff;     // Masked: 베이스 컬러 알파(텍스처 × 정점 색 × 팩터)가 이보다 작으면 버린다
};

// 머티리얼 텍스처 테이블 (EMaterialTextureSlot 순서)
Texture2D BaseColorTexture         : register(t0); // sRGB
Texture2D MetallicRoughnessTexture : register(t1); // 선형, G=거칠기 B=금속
Texture2D NormalTexture            : register(t2); // 선형, 탄젠트 공간
Texture2D OcclusionTexture         : register(t3); // 선형, R
Texture2D EmissiveTexture          : register(t4); // sRGB

#define E_MATERIAL_ALPHA_CUTOFF AlphaCutoff
#endif

void EvaluateMaterial(in FMaterialPixelInputs In, out FMaterialSurface Out)
{
	const float4 BaseColor = MATERIAL_SAMPLE(BaseColorTexture, E_MATERIAL_SAMPLER_WRAP, In.UV0) * In.VertexColor * BaseColorFactor;
	const float4 MR        = MATERIAL_SAMPLE(MetallicRoughnessTexture, E_MATERIAL_SAMPLER_WRAP, In.UV0);
	const float  AO        = MATERIAL_SAMPLE(OcclusionTexture, E_MATERIAL_SAMPLER_WRAP, In.UV0).r;

	Out.BaseColor   = BaseColor.rgb;
	Out.Opacity     = BaseColor.a;
	Out.OpacityMask = BaseColor.a;
	Out.Metallic    = MR.b * MetallicFactor;
	Out.Roughness   = MR.g * RoughnessFactor;

	// XY만 사용하고 Z는 재구성 (BC5 노멀 맵은 RG만 저장)
	float3 TangentNormal;
	TangentNormal.xy = MATERIAL_SAMPLE(NormalTexture, E_MATERIAL_SAMPLER_WRAP, In.UV0).xy * 2.0f - 1.0f;
	TangentNormal.z  = sqrt(saturate(1.0f - dot(TangentNormal.xy, TangentNormal.xy)));
	TangentNormal.xy *= NormalScale;
	Out.Normal = TangentNormal;

	Out.AmbientOcclusion = lerp(1.0f, AO, OcclusionStrength); // IBL만 사용
	Out.Emissive         = MATERIAL_SAMPLE(EmissiveTexture, E_MATERIAL_SAMPLER_WRAP, In.UV0).rgb * EmissiveFactor;
}

#ifdef E_MATERIAL_CUSTOM_RESOURCES
#undef BaseColorFactor
#undef EmissiveFactor
#undef MetallicFactor
#undef RoughnessFactor
#undef NormalScale
#undef OcclusionStrength
#undef BaseColorTexture
#undef MetallicRoughnessTexture
#undef NormalTexture
#undef OcclusionTexture
#undef EmissiveTexture
#endif

#endif // E_MATERIAL_DEFAULT_HLSLI
