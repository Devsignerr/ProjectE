#include "Renderer/ResourceManager.h"

#include "Core/Profiling.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/AssetCache.h"
#include "Renderer/LodMath.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/MeshSimplifier.h"
#include "Renderer/PrimitiveShapes.h"
#include "Renderer/RendererConsoleVariables.h"
#include "Scene/Particles.h"

#include <algorithm>
#include <cwctype>
#include <optional>
#include <unordered_set>

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
	InitCollector();

	E_LOG(LogRenderer, Display, "리소스 관리자 초기화 완료");
	return true;
}

void FResourceManager::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}

	ShutdownCollector();
	// 작업 스레드를 먼저 멈춘다 (아직 시작하지 않은 로드와 완료 콜백은 버림)
	LoadJobs.Shutdown();
	if (BeginFrameCallbackId != 0)
	{
		Rhi->RemoveBeginFrameCallback(BeginFrameCallbackId);
		BeginFrameCallbackId = 0;
	}
	bAsyncLoadingEnabled = false;
	PendingUploads.clear();

	// 즉시 해제 전에 GPU 작업 완료 보장 (복사 큐가 아직 쓰는 대상도)
	Rhi->GetUploadQueue().WaitIdle();
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
	PrefetchedModels.clear();
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

	if (IsAsyncUpload())
	{
		// 자리표시 핸들을 바로 캐시 (같은 파일 중복 로드 방지). 읽기/디코드/압축은 작업 스레드, GPU 생성은 메인 스레드 완료 콜백
		const FTextureHandle Handle = Textures.Add(std::make_unique<FD3D12Texture>());
		TextureCache[CacheKey]      = Handle;
		struct FJobResult
		{
			FCompressedTexture Texture;
			bool               bLoaded = false;
		};
		auto Result = std::make_shared<FJobResult>();
		NoteAsyncRequest();
		LoadJobs.Submit(
			[Result, Canonical, Usage] {
				E_PROFILE_SCOPE("텍스처 로드 작업");
				Result->bLoaded = FAssetCache::LoadTextureAsset(Canonical, Usage, Result->Texture) != FAssetCache::ESource::Failed;
			},
			[this, Result, Handle, Name = Canonical.filename().wstring()] { FinishTextureLoad(Handle, Result->Texture, Result->bLoaded, Name); });
		return Handle;
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

FTextureHandle FResourceManager::CreateTexture(uint32 Width, uint32 Height, DXGI_FORMAT Format, const void* Pixels, uint32 BytesPerPixel,
                                               const std::wstring& DebugName)
{
	E_CHECKF(Rhi != nullptr, "리소스 관리자가 초기화되지 않았습니다");
	auto Texture = std::make_unique<FD3D12Texture>();
	if (!Texture->Init2D(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Rhi->GetSrvAllocator(), Width, Height, Format, Pixels, BytesPerPixel,
	                     DebugName.c_str(), /*bGenerateMips*/ false))
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

	const FTextureHandle Handle = Textures.Add(std::make_unique<FD3D12Texture>());
	if (!UploadCompressedTexture(*Textures.Get(Handle), Handle, Texture, DebugName))
	{
		Textures.Remove(Handle);
		return FTextureHandle{};
	}
	return Handle;
}

bool FResourceManager::UploadCompressedTexture(FD3D12Texture& Target, FTextureHandle Handle, const FCompressedTexture& Texture,
                                               const std::wstring& DebugName)
{
	std::vector<FD3D12Texture::FMipData> Mips;
	Mips.reserve(Texture.Mips.size());
	for (const FTextureMip& Mip : Texture.Mips)
	{
		Mips.push_back({ Mip.Data.data(), Mip.Data.size() });
	}
	const DXGI_FORMAT Format = GetTextureFormat(Texture.Format, Texture.bSRGB);
	if (!IsAsyncUpload())
	{
		return Target.Init2DFromMips(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Rhi->GetSrvAllocator(), Texture.GetWidth(), Texture.GetHeight(),
		                             Format, Mips.data(), static_cast<uint32>(Mips.size()), DebugName.c_str());
	}
	if (!Target.Init2DFromMipsAsync(Rhi->GetDevice(), Rhi->GetUploadQueue(), Rhi->GetSrvAllocator(), Texture.GetWidth(), Texture.GetHeight(),
	                                Format, Mips.data(), static_cast<uint32>(Mips.size()), DebugName.c_str()))
	{
		return false;
	}
	NoteAsyncRequest();
	PendingUploads.push_back({ Handle, FMeshHandle{}, Target.GetUploadFence() });
	return true;
}

void FResourceManager::FinishTextureLoad(FTextureHandle Handle, const FCompressedTexture& Texture, bool bLoaded, const std::wstring& DebugName)
{
	FD3D12Texture* Target = Textures.Get(Handle);
	if (Target == nullptr)
	{
		return; // 로드 중에 삭제됨
	}
	if (!bLoaded || !Texture.IsValid() || !UploadCompressedTexture(*Target, Handle, Texture, DebugName))
	{
		// 실패: 동기 경로와 같이 무효 핸들로 (쓰던 곳은 계속 기본 텍스처) — 캐시에서 빼 다음 요청이 다시 시도하게
		if (std::unique_ptr<FD3D12Texture> Removed = Textures.Remove(Handle))
		{
			Removed->ShutdownDeferred(*Rhi);
		}
		std::erase_if(TextureCache, [Handle](const auto& Entry) { return Entry.second == Handle; });
	}
}

void FResourceManager::EnableAsyncLoading(bool bInAutomationRun)
{
	E_CHECKF(Rhi != nullptr, "리소스 관리자가 초기화되지 않았습니다");
	if (bAsyncLoadingEnabled)
	{
		return;
	}
	bAsyncLoadingEnabled = true;
	bAutomationRun       = bInAutomationRun;
	LoadJobs.Init(FJobQueue::GetDefaultWorkerCount(), "리소스 로딩");
	BeginFrameCallbackId = Rhi->AddBeginFrameCallback([this] { ProcessAsyncLoads(); });
	static constexpr const char* ModeNames[] = { "동기", "비동기", "비동기 + 프레임마다 비우기" };
	E_LOG(LogRenderer, Display, "비동기 리소스 로딩 사용 (작업 스레드 {}개, 현재 방식: {})", LoadJobs.GetWorkerCount(),
	      ModeNames[static_cast<int32>(GetLoadMode())]);
}

EResourceLoadMode FResourceManager::GetLoadMode() const
{
	if (!bAsyncLoadingEnabled)
	{
		return EResourceLoadMode::Sync;
	}
	switch (RendererCVars::AsyncLoading.Get())
	{
	case 0:  return EResourceLoadMode::Sync;
	case 1:  return EResourceLoadMode::Async;
	case 2:  return EResourceLoadMode::AsyncDrain;
	default: return bAutomationRun ? EResourceLoadMode::AsyncDrain : EResourceLoadMode::Async;
	}
}

bool FResourceManager::IsReady(FTextureHandle Handle) const
{
	const FD3D12Texture* Texture = Textures.Get(Handle);
	return Texture != nullptr && Texture->IsReady();
}

bool FResourceManager::IsReady(FMeshHandle Handle) const
{
	const FStaticMesh* Mesh = Meshes.Get(Handle);
	return Mesh != nullptr && Mesh->IsReady();
}

uint32 FResourceManager::GetPendingLoadCount() const
{
	return LoadJobs.GetOutstandingCount() + static_cast<uint32>(PendingUploads.size());
}

void FResourceManager::WaitForPendingLoads()
{
	E_PROFILE_SCOPE("리소스 로딩 비우기");
	while (GetPendingLoadCount() > 0)
	{
		LoadJobs.WaitIdle();
		LoadJobs.PumpCompletions(); // 끝난 작업 → GPU 업로드 기록
		if (!PendingUploads.empty())
		{
			Rhi->FlushUploads();
			CompletePendingUploads(Rhi->GetUploadQueue().GetFinalizedFence());
		}
	}
}

void FResourceManager::ProcessAsyncLoads()
{
	E_PROFILE_SCOPE("비동기 로딩 처리");
	if (GetLoadMode() == EResourceLoadMode::AsyncDrain)
	{
		WaitForPendingLoads();
		bLoadBurstActive = false;
		return;
	}
	LoadJobs.PumpCompletions();
	CompletePendingUploads(Rhi->GetUploadQueue().GetFinalizedFence());
	if (bLoadBurstActive && GetPendingLoadCount() == 0)
	{
		// 한 번에 몰린 로드(씬 로드/맵 전환 등)가 모두 준비될 때까지 걸린 시간 (측정용)
		bLoadBurstActive = false;
		E_LOG(LogRenderer, Display, "비동기 로딩 완료: 요청 {}개, 첫 요청부터 {:.1f}ms", LoadBurstRequests,
		      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - LoadBurstStart).count());
	}
}

