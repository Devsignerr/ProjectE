#include "RHI/ShaderLibrary.h"

#include "Core/StringConv.h"
#include "RHI/ShaderManifest.h"

#include <fstream>
#include <vector>

namespace
{
	std::string MakeDisplayName(const FShaderCompileDesc& Desc)
	{
		return FStringConv::ToUtf8(Desc.FileName) + ":" + FStringConv::ToUtf8(Desc.EntryPoint);
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
		return Found->second;
	}

	const std::string DisplayName = MakeDisplayName(Desc);

	// 쿠킹된 DXIL
	if (ComPtr<IDxcBlob> Cooked = TryLoadCooked(Desc, DisplayName))
	{
		++Stats.CookedLoads;
		Cache[Key] = Cooked;
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
	Cache[Key] = Compiled;
	return Compiled;
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
	Cache[MakeCacheKey(Desc)] = Compiled;
	return SaveCooked(Desc, Compiled.Get(), DisplayName);
}

ComPtr<IDxcBlob> FShaderLibrary::TryLoadCooked(const FShaderCompileDesc& Desc, const std::string& DisplayName)
{
	const std::filesystem::path CookedPath = GetCookedPath(Desc);

	std::error_code ErrorCode;
	if (!std::filesystem::exists(CookedPath, ErrorCode))
	{
		return nullptr;
	}

	// 소스(+포함 파일)보다 오래되었으면 무효
	const std::filesystem::path ShaderDir  = FD3D12ShaderCompiler::GetEngineShaderDirectory();
	const std::filesystem::path SourcePath = ShaderDir / Desc.FileName;

	const std::vector<std::filesystem::path> Dependencies = CollectShaderDependencies(SourcePath, ShaderDir);
	std::vector<std::filesystem::file_time_type> SourceTimes;
	SourceTimes.reserve(Dependencies.size());
	for (const std::filesystem::path& Dependency : Dependencies)
	{
		const std::filesystem::file_time_type Time = std::filesystem::last_write_time(Dependency, ErrorCode);
		if (!ErrorCode)
		{
			SourceTimes.push_back(Time);
		}
	}
	const std::filesystem::file_time_type CookedTime = std::filesystem::last_write_time(CookedPath, ErrorCode);
	if (ErrorCode)
	{
		return nullptr;
	}
	// 소스가 없는 경우(패키지 배포)에는 쿠킹 파일을 그대로 신뢰한다
	if (!SourceTimes.empty() && !IsCookedShaderUpToDate(CookedTime, SourceTimes))
	{
		E_LOG(LogD3D12, Log, "쿠킹된 셰이더가 오래됨, 재컴파일: {}", DisplayName);
		return nullptr;
	}

	std::ifstream File(CookedPath, std::ios::binary | std::ios::ate);
	if (!File)
	{
		return nullptr;
	}
	const std::streamsize Size = File.tellg();
	if (Size <= 0)
	{
		return nullptr;
	}
	File.seekg(0);
	std::vector<char> Bytes(static_cast<size_t>(Size));
	if (!File.read(Bytes.data(), Size))
	{
		return nullptr;
	}

	ComPtr<IDxcBlob> Blob = Compiler->CreateBlob(Bytes.data(), Bytes.size());
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

	E_LOG(LogD3D12, Log, "셰이더 쿠킹 기록: {} → {}", DisplayName, FStringConv::ToUtf8(CookedPath.filename().wstring()));
	return true;
}
