#include "Editor/ContentBrowser/ThumbnailCache.h"

#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Editor/EditorContext.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "Renderer/Material.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneAssetResolver.h"
#include "Renderer/StaticMesh.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Particles.h"
#include "Scene/SceneSerializer.h"

#include <imgui.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	std::wstring Lower(std::wstring Text)
	{
		std::transform(Text.begin(), Text.end(), Text.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Text;
	}

	std::wstring MakeKey(const std::filesystem::path& Path) { return Lower(Path.lexically_normal().generic_wstring()); }

	bool IsImage(const std::wstring& Extension)
	{
		return Extension == L".png" || Extension == L".jpg" || Extension == L".jpeg" || Extension == L".tga" || Extension == L".bmp";
	}

	FBox ComputeBounds(FScene& Scene, const FResourceManager& Resources)
	{
		FBox Bounds;
		Scene.GetRegistry().View<FTransformComponent, FStaticMeshComponent>().Each([&](FEntity, FTransformComponent& Transform, FStaticMeshComponent& Mesh) {
			if (const FStaticMesh* StaticMesh = Resources.GetMesh(Mesh.Mesh); StaticMesh != nullptr && Mesh.bVisible)
			{
				Bounds.AddBox(StaticMesh->GetLocalBounds().TransformBy(Transform.WorldMatrix));
			}
		});
		return Bounds;
	}

	void AddLight(FScene& Scene)
	{
		const FEntity Sun                = Scene.CreateEntity("ThumbnailLight");
		Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-45.0f, 40.0f, 0.0f);
		Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun).Intensity = 3.0f;
	}
} // namespace

// ---------------------------------------------------------------- 드래그 앤 드롭 공용

const std::vector<std::filesystem::path>* FContentDragDrop::AcceptPayload()
{
	const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(PayloadType);
	return Payload != nullptr && !GetPaths().empty() ? &GetPaths() : nullptr;
}

// ---------------------------------------------------------------- 썸네일

FThumbnailCache::FThumbnailCache()  = default;
FThumbnailCache::~FThumbnailCache() = default;

bool FThumbnailCache::Supports(const std::filesystem::path& Path)
{
	const std::wstring Extension = Lower(Path.extension().wstring());
	return Extension == L".glb" || Extension == L".gltf" || Extension == L".fbx" || Extension == L".emat" || Extension == L".eparticle" || Extension == L".escene" ||
	       IsImage(Extension);
}

void FThumbnailCache::Shutdown(FEditorContext& Context)
{
	(void)Context;
	Entries.clear(); // GPU 유휴 상태에서 호출 (즉시 해제)
	Queue.clear();
	Scene.Clear();
	if (bReady)
	{
		Renderer.Shutdown();
		bReady = false;
	}
}

bool FThumbnailCache::EnsureRenderer(FEditorContext& Context)
{
	if (bReady || bFailed)
	{
		return bReady;
	}
	if (!Renderer.Init(*Context.Rhi, *Context.Resources))
	{
		E_LOG(LogEditor, Error, "썸네일 렌더러 초기화 실패");
		bFailed = true;
		return false;
	}
	// 작은 화면이므로 그림자 맵도 작게 (기본 2048 x 4장 → 512 x 2장)
	Renderer.ShadowSettings.Resolution   = 512;
	Renderer.ShadowSettings.CascadeCount = 2;
	Renderer.BackgroundColor             = FVector4(0.035f, 0.037f, 0.042f, 1.0f); // 하늘 없이 어두운 회색 (언리얼 썸네일처럼)
	Renderer.bDrawSkybox                 = false;

	FMaterial Material;
	Material.Name                     = "ThumbnailImage";
	Material.Constants.BaseColorFactor = FVector4(0.0f, 0.0f, 0.0f, 1.0f);
	Material.Constants.EmissiveFactor = FVector3::OneVector;
	Material.Constants.Roughness      = 1.0f;
	ImageMaterial                     = Context.Resources->CreateMaterial(Material);
	Camera.SetPerspective(40.0f, 1.0f, 1.0f, 100000.0f);
	bReady = true;
	return true;
}