void FResourceManager::PrefetchModels(const std::vector<std::filesystem::path>& Paths)
{
	if (!bAsyncLoadingEnabled || GetLoadMode() == EResourceLoadMode::Sync)
	{
		return;
	}
	struct FPrefetch
	{
		std::filesystem::path Path;
		std::wstring          Key;
		FModelData            Model;
		bool                  bLoaded = false;
	};
	std::vector<std::unique_ptr<FPrefetch>> Work;
	for (const std::filesystem::path& Path : Paths)
	{
		std::error_code             ErrorCode;
		const std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
		std::wstring                Key       = (ErrorCode ? Path : Canonical).wstring();
		const bool bQueued = std::any_of(Work.begin(), Work.end(), [&](const auto& Item) { return Item->Key == Key; });
		if (!bQueued && !ModelCache.contains(Key) && !PrefetchedModels.contains(Key))
		{
			Work.push_back(std::make_unique<FPrefetch>(FPrefetch{ Path, std::move(Key), {}, false }));
		}
	}
	if (Work.size() < 2)
	{
		return; // 하나는 그냥 메인 스레드에서 읽는 것과 같다
	}
	E_PROFILE_SCOPE("모델 미리 읽기");
	const auto StartTime = std::chrono::steady_clock::now();
	// 텍스처 작업 대기열과 섞이지 않게 이번 호출 전용 작업자 (끝까지 기다리므로 짧게 산다)
	FJobQueue Prefetch;
	Prefetch.Init(std::min<uint32>(FJobQueue::GetDefaultWorkerCount(), static_cast<uint32>(Work.size())), "모델 미리 읽기");
	for (const std::unique_ptr<FPrefetch>& Item : Work)
	{
		FPrefetch* Target = Item.get();
		Prefetch.Submit([Target] { Target->bLoaded = FAssetCache::LoadModelAsset(Target->Path, Target->Model) != FAssetCache::ESource::Failed; });
	}
	Prefetch.WaitIdle();
	Prefetch.Shutdown();
	uint32 Loaded = 0;
	for (std::unique_ptr<FPrefetch>& Item : Work)
	{
		if (Item->bLoaded)
		{
			PrefetchedModels[Item->Key] = std::move(Item->Model);
			++Loaded;
		}
	}
	E_LOG(LogRenderer, Display, "모델 미리 읽기: {}개 병렬 ({:.1f}ms)", Loaded,
	      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - StartTime).count());
}

