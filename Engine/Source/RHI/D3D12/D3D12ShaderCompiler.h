#pragma once

#include "RHI/D3D12/D3D12Common.h"

#include <dxcapi.h> // ComPtr<IDxcBlob> 사용자를 위해 완전한 타입 필요

#include <filesystem>
#include <string>
#include <vector>

enum class EShaderStage : uint8
{
	Vertex,
	Pixel,
	Compute,
	Library, // DXR 라이브러리 (lib_6_3 이상 — 히트/미스/레이 생성 셰이더 묶음, 진입점 없이 컴파일. 상태 객체 DXIL 라이브러리로 쓴다)
};

// 디스크에 없는 포함 파일 (생성 소스 — 예: 머티리얼 그래프 MaterialGraph.generated.hlsli). #include "Name"을 이 내용으로 푼다
struct FShaderVirtualFile
{
	std::wstring Name;    // 파일 이름 (경로 없이)
	std::string  Content; // UTF-8
};

struct FShaderCompileDesc
{
	std::wstring              FileName;   // 엔진 셰이더 디렉터리 기준 상대 경로 (예: L"Triangle.hlsl")
	std::wstring              EntryPoint; // 예: L"VSMain"
	EShaderStage              Stage = EShaderStage::Vertex;
	std::vector<std::wstring> Defines;    // "NAME" 또는 "NAME=VALUE"
	std::vector<FShaderVirtualFile> VirtualFiles; // 생성 소스 포함 파일 (내용 해시가 캐시 키·쿠킹 파일명에 들어간다)
	// 셰이더 모델 "6_5" 같은 형식 (비면 기본: 정점/픽셀/계산 6_0, 라이브러리 6_3). 인라인 RayQuery는 6_5 이상.
	// 기본이 아니면 쿠킹 파일명에 _sm<모델>이 붙는다 (GetCookedShaderFileName)
	std::wstring ShaderModel;
};

// 대상 프로필 ("cs_6_5", "lib_6_3" ...). Desc.ShaderModel이 비면 단계별 기본 모델
std::wstring GetShaderTargetProfile(EShaderStage Stage, const std::wstring& ShaderModel);

// DXC 기반 HLSL → DXIL 런타임 컴파일러 (기본 Shader Model 6.0, FShaderCompileDesc::ShaderModel로 올림).
// dxcompiler.dll은 지연 로드된다: DLL이 없으면(쿠킹 셰이더만 배포한 패키지) Init은 성공하지만 IsAvailable() == false이고
// Compile은 실패한다. 쿠킹 DXIL 로드(CreateBlob)는 DXC 없이 동작한다.
class FD3D12ShaderCompiler
{
public:
	~FD3D12ShaderCompiler();

	bool Init();
	bool IsAvailable() const { return Compiler != nullptr; }
	void Shutdown();

	// 실패 시 nullptr 반환 (에러 메시지는 로그로 출력)
	ComPtr<IDxcBlob> Compile(const FShaderCompileDesc& Desc) const;

	// 메모리의 DXIL 바이트를 복사해 블롭으로 만든다 (쿠킹된 셰이더 로드용, DXC 불필요)
	static ComPtr<IDxcBlob> CreateBlob(const void* Data, size_t SizeInBytes);

	// 엔진 셰이더 소스 디렉터리 (빌드 시점에 CMake가 정의; 에셋 파이프라인 도입 전 임시)
	static std::filesystem::path GetEngineShaderDirectory();

	static D3D12_SHADER_BYTECODE ToBytecode(IDxcBlob* Blob);

private:
	ComPtr<IDxcUtils>          Utils;
	ComPtr<IDxcCompiler3>      Compiler;
	ComPtr<IDxcIncludeHandler> IncludeHandler;
};
