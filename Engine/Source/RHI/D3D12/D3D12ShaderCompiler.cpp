#include "RHI/D3D12/D3D12ShaderCompiler.h"

#include "Core/Paths.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"

#include <dxcapi.h>
#include <wrl/implements.h>

#include <vector>

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

	// DXC 없이 쓰는 최소 IDxcBlob (쿠킹 DXIL 보관). 참조 카운트는 WRL이 관리
	class FMemoryBlob final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDxcBlob>
	{
	public:
		FMemoryBlob(const void* Data, size_t SizeInBytes)
			: Bytes(static_cast<const uint8*>(Data), static_cast<const uint8*>(Data) + SizeInBytes)
		{
		}

		LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return Bytes.data(); }
		SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return Bytes.size(); }

	private:
		std::vector<uint8> Bytes;
	};

	// 가상 포함 파일(FShaderCompileDesc::VirtualFiles)을 먼저 찾고, 없으면 기본 핸들러(디스크)에 넘긴다
	class FVirtualIncludeHandler final
		: public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDxcIncludeHandler>
	{
	public:
		FVirtualIncludeHandler(IDxcUtils* InUtils, IDxcIncludeHandler* InFallback, const std::vector<FShaderVirtualFile>* InFiles)
			: Utils(InUtils)
			, Fallback(InFallback)
			, Files(InFiles)
		{
		}

		HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR FileName, IDxcBlob** OutSource) override
		{
			const std::wstring Name = std::filesystem::path(FileName).filename().wstring();
			for (const FShaderVirtualFile& File : *Files)
			{
				if (_wcsicmp(File.Name.c_str(), Name.c_str()) == 0)
				{
					ComPtr<IDxcBlobEncoding> Blob;
					const HRESULT Result = Utils->CreateBlob(File.Content.data(), static_cast<UINT32>(File.Content.size()), DXC_CP_UTF8, &Blob);
					if (FAILED(Result))
					{
						return Result;
					}
					*OutSource = Blob.Detach();
					return S_OK;
				}
			}
			return Fallback->LoadSource(FileName, OutSource);
		}

	private:
		IDxcUtils*                             Utils;
		IDxcIncludeHandler*                    Fallback;
		const std::vector<FShaderVirtualFile>* Files;
	};
} // namespace

FD3D12ShaderCompiler::~FD3D12ShaderCompiler()
{
	Shutdown();
}

bool FD3D12ShaderCompiler::Init()
{
	// 지연 로드 DLL: 먼저 존재를 확인한다 (없는 상태로 DXC 함수를 부르면 SEH 예외)
	if (LoadLibraryW(L"dxcompiler.dll") == nullptr)
	{
		E_LOG(LogD3D12, Display, "DXC(dxcompiler.dll)가 없어 쿠킹된 셰이더만 사용합니다");
		return true;
	}
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
	if (!IsAvailable())
	{
		E_LOG(LogD3D12, Error, "DXC가 없어 셰이더를 컴파일할 수 없습니다 (쿠킹 파일 누락?): {}:{}", FStringConv::ToUtf8(Desc.FileName),
		      FStringConv::ToUtf8(Desc.EntryPoint));
		return nullptr;
	}

	const std::filesystem::path ShaderDir = GetEngineShaderDirectory();
	const std::filesystem::path FullPath  = ShaderDir / Desc.FileName;
	const std::string           DisplayName = FStringConv::ToUtf8(Desc.FileName) + ":" + FStringConv::ToUtf8(Desc.EntryPoint);

	// 진입 파일도 가상 파일일 수 있다 (생성 소스 — 포함 경로는 엔진 셰이더 폴더)
	const FShaderVirtualFile* VirtualSource = nullptr;
	for (const FShaderVirtualFile& File : Desc.VirtualFiles)
	{
		VirtualSource = _wcsicmp(File.Name.c_str(), Desc.FileName.c_str()) == 0 ? &File : VirtualSource;
	}
	ComPtr<IDxcBlobEncoding> Source;
	if (VirtualSource != nullptr)
	{
		if (FAILED(Utils->CreateBlob(VirtualSource->Content.data(), static_cast<UINT32>(VirtualSource->Content.size()), DXC_CP_UTF8, &Source)))
		{
			return nullptr;
		}
	}
	else if (FAILED(Utils->LoadFile(FullPath.c_str(), nullptr, &Source)))
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

	ComPtr<IDxcIncludeHandler> Includes = IncludeHandler;
	if (!Desc.VirtualFiles.empty())
	{
		Includes = Microsoft::WRL::Make<FVirtualIncludeHandler>(Utils.Get(), IncludeHandler.Get(), &Desc.VirtualFiles);
	}

	ComPtr<IDxcResult> Result;
	const HRESULT CompileHr = Compiler->Compile(&SourceBuffer, Args.data(), static_cast<UINT32>(Args.size()),
	                                            Includes.Get(), IID_PPV_ARGS(&Result));
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

ComPtr<IDxcBlob> FD3D12ShaderCompiler::CreateBlob(const void* Data, size_t SizeInBytes)
{
	return Microsoft::WRL::Make<FMemoryBlob>(Data, SizeInBytes);
}

std::filesystem::path FD3D12ShaderCompiler::GetEngineShaderDirectory()
{
	return FPaths::GetEngineShaderDirectory();
}

D3D12_SHADER_BYTECODE FD3D12ShaderCompiler::ToBytecode(IDxcBlob* Blob)
{
	E_CHECKF(Blob != nullptr, "셰이더 블롭이 비어 있습니다");
	return { Blob->GetBufferPointer(), Blob->GetBufferSize() };
}
