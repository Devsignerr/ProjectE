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

	// 기본 리소스: 1x1 흰색/평면 노멀 텍스처, 기본 머티리얼
	WhiteTexture      = CreateTexture(FImage::MakeSolidColor(1, 1, 255, 255, 255), true, L"DefaultWhite");
	FlatNormalTexture = CreateTexture(FImage::MakeSolidColor(1, 1, 128, 128, 255), false, L"DefaultFlatNormal");
	if (!WhiteTexture.IsValid() || !FlatNormalTexture.IsValid())
	{
		return false;
	}

	FMaterial Default;
	Default.Name    = "DefaultMaterial";
	DefaultMaterial = CreateMaterial(Default);

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

	Materials.ForEach([this](FMaterialHandle, FMaterial& Material) { Rhi->GetSrvAllocator().Free(Material.TextureTable); });
	Meshes.ForEach([](FMeshHandle, FStaticMesh& Mesh) { Mesh.Shutdown(); });
	Textures.ForEach([](FTextureHandle, FD3D12Texture& Texture) { Texture.Shutdown(); });
	Meshes.Clear();
	Textures.Clear();
	Materials.Clear();
	TextureCache.clear();
	MaterialCache.clear();
	PrimitiveMeshes.clear();

	WhiteTexture      = FTextureHandle{};
	FlatNormalTexture = FTextureHandle{};
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
	if (Handle == WhiteTexture || Handle == FlatNormalTexture)
	{
		E_LOG(LogRenderer, Warning, "기본 텍스처는 삭제할 수 없습니다");
		return;
	}
	if (std::unique_ptr<FD3D12Texture> Texture = Textures.Remove(Handle))
	{
		Texture->ShutdownDeferred(*Rhi);
		// 이 텍스처를 쓰던 머티리얼은 기본 텍스처로 테이블을 다시 만든다 (이전 테이블은 지연 해제)
		Materials.ForEach([&](FMaterialHandle, FMaterial& Material) {
			for (const FTextureHandle& Slot : Material.Textures)
			{
				if (Slot == Handle)
				{
					BuildMaterialTable(Material);
					break;
				}
			}
		});
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
		Handle = CreateMesh(FPrimitiveShapes::MakeCube(FUnits::MetersToUnits), L"Primitive_Cube") /* 1m 큐브 */;
	}
	else
	{
		E_LOG(LogRenderer, Warning, "알 수 없는 내장 도형: {}", Key);
		return FMeshHandle{};
	}
	PrimitiveMeshes[Key] = Handle;
	return Handle;
}

const FD3D12Texture& FResourceManager::ResolveSlotTexture(const FMaterial& Material, uint32 Slot) const
{
	if (const FD3D12Texture* Texture = Textures.Get(Material.Textures[Slot]))
	{
		return *Texture;
	}
	return *Textures.Get(Slot == MaterialSlot_Normal ? FlatNormalTexture : WhiteTexture);
}

void FResourceManager::BuildMaterialTable(FMaterial& Material)
{
	FD3D12DescriptorAllocator& Allocator = Rhi->GetSrvAllocator();
	const FD3D12DescriptorHandle NewTable = Allocator.AllocateRange(MaterialSlot_Count);

	// 디스크립터 복사 대신 SRV를 직접 기록 (셰이더 가시 힙은 읽기가 느려 복사 원본으로 부적합)
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
	{
		const FD3D12Texture& Texture = ResolveSlotTexture(Material, Slot);

		D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
		SrvDesc.Format                  = Texture.GetFormat();
		SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
		SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SrvDesc.Texture2D.MipLevels     = Texture.GetMipCount();
		Device->CreateShaderResourceView(Texture.GetResource(), &SrvDesc, Allocator.GetCpuHandle(NewTable.Index + Slot));
	}

	// 진행 중인 프레임이 이전 테이블을 읽을 수 있으므로 지연 해제
	Rhi->DeferFreeDescriptor(Material.TextureTable);
	Material.TextureTable = NewTable;
}

FMaterialHandle FResourceManager::CreateMaterial(const FMaterial& Material)
{
	auto NewMaterial          = std::make_unique<FMaterial>(Material);
	NewMaterial->TextureTable = FD3D12DescriptorHandle{};
	BuildMaterialTable(*NewMaterial);
	return Materials.Add(std::move(NewMaterial));
}

void FResourceManager::RefreshMaterialTextures(FMaterialHandle Handle)
{
	if (FMaterial* Material = Materials.Get(Handle))
	{
		BuildMaterialTable(*Material);
	}
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
	for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
	{
		if (!Asset.TexturePaths[Slot].empty())
		{
			Material.Textures[Slot] = LoadTexture(Canonical.parent_path() / FStringConv::ToWide(Asset.TexturePaths[Slot]), FMaterialAsset::IsSrgbSlot(Slot));
		}
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
	if (std::unique_ptr<FMaterial> Material = Materials.Remove(Handle))
	{
		Rhi->DeferFreeDescriptor(Material->TextureTable);
		for (auto Iterator = MaterialCache.begin(); Iterator != MaterialCache.end();)
		{
			Iterator = (Iterator->second == Handle) ? MaterialCache.erase(Iterator) : std::next(Iterator);
		}
	}
}

const FMaterial& FResourceManager::ResolveMaterial(FMaterialHandle Handle) const
{
	if (const FMaterial* Material = Materials.Get(Handle))
	{
		return *Material;
	}
	return *Materials.Get(DefaultMaterial);
}
