#include "RHI/ShaderManifest.h"

#include "Core/Paths.h"
#include "Core/StringConv.h"

#include <json.hpp>

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

FShaderCompileDesc FShaderManifestEntry::ToCompileDesc() const
{
	FShaderCompileDesc Desc;
	Desc.FileName   = File;
	Desc.EntryPoint = EntryPoint;
	Desc.Stage      = Stage;
	Desc.Defines    = Defines;
	return Desc;
}

bool FShaderManifest::LoadFromFile(const std::filesystem::path& Path)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
	{
		E_LOG(LogD3D12, Error, "셰이더 매니페스트를 열 수 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	std::stringstream Buffer;
	Buffer << File.rdbuf();

	std::string Error;
	if (!ParseJson(Buffer.str(), Error))
	{
		E_LOG(LogD3D12, Error, "셰이더 매니페스트 파싱 실패: {} ({})", FStringConv::ToUtf8(Path.wstring()), Error);
		return false;
	}
	return true;
}

bool FShaderManifest::ParseJson(std::string_view Json, std::string& OutError)
{
	Entries.clear();

	// UTF-8 BOM 허용
	if (Json.size() >= 3 && static_cast<uint8>(Json[0]) == 0xEF && static_cast<uint8>(Json[1]) == 0xBB && static_cast<uint8>(Json[2]) == 0xBF)
	{
		Json.remove_prefix(3);
	}

	const nlohmann::json Root = nlohmann::json::parse(Json, nullptr, false, true);
	if (Root.is_discarded())
	{
		OutError = "JSON 구문 오류";
		return false;
	}
	if (!Root.contains("Shaders") || !Root["Shaders"].is_array())
	{
		OutError = "\"Shaders\" 배열이 없습니다";
		return false;
	}

	for (const nlohmann::json& Item : Root["Shaders"])
	{
		if (!Item.is_object() || !Item.contains("File") || !Item.contains("Entry") || !Item.contains("Stage"))
		{
			OutError = "항목에 File/Entry/Stage가 모두 필요합니다";
			return false;
		}

		FShaderManifestEntry Entry;
		Entry.File       = FStringConv::ToWide(Item["File"].get<std::string>());
		Entry.EntryPoint = FStringConv::ToWide(Item["Entry"].get<std::string>());
		if (!ParseShaderStage(Item["Stage"].get<std::string>(), Entry.Stage))
		{
			OutError = "알 수 없는 Stage: " + Item["Stage"].get<std::string>();
			return false;
		}
		if (Item.contains("Defines") && Item["Defines"].is_array())
		{
			for (const nlohmann::json& Define : Item["Defines"])
			{
				Entry.Defines.push_back(FStringConv::ToWide(Define.get<std::string>()));
			}
		}
		Entries.push_back(std::move(Entry));
	}
	return true;
}

bool ParseShaderStage(std::string_view Text, EShaderStage& OutStage)
{
	std::string Lower(Text);
	std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char Char) { return static_cast<char>(std::tolower(Char)); });

	if (Lower == "vertex")
	{
		OutStage = EShaderStage::Vertex;
		return true;
	}
	if (Lower == "pixel")
	{
		OutStage = EShaderStage::Pixel;
		return true;
	}
	if (Lower == "compute")
	{
		OutStage = EShaderStage::Compute;
		return true;
	}
	return false;
}

const char* ShaderStageToString(EShaderStage Stage)
{
	switch (Stage)
	{
	case EShaderStage::Vertex:  return "Vertex";
	case EShaderStage::Pixel:   return "Pixel";
	case EShaderStage::Compute: return "Compute";
	}
	return "Unknown";
}

std::filesystem::path GetCookedShaderDirectory()
{
	return FPaths::GetEngineShaderDirectory() / L"Cooked";
}

uint64 HashShaderDefines(std::span<const std::wstring> Defines)
{
	if (Defines.empty())
	{
		return 0;
	}

	// FNV-1a 64
	uint64 Hash = 14695981039346656037ull;
	for (const std::wstring& Define : Defines)
	{
		for (const wchar_t Char : Define)
		{
			Hash ^= static_cast<uint64>(Char);
			Hash *= 1099511628211ull;
		}
		Hash ^= static_cast<uint64>(L';');
		Hash *= 1099511628211ull;
	}
	return Hash;
}

std::wstring GetCookedShaderFileName(const FShaderCompileDesc& Desc, bool bDebugVariant)
{
	std::wstring Name = std::filesystem::path(Desc.FileName).stem().wstring();
	Name += L"_" + Desc.EntryPoint;
	Name += L"_" + FStringConv::ToWide(ShaderStageToString(Desc.Stage));

	const uint64 DefineHash = HashShaderDefines(Desc.Defines);
	if (DefineHash != 0)
	{
		Name += std::format(L"_{:016x}", DefineHash);
	}
	if (bDebugVariant)
	{
		Name += L".debug";
	}
	Name += L".dxil";
	return Name;
}

namespace
{
	void CollectDependenciesRecursive(const std::filesystem::path& SourcePath, const std::filesystem::path& ShaderDirectory,
	                                  std::vector<std::filesystem::path>& OutPaths)
	{
		std::error_code ErrorCode;
		const std::filesystem::path Canonical = std::filesystem::weakly_canonical(SourcePath, ErrorCode);
		const std::filesystem::path Key       = ErrorCode ? SourcePath : Canonical;
		if (std::find(OutPaths.begin(), OutPaths.end(), Key) != OutPaths.end())
		{
			return;
		}
		if (!std::filesystem::exists(Key, ErrorCode))
		{
			return;
		}
		OutPaths.push_back(Key);

		std::ifstream File(Key);
		if (!File)
		{
			return;
		}

		static const std::regex IncludePattern(R"re(^\s*#\s*include\s*"([^"]+)")re");
		std::string              Line;
		while (std::getline(File, Line))
		{
			std::smatch Match;
			if (std::regex_search(Line, Match, IncludePattern))
			{
				const std::filesystem::path Included = FStringConv::ToWide(Match[1].str());
				// 소스 옆 → 셰이더 디렉터리 순으로 탐색 (DXC 기본 include 핸들러와 동일)
				const std::filesystem::path Sibling = Key.parent_path() / Included;
				if (std::filesystem::exists(Sibling, ErrorCode))
				{
					CollectDependenciesRecursive(Sibling, ShaderDirectory, OutPaths);
				}
				else
				{
					CollectDependenciesRecursive(ShaderDirectory / Included, ShaderDirectory, OutPaths);
				}
			}
		}
	}
} // namespace

std::vector<std::filesystem::path> CollectShaderDependencies(const std::filesystem::path& SourcePath,
                                                             const std::filesystem::path& ShaderDirectory)
{
	std::vector<std::filesystem::path> Paths;
	CollectDependenciesRecursive(SourcePath, ShaderDirectory, Paths);
	return Paths;
}

bool IsCookedShaderUpToDate(std::filesystem::file_time_type CookedTime, std::span<const std::filesystem::file_time_type> SourceTimes)
{
	if (SourceTimes.empty())
	{
		return false;
	}
	for (const std::filesystem::file_time_type& SourceTime : SourceTimes)
	{
		if (SourceTime > CookedTime)
		{
			return false;
		}
	}
	return true;
}
