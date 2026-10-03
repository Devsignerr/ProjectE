#pragma once

#include "RHI/D3D12/D3D12ShaderCompiler.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// 셰이더 바이트코드 공급자. 조회 순서: 메모리 캐시 → 쿠킹된 DXIL(소스보다 새로울 때) → DXC 컴파일(+쿠킹 기록).
// 키 = 파일 + 엔트리 + 스테이지 + 디파인 (+ 가상 포함 파일 내용 해시 — 머티리얼 그래프 생성 소스, 같은 해시는 같은 쿠킹 파일을 공유).
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

	// 핫 리로드: ChangedFile(셰이더 소스 또는 #include 파일)을 쓰는 캐시 항목을 제거하고 영향받은 desc 목록을 반환.
	// 다음 GetShader는 쿠킹 파일 mtime 검사로 재컴파일한다. 비어 있지 않으면 세대 카운터 증가.
	std::vector<FShaderCompileDesc> Invalidate(const std::filesystem::path& ChangedFile);

	// 캐시된 모든 셰이더를 무효화하고 그 desc 목록 반환
	std::vector<FShaderCompileDesc> InvalidateAll();

	// 무효화가 일어날 때마다 증가 (셰이더를 캐시한 쪽이 변경 여부를 판단할 때 사용)
	uint64 GetGeneration() const { return Generation; }

	const FStats&                GetStats() const { return Stats; }
	const std::filesystem::path& GetCookedDirectory() const { return CookedDir; }

	// 쿠킹 파일 경로 (CookedDirectory / GetCookedShaderFileName)
	std::filesystem::path GetCookedPath(const FShaderCompileDesc& Desc) const;

	static std::wstring MakeCacheKey(const FShaderCompileDesc& Desc);

private:
	ComPtr<IDxcBlob> TryLoadCooked(const FShaderCompileDesc& Desc, const std::string& DisplayName);
	bool             SaveCooked(const FShaderCompileDesc& Desc, IDxcBlob* Blob, const std::string& DisplayName);

	FD3D12ShaderCompiler* Compiler = nullptr;
	std::filesystem::path CookedDir;
	bool                  bWriteCooked = true;
	struct FCacheEntry
	{
		FShaderCompileDesc Desc;
		ComPtr<IDxcBlob>   Blob;
	};

	std::unordered_map<std::wstring, FCacheEntry> Cache;
	FStats                                        Stats;
	uint64                                        Generation = 0;
};
