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

std::string FMaterialAsset::ToJsonString() const
{
	json Document;
	Document["Name"]             = Name;
	Document["BaseColorTexture"] = BaseColorTexture;
	Document["BaseColorTint"]    = { Constants.BaseColorTint.X, Constants.BaseColorTint.Y, Constants.BaseColorTint.Z, Constants.BaseColorTint.W };
	Document["SpecularColor"]    = { Constants.SpecularColor.X, Constants.SpecularColor.Y, Constants.SpecularColor.Z };
	Document["Shininess"]        = Constants.Shininess;
	Document["SpecularStrength"] = Constants.SpecularStrength;
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

	Name             = Document.value("Name", std::string());
	BaseColorTexture = Document.value("BaseColorTexture", std::string());
	Constants        = FMaterialConstants{};
	ReadFloats(Document, "BaseColorTint", &Constants.BaseColorTint.X, 4);
	ReadFloats(Document, "SpecularColor", &Constants.SpecularColor.X, 3);
	Constants.Shininess        = Document.value("Shininess", Constants.Shininess);
	Constants.SpecularStrength = Document.value("SpecularStrength", Constants.SpecularStrength);
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
