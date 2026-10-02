#include "Renderer/MaterialAsset.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"

#include <json.hpp>

#include <algorithm>
#include <format>
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

const char* FMaterialAsset::GetBlendModeName(EMaterialBlendMode Mode)
{
	switch (Mode)
	{
	case EMaterialBlendMode::Masked:      return "Masked";
	case EMaterialBlendMode::Translucent: return "Translucent";
	case EMaterialBlendMode::Additive:    return "Additive";
	default:                              return "Opaque";
	}
}

EMaterialBlendMode FMaterialAsset::ParseBlendMode(const std::string& Name, EMaterialBlendMode Fallback)
{
	for (uint32 Index = 0; Index < static_cast<uint32>(EMaterialBlendMode::Count); ++Index)
	{
		const EMaterialBlendMode Mode = static_cast<EMaterialBlendMode>(Index);
		if (Name == GetBlendModeName(Mode))
		{
			return Mode;
		}
	}
	return Fallback;
}

std::string FMaterialAsset::ToJsonString() const
{
	// 인스턴스는 덮어쓰는 키만 쓴다 (나머지는 부모를 따른다)
	json Document;
	Document["Name"] = Name;
	if (IsInstance())
	{
		Document["Parent"] = Parent;
	}
	if (Overrides(Field_BaseColorFactor))
	{
		Document["BaseColorFactor"] = { Constants.BaseColorFactor.X, Constants.BaseColorFactor.Y, Constants.BaseColorFactor.Z, Constants.BaseColorFactor.W };
	}
	if (Overrides(Field_EmissiveFactor))
	{
		Document["EmissiveFactor"] = { Constants.EmissiveFactor.X, Constants.EmissiveFactor.Y, Constants.EmissiveFactor.Z };
	}
	if (Overrides(Field_Metallic))
	{
		Document["Metallic"] = Constants.Metallic;
	}
	if (Overrides(Field_Roughness))
	{
		Document["Roughness"] = Constants.Roughness;
	}
	if (Overrides(Field_NormalScale))
	{
		Document["NormalScale"] = Constants.NormalScale;
	}
	if (Overrides(Field_OcclusionStrength))
	{
		Document["OcclusionStrength"] = Constants.OcclusionStrength;
	}
	if (Overrides(Field_BlendMode))
	{
		Document["BlendMode"] = GetBlendModeName(BlendMode);
	}
	if (Overrides(Field_AlphaCutoff))
	{
		Document["AlphaCutoff"] = Constants.AlphaCutoff;
	}
	if (Overrides(Field_TwoSided))
	{
		Document["TwoSided"] = bTwoSided;
	}
	for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
	{
		if (Overrides(GetTextureField(Slot)))
		{
			Document[GetTextureKey(Slot)] = TexturePaths[Slot];
		}
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

	Name         = Document.value("Name", std::string());
	Parent       = Document.value("Parent", std::string());
	Constants    = FMaterialConstants{};
	BlendMode    = EMaterialBlendMode::Opaque;
	bTwoSided    = false;
	OverrideMask = 0;
	const auto Has = [&](const char* Key) { return Document.contains(Key); };

	if (ReadFloats(Document, "BaseColorFactor", &Constants.BaseColorFactor.X, 4) ||
	    ReadFloats(Document, "BaseColorTint", &Constants.BaseColorFactor.X, 4)) // 구 형식 호환
	{
		OverrideMask |= Field_BaseColorFactor;
	}
	if (ReadFloats(Document, "EmissiveFactor", &Constants.EmissiveFactor.X, 3))
	{
		OverrideMask |= Field_EmissiveFactor;
	}
	Constants.Metallic          = Document.value("Metallic", Constants.Metallic);
	Constants.Roughness         = Document.value("Roughness", Constants.Roughness);
	Constants.NormalScale       = Document.value("NormalScale", Constants.NormalScale);
	Constants.OcclusionStrength = Document.value("OcclusionStrength", Constants.OcclusionStrength);
	Constants.AlphaCutoff       = Document.value("AlphaCutoff", Constants.AlphaCutoff);
	BlendMode                   = ParseBlendMode(Document.value("BlendMode", std::string()), BlendMode);
	bTwoSided                   = Document.value("TwoSided", bTwoSided);
	OverrideMask |= (Has("Metallic") ? Field_Metallic : 0u) | (Has("Roughness") ? Field_Roughness : 0u) | (Has("NormalScale") ? Field_NormalScale : 0u) |
	                (Has("OcclusionStrength") ? Field_OcclusionStrength : 0u) | (Has("AlphaCutoff") ? Field_AlphaCutoff : 0u) |
	                (Has("BlendMode") ? Field_BlendMode : 0u) | (Has("TwoSided") ? Field_TwoSided : 0u);
	for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
	{
		TexturePaths[Slot] = Document.value(GetTextureKey(Slot), std::string());
		if (Has(GetTextureKey(Slot)))
		{
			OverrideMask |= GetTextureField(Slot);
		}
	}
	if (!IsInstance())
	{
		OverrideMask = Field_All; // 일반 머티리얼: 없는 키는 기본값이 자기 값
	}
	return true;
}

std::wstring FMaterialAsset::MakePathKey(const std::filesystem::path& Path)
{
	std::wstring Key = Path.lexically_normal().generic_wstring();
	for (wchar_t& Char : Key)
	{
		if (Char >= L'A' && Char <= L'Z')
		{
			Char = static_cast<wchar_t>(Char - L'A' + L'a');
		}
	}
	return Key;
}

bool FMaterialAsset::Resolve(const FMaterialAsset& Asset, const std::filesystem::path& Path, const FLoader& Loader, FMaterialAsset& OutResolved,
                             std::vector<std::filesystem::path>* OutChain, std::string* OutError)
{
	// 1) 체인 수집: [자신, 부모, 조부모, ...] (각자 파일 경로와 함께). 끊기면 맨 끝 고리는 인스턴스로 남아 덮어쓴 키만 적용된다
	struct FLink
	{
		FMaterialAsset        Asset;
		std::filesystem::path Path;
	};
	std::vector<FLink> Chain;
	Chain.push_back({ Asset, Path.lexically_normal() });
	std::vector<std::wstring> Visited = { MakePathKey(Path) };
	std::string               Error;
	if (OutChain != nullptr)
	{
		OutChain->clear();
	}
	while (Chain.back().Asset.IsInstance())
	{
		const FLink&                Child      = Chain.back();
		const std::filesystem::path ParentPath = (Child.Path.parent_path() / FStringConv::ToWide(Child.Asset.Parent)).lexically_normal();
		const std::wstring          Key        = MakePathKey(ParentPath);
		if (std::find(Visited.begin(), Visited.end(), Key) != Visited.end())
		{
			Error = std::format("머티리얼 부모 순환: {} → {}", FStringConv::ToUtf8(Child.Path.generic_wstring()), Child.Asset.Parent);
			break;
		}
		if (Chain.size() > MaxParentDepth)
		{
			Error = std::format("머티리얼 부모 체인이 너무 깁니다 (최대 {}): {}", MaxParentDepth, FStringConv::ToUtf8(Path.generic_wstring()));
			break;
		}
		FMaterialAsset ParentAsset;
		if (!Loader || !Loader(ParentPath, ParentAsset))
		{
			Error = std::format("머티리얼 부모를 읽을 수 없습니다: {}", FStringConv::ToUtf8(ParentPath.generic_wstring()));
			break;
		}
		Visited.push_back(Key);
		if (OutChain != nullptr)
		{
			OutChain->push_back(ParentPath);
		}
		Chain.push_back({ std::move(ParentAsset), ParentPath });
	}

	// 2) 가장 먼 조상부터 덮어쓰기 (일반 머티리얼 고리는 모든 필드, 인스턴스 고리는 OverrideMask 필드만)
	const std::filesystem::path BaseDirectory = Chain.front().Path.parent_path();
	FMaterialAsset              Result;
	for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
	{
		const FMaterialAsset& Link = It->Asset;
		if (Link.Overrides(Field_BaseColorFactor)) Result.Constants.BaseColorFactor = Link.Constants.BaseColorFactor;
		if (Link.Overrides(Field_EmissiveFactor)) Result.Constants.EmissiveFactor = Link.Constants.EmissiveFactor;
		if (Link.Overrides(Field_Metallic)) Result.Constants.Metallic = Link.Constants.Metallic;
		if (Link.Overrides(Field_Roughness)) Result.Constants.Roughness = Link.Constants.Roughness;
		if (Link.Overrides(Field_NormalScale)) Result.Constants.NormalScale = Link.Constants.NormalScale;
		if (Link.Overrides(Field_OcclusionStrength)) Result.Constants.OcclusionStrength = Link.Constants.OcclusionStrength;
		if (Link.Overrides(Field_AlphaCutoff)) Result.Constants.AlphaCutoff = Link.Constants.AlphaCutoff;
		if (Link.Overrides(Field_BlendMode)) Result.BlendMode = Link.BlendMode;
		if (Link.Overrides(Field_TwoSided)) Result.bTwoSided = Link.bTwoSided;
		for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
		{
			if (!Link.Overrides(GetTextureField(Slot)))
			{
				continue;
			}
			const std::string& Texture = Link.TexturePaths[Slot];
			if (Texture.empty() || It->Path.parent_path() == BaseDirectory)
			{
				Result.TexturePaths[Slot] = Texture;
				continue;
			}
			// 조상 폴더 기준 → 이 파일 폴더 기준 (상대 경로를 만들 수 없으면(드라이브가 다름) 절대 경로)
			const std::filesystem::path Absolute = (It->Path.parent_path() / FStringConv::ToWide(Texture)).lexically_normal();
			const std::filesystem::path Relative = Absolute.lexically_relative(BaseDirectory);
			Result.TexturePaths[Slot]            = FStringConv::ToUtf8((Relative.empty() ? Absolute : Relative).generic_wstring());
		}
	}
	Result.Name         = Asset.Name;
	Result.Parent.clear();
	Result.OverrideMask = Field_All;
	OutResolved         = std::move(Result);

	if (!Error.empty())
	{
		E_LOG(LogRenderer, Error, "{}", Error);
		if (OutError != nullptr)
		{
			*OutError = Error;
		}
		return false;
	}
	return true;
}

bool FMaterialAsset::LoadFromFile(const std::filesystem::path& Path)
{
	std::string Text;
	if (!FFileSystem::ReadTextFile(Path, Text))
	{
		E_LOG(LogRenderer, Error, "머티리얼 파일을 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	if (!FromJsonString(Text))
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
