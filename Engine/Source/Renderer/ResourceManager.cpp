#include "Renderer/ResourceManager.h"

#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/AssetCache.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/PrimitiveShapes.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr DXGI_FORMAT GetTextureFormat(bool bSRGB)
	{
		return bSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
	}
} // namespace

bool FResourceManager::Init(FD3D12RHI& InRhi)
{
	E_CHECKF(Rhi == nullptr, "리소스 관리자가 이미 초기화되어 있습니다");
	Rhi = &InRhi;

	// 기본 리소스: 1x1 흰색 텍스처, 흰색 머티리얼
	WhiteTexture = CreateTexture(FImage::MakeSolidColor(1, 1, 255, 255, 255), true, L"DefaultWhite");
	if (!WhiteTexture.IsValid())
	{
		return false;
	}

	FMaterial Default;
	Default.Name             = "DefaultMaterial";
	Default.BaseColorTexture = WhiteTexture;
	DefaultMaterial          = CreateMaterial(Default);

	E_LOG(LogRenderer, Display, "리소스 관리자 초기화 완료");
	return true;
}

void FResourceManager::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}

	// 즉시 해제 전에 GPU 작업 완료 보장
	Rhi->GetGraphicsQueue().Flush();

	Meshes.ForEach([](FMeshHandle, FStaticMesh& Mesh) { Mesh.Shutdown(); });
	Textures.ForEach([](FTextureHandle, FD3D12Texture& Texture) { Texture.Shutdown(); });
	Meshes.Clear();
	Textures.Clear();
	Materials.Clear();
	TextureCache.clear();
	MaterialCache.clear();
	PrimitiveMeshes.clear();

	WhiteTexture    = FTextureHandle{};
	DefaultMaterial = FMaterialHandle{};
	Rhi             = nullptr;
}

FTextureHandle FResourceManager::LoadTexture(const std::filesystem::path& Path, bool bSRGB)
{
	std::error_code ErrorCode;
	std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
	if (ErrorCode)
	{
		Canonical = Path;
	}
	const std::wstring CacheKey = Canonical.wstring() + (bSRGB ? L"|srgb" : L"|linear");

	if (const auto Found = TextureCache.find(CacheKey); Found != TextureCache.end() && Textures.IsValid(Found->second))
	{
		return Found->second;
	}

	FImage Image;
	if (FAssetCache::LoadImageAsset(Canonical, Image) == FAssetCache::ESource::Failed)
	{
		return FTextureHandle{};
	}

	const FTextureHandle Handle = CreateTexture(Image, bSRGB, Canonical.filename().wstring());
	if (Handle.IsValid())
	{
		TextureCache[CacheKey] = Handle;
	}
	return Handle;
}

FTextureHandle FResourceManager::CreateTexture(const FImage& Image, bool bSRGB, const std::wstring& DebugName)
{
	E_CHECKF(Rhi != nullptr, "리소스 관리자가 초기화되지 않았습니다");
	if (!Image.IsValid())
	{
		E_LOG(LogRenderer, Error, "유효하지 않은 이미지로 텍스처를 만들 수 없습니다: {}", FStringConv::ToUtf8(DebugName));
		return FTextureHandle{};
	}

	auto Texture = std::make_unique<FD3D12Texture>();
	if (!Texture->Init2D(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Rhi->GetSrvAllocator(), Image.Width, Image.Height,
	                     GetTextureFormat(bSRGB), Image.Pixels.data(), FImage::BytesPerPixel, DebugName.c_str(),
	                     /*bGenerateMips*/ true))
	{
		return FTextureHandle{};
	}
	return Textures.Add(std::move(Texture));
}