uint64 FThumbnailCache::Request(const std::filesystem::path& Path)
{
	if (!Supports(Path))
	{
		return 0;
	}
	std::error_code                       ErrorCode;
	const std::filesystem::file_time_type WriteTime = std::filesystem::last_write_time(Path, ErrorCode);
	FEntry&                               Entry     = Entries[MakeKey(Path)];
	const bool                            bStale    = Entry.WriteTime != WriteTime;
	if ((bStale || (!Entry.Target && !Entry.bFailed)) && !Entry.bQueued)
	{
		Entry.bQueued = true;
		Entry.bFailed = false;
		Queue.push_back(Path);
	}
	// 다시 그리는 중에도 이전 썸네일을 보여 준다
	return Entry.Target ? static_cast<uint64>(Entry.Target->GetSrv().Gpu.ptr) : 0;
}

void FThumbnailCache::Invalidate(const std::filesystem::path& Path)
{
	if (const auto Found = Entries.find(MakeKey(Path)); Found != Entries.end())
	{
		Found->second.WriteTime = {};
	}
}

bool FThumbnailCache::BuildScene(FEditorContext& Context, const std::filesystem::path& Path, bool& bOutImage)
{
	FResourceManager&  Resources = *Context.Resources;
	const std::wstring Extension = Lower(Path.extension().wstring());
	bOutImage                    = false;
	Scene.Clear();

	if (Extension == L".escene")
	{
		std::ifstream File(Path, std::ios::binary);
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		if (!File || !FSceneSerializer::FromJsonString(Scene, Buffer.str()))
		{
			return false;
		}
		FSceneAssetResolver::Resolve(Scene, Resources, Context.ContentDirectory);
		bool bHasLight = false;
		Scene.GetRegistry().View<FDirectionalLightComponent>().Each([&](FEntity, FDirectionalLightComponent&) { bHasLight = true; });
		if (!bHasLight)
		{
			AddLight(Scene);
		}
		return true;
	}

	AddLight(Scene);
	if (Extension == L".glb" || Extension == L".gltf" || Extension == L".fbx")
	{
		const FEntity Root = FModelLoader::LoadIntoScene(Path, Scene, Resources);
		if (!Scene.GetRegistry().IsValid(Root))
		{
			return false;
		}
		FAnimationSystem::Update(Scene, 0.0f); // 애니메이션 모델은 첫 클립 첫 포즈
		return true;
	}
	if (Extension == L".emat")
	{
		const FMaterialHandle Material = Resources.LoadMaterial(Path);
		if (!Material.IsValid())
		{
			return false;
		}
		const FEntity         Sphere = Scene.CreateEntity("Sphere");
		FStaticMeshComponent& Mesh   = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Sphere);
		Mesh.Mesh                    = Resources.GetOrCreatePrimitiveMesh("sphere");
		Mesh.Material                = Material;
		return true;
	}
	if (Extension == L".eparticle")
	{
		std::shared_ptr<FParticleSystemAsset> System = Resources.LoadParticleSystem(Path);
		if (!System)
		{
			return false;
		}
		const FEntity             Emitter   = Scene.CreateEntity("Emitter");
		FParticleSystemComponent& Component = Scene.GetRegistry().Emplace<FParticleSystemComponent>(Emitter);
		Component.Runtime.System            = System;
		Component.Runtime.ResolvedAsset     = "(thumbnail)";
		Component.Asset                     = "(thumbnail)";
		Scene.UpdateTransforms();
		for (int32 Step = 0; Step < 60; ++Step) // 1초 진행한 모습 (시작 버스트가 아직 살아 있게)
		{
			FParticleSystem::Update(Scene, 1.0f / 60.0f);
		}
		return true;
	}
	if (IsImage(Extension))
	{
		const FTextureHandle Texture = Resources.LoadTexture(Path, ETextureUsage::Color);
		FMaterial*           Material = Resources.GetMaterial(ImageMaterial);
		if (!Texture.IsValid() || Material == nullptr)
		{
			return false;
		}
		// 발광 슬롯에 이미지 → 조명과 무관하게 원래 색 (톤매핑 끔)
		Material->Textures[MaterialSlot_Emissive] = Texture;
		Resources.RefreshMaterialTextures(ImageMaterial);
		const FEntity Quad = Scene.CreateEntity("Image");
		Scene.GetTransform(Quad).Scale = FVector3(0.01f, 1.0f, 1.0f); // 100cm 큐브를 얇은 판으로 (-X 면이 카메라 쪽)
		FStaticMeshComponent& Mesh      = Scene.GetRegistry().Emplace<FStaticMeshComponent>(Quad);
		Mesh.Mesh                       = Resources.GetOrCreatePrimitiveMesh("cube");
		Mesh.Material                   = ImageMaterial;
		bOutImage                       = true;
		return true;
	}
	return false;
}

