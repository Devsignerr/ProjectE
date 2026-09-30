#include "RHI/ShaderLibrary.h"

#include "Core/FileSystem.h"
#include "Core/StringConv.h"
#include "RHI/ShaderManifest.h"

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <format>
#include <fstream>
#include <vector>

namespace
{
	std::string MakeDisplayName(const FShaderCompileDesc& Desc)
	{
		return FStringConv::ToUtf8(Desc.FileName) + ":" + FStringConv::ToUtf8(Desc.EntryPoint);
	}

	// 경로 비교용 정규화 (절대 경로, 소문자 — Windows 파일 시스템은 대소문자 무시)
	std::wstring NormalizePathKey(const std::filesystem::path& Path)
	{
		std::error_code       ErrorCode;
		std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
		if (ErrorCode)
		{
			Canonical = Path;
		}
		std::wstring Key = Canonical.lexically_normal().wstring();
		std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Key;
	}

	std::filesystem::path GetSourceHashPath(const std::filesystem::path& CookedPath)
	{
		std::filesystem::path Path = CookedPath;
		Path += L".srchash";
		return Path;
	}

	// 사이드카가 없거나 형식이 틀리면 0
	uint64 ReadSourceHash(const std::filesystem::path& CookedPath)
	{
		std::string Text;
		if (!FFileSystem::ReadTextFile(GetSourceHashPath(CookedPath), Text))
		{
			return 0;
		}
		while (!Text.empty() && (Text.back() == '\n' || Text.back() == '\r' || Text.back() == ' '))
		{
			Text.pop_back();
		}
		if (Text.size() != 16)
		{
			return 0;
		}
		uint64 Value = 0;
		const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value, 16);
		return Error == std::errc() && End == Text.data() + Text.size() ? Value : 0;
	}
} // namespace

bool FShaderLibrary::Init(FD3D12ShaderCompiler& InCompiler, const std::filesystem::path& CookedDirectory, bool bWriteCookedOnCompile)
{
	Compiler     = &InCompiler;
	CookedDir    = CookedDirectory.empty() ? GetCookedShaderDirectory() : CookedDirectory;
	bWriteCooked = bWriteCookedOnCompile;
	Cache.clear();
	Stats = FStats{};

	E_LOG(LogD3D12, Log, "셰이더 라이브러리 초기화: 쿠킹 디렉터리 {}", FStringConv::ToUtf8(CookedDir.wstring()));
	return true;
}

void FShaderLibrary::Shutdown()
{
	Cache.clear();
	Compiler = nullptr;
}

std::wstring FShaderLibrary::MakeCacheKey(const FShaderCompileDesc& Desc)
{
	std::wstring Key = Desc.FileName + L"|" + Desc.EntryPoint + L"|" + FStringConv::ToWide(ShaderStageToString(Desc.Stage));
	for (const std::wstring& Define : Desc.Defines)
	{
		Key += L"|" + Define;
	}
	return Key;
}

std::filesystem::path FShaderLibrary::GetCookedPath(const FShaderCompileDesc& Desc) const
{
	return CookedDir / GetCookedShaderFileName(Desc);
}

ComPtr<IDxcBlob> FShaderLibrary::GetShader(const FShaderCompileDesc& Desc)
{
	E_CHECKF(Compiler != nullptr, "셰이더 라이브러리가 초기화되지 않았습니다");

	const std::wstring Key = MakeCacheKey(Desc);
	if (const auto Found = Cache.find(Key); Found != Cache.end())
	{
		++Stats.MemoryHits;
		return Found->second.Blob;
	}

	const std::string DisplayName = MakeDisplayName(Desc);

	// 쿠킹된 DXIL
	if (ComPtr<IDxcBlob> Cooked = TryLoadCooked(Desc, DisplayName))
	{
		++Stats.CookedLoads;
		Cache[Key] = FCacheEntry{ Desc, Cooked };
		return Cooked;
	}

	// 컴파일
	ComPtr<IDxcBlob> Compiled = Compiler->Compile(Desc);
	if (!Compiled)
	{
		++Stats.Failures;
		return nullptr;
	}
	++Stats.Compiles;
	if (bWriteCooked)
	{
		SaveCooked(Desc, Compiled.Get(), DisplayName);
	}
	Cache[Key] = FCacheEntry{ Desc, Compiled };
	return Compiled;
}

std::vector<FShaderCompileDesc> FShaderLibrary::Invalidate(const std::filesystem::path& ChangedFile)
{
	const std::filesystem::path ShaderDir  = FD3D12ShaderCompiler::GetEngineShaderDirectory();
	const std::wstring          ChangedKey = NormalizePathKey(ChangedFile);

	std::vector<FShaderCompileDesc> Affected;
	for (auto Iterator = Cache.begin(); Iterator != Cache.end();)
	{
		const std::vector<std::filesystem::path> Dependencies =
			CollectShaderDependencies(ShaderDir / Iterator->second.Desc.FileName, ShaderDir);
		const bool bUses = std::any_of(Dependencies.begin(), Dependencies.end(),
		                               [&](const std::filesystem::path& Dependency) { return NormalizePathKey(Dependency) == ChangedKey; });
		if (bUses)
		{
			Affected.push_back(Iterator->second.Desc);
			Iterator = Cache.erase(Iterator);
		}
		else
		{
			++Iterator;
		}
	}

	if (!Affected.empty())
	{
		++Generation;
		E_LOG(LogD3D12, Log, "셰이더 캐시 무효화: {} → {}개", FStringConv::ToUtf8(ChangedFile.filename().wstring()), Affected.size());
	}
	return Affected;
}

