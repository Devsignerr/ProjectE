#include "ExeStamp.h"
#include "PackageManifest.h"
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
#include "Renderer/MaterialRender.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <optional>
#include <set>

E_DEFINE_LOG_CATEGORY(LogCook, Log)

// 사용법: ProjectECook [--project <.eproject 또는 폴더>]
//        ProjectECook --project <...> --stamp-exe <exe>   (패키징: exe에 프로젝트 아이콘/버전 리소스만 기록하고 끝)
//        ProjectECook --make-pak <out.epak> --pak-root <폴더> --pak-dirs "상대1;상대2"   (패키징: 폴더들을 pak 하나로 묶고 끝)
//   1) Engine/Shaders/Shaders.json의 모든 셰이더를 DXC로 컴파일해 Engine/Shaders/Cooked/에 DXIL 기록
//   2) 프로젝트 Content의 모델(glTF/GLB)과 이미지(PNG/JPG/TGA/BMP)를 <프로젝트>/Cooked/에 엔진 바이너리로 기록
//      이미지는 .emat가 참조하는 슬롯 용도(색상/선형/노멀/마스크)별로, 참조되지 않으면 색상으로 압축 쿠킹
//   3) 그래프 머티리얼(.emat Graph, 인스턴스는 부모 체인 해석 후)의 픽셀 셰이더 변형을 모두 Engine/Shaders/Cooked/에 기록
//      (패키지는 DXC 없이 쿠킹 DXIL만 읽는다 — 같은 생성 소스 해시는 한 번만)
//   패키징: --package-manifest <out.txt>  루트(시작 맵·플레이어 프리팹·설정)에서 참조를 따라간 매니페스트를 쓰고(PackageManifest.h),
//           2)·3)을 매니페스트의 모델/단독 이미지(실제 용도만)/머티리얼로만 한다. --manifest-only면 매니페스트만 쓰고 끝.
//           --assets-from <manifest>  이미 만든 매니페스트로 2)·3)을 제한한다. 둘 다 없으면 Content 전체 (에디터/개발 흐름).
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

	// ---- 패키지 매니페스트 (있으면 에셋/그래프 머티리얼 쿠킹을 도달 가능한 것으로 제한)
	std::optional<FPackageManifest> PackageAssets;
	if (const std::wstring ManifestOut = CommandLine.GetValue(L"--package-manifest"); !ManifestOut.empty() && FPaths::HasProject())
	{
		FPackageManifest Built = BuildProjectPackageManifest();
		if (!Built.WriteToFile(ManifestOut))
		{
			E_LOG(LogCook, Error, "매니페스트를 쓰지 못했습니다: {}", FStringConv::ToUtf8(ManifestOut));
			return 1;
		}
		E_LOG(LogCook, Display, "패키지 매니페스트: 파일 {}, 모델 {}, 단독 이미지 {}, 모델 내부 {}, 폴더 참조 {}, 경고 {} → {}", Built.Files.size(),
		      Built.Models.size(), Built.Images.size(), Built.Internal.size(), Built.Directories.size(), Built.Warnings.size(),
		      FStringConv::ToUtf8(ManifestOut));
		if (Built.bHasErrors)
		{
			E_LOG(LogCook, Error, "패키지 루트가 Content에 없습니다 (매니페스트 W 줄 참고)");
			return 1;
		}
		if (CommandLine.HasFlag(L"--manifest-only"))
		{
			FLog::Shutdown();
			return 0;
		}
		PackageAssets = std::move(Built);
	}
	else if (const std::wstring ManifestIn = CommandLine.GetValue(L"--assets-from"); !ManifestIn.empty() && FPaths::HasProject())
	{
		FPackageManifest Loaded;
		if (!FPackageManifest::ReadFromFile(ManifestIn, Loaded))
		{
			E_LOG(LogCook, Error, "매니페스트를 읽지 못했습니다: {}", FStringConv::ToUtf8(ManifestIn));
			return 1;
		}
		PackageAssets = std::move(Loaded);
	}
	const auto ContentPath = [](const std::string& Relative) { return FPaths::GetProjectContentDirectory() / FStringConv::ToWide(Relative); };

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

	// ---- 그래프 머티리얼 셰이더 변형 (프로젝트 Content의 .emat 전부, 인스턴스는 해석 결과 — 정적 스위치 덮어쓰기는 다른 변형)
	if (FPaths::HasProject())
	{
		std::set<uint64> CookedHashes;
		uint32           GraphMaterials = 0;
		std::error_code  ErrorCode;
		const auto       Loader = [](const std::filesystem::path& ParentPath, FMaterialAsset& OutAsset) { return OutAsset.LoadFromFile(ParentPath); };
		std::vector<std::filesystem::path> MaterialFiles; // 매니페스트가 있으면 패키지에 들어가는 .emat만
		if (PackageAssets)
		{
			for (const std::string& File : PackageAssets->Files)
			{
				if (ContentPath(File).extension() == FMaterialAsset::Extension)
				{
					MaterialFiles.push_back(ContentPath(File));
				}
			}
		}
		else
		{
			for (const auto& Entry : std::filesystem::recursive_directory_iterator(FPaths::GetProjectContentDirectory(), ErrorCode))
			{
				if (Entry.is_regular_file(ErrorCode) && Entry.path().extension() == FMaterialAsset::Extension)
				{
					MaterialFiles.push_back(Entry.path());
				}
			}
		}
		for (const std::filesystem::path& MaterialFile : MaterialFiles)
		{
			FMaterialAsset Asset;
			FMaterialAsset Resolved;
			if (!Asset.LoadFromFile(MaterialFile) || !FMaterialAsset::Resolve(Asset, MaterialFile, Loader, Resolved) || !Resolved.IsGraphMaterial())
			{
				continue;
			}
			++GraphMaterials;
			const FMaterialGraphCompileResult Compiled = FMaterialGraphCompiler::Compile(Resolved.Graph, Resolved.Parameters);
			if (!Compiled.bSuccess)
			{
				++Failed;
				E_LOG(LogCook, Error, "머티리얼 그래프 컴파일 실패: {}\n{}", FStringConv::ToUtf8(MaterialFile.filename().wstring()), Compiled.JoinErrors());
				continue;
			}
			if (!CookedHashes.insert(Compiled.Shader->Hash).second)
			{
				continue; // 같은 그래프 (인스턴스 등)
			}
			for (const FShaderCompileDesc& Desc : MaterialRender::GetGraphShaderDescs(*Compiled.Shader))
			{
				if (!Library.CookShader(Desc))
				{
					++Failed;
					E_LOG(LogCook, Error, "그래프 머티리얼 셰이더 쿠킹 실패: {} ({})", FStringConv::ToUtf8(MaterialFile.filename().wstring()),
					      FStringConv::ToUtf8(Desc.EntryPoint));
				}
			}
		}
		E_LOG(LogCook, Display, "그래프 머티리얼 셰이더 쿠킹: 머티리얼 {}개, 셰이더 {}종", GraphMaterials, CookedHashes.size());
	}

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
			if (!PackageAssets && Entry.is_regular_file(ErrorCode) && Entry.path().extension() == L".emat" && Material.LoadFromFile(Entry.path()))
			{
				for (const FMaterialParameter& Parameter : Material.Parameters) // 그래프 텍스처 파라미터 (적힌 Usage)
				{
					if (Parameter.Type == EMaterialParameterType::Texture && !Parameter.Texture.empty())
					{
						TextureUsages[std::filesystem::weakly_canonical(Entry.path().parent_path() / FStringConv::ToWide(Parameter.Texture), ErrorCode)]
							.insert(Parameter.Usage);
					}
				}
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

		// 쿠킹 입력: 매니페스트가 있으면 그 모델/단독 이미지(용도 포함)/환경맵만, 없으면 Content 전체
		std::vector<std::pair<std::filesystem::path, std::set<ETextureUsage>>> Inputs;
		if (PackageAssets)
		{
			for (const std::string& Model : PackageAssets->Models)
			{
				Inputs.emplace_back(ContentPath(Model), std::set<ETextureUsage>{});
			}
			for (const auto& [Image, Usages] : PackageAssets->Images)
			{
				Inputs.emplace_back(ContentPath(Image), Usages);
			}
			for (const std::string& File : PackageAssets->Files)
			{
				if (ContentPath(File).extension() == L".hdr")
				{
					Inputs.emplace_back(ContentPath(File), std::set<ETextureUsage>{});
				}
			}
		}
		else
		{
			for (const auto& Entry : std::filesystem::recursive_directory_iterator(FPaths::GetProjectContentDirectory(), ErrorCode))
			{
				if (Entry.is_regular_file(ErrorCode))
				{
					Inputs.emplace_back(Entry.path(), std::set<ETextureUsage>{});
				}
			}
		}

		for (const auto& [SourcePath, ManifestUsages] : Inputs)
		{
			std::wstring Extension = SourcePath.extension().wstring();
			std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t C) { return static_cast<wchar_t>(std::towlower(C)); });

			const bool bModel = Extension == L".glb" || Extension == L".gltf" || Extension == L".fbx";
			const bool bImage = Extension == L".png" || Extension == L".jpg" || Extension == L".jpeg" || Extension == L".tga" || Extension == L".bmp";
			const bool bEnvironment = Extension == L".hdr"; // 하늘/IBL 환경맵 (Phase 33-7)
			if (!bModel && !bImage && !bEnvironment)
			{
				continue;
			}

			const std::string DisplayName = FStringConv::ToUtf8(std::filesystem::relative(SourcePath, FPaths::GetProjectContentDirectory()).wstring());
			if (bEnvironment)
			{
				const std::filesystem::path CookedPath = FAssetCache::GetCookedPath(SourcePath, FAssetCache::EnvironmentExtension);
				if (!bForce && FAssetCache::IsCookedUpToDate(SourcePath, CookedPath))
				{
					++Skipped;
				}
				else if (FAssetCache::CookEnvironmentAsset(SourcePath))
				{
					++Cooked;
					E_LOG(LogCook, Display, "환경맵 쿠킹: {}", DisplayName);
				}
				else
				{
					++AssetFailed;
					E_LOG(LogCook, Error, "환경맵 쿠킹 실패: {}", DisplayName);
				}
				continue;
			}
			if (bModel)
			{
				const std::filesystem::path CookedPath = FAssetCache::GetCookedPath(SourcePath, FAssetCache::ModelExtension);
				if (!bForce && FAssetCache::IsCookedUpToDate(SourcePath, CookedPath))
				{
					++Skipped;
				}
				else if (FAssetCache::CookModelAsset(SourcePath))
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
			if (PackageAssets)
			{
				Usages = ManifestUsages; // 실제로 쓰는 용도만
			}
			else if (const auto Found = TextureUsages.find(std::filesystem::weakly_canonical(SourcePath, ErrorCode)); Found != TextureUsages.end())
			{
				Usages = Found->second;
			}
			for (const ETextureUsage Usage : Usages)
			{
				const std::filesystem::path CookedPath = FAssetCache::GetCookedPath(SourcePath, FAssetCache::GetTextureExtension(Usage));
				if (!bForce && FAssetCache::IsCookedUpToDate(SourcePath, CookedPath))
				{
					++Skipped;
				}
				else if (FAssetCache::CookTextureAsset(SourcePath, Usage))
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
