#include "Renderer/ResourceManager.h"

#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/AssetCache.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/PrimitiveShapes.h"
#include "Scene/Particles.h"

#include <algorithm>
#include <cwctype>
#include <optional>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr DXGI_FORMAT GetTextureFormat(bool bSRGB)
	{
		return bSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
	}

	constexpr DXGI_FORMAT GetTextureFormat(ETextureFormat Format, bool bSRGB)
	{
		switch (Format)
		{
		case ETextureFormat::BC7: return bSRGB ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
		case ETextureFormat::BC5: return DXGI_FORMAT_BC5_UNORM;
		case ETextureFormat::BC4: return DXGI_FORMAT_BC4_UNORM;
		default:                  return GetTextureFormat(bSRGB);
		}
	}

	const wchar_t* GetUsageKey(ETextureUsage Usage)
	{
		switch (Usage)
		{
		case ETextureUsage::Color:  return L"|color";
		case ETextureUsage::Linear: return L"|linear";
		case ETextureUsage::Normal: return L"|normal";
		case ETextureUsage::Mask:   return L"|mask";
		}
		return L"|?";
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
	ModelCache.clear();
	ParticleCache.clear();

	WhiteTexture      = FTextureHandle{};
	FlatNormalTexture = FTextureHandle{};
	DefaultMaterial = FMaterialHandle{};
	Rhi             = nullptr;
}

