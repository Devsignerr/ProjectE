#include "RHI/D3D12/D3D12ShaderCompiler.h"

#include "Core/StringConv.h"

#include <dxcapi.h>

namespace
{
	const wchar_t* GetTargetProfile(EShaderStage Stage)
	{
		switch (Stage)
		{
		case EShaderStage::Vertex:  return L"vs_6_0";
		case EShaderStage::Pixel:   return L"ps_6_0";
		case EShaderStage::Compute: return L"cs_6_0";
		}
		return L"";
	}
} // namespace

FD3D12ShaderCompiler::~FD3D12ShaderCompiler()
{
	Shutdown();
}

bool FD3D12ShaderCompiler::Init()
{
	E_D3D_VERIFY(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&Utils)));
	E_D3D_VERIFY(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&Compiler)));
	E_D3D_VERIFY(Utils->CreateDefaultIncludeHandler(&IncludeHandler));

	E_LOG(LogD3D12, Display, "셰이더 컴파일러(DXC) 초기화 완료, 셰이더 디렉터리: {}",
	      FStringConv::ToUtf8(GetEngineShaderDirectory().wstring()));
	return true;
}

void FD3D12ShaderCompiler::Shutdown()
{
	IncludeHandler.Reset();
	Compiler.Reset();
	Utils.Reset();
}

ComPtr<IDxcBlob> FD3D12ShaderCompiler::Compile(const FShaderCompileDesc& Desc) const
{
	E_CHECKF(Compiler != nullptr, "셰이더 컴파일러가 초기화되지 않았습니다");

	const std::filesystem::path ShaderDir = GetEngineShaderDirectory();
	const std::filesystem::path FullPath  = ShaderDir / Desc.FileName;
	const std::string           DisplayName = FStringConv::ToUtf8(Desc.FileName) + ":" + FStringConv::ToUtf8(Desc.EntryPoint);

	ComPtr<IDxcBlobEncoding> Source;
	if (FAILED(Utils->LoadFile(FullPath.c_str(), nullptr, &Source)))
	{
		E_LOG(LogD3D12, Error, "셰이더 파일을 열 수 없습니다: {}", FStringConv::ToUtf8(FullPath.wstring()));
		return nullptr;
	}

	BOOL   bKnownEncoding = FALSE;
	UINT32 CodePage       = DXC_CP_ACP;
	Source->GetEncoding(&bKnownEncoding, &CodePage);

	DxcBuffer SourceBuffer{};
	SourceBuffer.Ptr      = Source->GetBufferPointer();
	SourceBuffer.Size     = Source->GetBufferSize();
	SourceBuffer.Encoding = bKnownEncoding ? CodePage : DXC_CP_ACP;

	// 인자 문자열 수명 유지용 저장소
	std::vector<std::wstring> ArgStorage;
	ArgStorage.reserve(24 + Desc.Defines.size() * 2);
	ArgStorage.push_back(FullPath.wstring()); // 첫 인자: 오류 메시지용 소스 이름
	ArgStorage.push_back(L"-E");
	ArgStorage.push_back(Desc.EntryPoint);
	ArgStorage.push_back(L"-T");
	ArgStorage.push_back(GetTargetProfile(Desc.Stage));
	ArgStorage.push_back(L"-I");
	ArgStorage.push_back(ShaderDir.wstring());
	ArgStorage.push_back(L"-HV");
	ArgStorage.push_back(L"2021");
	ArgStorage.push_back(L"-Zpr"); // 행우선 행렬 (CPU FMatrix4x4 레이아웃과 동일)
	ArgStorage.push_back(L"-WX");  // 경고를 에러로
#if E_DEBUG
	ArgStorage.push_back(L"-Zi");
	ArgStorage.push_back(L"-Od");
	ArgStorage.push_back(L"-Qembed_debug");
#else
	ArgStorage.push_back(L"-O3");
	ArgStorage.push_back(L"-Qstrip_debug");
#endif
	for (const std::wstring& Define : Desc.Defines)
	{
		ArgStorage.push_back(L"-D");
		ArgStorage.push_back(Define);
	}

	std::vector<LPCWSTR> Args;
	Args.reserve(ArgStorage.size());
	for (const std::wstring& Arg : ArgStorage)
	{
		Args.push_back(Arg.c_str());
	}

	ComPtr<IDxcResult> Result;
	const HRESULT CompileHr = Compiler->Compile(&SourceBuffer, Args.data(), static_cast<UINT32>(Args.size()),
	                                            IncludeHandler.Get(), IID_PPV_ARGS(&Result));
	if (FAILED(CompileHr) || Result == nullptr)
	{
		E_LOG(LogD3D12, Error, "셰이더 컴파일 호출 실패: {} ({})", DisplayName, HResultToString(CompileHr));
		return nullptr;
	}

	HRESULT Status = S_OK;
	Result->GetStatus(&Status);

	// 경고/에러 메시지 (UTF-8)
	ComPtr<IDxcBlobUtf8> Errors;
	Result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&Errors), nullptr);
	if (Errors != nullptr && Errors->GetStringLength() > 0)
	{
		if (FAILED(Status))
		{
			E_LOG(LogD3D12, Error, "셰이더 컴파일 에러: {}\n{}", DisplayName, Errors->GetStringPointer());
		}
		else
		{
			E_LOG(LogD3D12, Warning, "셰이더 컴파일 경고: {}\n{}", DisplayName, Errors->GetStringPointer());
		}
	}

	if (FAILED(Status))
	{
		return nullptr;
	}

	ComPtr<IDxcBlob> Object;
	if (FAILED(Result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&Object), nullptr)) || Object == nullptr)
	{
		E_LOG(LogD3D12, Error, "셰이더 오브젝트를 가져올 수 없습니다: {}", DisplayName);
		return nullptr;
	}

	E_LOG(LogD3D12, Log, "셰이더 컴파일 완료: {} ({} bytes)", DisplayName, Object->GetBufferSize());
	return Object;
}

std::filesystem::path FD3D12ShaderCompiler::GetEngineShaderDirectory()
{
	return std::filesystem::path(E_ENGINE_SHADER_DIR);
}

D3D12_SHADER_BYTECODE FD3D12ShaderCompiler::ToBytecode(IDxcBlob* Blob)
{
	E_CHECKF(Blob != nullptr, "셰이더 블롭이 비어 있습니다");
	return { Blob->GetBufferPointer(), Blob->GetBufferSize() };
}