bool FResourceManager::TakePrefetchedModel(const std::wstring& Key, FModelData& OutModel)
{
	const auto Found = PrefetchedModels.find(Key);
	if (Found == PrefetchedModels.end())
	{
		return false;
	}
	OutModel = std::move(Found->second);
	PrefetchedModels.erase(Found);
	return true;
}

void FResourceManager::NoteAsyncRequest()
{
	if (!bLoadBurstActive)
	{
		bLoadBurstActive  = true;
		LoadBurstStart    = std::chrono::steady_clock::now();
		LoadBurstRequests = 0;
	}
	++LoadBurstRequests;
}

void FResourceManager::CompletePendingUploads(uint64 FinalizedFence)
{
	std::unordered_set<uint64> ReadyTextures; // FTextureHandle::ToId
	std::erase_if(PendingUploads, [&](const FPendingUpload& Pending) {
		if (Pending.Fence > FinalizedFence)
		{
			return false;
		}
		if (FD3D12Texture* Texture = Textures.Get(Pending.Texture))
		{
			Texture->MarkUploadComplete();
			ReadyTextures.insert(Pending.Texture.ToId());
		}
		if (FStaticMesh* Mesh = Meshes.Get(Pending.Mesh))
		{
			Mesh->MarkUploadComplete();
		}
		return true;
	});
	if (ReadyTextures.empty())
	{
		return;
	}
	// 기본 텍스처로 만들어 둔 테이블을 실제 텍스처로
	Materials.ForEach([&](FMaterialHandle, FMaterial& Material) {
		for (const FTextureHandle& Slot : Material.Textures)
		{
			if (Slot.IsValid() && ReadyTextures.contains(Slot.ToId()))
			{
				BuildMaterialTable(Material);
				break;
			}
		}
	});
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
	if (const FD3D12Texture* Texture = Textures.Get(Handle); Texture != nullptr && Texture->IsReady())
	{
		return *Texture;
	}
	return *Textures.Get(WhiteTexture);
}

