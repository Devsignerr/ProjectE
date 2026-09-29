#pragma once

#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "Renderer/ShaderTypes.h"
#include "Scene/ResourceHandles.h"

#include <string>

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

// PBR 머티리얼 (텍스처 핸들 + 상수). 리소스 관리자가 소유한다.
// 무효 텍스처 핸들은 기본 텍스처로 대체된다: 노멀은 평면 노멀, 나머지는 흰색(팩터가 곧 값).
// alignas(16) 상수를 첫 멤버로 두어 구조체 패딩 경고(C4324)를 피한다.
struct FMaterial
{
	FMaterialConstants Constants;
	std::string        Name;
	FTextureHandle     Textures[MaterialSlot_Count];

	// 셰이더 가시 힙의 연속 SRV 5칸 (t0~t4). FResourceManager가 생성/갱신/해제한다 — 직접 수정 금지
	FD3D12DescriptorHandle TextureTable;
};