std::vector<FShaderCompileDesc> FShaderLibrary::InvalidateAll()
{
	std::vector<FShaderCompileDesc> Affected;
	Affected.reserve(Cache.size());
	for (const auto& [Key, Entry] : Cache)
	{
		Affected.push_back(Entry.Desc);
	}
	Cache.clear();
	if (!Affected.empty())
	{
		++Generation;
	}
	return Affected;
}

bool FShaderLibrary::CookShader(const FShaderCompileDesc& Desc)
{
	E_CHECKF(Compiler != nullptr, "셰이더 라이브러리가 초기화되지 않았습니다");

	const std::string DisplayName = MakeDisplayName(Desc);
	ComPtr<IDxcBlob>  Compiled    = Compiler->Compile(Desc);
	if (!Compiled)
	{
		++Stats.Failures;
		return false;
	}
	++Stats.Compiles;
	Cache[MakeCacheKey(Desc)] = FCacheEntry{ Desc, Compiled };
	return SaveCooked(Desc, Compiled.Get(), DisplayName);
}

ComPtr<IDxcBlob> FShaderLibrary::TryLoadCooked(const FShaderCompileDesc& Desc, const std::string& DisplayName)
{
	const std::filesystem::path CookedPath = GetCookedPath(Desc);

	std::error_code ErrorCode;
	if (!FFileSystem::Exists(CookedPath))
	{
		return nullptr;
	}

	// 소스(+포함 파일) 내용이 쿠킹 당시와 같은지 (사이드카 해시). 소스가 없으면(패키지 배포) 쿠킹 파일을 그대로 신뢰한다
	const std::filesystem::path ShaderDir  = FD3D12ShaderCompiler::GetEngineShaderDirectory();
	const std::filesystem::path SourcePath = ShaderDir / Desc.FileName;
	if (std::filesystem::exists(SourcePath, ErrorCode))
	{
		const uint64 SourceHash = HashShaderSources(CollectShaderDependencies(SourcePath, ShaderDir));
		if (SourceHash == 0 || ReadSourceHash(CookedPath) != SourceHash)
		{
			E_LOG(LogD3D12, Log, "쿠킹된 셰이더가 현재 소스와 다름(내용 해시), 재컴파일: {}", DisplayName);
			return nullptr;
		}
	}

	std::vector<uint8> Bytes;
	if (!FFileSystem::ReadFile(CookedPath, Bytes) || Bytes.empty())
	{
		return nullptr;
	}

	ComPtr<IDxcBlob> Blob = FD3D12ShaderCompiler::CreateBlob(Bytes.data(), Bytes.size());
	if (Blob)
	{
		E_LOG(LogD3D12, Log, "쿠킹된 셰이더 로드: {} ({} bytes)", DisplayName, Bytes.size());
	}
	return Blob;
}

bool FShaderLibrary::SaveCooked(const FShaderCompileDesc& Desc, IDxcBlob* Blob, const std::string& DisplayName)
{
	std::error_code ErrorCode;
	std::filesystem::create_directories(CookedDir, ErrorCode);

	const std::filesystem::path CookedPath = GetCookedPath(Desc);
	std::ofstream               File(CookedPath, std::ios::binary | std::ios::trunc);
	if (!File)
	{
		// 읽기 전용 배포 위치 등: 실패해도 런타임 동작에는 지장 없음
		E_LOG(LogD3D12, Warning, "쿠킹 파일을 쓸 수 없습니다: {}", FStringConv::ToUtf8(CookedPath.wstring()));
		return false;
	}
	File.write(static_cast<const char*>(Blob->GetBufferPointer()), static_cast<std::streamsize>(Blob->GetBufferSize()));
	if (!File)
	{
		E_LOG(LogD3D12, Warning, "쿠킹 파일 기록 실패: {}", FStringConv::ToUtf8(CookedPath.wstring()));
		return false;
	}

	File.close();

	// 소스 내용 해시 사이드카 (로드 시 유효성 검사)
	const std::filesystem::path ShaderDir = FD3D12ShaderCompiler::GetEngineShaderDirectory();
	const uint64 SourceHash = HashShaderSources(CollectShaderDependencies(ShaderDir / Desc.FileName, ShaderDir));
	std::ofstream HashFile(GetSourceHashPath(CookedPath), std::ios::binary | std::ios::trunc);
	HashFile << std::format("{:016X}", SourceHash);

	E_LOG(LogD3D12, Log, "셰이더 쿠킹 기록: {} → {}", DisplayName, FStringConv::ToUtf8(CookedPath.filename().wstring()));
	return true;
}