void FThumbnailCache::RenderPending(FEditorContext& Context, uint32 Budget)
{
	if (Queue.empty() || Context.Rhi == nullptr || !EnsureRenderer(Context))
	{
		return;
	}
	for (uint32 Rendered = 0; Rendered < Budget && !Queue.empty();)
	{
		const std::filesystem::path Path = Queue.front();
		Queue.pop_front();
		FEntry& Entry = Entries[MakeKey(Path)];
		Entry.bQueued = false;
		std::error_code ErrorCode;
		Entry.WriteTime = std::filesystem::last_write_time(Path, ErrorCode);

		bool bImage = false;
		if (!BuildScene(Context, Path, bImage))
		{
			Entry.bFailed = true;
			Scene.Clear();
			continue;
		}
		Scene.UpdateTransforms();

		// 구도: 이미지는 판을 정면으로 꽉 차게, 나머지는 비스듬히 위에서 경계에 맞춤
		if (bImage)
		{
			Orbit.Target   = FVector3::ZeroVector;
			Orbit.Yaw      = 0.0f;
			Orbit.Pitch    = 0.0f;
			Orbit.Distance = 50.0f / FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees() * 0.5f)) + 0.5f;
		}
		else
		{
			Orbit.Yaw   = 35.0f;
			Orbit.Pitch = -22.0f;
			const FBox Bounds = ComputeBounds(Scene, *Context.Resources);
			Orbit.Frame(Bounds.IsValid() ? Bounds : FBox(FVector3(-120.0f, -120.0f, 0.0f), FVector3(120.0f, 120.0f, 260.0f)), Camera.GetFovYDegrees(), 1.0f);
			Orbit.Distance *= 0.92f; // 경계 구 기준 맞춤은 여백이 커서 조금만 당긴다
		}
		Camera.SetPerspective(Camera.GetFovYDegrees(), 1.0f, FMath::Clamp(Orbit.Distance * 0.01f, 0.1f, 10.0f), FMath::Max(Orbit.Distance * 50.0f, 20000.0f));
		Orbit.ApplyTo(Camera);
		Renderer.ShadowSettings.ShadowDistance = FMath::Max(Orbit.Distance * 3.0f, 500.0f);

		if (!Entry.Target)
		{
			Entry.Target = std::make_unique<FD3D12RenderTarget>();
			FRenderTargetDesc Desc = FRenderTargetDesc::MakeLdrDisplay();
			Desc.bWithDepth        = false; // 씬 렌더러가 HDR 버퍼(깊이 포함)에 그린 뒤 톤매핑만 여기로 쓴다
			if (!Entry.Target->Init(Context.Rhi->GetDevice(), Context.Rhi->GetSrvAllocator(), Size, Size, L"Thumbnail", Desc))
			{
				Entry.Target.reset();
				Entry.bFailed = true;
				Scene.Clear();
				continue;
			}
		}

		const ETonemapOperator SavedTonemapper = Renderer.PostProcessSettings.Tonemapper;
		const bool             bSavedBloom     = Renderer.PostProcessSettings.bBloomEnabled;
		if (bImage)
		{
			Renderer.PostProcessSettings.Tonemapper    = ETonemapOperator::None;
			Renderer.PostProcessSettings.bBloomEnabled = false;
		}
		ID3D12GraphicsCommandList* CommandList = Context.Rhi->GetCommandList();
		Entry.Target->Begin(CommandList, nullptr);
		Renderer.Render(Scene, Camera, Entry.Target->GetOutput());
		Entry.Target->End(CommandList);
		Renderer.PostProcessSettings.Tonemapper    = SavedTonemapper;
		Renderer.PostProcessSettings.bBloomEnabled = bSavedBloom;

		Scene.Clear();
		++Rendered;
	}
}

bool FThumbnailCache::ReloadShaders(const std::vector<std::filesystem::path>* ChangedFiles)
{
	if (!bReady)
	{
		return true;
	}
	FShaderLibrary& Library = Renderer.GetShaderLibrary();
	if (ChangedFiles == nullptr)
	{
		Library.InvalidateAll();
	}
	else
	{
		for (const std::filesystem::path& File : *ChangedFiles)
		{
			Library.Invalidate(File);
		}
	}
	// 셰이더가 바뀌면 이미 그린 썸네일도 다시
	for (auto& [Key, Entry] : Entries)
	{
		Entry.WriteTime = {};
	}
	return Renderer.ReloadShaders(ChangedFiles == nullptr);
}