FMeshHandle FResourceManager::CreateMesh(const FMeshData& MeshData, const std::wstring& DebugName)
{
	E_CHECKF(Rhi != nullptr, "리소스 관리자가 초기화되지 않았습니다");

	auto Mesh = std::make_unique<FStaticMesh>();
	if (IsAsyncUpload())
	{
		if (!Mesh->InitAsync(Rhi->GetDevice(), Rhi->GetUploadQueue(), MeshData, DebugName.c_str()))
		{
			return FMeshHandle{};
		}
		const uint64      Fence  = Mesh->GetUploadFence();
		const FMeshHandle Handle = Meshes.Add(std::move(Mesh));
		NoteAsyncRequest();
		PendingUploads.push_back({ FTextureHandle{}, Handle, Fence });
		return Handle;
	}
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
	if (IsAsyncUpload())
	{
		if (!Mesh->InitAsync(Rhi->GetDevice(), Rhi->GetUploadQueue(), MeshData, DebugName.c_str()) ||
		    !Mesh->InitSkinAsync(Rhi->GetDevice(), Rhi->GetUploadQueue(), SkinVertices, DebugName.c_str()))
		{
			return FMeshHandle{};
		}
		const uint64      Fence  = Mesh->GetUploadFence();
		const FMeshHandle Handle = Meshes.Add(std::move(Mesh));
		NoteAsyncRequest();
		PendingUploads.push_back({ FTextureHandle{}, Handle, Fence });
		return Handle;
	}
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
		FMeshData Sphere = FPrimitiveShapes::MakeSphere(0.5f * FUnits::MetersToUnits); // 지름 1m 구
		MeshSimplifier::GenerateLods(Sphere, LodMath::MaxLods);              // 곡면 도형은 LOD (정육면체는 12삼각형이라 불필요)
		Handle = CreateMesh(Sphere, L"Primitive_Sphere");
	}
	else if (Key == "plane")
	{
		Handle = CreateMesh(FPrimitiveShapes::MakePlane(FUnits::MetersToUnits), L"Primitive_Plane"); // 1m 사각형 한 장 (법선 +Z)
	}
	else if (Key == "capsule")
	{
		// 반지름 50cm, 원기둥 절반 50cm → 높이 2m (캡슐 콜라이더 Radius 50 / HalfHeight 50과 같은 모양)
		FMeshData Capsule = FPrimitiveShapes::MakeCapsule(0.5f * FUnits::MetersToUnits, 0.5f * FUnits::MetersToUnits);
		MeshSimplifier::GenerateLods(Capsule, LodMath::MaxLods);
		Handle = CreateMesh(Capsule, L"Primitive_Capsule");
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
	if (const FD3D12Texture* Texture = Textures.Get(Material.Textures[Slot]); Texture != nullptr && Texture->IsReady())
	{
		return *Texture; // 로딩 중이면 아래 기본 텍스처 (준비되면 CompletePendingUploads가 테이블을 다시 만든다)
	}
	// 기본 텍스처: 노멀 용도는 평면 노멀, 나머지 흰색 (그래프 머티리얼은 레이아웃의 텍스처 용도)
	bool bNormal = Slot == MaterialSlot_Normal;
	if (Material.Shader != nullptr)
	{
		bNormal = false;
		for (const FMaterialParameterSlot& Parameter : Material.Shader->Layout.Slots)
		{
			bNormal |= Parameter.Type == EMaterialParameterType::Texture && Parameter.Register == Slot && Parameter.Usage == ETextureUsage::Normal;
		}
	}
	return *Textures.Get(bNormal ? FlatNormalTexture : WhiteTexture);
}

void FResourceManager::BuildMaterialTable(FMaterial& Material)
{
	FD3D12DescriptorAllocator& Allocator = Rhi->GetSrvAllocator();
	// 칸 수: 고정 PBR 5, 그래프 = 텍스처 파라미터 수. 최소 5칸 (지형 레이어 테이블 등 5칸 범위로 묶는 쪽이 넘어가지 않게 — 남는 칸은 기본 텍스처)
	const uint32                 TableSize = std::max(std::min(Material.TextureCount, MaterialTextureMax), static_cast<uint32>(MaterialSlot_Count));
	const FD3D12DescriptorHandle NewTable  = Allocator.AllocateRange(TableSize);

	// 디스크립터 복사 대신 SRV를 직접 기록 (셰이더 가시 힙은 읽기가 느려 복사 원본으로 부적합)
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	for (uint32 Slot = 0; Slot < TableSize; ++Slot)
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
	ResolveAndFillMaterial(Material, Asset, Canonical, false);
	const FMaterialHandle Handle = CreateMaterial(Material);
	MaterialCache[CacheKey]      = Handle;
	E_LOG(LogRenderer, Log, "머티리얼 로드: {}{}", Asset.Name, Asset.IsInstance() ? " (인스턴스)" : "");
	return Handle;
}

