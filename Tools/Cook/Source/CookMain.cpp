#include "ExeStamp.h"
#include "PakCommand.h"

#include "Core/CommandLine.h"
#include "Core/CoreMinimal.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "RHI/ShaderManifest.h"
#include "Renderer/AssetCache.h"
#include "Renderer/MaterialAsset.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>

E_DEFINE_LOG_CATEGORY(LogCook, Log)

// 사용법: ProjectECook [--project <.eproject 또는 폴더>]
//        ProjectECook --project <...> --stamp-exe <exe>   (패키징: exe에 프로젝트 아이콘/버전 리소스만 기록하고 끝)
//        ProjectECook --make-pak <out.epak> --pak-root <폴더> --pak-dirs "상대1;상대2"   (패키징: 폴더들을 pak 하나로 묶고 끝)
//   1) Engine/Shaders/Shaders.json의 모든 셰이더를 DXC로 컴파일해 Engine/Shaders/Cooked/에 DXIL 기록
//   2) 프로젝트 Content의 모델(glTF/GLB)과 이미지(PNG/JPG/TGA/BMP)를 <프로젝트>/Cooked/에 엔진 바이너리로 기록
//      이미지는 .emat가 참조하는 슬롯 용도(색상/선형/노멀/마스크)별로, 참조되지 않으면 색상으로 압축 쿠킹
int main()
{
	FLog::Init();

	const FCommandLine CommandLine = FCommandLine::FromProcess();
	if (const std::wstring PakFile = CommandLine.GetValue(L"--make-pak"); !PakFile.empty())
	{
		std::vector<std::filesystem::path> Directories;
		const std::wstring                 DirList = CommandLine.GetValue(L"--pak-dirs");
		for (size_t Begin = 0; Begin <= DirList.size();)
		{
			const size_t End = std::min(DirList.find(L';', Begin), DirList.size());
			if (End > Begin)
			{
				Directories.emplace_back(DirList.substr(Begin, End - Begin));
			}
			Begin = End + 1;
		}
		return MakePak(PakFile, CommandLine.GetValue(L"--pak-root"), Directories); // 엔진/프로젝트 경로가 필요 없다
	}
	FPaths::Initialize(CommandLine);

	if (const std::wstring StampTarget = CommandLine.GetValue(L"--stamp-exe"); !StampTarget.empty())
	{
		return StampExecutable(StampTarget);
	}

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

		// .emat 참조로 이미지별 텍스처 용도 수집
		std::map<std::filesystem::path, std::set<ETextureUsage>> TextureUsages;
		std::error_code                                          ErrorCode;
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(FPaths::GetProjectContentDirectory(), ErrorCode))
		{
			FMaterialAsset Material;
			if (Entry.is_regular_file(ErrorCode) && Entry.path().extension() == L".emat" && Material.LoadFromFile(Entry.path()))
			{
				for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
				{
					if (!Material.TexturePaths[Slot].empty())
					{
						const std::filesystem::path TexturePath = std::filesystem::weakly_canonical(Entry.path().parent_path() / FStringConv::ToWide(Material.TexturePaths[Slot]), ErrorCode);
						TextureUsages[TexturePath].insert(FMaterialAsset::GetSlotUsage(Slot));
					}
				}
			}
		}

		for (const auto& Entry : std::filesystem::recursive_directory_iterator(FPaths::GetProjectContentDirectory(), ErrorCode))
		{
			if (!Entry.is_regular_file(ErrorCode))
			{
				continue;
			}
			std::wstring Extension = Entry.path().extension().wstring();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t C) { return static_cast<wchar_t>(std::towlower(C)); });

			const bool bModel = Extension == L".glb" || Extension == L".gltf" || Extension == L".fbx";
			const bool bImage = Extension == L".png" || Extension == L".jpg" || Extension == L".jpeg" || Extension == L".tga" || Extension == L".bmp";
			if (!bModel && !bImage)
			{
				continue;
			}

			const std::string DisplayName = FStringConv::ToUtf8(std::filesystem::relative(Entry.path(), FPaths::GetProjectContentDirectory()).wstring());
			if (bModel)
			{
				const std::filesystem::path CookedPath = FAssetCache::GetCookedPath(Entry.path(), FAssetCache::ModelExtension);
				if (!bForce && FAssetCache::IsCookedUpToDate(Entry.path(), CookedPath))
				{
					++Skipped;
				}
				else if (FAssetCache::CookModelAsset(Entry.path()))
				{
					++Cooked;
					E_LOG(LogCook, Display, "에셋 쿠킹: {}", DisplayName);
				}
				else
				{
					++AssetFailed;
					E_LOG(LogCook, Error, "에셋 쿠킹 실패: {}", DisplayName);
				}
				continue;
			}

			std::set<ETextureUsage> Usages = { ETextureUsage::Color };
			if (const auto Found = TextureUsages.find(std::filesystem::weakly_canonical(Entry.path(), ErrorCode)); Found != TextureUsages.end())
			{
				Usages = Found->second;
			}
			for (const ETextureUsage Usage : Usages)
			{
				const std::filesystem::path CookedPath = FAssetCache::GetCookedPath(Entry.path(), FAssetCache::GetTextureExtension(Usage));
				if (!bForce && FAssetCache::IsCookedUpToDate(Entry.path(), CookedPath))
				{
					++Skipped;
				}
				else if (FAssetCache::CookTextureAsset(Entry.path(), Usage))
				{
					++Cooked;
					E_LOG(LogCook, Display, "텍스처 쿠킹: {} ({})", DisplayName, FStringConv::ToUtf8(FAssetCache::GetTextureExtension(Usage)));
				}
				else
				{
					++AssetFailed;
					E_LOG(LogCook, Error, "텍스처 쿠킹 실패: {}", DisplayName);
				}
			}
		}
		E_LOG(LogCook, Display, "에셋 쿠킹 완료: 쿠킹 {}, 최신 유지 {}, 실패 {} → {}", Cooked, Skipped, AssetFailed,
		      FStringConv::ToUtf8(FAssetCache::GetCookedDirectory().wstring()));
		Failed += AssetFailed;
	}
	FLog::Shutdown();
	return Failed == 0 ? 0 : 1;
}