void FResourceManager::DestroyTexture(FTextureHandle Handle)
{
	if (Handle == WhiteTexture)
	{
		E_LOG(LogRenderer, Warning, "기본 흰색 텍스처는 삭제할 수 없습니다");
		return;
	}
	if (std::unique_ptr<FD3D12Texture> Texture = Textures.Remove(Handle))
	{
		Texture->ShutdownDeferred(*Rhi);
		for (auto Iterator = TextureCache.begin(); Iterator != TextureCache.end();)
		{
			Iterator = (Iterator->second == Handle) ? TextureCache.erase(Iterator) : std::next(Iterator);
		}
	}
}

const FD3D12Texture& FResourceManager::ResolveTexture(FTextureHandle Handle) const
{
	if (const FD3D12Texture* Texture = Textures.Get(Handle))
	{
		return *Texture;
	}
	return *Textures.Get(WhiteTexture);
}

FMeshHandle FResourceManager::CreateMesh(const FMeshData& MeshData, const std::wstring& DebugName)
{
	E_CHECKF(Rhi != nullptr, "리소스 관리자가 초기화되지 않았습니다");

	auto Mesh = std::make_unique<FStaticMesh>();
	if (!Mesh->Init(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), MeshData, DebugName.c_str()))
	{
		return FMeshHandle{};
	}
	return Meshes.Add(std::move(Mesh));
}

void FResourceManager::DestroyMesh(FMeshHandle Handle)
{
	if (std::unique_ptr<FStaticMesh> Mesh = Meshes.Remove(Handle))
	{
		Mesh->ShutdownDeferred(*Rhi);
	}
}

FMeshHandle FResourceManager::GetOrCreatePrimitiveMesh(std::string_view Name)
{
	const std::string Key(Name);
	if (const auto Found = PrimitiveMeshes.find(Key); Found != PrimitiveMeshes.end() && Meshes.IsValid(Found->second))
	{
		return Found->second;
	}

	FMeshHandle Handle;
	if (Key == "cube")
	{
		Handle = CreateMesh(FPrimitiveShapes::MakeCube(1.0f), L"Primitive_Cube");
	}
	else
	{
		E_LOG(LogRenderer, Warning, "알 수 없는 내장 도형: {}", Key);
		return FMeshHandle{};
	}
	PrimitiveMeshes[Key] = Handle;
	return Handle;
}

FMaterialHandle FResourceManager::CreateMaterial(const FMaterial& Material)
{
	return Materials.Add(std::make_unique<FMaterial>(Material));
}

FMaterialHandle FResourceManager::LoadMaterial(const std::filesystem::path& Path)
{
	std::error_code       ErrorCode;
	std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
	if (ErrorCode)
	{
		Canonical = Path;
	}
	const std::wstring CacheKey = Canonical.wstring();
	if (const auto Found = MaterialCache.find(CacheKey); Found != MaterialCache.end() && Materials.IsValid(Found->second))
	{
		return Found->second;
	}

	FMaterialAsset Asset;
	if (!Asset.LoadFromFile(Canonical))
	{
		return FMaterialHandle{};
	}

	FMaterial Material;
	Material.Name      = Asset.Name;
	Material.Constants = Asset.Constants;
	if (!Asset.BaseColorTexture.empty())
	{
		Material.BaseColorTexture = LoadTexture(Canonical.parent_path() / FStringConv::ToWide(Asset.BaseColorTexture), true);
	}

	const FMaterialHandle Handle = CreateMaterial(Material);
	MaterialCache[CacheKey]      = Handle;
	E_LOG(LogRenderer, Log, "머티리얼 로드: {}", Asset.Name);
	return Handle;
}

void FResourceManager::DestroyMaterial(FMaterialHandle Handle)
{
	if (Handle == DefaultMaterial)
	{
		E_LOG(LogRenderer, Warning, "기본 머티리얼은 삭제할 수 없습니다");
		return;
	}
	Materials.Remove(Handle);
}

const FMaterial& FResourceManager::ResolveMaterial(FMaterialHandle Handle) const
{
	if (const FMaterial* Material = Materials.Get(Handle))
	{
		return *Material;
	}
	return *Materials.Get(DefaultMaterial);
}