bool FResourceManager::ResolveMaterialAsset(const FMaterialAsset& Asset, const std::filesystem::path& AssetPath, FMaterialAsset& OutResolved,
                                            std::vector<std::filesystem::path>* OutChain, std::string* OutError) const
{
	// 부모는 편집 중 원본(에디터가 아직 저장하지 않은 값) → 디스크 순
	const FMaterialAsset::FLoader Loader = [this](const std::filesystem::path& ParentPath, FMaterialAsset& OutAsset) {
		if (const auto Found = EditedMaterialSources.find(FMaterialAsset::MakePathKey(ParentPath)); Found != EditedMaterialSources.end())
		{
			OutAsset = Found->second;
			return true;
		}
		return OutAsset.LoadFromFile(ParentPath);
	};
	return FMaterialAsset::Resolve(Asset, AssetPath, Loader, OutResolved, OutChain, OutError);
}

void FResourceManager::ResolveAndFillMaterial(FMaterial& Material, const FMaterialAsset& Asset, const std::filesystem::path& AssetPath, bool bBuildTable)
{
	FMaterialAsset                     Resolved;
	std::vector<std::filesystem::path> Chain;
	ResolveMaterialAsset(Asset, AssetPath, Resolved, &Chain); // 실패해도 읽은 데까지의 값으로 채운다 (오류 로그)
	Material.ParentChain.clear();
	for (const std::filesystem::path& Parent : Chain)
	{
		Material.ParentChain.push_back(FMaterialAsset::MakePathKey(Parent));
	}
	const bool bTexturesChanged = FillMaterialFromAsset(Material, Resolved, AssetPath.parent_path());
	if (bBuildTable && (bTexturesChanged || !Material.TextureTable.IsValid()))
	{
		BuildMaterialTable(Material);
	}
}

void FResourceManager::ApplyMaterialAsset(FMaterialHandle Handle, const FMaterialAsset& Asset, const std::filesystem::path& BaseDirectory)
{
	FMaterial* Material = Materials.Get(Handle);
	if (Material == nullptr)
	{
		return;
	}
	// 이 머티리얼의 파일 경로 (경로 캐시에서 찾고, 없으면 BaseDirectory 안 이름 없는 파일로 본다)
	std::filesystem::path AssetPath = BaseDirectory / L"_.emat";
	for (const auto& [Key, Cached] : MaterialCache)
	{
		if (Cached == Handle)
		{
			AssetPath = Key;
			break;
		}
	}
	const std::wstring PathKey      = FMaterialAsset::MakePathKey(AssetPath);
	EditedMaterialSources[PathKey]  = Asset;
	ResolveAndFillMaterial(*Material, Asset, AssetPath, true);

	// 이 머티리얼을 조상으로 둔 캐시된 인스턴스를 다시 해석 (체인에 조상이 모두 들어 있으므로 한 번 돌면 된다)
	for (const auto& [Key, Cached] : MaterialCache)
	{
		FMaterial* Child = Materials.Get(Cached);
		if (Child == nullptr || Cached == Handle ||
		    std::find(Child->ParentChain.begin(), Child->ParentChain.end(), PathKey) == Child->ParentChain.end())
		{
			continue;
		}
		const std::filesystem::path ChildPath = Key;
		FMaterialAsset              ChildAsset;
		if (const auto Edited = EditedMaterialSources.find(FMaterialAsset::MakePathKey(ChildPath)); Edited != EditedMaterialSources.end())
		{
			ChildAsset = Edited->second;
		}
		else if (!ChildAsset.LoadFromFile(ChildPath))
		{
			continue;
		}
		ResolveAndFillMaterial(*Child, ChildAsset, ChildPath, true);
	}
}

