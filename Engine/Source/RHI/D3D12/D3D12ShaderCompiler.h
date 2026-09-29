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
};

struct FShaderCompileDesc
{
	std::wstring              FileName;   // 엔진 셰이더 디렉터리 기준 상대 경로 (예: L"Triangle.hlsl")
	std::wstring              EntryPoint; // 예: L"VSMain"
	EShaderStage              Stage = EShaderStage::Vertex;
	std::vector<std::wstring> Defines;    // "NAME" 또는 "NAME=VALUE"
};

// DXC 기반 HLSL → DXIL 런타임 컴파일러 (Shader Model 6.0)
class FD3D12ShaderCompiler
{
public:
	~FD3D12ShaderCompiler();

	bool Init();
	void Shutdown();

	// 실패 시 nullptr 반환 (에러 메시지는 로그로 출력)
	ComPtr<IDxcBlob> Compile(const FShaderCompileDesc& Desc) const;

	// 메모리의 DXIL 바이트를 복사해 블롭으로 만든다 (쿠킹된 셰이더 로드용)
	ComPtr<IDxcBlob> CreateBlob(const void* Data, size_t SizeInBytes) const;

	// 엔진 셰이더 소스 디렉터리 (빌드 시점에 CMake가 정의; 에셋 파이프라인 도입 전 임시)
	static std::filesystem::path GetEngineShaderDirectory();

	static D3D12_SHADER_BYTECODE ToBytecode(IDxcBlob* Blob);

private:
	ComPtr<IDxcUtils>          Utils;
	ComPtr<IDxcCompiler3>      Compiler;
	ComPtr<IDxcIncludeHandler> IncludeHandler;
};
