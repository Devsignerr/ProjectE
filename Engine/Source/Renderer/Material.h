#pragma once

#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "Renderer/ShaderTypes.h"
#include "Scene/ResourceHandles.h"

#include <string>
#include <vector>

// 머티리얼 텍스처 슬롯 (셰이더 t0~t4와 순서 일치). glTF 2.0 금속/거칠기 규약
enum EMaterialTextureSlot : uint32
{
	MaterialSlot_BaseColor         = 0, // sRGB, RGB=알베도 A=알파
	MaterialSlot_MetallicRoughness = 1, // 선형, G=거칠기 B=금속
	MaterialSlot_Normal            = 2, // 선형, 탄젠트 공간 (+Y = UV 위쪽)
	MaterialSlot_Occlusion         = 3, // 선형, R=AO
	MaterialSlot_Emissive          = 4, // sRGB
	MaterialSlot_Count             = 5,
};

// 블렌드 모드 (.emat "BlendMode" — JSON은 이름 저장, 번호는 끝에만 추가)
//   Opaque      불투명 (사전 패스 + 메인 깊이 같음)
//   Masked      알파 테스트: 베이스 컬러 알파 < AlphaCutoff인 픽셀을 버린다 (메인/사전 패스/그림자 모두)
//   Translucent 알파 블렌드: 불투명·안개 뒤 정렬된 전방 패스 (깊이 쓰기 없음, 그림자 없음)
//   Additive    가산: Translucent와 같은 패스, 색을 더한다
enum class EMaterialBlendMode : uint8
{
	Opaque,
	Masked,
	Translucent,
	Additive,
	Count
};

namespace MaterialRender
{
	// 반투명 패스(정렬된 전방 패스)에서 그리는가
	constexpr bool IsTranslucent(EMaterialBlendMode Mode) { return Mode == EMaterialBlendMode::Translucent || Mode == EMaterialBlendMode::Additive; }

	// 메시 패스 PSO 변형 비트 (묶음 키의 Pipeline 필드 3비트 = PSO 배열 인덱스).
	//   bit0 = Masked(불투명 패스) / Additive(반투명 패스), bit1 = 양면(컬링 없음), bit2 = 스킨.
	//   스킨을 최상위에 두어 정적 묶음이 모두 스킨 묶음보다 앞에 정렬된다 (오클루전 1단계 번호 목록 전환이 이 순서에 기댄다)
	constexpr uint32 VariantMaskedOrAdditive = 1u;
	constexpr uint32 VariantTwoSided         = 2u;
	constexpr uint32 VariantSkinned          = 4u;
	constexpr uint32 VariantCount            = 8u;
	constexpr uint32 MakeVariant(bool bSkinned, bool bMaskedOrAdditive, bool bTwoSided)
	{
		return (bMaskedOrAdditive ? VariantMaskedOrAdditive : 0u) | (bTwoSided ? VariantTwoSided : 0u) | (bSkinned ? VariantSkinned : 0u);
	}
} // namespace MaterialRender

// PBR 머티리얼 (텍스처 핸들 + 상수). 리소스 관리자가 소유한다.
// 무효 텍스처 핸들은 기본 텍스처로 대체된다: 노멀은 평면 노멀, 나머지는 흰색(팩터가 곧 값).
// alignas(16) 상수를 첫 멤버로 두어 구조체 패딩 경고(C4324)를 피한다.
struct FMaterial
{
	FMaterialConstants Constants;
	std::string        Name;
	FTextureHandle     Textures[MaterialSlot_Count];
	EMaterialBlendMode BlendMode = EMaterialBlendMode::Opaque; // 알파 컷오프는 Constants.AlphaCutoff
	bool               bTwoSided = false;                      // 컬링 없음 + 뒷면은 법선을 뒤집어 조명

	// 인스턴스(.emat Parent)면 부모 체인의 정규화 경로 (가까운 부모부터). 부모가 바뀌면 FResourceManager가 다시 해석한다
	std::vector<std::wstring> ParentChain;

	// 셰이더 가시 힙의 연속 SRV 5칸 (t0~t4). FResourceManager가 생성/갱신/해제한다 — 직접 수정 금지
	FD3D12DescriptorHandle TextureTable;
};
