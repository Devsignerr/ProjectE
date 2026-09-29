#pragma once

#include "RHI/D3D12/D3D12ShaderCompiler.h"

#include <filesystem>
#include <string>
#include <unordered_map>

// 셰이더 바이트코드 공급자. 조회 순서: 메모리 캐시 → 쿠킹된 DXIL(소스보다 새로울 때) → DXC 컴파일(+쿠킹 기록).
// 키 = 파일 + 엔트리 + 스테이지 + 디파인.
class FShaderLibrary
{
public:
	struct FStats
	{
		uint32 MemoryHits  = 0;
		uint32 CookedLoads = 0;
		uint32 Compiles    = 0;
		uint32 Failures    = 0;
	};

	// CookedDirectory 비어 있으면 GetCookedShaderDirectory(). bWriteCookedOnCompile: 컴파일 결과를 쿠킹 디렉터리에 기록
	bool Init(FD3D12ShaderCompiler& InCompiler, const std::filesystem::path& CookedDirectory = {}, bool bWriteCookedOnCompile = true);
	void Shutdown();

	// 실패 시 nullptr (로그 출력)
	ComPtr<IDxcBlob> GetShader(const FShaderCompileDesc& Desc);

	// 쿠킹 도구용: 캐시를 무시하고 컴파일해 쿠킹 파일로 기록
	bool CookShader(const FShaderCompileDesc& Desc);

	void ClearMemoryCache() { Cache.clear(); }

	const FStats&                GetStats() const { return Stats; }
	const std::filesystem::path& GetCookedDirectory() const { return CookedDir; }

	// 쿠킹 파일 경로 (CookedDirectory / GetCookedShaderFileName)
	std::filesystem::path GetCookedPath(const FShaderCompileDesc& Desc) const;

	static std::wstring MakeCacheKey(const FShaderCompileDesc& Desc);

private:
	ComPtr<IDxcBlob> TryLoadCooked(const FShaderCompileDesc& Desc, const std::string& DisplayName);
	bool             SaveCooked(const FShaderCompileDesc& Desc, IDxcBlob* Blob, const std::string& DisplayName);

	FD3D12ShaderCompiler*                              Compiler = nullptr;
	std::filesystem::path                              CookedDir;
	bool                                               bWriteCooked = true;
	std::unordered_map<std::wstring, ComPtr<IDxcBlob>> Cache;
	FStats                                             Stats;
};
