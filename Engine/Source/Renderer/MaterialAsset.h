#pragma once

#include "Renderer/Material.h"
#include "Renderer/ShaderTypes.h"

#include <filesystem>
#include <string>

// .emat 파일 내용 (JSON, PBR 금속/거칠기). 텍스처 경로는 .emat 파일이 있는 디렉터리 기준 상대 경로, 비어 있으면 기본 텍스처.
//   { "Name": "Checker", "BaseColorFactor": [1,1,1,1], "Metallic": 0, "Roughness": 0.8,
//     "EmissiveFactor": [0,0,0], "NormalScale": 1, "OcclusionStrength": 1,
//     "BaseColorTexture": "../UVChecker.png", "MetallicRoughnessTexture": "", "NormalTexture": "",
//     "OcclusionTexture": "", "EmissiveTexture": "" }
// 구 형식(Blinn-Phong)의 BaseColorTint는 BaseColorFactor로 읽고, SpecularColor/Shininess/SpecularStrength는 무시한다.
struct FMaterialAsset
{
	static constexpr const wchar_t* Extension = L".emat";

	std::string        Name;
	std::string        TexturePaths[MaterialSlot_Count]; // EMaterialTextureSlot 순서
	FMaterialConstants Constants;

	// 슬롯별 JSON 키 ("BaseColorTexture" 등)
	static const char* GetTextureKey(uint32 Slot);
	// 슬롯이 색상(sRGB) 텍스처인지 (베이스 컬러/발광)
	static bool IsSrgbSlot(uint32 Slot) { return Slot == MaterialSlot_BaseColor || Slot == MaterialSlot_Emissive; }

	std::string ToJsonString() const;
	bool        FromJsonString(const std::string& Json);

	bool LoadFromFile(const std::filesystem::path& Path);
	bool SaveToFile(const std::filesystem::path& Path) const;
};
