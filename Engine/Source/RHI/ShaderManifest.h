#pragma once

#include "Core/CoreTypes.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 셰이더 매니페스트(Engine/Shaders/Shaders.json)와 쿠킹 파일 규칙.
// 런타임 라이브러리(FShaderLibrary)와 쿠킹 도구(ProjectECook)가 같은 규칙을 공유한다.

struct FShaderManifestEntry
{
	std::wstring              File;       // 셰이더 디렉터리 기준 상대 경로
	std::wstring              EntryPoint;
	EShaderStage              Stage = EShaderStage::Vertex;
	std::vector<std::wstring> Defines;
	std::wstring              ShaderModel; // JSON "ShaderModel": "6_5" (비면 단계 기본 — FShaderCompileDesc::ShaderModel)

	FShaderCompileDesc ToCompileDesc() const;
};

struct FShaderManifest
{
	static constexpr const wchar_t* DefaultFileName = L"Shaders.json";

	std::vector<FShaderManifestEntry> Entries;

	// 파일 로드 (실패 시 Error 로그 + false)
	bool LoadFromFile(const std::filesystem::path& Path);

	// JSON 문자열 파싱 (순수 함수, 테스트용). 실패 시 OutError에 사유
	bool ParseJson(std::string_view Json, std::string& OutError);
};

// "Vertex" / "Pixel" / "Compute" / "Library" (대소문자 무시)
bool        ParseShaderStage(std::string_view Text, EShaderStage& OutStage);
const char* ShaderStageToString(EShaderStage Stage);

// 쿠킹 디렉터리: <엔진>/Engine/Shaders/Cooked
std::filesystem::path GetCookedShaderDirectory();

// 쿠킹 파일명: <File 스템>_<Entry>_<Stage>[_sm<모델>][_<디파인 FNV-1a 64비트 16진>][.debug].dxil
// Debug 구성의 DXIL(-Zi -Od)과 Release(-O3)를 구분하기 위해 bDebugVariant가 접미사를 붙인다.
std::wstring GetCookedShaderFileName(const FShaderCompileDesc& Desc, bool bDebugVariant = (E_DEBUG != 0));

// 디파인 목록 해시 (순서 포함). 비어 있으면 0
uint64 HashShaderDefines(std::span<const std::wstring> Defines);

// 변형 해시 = 디파인 해시에 가상 포함 파일(이름 + 내용)을 이어 섞은 값. 가상 파일이 없으면 HashShaderDefines와 같다
// (쿠킹 파일명 _<해시>, 캐시 키 — 같은 생성 소스는 같은 파일을 공유)
uint64 HashShaderVariant(const FShaderCompileDesc& Desc);

// 셰이더 소스와 그것이 #include "..." 로 참조하는 파일들(재귀). 첫 원소는 소스 자신.
// 존재하지 않는 포함 파일은 목록에 넣지 않는다.
std::vector<std::filesystem::path> CollectShaderDependencies(const std::filesystem::path& SourcePath,
                                                             const std::filesystem::path& ShaderDirectory);

// 소스 + 포함 파일 내용 해시 (FNV-1a 64, 파일 순서 포함). 파일을 하나라도 읽지 못하면 0.
// 쿠킹 DXIL 옆 사이드카(<쿠킹 파일>.srchash)에 기록해 두고, 로드 시 다시 계산해 비교한다
// (파일 시각은 git 체크아웃/머지/복사로 쉽게 어긋나므로 캐시 유효성에 쓰지 않는다)
uint64 HashShaderSources(std::span<const std::filesystem::path> Dependencies);