FTextureHandle FResourceManager::LoadTexture(const std::filesystem::path& Path, ETextureUsage Usage)
{
	std::error_code ErrorCode;
	std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
	if (ErrorCode)
	{
		Canonical = Path;
	}
	const std::wstring CacheKey = Canonical.wstring() + GetUsageKey(Usage);

	if (const auto Found = TextureCache.find(CacheKey); Found != TextureCache.end() && Textures.IsValid(Found->second))
	{
		return Found->second;
	}

	FCompressedTexture Texture;
	if (FAssetCache::LoadTextureAsset(Canonical, Usage, Texture) == FAssetCache::ESource::Failed)
	{
		return FTextureHandle{};
	}

	const FTextureHandle Handle = CreateTexture(Texture, Canonical.filename().wstring());
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

FTextureHandle FResourceManager::CreateTexture(const FCompressedTexture& Texture, const std::wstring& DebugName)
{
	E_CHECKF(Rhi != nullptr, "리소스 관리자가 초기화되지 않았습니다");
	if (!Texture.IsValid())
	{
		E_LOG(LogRenderer, Error, "유효하지 않은 텍스처 데이터: {}", FStringConv::ToUtf8(DebugName));
		return FTextureHandle{};
	}

	std::vector<FD3D12Texture::FMipData> Mips;
	Mips.reserve(Texture.Mips.size());
	for (const FTextureMip& Mip : Texture.Mips)
	{
		Mips.push_back({ Mip.Data.data(), Mip.Data.size() });
	}

	auto GpuTexture = std::make_unique<FD3D12Texture>();
	if (!GpuTexture->Init2DFromMips(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Rhi->GetSrvAllocator(), Texture.GetWidth(),
	                                Texture.GetHeight(), GetTextureFormat(Texture.Format, Texture.bSRGB), Mips.data(),
	                                static_cast<uint32>(Mips.size()), DebugName.c_str()))
	{
		return FTextureHandle{};
	}
	return Textures.Add(std::move(GpuTexture));
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

FMeshHandle FResourceManager::CreateSkinnedMesh(const FMeshData& MeshData, const std::vector<FSkinVertex>& SkinVertices, const std::wstring& DebugName)
{
	E_CHECKF(Rhi != nullptr, "리소스 관리자가 초기화되지 않았습니다");

	auto Mesh = std::make_unique<FStaticMesh>();
	if (!Mesh->Init(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), MeshData, DebugName.c_str()) ||
	    !Mesh->InitSkin(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), SkinVertices, DebugName.c_str()))
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
	else if (Key == "sphere")
	{
		Handle = CreateMesh(FPrimitiveShapes::MakeSphere(0.5f * FUnits::MetersToUnits), L"Primitive_Sphere"); // 지름 1m 구
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
	FillMaterialFromAsset(Material, Asset, Canonical.parent_path());
	const FMaterialHandle Handle = CreateMaterial(Material);
	MaterialCache[CacheKey]      = Handle;
	E_LOG(LogRenderer, Log, "머티리얼 로드: {}", Asset.Name);
	return Handle;
}

void FResourceManager::ApplyMaterialAsset(FMaterialHandle Handle, const FMaterialAsset& Asset, const std::filesystem::path& BaseDirectory)
{
	if (FMaterial* Material = Materials.Get(Handle))
	{
		FillMaterialFromAsset(*Material, Asset, BaseDirectory);
		BuildMaterialTable(*Material);
	}
}

void FResourceManager::FillMaterialFromAsset(FMaterial& Material, const FMaterialAsset& Asset, const std::filesystem::path& BaseDirectory)
{
	Material.Name      = Asset.Name;
	Material.Constants = Asset.Constants;
	for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
	{
		Material.Textures[Slot] = Asset.TexturePaths[Slot].empty()
		                              ? FTextureHandle{}
		                              : LoadTexture(BaseDirectory / FStringConv::ToWide(Asset.TexturePaths[Slot]), FMaterialAsset::GetSlotUsage(Slot));
	}
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

const FModelResources* FResourceManager::FindModelResources(const std::wstring& Key)
{
	const auto Found = ModelCache.find(Key);
	if (Found == ModelCache.end())
	{
		return nullptr;
	}
	const FModelResources& Cached = *Found->second;
	const bool bMeshesAlive = std::all_of(Cached.Meshes.begin(), Cached.Meshes.end(),
	                                      [this](FMeshHandle Handle) { return !Handle.IsValid() || Meshes.IsValid(Handle); });
	const bool bMaterialsAlive = std::all_of(Cached.Materials.begin(), Cached.Materials.end(),
	                                         [this](FMaterialHandle Handle) { return !Handle.IsValid() || Materials.IsValid(Handle); });
	if (!bMeshesAlive || !bMaterialsAlive)
	{
		ModelCache.erase(Found);
		return nullptr;
	}
	return &Cached;
}

const FModelResources& FResourceManager::AddModelResources(const std::wstring& Key, FModelResources Resources)
{
	std::unique_ptr<FModelResources>& Slot = ModelCache[Key];
	Slot                                   = std::make_unique<FModelResources>(std::move(Resources));
	return *Slot;
}

std::shared_ptr<FParticleSystemAsset> FResourceManager::LoadParticleSystem(const std::filesystem::path& Path)
{
	std::error_code       ErrorCode;
	std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
	if (ErrorCode)
	{
		Canonical = Path;
	}
	const std::wstring CacheKey = Canonical.wstring();
	if (const auto Found = ParticleCache.find(CacheKey); Found != ParticleCache.end())
	{
		return Found->second;
	}

	auto System = std::make_shared<FParticleSystemAsset>();
	if (!System->LoadFromFile(Canonical))
	{
		return nullptr;
	}
	ResolveParticleResources(*System, Canonical.parent_path());
	ParticleCache[CacheKey] = System;
	E_LOG(LogRenderer, Log, "파티클 로드: {} (이미터 {}개)", System->Name, System->Emitters.size());
	return System;
}

void FResourceManager::ResolveParticleResources(FParticleSystemAsset& System, const std::filesystem::path& BaseDirectory)
{
	constexpr std::string_view PrimitivePrefix = "primitive:";
	for (FParticleEmitter& Emitter : System.Emitters)
	{
		for (FParticleRendererSettings& Renderer : Emitter.Renderers)
		{
			Renderer.Texture = Renderer.TexturePath.empty() ? FTextureHandle{}
			                                                : LoadTexture(BaseDirectory / FStringConv::ToWide(Renderer.TexturePath), ETextureUsage::Color);
			Renderer.Mesh = {};
			if (Renderer.Type == EParticleRendererType::Mesh)
			{
				// 메시 렌더러는 내장 도형만 (파일 메시는 추후)
				const bool             bPrimitive = Renderer.MeshAsset.rfind(PrimitivePrefix, 0) == 0;
				const std::string_view Name = bPrimitive ? std::string_view(Renderer.MeshAsset).substr(PrimitivePrefix.size()) : std::string_view("sphere");
				Renderer.Mesh               = GetOrCreatePrimitiveMesh(Name);
			}
		}
	}
}

void FResourceManager::OnAssetMoved(const std::filesystem::path& From, const std::filesystem::path& To)
{
	// 캐시 키 = weakly_canonical 경로 (+ 텍스처는 "|용도"). From은 이미 없어도 있는 부분까지 정규화된다
	std::error_code             ErrorCode;
	const std::filesystem::path CanonicalTo   = std::filesystem::weakly_canonical(To, ErrorCode);
	const std::filesystem::path CanonicalFrom = std::filesystem::weakly_canonical(From, ErrorCode);
	const std::wstring          OldPrefix     = CanonicalFrom.wstring();
	const std::wstring          NewPrefix     = CanonicalTo.wstring();

	const auto Lower = [](std::wstring Text) {
		std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Text;
	};
	const std::wstring OldLower = Lower(OldPrefix);
	// 키가 옛 경로 자체(뒤에 "|용도" 가능)이거나 옛 폴더 아래이면 새 키
	const auto Remap = [&](const std::wstring& Key) -> std::optional<std::wstring> {
		const std::wstring KeyLower = Lower(Key);
		if (KeyLower.rfind(OldLower, 0) != 0)
		{
			return std::nullopt;
		}
		const wchar_t Next = Key.size() > OldPrefix.size() ? Key[OldPrefix.size()] : L'\0';
		if (Next != L'\0' && Next != L'\\' && Next != L'/' && Next != L'|')
		{
			return std::nullopt;
		}
		return NewPrefix + Key.substr(OldPrefix.size());
	};
	const auto Rekey = [&](auto& Map) {
		std::vector<std::pair<std::wstring, std::wstring>> Changes;
		for (const auto& Entry : Map)
		{
			if (const std::optional<std::wstring> NewKey = Remap(Entry.first))
			{
				Changes.emplace_back(Entry.first, *NewKey);
			}
		}
		for (const auto& [OldKey, NewKey] : Changes)
		{
			auto Node  = Map.extract(OldKey);
			Node.key() = NewKey;
			Map.insert(std::move(Node));
		}
	};
	Rekey(TextureCache);
	Rekey(MaterialCache);
	Rekey(ModelCache);
	Rekey(ParticleCache);
}

std::unique_ptr<FModelResources> FResourceManager::TakeModelResources(const std::filesystem::path& Path)
{
	std::error_code             ErrorCode;
	const std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
	const auto                  Found     = ModelCache.find((ErrorCode ? Path : Canonical).wstring());
	if (Found == ModelCache.end())
	{
		return nullptr;
	}
	std::unique_ptr<FModelResources> Taken = std::move(Found->second);
	ModelCache.erase(Found);
	return Taken;
}

void FResourceManager::DestroyModelResources(const FModelResources& Model)
{
	// 모델 머티리얼의 텍스처는 모델 로드 때 만든 것(경로 캐시에 없음)이라 함께 해제한다
	std::vector<FTextureHandle> TexturesToDestroy;
	for (const FMaterialHandle Handle : Model.Materials)
	{
		if (const FMaterial* Material = Materials.Get(Handle); Material != nullptr && Handle != DefaultMaterial)
		{
			for (const FTextureHandle Texture : Material->Textures)
			{
				if (Texture.IsValid() && Texture != WhiteTexture && Texture != FlatNormalTexture &&
				    std::find(TexturesToDestroy.begin(), TexturesToDestroy.end(), Texture) == TexturesToDestroy.end())
				{
					TexturesToDestroy.push_back(Texture);
				}
			}
			DestroyMaterial(Handle);
		}
	}
	for (const FTextureHandle Texture : TexturesToDestroy)
	{
		DestroyTexture(Texture);
	}
	for (const FMeshHandle Mesh : Model.Meshes)
	{
		if (Mesh.IsValid())
		{
			DestroyMesh(Mesh);
		}
	}
}
