#pragma once

#include "Renderer/ShaderTypes.h"

#include <filesystem>
#include <string>

// .emat 파일 내용 (JSON). 텍스처 경로는 .emat 파일이 있는 디렉터리 기준 상대 경로.
//   { "Name": "Checker", "BaseColorTexture": "../UVChecker.png",
//     "BaseColorTint": [1,1,1,1], "SpecularColor": [0.04,0.04,0.04], "Shininess": 64, "SpecularStrength": 1 }
struct FMaterialAsset
{
	static constexpr const wchar_t* Extension = L".emat";

	std::string        Name;
	std::string        BaseColorTexture; // 비어 있으면 흰색
	FMaterialConstants Constants;

	std::string ToJsonString() const;
	bool        FromJsonString(const std::string& Json);

	bool LoadFromFile(const std::filesystem::path& Path);
	bool SaveToFile(const std::filesystem::path& Path) const;
};