bool FResourceManager::FillMaterialFromAsset(FMaterial& Material, const FMaterialAsset& Asset, const std::filesystem::path& BaseDirectory)
{
	// 그래프 머티리얼: 컴파일 실패면 이전 셰이더/값을 그대로 둔다 (편집 중 실수로 화면이 깨지지 않게 — 오류 로그).
	// 처음부터 실패면 고정 PBR 기본값으로 그린다
	if (Asset.IsGraphMaterial())
	{
		const FMaterialGraphCompileResult Compiled = FMaterialGraphCompiler::Compile(Asset.Graph, Asset.Parameters);
		if (Compiled.bSuccess)
		{
			return FillGraphMaterial(Material, Asset, Compiled.Shader, BaseDirectory);
		}
		E_LOG(LogRenderer, Error, "머티리얼 그래프 컴파일 실패 ({}){}:\n{}", Asset.Name, Material.Shader != nullptr ? " — 이전 셰이더 유지" : "",
		      Compiled.JoinErrors());
		if (Material.Shader != nullptr)
		{
			Material.Name      = Asset.Name;
			Material.BlendMode = Asset.BlendMode;
			Material.bTwoSided = Asset.bTwoSided;
			return false;
		}
	}

	Material.Name      = Asset.Name;
	Material.Constants = Asset.Constants;
	Material.BlendMode = Asset.BlendMode;
	Material.bTwoSided = Asset.bTwoSided;
	bool bChanged      = Material.Shader != nullptr || Material.TextureCount != MaterialSlot_Count; // 그래프 → 고정: 테이블 다시
	Material.Shader.reset();
	Material.GraphConstants.clear();
	Material.TextureCount = MaterialSlot_Count;
	for (uint32 Slot = MaterialSlot_Count; Slot < MaterialTextureMax; ++Slot)
	{
		Material.Textures[Slot] = FTextureHandle{};
	}
	for (uint32 Slot = 0; Slot < MaterialSlot_Count; ++Slot)
	{
		const FTextureHandle Texture = Asset.TexturePaths[Slot].empty()
		                                   ? FTextureHandle{}
		                                   : LoadTexture(BaseDirectory / FStringConv::ToWide(Asset.TexturePaths[Slot]), FMaterialAsset::GetSlotUsage(Slot));
		bChanged |= Texture != Material.Textures[Slot];
		Material.Textures[Slot] = Texture;
	}
	return bChanged;
}

bool FResourceManager::FillGraphMaterial(FMaterial& Material, const FMaterialAsset& Asset, std::shared_ptr<const FMaterialShader> Shader,
                                         const std::filesystem::path& BaseDirectory)
{
	Material.Name      = Asset.Name;
	Material.Constants = Asset.Constants; // AlphaCutoff만 쓴다
	Material.BlendMode = Asset.BlendMode;
	Material.bTwoSided = Asset.bTwoSided;
	// 같은 HLSL이면 기존 셰이더 객체를 공유 (렌더러 PSO 키 = 해시)
	if (const auto Found = GraphShaders.find(Shader->Hash); Found != GraphShaders.end())
	{
		if (std::shared_ptr<const FMaterialShader> Existing = Found->second.lock())
		{
			Shader = std::move(Existing);
		}
	}
	GraphShaders[Shader->Hash] = Shader;

	const FMaterialParameterLayout& Layout = Shader->Layout;
	bool bChanged           = Material.Shader == nullptr || Material.TextureCount != Layout.TextureCount;
	Material.GraphConstants = Layout.BuildConstants(Asset.Parameters);
	Material.TextureCount   = Layout.TextureCount;
	for (uint32 Slot = 0; Slot < MaterialTextureMax; ++Slot)
	{
		FTextureHandle Texture;
		if (Slot < Layout.TextureCount)
		{
			const FMaterialParameter* Parameter = Layout.FindTextureParameter(Slot, Asset.Parameters);
			if (Parameter != nullptr && !Parameter->Texture.empty())
			{
				Texture = LoadTexture(BaseDirectory / FStringConv::ToWide(Parameter->Texture), Parameter->Usage);
			}
		}
		bChanged |= Texture != Material.Textures[Slot];
		Material.Textures[Slot] = Texture;
	}
	// 레이아웃(텍스처 용도)이 바뀌면 기본 텍스처가 달라질 수 있으므로 셰이더가 바뀌어도 테이블을 다시 만든다
	bChanged |= Material.Shader == nullptr || Material.Shader->Hash != Shader->Hash;
	Material.Shader = std::move(Shader);
	return bChanged;
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
