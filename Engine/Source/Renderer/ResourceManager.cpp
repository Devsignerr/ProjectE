#include "Renderer/ResourceManager.h"

#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"

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
	if (!FImageLoader::LoadFromFile(Canonical, Image))
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
	                     GetTextureFormat(bSRGB), Image.Pixels.data(), FImage::BytesPerPixel, DebugName.c_str()))
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

FMaterialHandle FResourceManager::CreateMaterial(const FMaterial& Material)
{
	return Materials.Add(std::make_unique<FMaterial>(Material));
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
