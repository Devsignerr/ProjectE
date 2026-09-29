#include "Core/CommandLine.h"
#include "Core/CoreMinimal.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "RHI/ShaderManifest.h"
#include "Renderer/AssetCache.h"

#include <algorithm>
#include <cwctype>

E_DEFINE_LOG_CATEGORY(LogCook, Log)

// 사용법: ProjectECook [--project <.eproject 또는 폴더>]
//   1) Engine/Shaders/Shaders.json의 모든 셰이더를 DXC로 컴파일해 Engine/Shaders/Cooked/에 DXIL 기록
//   2) 프로젝트 Content의 모델(glTF/GLB)과 이미지(PNG/JPG/TGA/BMP)를 <프로젝트>/Cooked/에 엔진 바이너리로 기록
int main()
{
	FLog::Init();

	const FCommandLine CommandLine = FCommandLine::FromProcess();
	FPaths::Initialize(CommandLine);

	if (FPaths::HasProject())
	{
		E_LOG(LogCook, Display, "프로젝트: {} ({})", FPaths::GetProjectName(), FStringConv::ToUtf8(FPaths::GetProjectDirectory().wstring()));
	}
	else
	{
		E_LOG(LogCook, Warning, "프로젝트가 지정되지 않았습니다. 엔진 셰이더만 쿠킹합니다 (--project <경로>)");
	}

	// ---- 셰이더
	const std::filesystem::path ManifestPath = FPaths::GetEngineShaderDirectory() / FShaderManifest::DefaultFileName;
	FShaderManifest             Manifest;
	if (!Manifest.LoadFromFile(ManifestPath))
	{
		return 1;
	}

	FD3D12ShaderCompiler Compiler;
	FShaderLibrary       Library;
	if (!Compiler.Init() || !Library.Init(Compiler, GetCookedShaderDirectory(), true))
	{
		E_LOG(LogCook, Error, "셰이더 컴파일러 초기화 실패");
		return 1;
	}

	uint32 Succeeded = 0;
	uint32 Failed    = 0;
	for (const FShaderManifestEntry& Entry : Manifest.Entries)
	{
		const FShaderCompileDesc Desc = Entry.ToCompileDesc();
		if (Library.CookShader(Desc))
		{
			++Succeeded;
		}
		else
		{
			++Failed;
			E_LOG(LogCook, Error, "쿠킹 실패: {}:{}", FStringConv::ToUtf8(Desc.FileName), FStringConv::ToUtf8(Desc.EntryPoint));
		}
	}

	E_LOG(LogCook, Display, "셰이더 쿠킹 완료: 성공 {}, 실패 {} → {}", Succeeded, Failed,
	      FStringConv::ToUtf8(Library.GetCookedDirectory().wstring()));

	Library.Shutdown();
	Compiler.Shutdown();

	// ---- 에셋
	if (FPaths::HasProject())
	{
		const bool bForce = CommandLine.HasFlag(L"--force");
		uint32     Cooked = 0;
		uint32     Skipped = 0;
		uint32     AssetFailed = 0;

		std::error_code ErrorCode;
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(FPaths::GetProjectContentDirectory(), ErrorCode))
		{
			if (!Entry.is_regular_file(ErrorCode))
			{
				continue;
			}
			std::wstring Extension = Entry.path().extension().wstring();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t C) { return static_cast<wchar_t>(std::towlower(C)); });

			const bool bModel = Extension == L".glb" || Extension == L".gltf";
			const bool bImage = Extension == L".png" || Extension == L".jpg" || Extension == L".jpeg" || Extension == L".tga" || Extension == L".bmp";
			if (!bModel && !bImage)
			{
				continue;
			}

			const std::filesystem::path CookedPath = FAssetCache::GetCookedPath(Entry.path(), bModel ? FAssetCache::ModelExtension : FAssetCache::ImageExtension);
			if (!bForce && FAssetCache::IsCookedUpToDate(Entry.path(), CookedPath))
			{
				++Skipped;
				continue;
			}

			const bool bOk = bModel ? FAssetCache::CookModelAsset(Entry.path()) : FAssetCache::CookImageAsset(Entry.path());
			if (bOk)
			{
				++Cooked;
				E_LOG(LogCook, Display, "에셋 쿠킹: {}", FStringConv::ToUtf8(std::filesystem::relative(Entry.path(), FPaths::GetProjectContentDirectory()).wstring()));
			}
			else
			{
				++AssetFailed;
				E_LOG(LogCook, Error, "에셋 쿠킹 실패: {}", FStringConv::ToUtf8(Entry.path().wstring()));
			}
		}
		E_LOG(LogCook, Display, "에셋 쿠킹 완료: 쿠킹 {}, 최신 유지 {}, 실패 {} → {}", Cooked, Skipped, AssetFailed,
		      FStringConv::ToUtf8(FAssetCache::GetCookedDirectory().wstring()));
		Failed += AssetFailed;
	}
	FLog::Shutdown();
	return Failed == 0 ? 0 : 1;
}
