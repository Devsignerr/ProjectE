#include "Renderer/MaterialAsset.h"

#include "Core/Log.h"
#include "Core/StringConv.h"

#include <json.hpp>

#include <fstream>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	using nlohmann::json;

	bool ReadFloats(const json& Document, const char* Key, float* Out, size_t Count)
	{
		const auto It = Document.find(Key);
		if (It == Document.end() || !It->is_array() || It->size() != Count)
		{
			return false;
		}
		for (size_t Index = 0; Index < Count; ++Index)
		{
			if (!(*It)[Index].is_number())
			{
				return false;
			}
			Out[Index] = (*It)[Index].get<float>();
		}
		return true;
	}
} // namespace

const char* FMaterialAsset::GetTextureKey(uint32 Slot)
{
	switch (Slot)
	{
	case MaterialSlot_BaseColor:         return "BaseColorTexture";
	case MaterialSlot_MetallicRoughness: return "MetallicRoughnessTexture";
	case MaterialSlot_Normal:            return "NormalTexture";
	case MaterialSlot_Occlusion:         return "OcclusionTexture";
	case MaterialSlot_Emissive:          return "EmissiveTexture";
	default:                             return "";
	}
}

std::string FMaterialAsset::ToJsonString() const
{
	json Document;
	Document["Name"]              = Name;
	Document["BaseColorFactor"]   = { Constants.BaseColorFactor.X, Constants.BaseColorFactor.Y, Constants.BaseColorFactor.Z, Constants.BaseColorFactor.W };
	Document["EmissiveFactor"]    = { Constants.EmissiveFactor.X, Constants.EmissiveFactor.Y, Constants.EmissiveFactor.Z };
	Document["Metallic"]          = Constants.Metallic;
	Document["Roughness"]         = Constants.Roughness;
	Document["NormalScale"]       = Constants.NormalScale;
	Document["OcclusionStrength"] = Constants.OcclusionStrength;
	for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
	{
		Document[GetTextureKey(Slot)] = TexturePaths[Slot];
	}
	return Document.dump(2);
}

bool FMaterialAsset::FromJsonString(const std::string& JsonText)
{
	const json Document = json::parse(JsonText, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
	if (Document.is_discarded() || !Document.is_object())
	{
		E_LOG(LogRenderer, Error, "머티리얼 JSON 파싱 실패");
		return false;
	}

	Name      = Document.value("Name", std::string());
	Constants = FMaterialConstants{};
	if (!ReadFloats(Document, "BaseColorFactor", &Constants.BaseColorFactor.X, 4))
	{
		ReadFloats(Document, "BaseColorTint", &Constants.BaseColorFactor.X, 4); // 구 형식 호환
	}
	ReadFloats(Document, "EmissiveFactor", &Constants.EmissiveFactor.X, 3);
	Constants.Metallic          = Document.value("Metallic", Constants.Metallic);
	Constants.Roughness         = Document.value("Roughness", Constants.Roughness);
	Constants.NormalScale       = Document.value("NormalScale", Constants.NormalScale);
	Constants.OcclusionStrength = Document.value("OcclusionStrength", Constants.OcclusionStrength);
	for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
	{
		TexturePaths[Slot] = Document.value(GetTextureKey(Slot), std::string());
	}
	return true;
}

bool FMaterialAsset::LoadFromFile(const std::filesystem::path& Path)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		E_LOG(LogRenderer, Error, "머티리얼 파일을 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	std::stringstream Buffer;
	Buffer << File.rdbuf();
	if (!FromJsonString(Buffer.str()))
	{
		E_LOG(LogRenderer, Error, "머티리얼 로드 실패: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	if (Name.empty())
	{
		Name = FStringConv::ToUtf8(Path.stem().wstring());
	}
	return true;
}

bool FMaterialAsset::SaveToFile(const std::filesystem::path& Path) const
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(Path.parent_path(), ErrorCode);

	std::ofstream File(Path, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		E_LOG(LogRenderer, Error, "머티리얼 파일을 쓸 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	File << ToJsonString();
	return true;
}
