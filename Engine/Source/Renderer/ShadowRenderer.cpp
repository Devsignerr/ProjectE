#include "Renderer/ShadowRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/StaticMesh.h"

#include "Core/Jobs/ParallelFor.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr DXGI_FORMAT ShadowResourceFormat = DXGI_FORMAT_R32_TYPELESS;
	constexpr DXGI_FORMAT ShadowDsvFormat      = DXGI_FORMAT_D32_FLOAT;
	constexpr DXGI_FORMAT ShadowSrvFormat      = DXGI_FORMAT_R32_FLOAT;

	enum EShadowRootParameter : uint32
	{
		ShadowParam_PassConstants   = 0, // b0 (루트 상수 17개: 라이트 뷰-투영 + 인스턴스 시작 위치)
		ShadowParam_SkinPalette     = 1, // t15 (프레임 스킨 팔레트)
		ShadowParam_Instances       = 2, // t13
		ShadowParam_InstanceIndices = 3, // t14
		ShadowParam_MaskConstants   = 4, // b1 (루트 상수 2개: 베이스 컬러 알파 팩터, 알파 컷오프 — Masked만)
		ShadowParam_MaskTexture     = 5, // t0 (머티리얼 텍스처 테이블 첫 칸 — Masked만)
		ShadowParam_MaterialConstants = 6, // b2 (그래프 머티리얼 Masked: 헤더 + 파라미터)
		ShadowParam_MaterialTextures  = 7, // 공간 2 t0~ (그래프 머티리얼 텍스처 테이블, 무제한 범위)
	};
	constexpr uint32 ShadowPassConstantCount = 17;
	constexpr uint32 MaxCasterChunks         = 64; // 캐스터 거르기 병렬 조각 상한 (합치기 비용 = 조각 수에 비례)
} // namespace

FShadowRenderer::~FShadowRenderer()
{
	Shutdown();
}

bool FShadowRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary)
{
	E_CHECKF(Rhi == nullptr, "섀도우 렌더러가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;

	const uint32 PassIndex      = RootSignature.AddConstants(ShadowPassConstantCount, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 PaletteIndex   = RootSignature.AddShaderResourceView(15, 0, D3D12_SHADER_VISIBILITY_VERTEX); // 스킨 팔레트 (SkinnedMesh.hlsli t15)
	const uint32 InstancesIndex = RootSignature.AddShaderResourceView(13, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 IndicesIndex   = RootSignature.AddShaderResourceView(14, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 MaskIndex      = RootSignature.AddConstants(2, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 MaskTexture    = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) },
	                                                               D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(PassIndex == ShadowParam_PassConstants && PaletteIndex == ShadowParam_SkinPalette && InstancesIndex == ShadowParam_Instances &&
	        IndicesIndex == ShadowParam_InstanceIndices && MaskIndex == ShadowParam_MaskConstants && MaskTexture == ShadowParam_MaskTexture);
	const uint32 MaterialConstants = RootSignature.AddConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 MaterialTextures  = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE) },
		D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(MaterialConstants == ShadowParam_MaterialConstants && MaterialTextures == ShadowParam_MaterialTextures);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR));
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP)); // 그래프 Clamp
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
	                            L"ShadowRootSignature"))
	{
		return false;
	}
	MaterialPipelines.Init(*Rhi, *ShaderLibrary, L"ShadowMaterialPipeline");
	MaterialPipelines.SetSkinCache(bSkinCache);
	for (uint32 Variant = 0; Variant < DepthVariantCount; ++Variant)
	{
		if (!CreatePipeline(Pipelines[Variant], false, Variant))
		{
			return false;
		}
	}

	// 셰이더가 항상 유효한 SRV를 참조하도록 기본 해상도로 미리 생성
	EnsureShadowMap(FShadowSettings{}.Resolution, ShadowMath::MaxCascades);
	return ShadowMap != nullptr;
}

void FShadowRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	if (Srv.IsValid())
	{
		Rhi->GetSrvAllocator().Free(Srv);
	}
	ShadowMap.Reset();
	DsvHeap.Shutdown();
	CacheMap.Reset();
	CacheDsvHeap.Shutdown();
	for (FD3D12PipelineState& VariantPipeline : Pipelines)
	{
		VariantPipeline.Shutdown();
	}
	MaterialPipelines.Shutdown();
	RootSignature.Shutdown();
	MapResolution = 0;
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
}

bool FShadowRenderer::CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, uint32 Variant)
{
	const bool         bSkinned = (Variant & DepthVariantSkinned) != 0;
	const bool         bMasked  = (Variant & DepthVariantMasked) != 0;
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Shadow.hlsl";
	VertexDesc.EntryPoint = bMasked ? (bSkinned ? L"ShadowSkinnedMaskedVS" : L"ShadowMaskedVS") : (bSkinned ? L"ShadowSkinnedVS" : L"ShadowVS");
	VertexDesc.Stage      = EShaderStage::Vertex;
	const bool bSkinCacheVertex = bSkinned && bSkinCache; // 스킨 캐시 정점 (SkinnedMesh.hlsli E_SKIN_CACHE — 슬롯 1 스트림 없음)
	if (bSkinCacheVertex)
	{
		VertexDesc.Defines.push_back(L"E_SKIN_CACHE");
	}
	FShaderCompileDesc PixelDesc;
	PixelDesc.FileName   = L"Shadow.hlsl";
	PixelDesc.EntryPoint = L"ShadowMaskedPS";
	PixelDesc.Stage      = EShaderStage::Pixel;
	if (bForceRecompile && (!ShaderLibrary->CookShader(VertexDesc) || (bMasked && !ShaderLibrary->CookShader(PixelDesc))))
	{
		return false;
	}
	const ComPtr<IDxcBlob> VertexShader = ShaderLibrary->GetShader(VertexDesc);
	const ComPtr<IDxcBlob> PixelShader  = bMasked ? ShaderLibrary->GetShader(PixelDesc) : ComPtr<IDxcBlob>();
	if (!VertexShader || (bMasked && !PixelShader))
	{
		return false;
	}

	FGraphicsPipelineDesc Desc;
	Desc.RootSignature        = RootSignature.Get();
	Desc.VertexShader         = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	if (bMasked)
	{
		Desc.PixelShader = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	}
	Desc.InputLayout          = bSkinned && !bSkinCacheVertex ? FStaticMesh::GetSkinnedInputLayout() : FStaticMesh::GetInputLayout(); // POSITION(+UV/COLOR, 스킨)만 사용
	Desc.NumRenderTargets     = 0;
	Desc.DepthStencilFormat   = ShadowDsvFormat;
	Desc.bDepthEnable         = true;
	Desc.CullMode             = D3D12_CULL_MODE_NONE; // 한 면짜리 메시도 그림자를 드리운다
	Desc.bDepthClip           = false;                // 광원 근평면 뒤 캐스터를 근평면에 붙여 그린다 (팬케이킹)
	Desc.DepthBias            = BakedDepthBias;
	Desc.SlopeScaledDepthBias = BakedSlopeBias;
	if (Variant == 0)
	{
		MaterialPipelines.SetBaseDesc(Desc); // 그래프 머티리얼 Masked PSO도 같은 설정 (바이어스가 바뀌면 다시 만든다)
	}
	static const wchar_t* const Names[DepthVariantCount] = { L"ShadowPipeline", L"ShadowSkinnedPipeline", L"ShadowMaskedPipeline",
	                                                         L"ShadowSkinnedMaskedPipeline" };
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, Names[Variant]);
}

bool FShadowRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	MaterialPipelines.Reset(); // 그래프 머티리얼 PSO는 다음 그리기에 다시 만든다
	MaterialPipelines.SetSkinCache(bSkinCache);
	InvalidateCache();         // 셰이더가 바뀌었을 수 있다
	for (uint32 Variant = 0; Variant < DepthVariantCount; ++Variant)
	{
		FD3D12PipelineState NewPipeline;
		if (!CreatePipeline(NewPipeline, bForceRecompile, Variant))
		{
			E_LOG(LogRenderer, Error, "섀도우 셰이더 다시 로드 실패 (변형 {}): 기존 파이프라인 유지", Variant);
			return false;
		}
		Pipelines[Variant].Swap(NewPipeline);
		Rhi->DeferRelease(NewPipeline.Detach());
	}
	return true;
}

void FShadowRenderer::EnsureShadowMap(uint32 Resolution, uint32 Cascades)
{
	Cascades = ShadowMath::MaxCascades; // SRV 레이아웃 고정 (셰이더가 인덱스로 접근)
	if (ShadowMap && MapResolution == Resolution)
	{
		return;
	}
	ComPtr<ID3D12Resource> NewShadowMap;
	FD3D12DescriptorHeap NewDsvHeap;

	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	D3D12_RESOURCE_DESC Desc = MakeTexture2DDesc(Resolution, Resolution, ShadowResourceFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	Desc.DepthOrArraySize    = static_cast<UINT16>(Cascades);

	D3D12_CLEAR_VALUE ClearValue{};
	ClearValue.Format             = ShadowDsvFormat;
	ClearValue.DepthStencil.Depth = 1.0f;

	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	                                           &ClearValue, IID_PPV_ARGS(&NewShadowMap))))
	{
		E_LOG(LogRenderer, Error, "섀도우 맵 생성 실패 ({}x{} x {})", Resolution, Resolution, Cascades);
		return;
	}
	NewShadowMap->SetName(L"CascadedShadowMap");

	if (!NewDsvHeap.Init(Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, Cascades, false, L"ShadowDsvHeap"))
	{
		NewShadowMap.Reset();
		return;
	}
	for (uint32 Index = 0; Index < Cascades; ++Index)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC DsvDesc{};
		DsvDesc.Format                         = ShadowDsvFormat;
		DsvDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		DsvDesc.Texture2DArray.FirstArraySlice = Index;
		DsvDesc.Texture2DArray.ArraySize       = 1;
		Device->CreateDepthStencilView(NewShadowMap.Get(), &DsvDesc, NewDsvHeap.GetCpuHandle(Index));
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                   = ShadowSrvFormat;
	SrvDesc.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	SrvDesc.Shader4ComponentMapping  = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Texture2DArray.MipLevels = 1;
	SrvDesc.Texture2DArray.ArraySize = Cascades;
	const FD3D12DescriptorHandle NewSrv = Rhi->GetSrvAllocator().Allocate();
	Device->CreateShaderResourceView(NewShadowMap.Get(), &SrvDesc, NewSrv.Cpu);

	// 새 리소스가 모두 준비된 뒤 기존 프레임의 리소스를 지연 해제한다.
	ReleaseShadowMap();
	ShadowMap = std::move(NewShadowMap);
	DsvHeap = std::move(NewDsvHeap);
	Srv = NewSrv;
	MapResolution = Resolution;
	MapCascades   = Cascades;
	E_LOG(LogRenderer, Log, "섀도우 맵 생성: {}x{} x {} 캐스케이드", Resolution, Resolution, Cascades);
}

void FShadowRenderer::ReleaseShadowMap()
{
	if (!ShadowMap)
	{
		return;
	}
	// 진행 중인 프레임이 참조할 수 있으므로 지연 해제
	Rhi->DeferRelease(ShadowMap);
	Rhi->DeferFreeDescriptor(Srv);
	Srv = FD3D12DescriptorHandle{};
	ShadowMap.Reset();
	DsvHeap.Shutdown(); // DSV 힙은 기록 시점에만 읽히므로 즉시 해제
	MapResolution = 0;
	for (bool& bClean : bMapSliceClean)
	{
		bClean = false; // 새 섀도우 맵은 내용이 없다
	}
	ReleaseCache(); // 크기가 바뀌면 캐시도 다시 만든다
}

bool FShadowRenderer::EnsureCache()
{
	if (CacheMap)
	{
		return true; // 섀도우 맵 크기가 바뀌면 ReleaseShadowMap이 캐시도 내린다
	}
	ID3D12Device*       Device = Rhi->GetDevice().GetDevice();
	D3D12_RESOURCE_DESC Desc   = MakeTexture2DDesc(MapResolution, MapResolution, ShadowResourceFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	Desc.DepthOrArraySize      = static_cast<UINT16>(MapCascades);
	D3D12_CLEAR_VALUE ClearValue{};
	ClearValue.Format             = ShadowDsvFormat;
	ClearValue.DepthStencil.Depth = 1.0f;
	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	ComPtr<ID3D12Resource>      NewCache;
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_SOURCE, &ClearValue, IID_PPV_ARGS(&NewCache))))
	{
		E_LOG(LogRenderer, Error, "그림자 캐시 생성 실패 ({}x{} x {}) — 캐시 없이 그린다", MapResolution, MapResolution, MapCascades);
		return false;
	}
	NewCache->SetName(L"CascadedShadowCache");
	if (!CacheDsvHeap.Init(Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, MapCascades, false, L"ShadowCacheDsvHeap"))
	{
		return false;
	}
	for (uint32 Index = 0; Index < MapCascades; ++Index)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC DsvDesc{};
		DsvDesc.Format                         = ShadowDsvFormat;
		DsvDesc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
		DsvDesc.Texture2DArray.FirstArraySlice = Index;
		DsvDesc.Texture2DArray.ArraySize       = 1;
		Device->CreateDepthStencilView(NewCache.Get(), &DsvDesc, CacheDsvHeap.GetCpuHandle(Index));
	}
	CacheMap = std::move(NewCache);
	E_LOG(LogRenderer, Log, "그림자 캐시 생성: {}x{} x {}", MapResolution, MapResolution, MapCascades);
	return true;
}

void FShadowRenderer::ReleaseCache()
{
	if (!CacheMap)
	{
		return; // 캐시가 없으면 유효한 캐시 상태도 없다 (Decide의 Rebuild 뒤 EnsureCache가 만든다)
	}
	for (ShadowCacheMath::FCascadeCacheState& State : CacheStates)
	{
		State = ShadowCacheMath::FCascadeCacheState{};
	}
	Rhi->DeferRelease(CacheMap);
	CacheMap.Reset();
	CacheDsvHeap.Shutdown();
}

void FShadowRenderer::PrepareCascades(const FCamera& Camera, const FVector3& LightDirection, const FShadowSettings& Settings)
{
	E_CHECKF(Rhi != nullptr, "섀도우 렌더러가 초기화되지 않았습니다");

	Constants               = FShadowConstants{};
	ActiveCascades          = 0;
	FrameSettings           = Settings;
	if (!Settings.bCacheStatic && CacheMap)
	{
		ReleaseCache(); // 끄면 메모리도 돌려준다
	}
	Constants.CameraForward = Camera.GetForwardVector();
	if (!Settings.bEnabled || LightDirection.IsNearlyZero())
	{
		return; // ShadowEnabled = 0: 셰이더는 그림자를 건너뛴다
	}

	// 바이어스는 PSO에 고정되므로 바뀌면 재생성
	if (Settings.DepthBias != BakedDepthBias || Settings.SlopeBias != BakedSlopeBias)
	{
		BakedDepthBias = Settings.DepthBias;
		BakedSlopeBias = Settings.SlopeBias;
		ReloadShaders(false);
	}

	const uint32 RequestedResolution = FMath::Clamp<uint32>(Settings.Resolution, 256, 8192);
	EnsureShadowMap(RequestedResolution, ShadowMath::MaxCascades);
	if (!ShadowMap)
	{
		return;
	}

	const uint32 Resolution = MapResolution;

	// ---- 캐스케이드 계산
	const uint32 CascadeCount = FMath::Clamp<uint32>(Settings.CascadeCount, 1, ShadowMath::MaxCascades);
	const float  NearZ        = Camera.GetNearZ();
	const float  FarZ         = FMath::Max(FMath::Min(Settings.ShadowDistance, Camera.GetFarZ()), NearZ + 1.0f);
	const auto   Splits       = ShadowMath::ComputeCascadeSplits(NearZ, FarZ, CascadeCount, Settings.SplitLambda);

	ShadowMath::FCascade* Cascades  = CascadeData;
	float                 SliceNear = NearZ;
	for (uint32 Index = 0; Index < CascadeCount; ++Index)
	{
		const float HalfHeight = Camera.GetOrthoHeight() * 0.5f;
		const auto  Corners    = Camera.IsOrthographic()
		                             ? ShadowMath::ComputeOrthoSliceCorners(Camera.GetPosition(), Camera.GetForwardVector(), Camera.GetRightVector(),
		                                                                    Camera.GetUpVector(), HalfHeight * Camera.GetAspectRatio(), HalfHeight,
		                                                                    SliceNear, Splits[Index])
		                             : ShadowMath::ComputeFrustumSliceCorners(Camera.GetPosition(), Camera.GetForwardVector(), Camera.GetRightVector(),
		                                                                      Camera.GetUpVector(), FMath::DegreesToRadians(Camera.GetFovYDegrees()),
		                                                                      Camera.GetAspectRatio(), SliceNear, Splits[Index]);
		const float Quantize = Settings.bCacheStatic && Index >= Settings.CacheQuantizeFirst ? Settings.CacheQuantize : 0.0f;
		Cascades[Index] = ShadowMath::ComputeCascade(Corners, LightDirection, Resolution, Settings.CasterExtension, Quantize);
		Constants.CascadeViewProjection[Index] = Cascades[Index].ViewProjection;
		Constants.CascadeSplits[Index]         = Splits[Index];
		Constants.CascadeTexelWorld[Index]     = Cascades[Index].WorldTexelSize;
		CascadeFrustums[Index]                 = FFrustum::FromViewProjection(Cascades[Index].ViewProjection);
		SliceNear                              = Splits[Index];
	}
	ActiveCascades              = CascadeCount;
	Constants.ShadowEnabled     = 1.0f;
	Constants.TexelSize         = 1.0f / static_cast<float>(Resolution);
	Constants.NormalOffset      = Settings.NormalOffset;
	Constants.CascadeCount      = CascadeCount;
	Constants.VisualizeCascades = Settings.bVisualizeCascades ? 1u : 0u;
}

bool FShadowRenderer::IntersectsCasterVolume(const FBox& WorldBounds) const
{
	for (uint32 Index = 0; Index < ActiveCascades; ++Index)
	{
		if (CascadeFrustums[Index].Intersects(WorldBounds))
		{
			return true;
		}
	}
	return false;
}

FRGResourceRef FShadowRenderer::ImportShadowMap(FRenderGraph& Graph) const
{
	return ShadowMap ? Graph.Import("CascadedShadowMap", ShadowMap.Get(), ERGAccess::SrvPixel, ERGAccess::SrvPixel, 1, MapCascades) : FRGResourceRef{};
}

uint64 FShadowRenderer::ComputeStaticSetHash(const FMeshInstanceList& Instances) const
{
	// 정적 캐스터 집합: 인스턴스 해시의 합 (목록 순서 무관). 그리는 결과를 바꾸는 값을 모두 넣는다
	using namespace ShadowCacheMath;
	uint64 Sum   = 0;
	uint64 Count = 0;
	for (const FMeshInstance& Instance : Instances.GetInstances())
	{
		if (!Instance.bShadowStatic || Instance.IsSkinned() || !Instance.CastsShadow())
		{
			continue;
		}
		uint64 Hash = HashSeed;
		Hash        = HashValue(Hash, Instance.Mesh);
		Hash        = HashValue(Hash, Instance.MeshHandle);
		Hash        = HashValue(Hash, Instance.World);
		// LOD는 넣지 않는다: 카메라 이동으로 바뀌는 메인 LOD가 캐시를 매번 무효로 만들지 않게 (캐시는 다시 그린 시점 LOD — 캐스케이드가
		// 바뀌거나 정지하면 맞춰진다, LOD 설정 변경은 씬 렌더러가 InvalidateCache)
		Hash        = HashValue(Hash, GetDepthVariant(Instance));
		if (Instance.IsMasked())
		{
			const FMaterial& Material = *Instance.Material;
			Hash = HashValue(Hash, Instance.MaterialHandle);
			Hash = HashValue(Hash, Instance.Material);
			Hash = HashValue(Hash, Material.Shader.get());
			Hash = HashValue(Hash, Material.Constants.BaseColorFactor.W);
			Hash = HashValue(Hash, Material.Constants.AlphaCutoff);
			Hash = HashValue(Hash, Material.TextureTable.Gpu.ptr);
		}
		Sum += Finalize(Hash);
		++Count;
	}
	return Finalize(HashValue(HashValue(HashSeed, Sum), Count));
}

void FShadowRenderer::PrepareBatches(const FMeshInstanceList& Instances, FD3D12DynamicUploadBuffer& DynamicBuffer)
{
	using namespace ShadowCacheMath;
	const bool   bCache     = FrameSettings.bCacheStatic;
	const uint64 StaticHash = bCache ? ComputeStaticSetHash(Instances) : 0;
	const float  MinTexels  = FrameSettings.MinCasterTexels;
	bExtraStatic            = bCache && ExtraCasters && ExtraCasterState;
	bExtraDynamic           = ExtraDynamicCasters && HasExtraDynamicCasters && HasExtraDynamicCasters();

	// ---- 1) 캐스케이드별 캐시 행동 (메인 스레드 — 추가 캐스터 상태 콜백·캐시 생성)
	//   키 = 캐스케이드 뷰-투영 + 설정 + 정적 집합 + 추가 캐스터 상태 + 캐시 세대
	for (uint32 Index = 0; Index < ActiveCascades; ++Index)
	{
		ECacheAction Action = ECacheAction::Direct;
		if (bCache)
		{
			uint64 Key = HashSeed;
			Key        = HashValue(Key, CascadeData[Index].ViewProjection);
			Key        = HashValue(Key, Index);
			Key        = HashValue(Key, MapResolution);
			Key        = HashValue(Key, BakedDepthBias);
			Key        = HashValue(Key, BakedSlopeBias);
			Key        = HashValue(Key, FrameSettings.LodBias);
			Key        = HashValue(Key, MinTexels);
			Key        = HashValue(Key, CacheEpoch);
			Key        = HashValue(Key, StaticHash);
			Key        = HashValue(Key, bExtraStatic);
			if (bExtraStatic)
			{
				Key = HashValue(Key, ExtraCasterState(CascadeFrustums[Index]));
			}
			Action = Decide(CacheStates[Index], Key, true);
			if (Action == ECacheAction::Rebuild && !EnsureCache())
			{
				Action                         = ECacheAction::Direct;
				CacheStates[Index].bCacheValid = false;
			}
		}
		else
		{
			Decide(CacheStates[Index], 0, false);
		}
		CascadeActions[Index] = Action;
	}

	// ---- 2) 인스턴스 조각별 캐스터 거르기 (병렬, 조각마다 자기 목록): 인스턴스를 한 번 읽어 캐스케이드마다
	//   캐스케이드 프러스텀 + 작은 캐스터 컬링 → (정적/스킨 × 불투명/Masked)·메시·캐스케이드 LOD별 키 (Masked만 머티리얼별).
	//   반투명 머티리얼은 그림자를 드리우지 않는다. Direct = 모두 정적 목록(StaticBatches), Rebuild = 정적은 정적 목록(캐시)·동적은 동적 목록,
	//   Reuse = 동적만
	const std::vector<FMeshInstance>& List       = Instances.GetInstances();
	const uint32                      Count      = static_cast<uint32>(List.size());
	const uint32                      ChunkSize  = FMath::Max(1024u, (Count + MaxCasterChunks - 1) / MaxCasterChunks); // 조각 수 ≤ MaxCasterChunks
	const uint32                      ChunkCount = (Count + ChunkSize - 1) / ChunkSize;
	if (CasterChunks.size() < ChunkCount)
	{
		CasterChunks.resize(ChunkCount);
	}
	FParallel::ParallelFor(ChunkCount, 1, [this, &List, Count, ChunkSize, MinTexels](uint32 BeginChunk, uint32 EndChunk) {
		uint32 LodBias[ShadowMath::MaxCascades];
		for (uint32 Index = 0; Index < ActiveCascades; ++Index)
		{
			LodBias[Index] = ComputeCascadeLodBias(Index, FrameSettings.LodBias);
		}
		for (uint32 ChunkIndex = BeginChunk; ChunkIndex < EndChunk; ++ChunkIndex)
		{
			FCasterChunk& Chunk = CasterChunks[ChunkIndex];
			for (uint32 Index = 0; Index < ActiveCascades; ++Index)
			{
				Chunk.Static[Index].clear();
				Chunk.Dynamic[Index].clear();
			}
			const uint32 End = FMath::Min(Count, (ChunkIndex + 1) * ChunkSize);
			for (uint32 InstanceIndex = ChunkIndex * ChunkSize; InstanceIndex < End; ++InstanceIndex)
			{
				const FMeshInstance& Instance = List[InstanceIndex];
				if (!Instance.CastsShadow())
				{
					continue;
				}
				const bool   bStaticCaster = Instance.bShadowStatic && !Instance.IsSkinned();
				const float  Radius        = Instance.WorldBounds.GetExtent().Length();
				const uint32 LodCount      = Instance.Mesh->GetLodCount();
				const uint64 KeyBase       = MakeDepthBatchKey(Instance, 0); // LOD 칸(하위 4비트)은 캐스케이드마다
				for (uint32 Index = 0; Index < ActiveCascades; ++Index)
				{
					const ECacheAction Action  = CascadeActions[Index];
					const bool         bStatic = Action != ECacheAction::Direct && bStaticCaster;
					if (bStatic && Action == ECacheAction::Reuse)
					{
						continue; // 캐시에 있다
					}
					if (!CascadeFrustums[Index].Intersects(Instance.WorldBounds) || IsCasterTooSmall(Radius, CascadeData[Index].WorldTexelSize, MinTexels))
					{
						continue;
					}
					const uint32 Lod = SelectShadowLod(Instance.Lod, LodCount, LodBias[Index], Instance.bFixedLod); // 스킨 포함
					((bStatic || Action == ECacheAction::Direct) ? Chunk.Static[Index] : Chunk.Dynamic[Index])
						.push_back({ KeyBase | (Lod & 0xFu), 0.0f, InstanceIndex });
				}
			}
			// 조각 안에서 미리 정렬 (합치기는 3단계)
			for (uint32 Index = 0; Index < ActiveCascades; ++Index)
			{
				InstanceBatching::SortFrontToBack(Chunk.Static[Index]);
				InstanceBatching::SortFrontToBack(Chunk.Dynamic[Index]);
			}
		}
	});

	// ---- 3) 목록별 합치기·묶음 (병렬, 캐스케이드 × 정적/동적 목록마다 독립): 조각마다 정렬된 목록을 합친다 — 결과는 전체를 한 번에
	//   정렬한 것과 같다 (InstanceBatching::MergeSortedLists)
	FParallel::ParallelFor(ActiveCascades * 2, 1, [this, ChunkCount](uint32 Begin, uint32 End) {
		for (uint32 ListIndex = Begin; ListIndex < End; ++ListIndex)
		{
			const uint32       Index   = ListIndex / 2;
			const bool         bStatic = (ListIndex % 2) == 0;
			const ECacheAction Action  = CascadeActions[Index];
			FMeshPassBatches&  Batches = bStatic ? StaticBatches[Index] : DynamicBatches[Index];
			Batches.Reset();
			if (bStatic ? Action == ECacheAction::Reuse : Action == ECacheAction::Direct)
			{
				continue; // 이 행동에서는 쓰지 않는 목록 (비어 있음)
			}
			const std::vector<FInstanceSortItem>* Lists[MaxCasterChunks];
			uint32                                ListCount = 0;
			for (uint32 ChunkIndex = 0; ChunkIndex < ChunkCount; ++ChunkIndex)
			{
				Lists[ListCount++] = bStatic ? &CasterChunks[ChunkIndex].Static[Index] : &CasterChunks[ChunkIndex].Dynamic[Index];
			}
			Batches.BuildMerged(Lists, ListCount);
		}
	});

	// ---- 4) 업로드 (메인 스레드, 캐스케이드 순서) + 섀도우 맵 장이 지난 프레임부터 같은 캐시 내용(동적 캐스터 없음)이면 복사도 건너뛴다
	for (uint32 Index = 0; Index < ActiveCascades; ++Index)
	{
		const ECacheAction Action = CascadeActions[Index];
		if (Action != ECacheAction::Reuse)
		{
			StaticBatches[Index].Upload(DynamicBuffer);
		}
		if (Action != ECacheAction::Direct)
		{
			DynamicBatches[Index].Upload(DynamicBuffer);
		}
		const bool bCleanNow  = Action != ECacheAction::Direct && DynamicBatches[Index].IsEmpty() && (bExtraStatic || !ExtraCasters) && !bExtraDynamic;
		bSkipCopy[Index]      = bCleanNow && bMapSliceClean[Index] && MapSliceKey[Index] == CacheStates[Index].CachedKey;
		bMapSliceClean[Index] = bCleanNow;
		MapSliceKey[Index]    = CacheStates[Index].CachedKey;
	}
}

void FShadowRenderer::AddPass(FRenderGraph& Graph, FRGResourceRef ShadowMapRef, const FMeshInstanceList& Instances, const FSkinDrawSource& SkinSource,
                              int32 Timer)
{
	const D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes = SkinSource.Address;
	E_CHECKF(Rhi != nullptr, "섀도우 렌더러가 초기화되지 않았습니다");
	DrawCalls    = 0;
	Triangles    = 0;
	CacheReused  = 0;
	CacheRebuilt = 0;
	if (ActiveCascades == 0 || !ShadowMap || !ShadowMapRef.IsValid())
	{
		return;
	}
	PrepareBatches(Instances, Rhi->GetDynamicBuffer());

	for (uint32 Index = 0; Index < ActiveCascades; ++Index)
	{
		CacheRebuilt += CascadeActions[Index] == ShadowCacheMath::ECacheAction::Rebuild ? 1u : 0u;
		CacheReused += CascadeActions[Index] == ShadowCacheMath::ECacheAction::Reuse ? 1u : 0u;
	}
	const bool bAnyCached = CacheRebuilt + CacheReused > 0;
	if (bAnyCached)
	{
		// 캐시(평소 COPY_SOURCE): 다시 그릴 장만 깊이 쓰기 → 캐시를 쓰는 캐스케이드 장을 섀도우 맵으로 복사
		const FRGResourceRef CacheRef = Graph.Import("CascadedShadowCache", CacheMap.Get(), ERGAccess::CopySource, ERGAccess::CopySource, 1, MapCascades);
		if (CacheRebuilt > 0)
		{
			FRenderGraph::FPassBuilder Pass = Graph.AddPass("방향광 그림자 캐시");
			for (uint32 Index = 0; Index < ActiveCascades; ++Index)
			{
				if (CascadeActions[Index] == ShadowCacheMath::ECacheAction::Rebuild)
				{
					Pass.Write(CacheRef, ERGAccess::DepthWrite, FRGSubresourceRange::Slice(Index), true);
				}
			}
			SkinSource.DeclareRead(Pass);
			Pass.Timer(Timer).Execute([this, &Instances, SkinPalettes](FRGContext& Context) { RecordCache(Context.CommandList, Instances, SkinPalettes); });
		}
		bool bAnyCopy = false;
		for (uint32 Index = 0; Index < ActiveCascades; ++Index)
		{
			bAnyCopy |= CascadeActions[Index] != ShadowCacheMath::ECacheAction::Direct && !bSkipCopy[Index];
		}
		if (bAnyCopy)
		{
			FRenderGraph::FPassBuilder Copy = Graph.AddPass("방향광 그림자 캐시 복사");
			for (uint32 Index = 0; Index < ActiveCascades; ++Index)
			{
				if (CascadeActions[Index] != ShadowCacheMath::ECacheAction::Direct && !bSkipCopy[Index])
				{
					Copy.Read(CacheRef, ERGAccess::CopySource, FRGSubresourceRange::Slice(Index));
					Copy.Write(ShadowMapRef, ERGAccess::CopyDest, FRGSubresourceRange::Slice(Index), true);
				}
			}
			Copy.Timer(Timer).Execute([this](FRGContext& Context) { RecordCopy(Context.CommandList); });
		}
	}
	// 캐시 없는 캐스케이드는 지우고 모두 그리고, 캐시 캐스케이드는 복사된(또는 이미 같은) 정적 깊이 위에 동적 캐스터만
	// (캐시 캐스케이드가 있으면 이전 내용을 읽는 쓰기 — 덮어쓰기 아님. 쓰지 않는 장은 셰이더가 읽지 않는다)
	FRenderGraph::FPassBuilder Pass = Graph.AddPass("방향광 그림자");
	Pass.Write(ShadowMapRef, ERGAccess::DepthWrite, FRGSubresourceRange::All(), !bAnyCached);
	SkinSource.DeclareRead(Pass);
	Pass.Timer(Timer)
		.Execute([this, &Instances, SkinPalettes](FRGContext& Context) { Record(Context.CommandList, Instances, SkinPalettes); });
}

void FShadowRenderer::BindDepthPass(ID3D12GraphicsCommandList* CommandList, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes,
                                    FDepthPassBindings& OutBindings)
{
	const uint32         Resolution = MapResolution;
	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Resolution), static_cast<float>(Resolution), 0.0f, 1.0f };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Resolution), static_cast<LONG>(Resolution) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(Pipelines[0].Get());
	CommandList->SetGraphicsRootShaderResourceView(ShadowParam_Instances, Instances.GetGpuData());
	CommandList->SetGraphicsRootShaderResourceView(ShadowParam_SkinPalette, SkinPalettes);

	for (uint32 Variant = 0; Variant < DepthVariantCount; ++Variant)
	{
		OutBindings.Pipelines[Variant] = Pipelines[Variant].Get();
	}
	OutBindings.InstanceRootIndex    = ShadowParam_PassConstants;
	OutBindings.InstanceDestOffset   = 16;
	OutBindings.MaskRootIndex        = ShadowParam_MaskConstants;
	OutBindings.MaskTextureRoot      = ShadowParam_MaskTexture;
	OutBindings.MaterialPipelines    = &MaterialPipelines;
	OutBindings.DynamicBuffer        = &Rhi->GetDynamicBuffer();
	OutBindings.MaterialConstantRoot = ShadowParam_MaterialConstants;
	OutBindings.MaterialTextureRoot  = ShadowParam_MaterialTextures;
}

void FShadowRenderer::DrawBatches(ID3D12GraphicsCommandList* CommandList, const FMeshPassBatches& CascadeBatches, uint32 Cascade,
                                  const FMeshInstanceList& Instances, const FDepthPassBindings& Bindings)
{
	if (CascadeBatches.IsEmpty())
	{
		return;
	}
	CommandList->SetGraphicsRoot32BitConstants(ShadowParam_PassConstants, 16, &CascadeData[Cascade].ViewProjection.M[0][0], 0);
	CommandList->SetGraphicsRootShaderResourceView(ShadowParam_InstanceIndices, CascadeBatches.GetIndexBuffer());
	DrawDepthBatches(CommandList, CascadeBatches, Instances, Bindings, DrawCalls, Triangles);
}

void FShadowRenderer::RecordCache(ID3D12GraphicsCommandList* CommandList, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes)
{
	// 다시 그릴 캐스케이드: 캐시 장을 지우고 정적 캐스터만 (그래프가 해당 장을 DEPTH_WRITE로 전이)
	FDepthPassBindings Bindings;
	BindDepthPass(CommandList, Instances, SkinPalettes, Bindings);
	for (uint32 Index = 0; Index < ActiveCascades; ++Index)
	{
		if (CascadeActions[Index] != ShadowCacheMath::ECacheAction::Rebuild)
		{
			continue;
		}
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = CacheDsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		CommandList->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		DrawBatches(CommandList, StaticBatches[Index], Index, Instances, Bindings);
	}
	// 정적 추가 캐스터 (지형): 자기 루트 시그니처/PSO를 묶으므로 메시를 모두 그린 뒤
	for (uint32 Index = 0; bExtraStatic && Index < ActiveCascades; ++Index)
	{
		if (CascadeActions[Index] == ShadowCacheMath::ECacheAction::Rebuild)
		{
			const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = CacheDsvHeap.GetCpuHandle(Index);
			CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
			ExtraCasters(CommandList, CascadeData[Index].ViewProjection, CascadeFrustums[Index], false);
		}
	}
}

void FShadowRenderer::RecordCopy(ID3D12GraphicsCommandList* CommandList)
{
	for (uint32 Index = 0; Index < ActiveCascades; ++Index)
	{
		if (CascadeActions[Index] == ShadowCacheMath::ECacheAction::Direct || bSkipCopy[Index])
		{
			continue;
		}
		D3D12_TEXTURE_COPY_LOCATION Destination{};
		Destination.pResource        = ShadowMap.Get();
		Destination.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		Destination.SubresourceIndex = Index; // 밉 1개 → 장 번호
		D3D12_TEXTURE_COPY_LOCATION Source = Destination;
		Source.pResource                   = CacheMap.Get();
		CommandList->CopyTextureRegion(&Destination, 0, 0, 0, &Source, nullptr); // 깊이는 서브리소스 전체 복사만
	}
}

void FShadowRenderer::Record(ID3D12GraphicsCommandList* CommandList, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes)
{
	// ---- 깊이 패스 (섀도우 맵은 그래프가 DEPTH_WRITE로 전이)
	FDepthPassBindings Bindings;
	BindDepthPass(CommandList, Instances, SkinPalettes, Bindings);
	for (uint32 Index = 0; Index < ActiveCascades; ++Index)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		if (CascadeActions[Index] == ShadowCacheMath::ECacheAction::Direct)
		{
			CommandList->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			DrawBatches(CommandList, StaticBatches[Index], Index, Instances, Bindings);
		}
		else
		{
			DrawBatches(CommandList, DynamicBatches[Index], Index, Instances, Bindings); // 정적 깊이는 캐시에서 복사됨
		}
	}
	// 추가 캐스터 (지형 등): 캐스케이드마다 DSV를 다시 바인딩해 그린다 (정적으로 캐시에 들어간 캐스케이드는 건너뜀)
	for (uint32 Index = 0; ExtraCasters && Index < ActiveCascades; ++Index)
	{
		if (bExtraStatic && CascadeActions[Index] != ShadowCacheMath::ECacheAction::Direct)
		{
			continue;
		}
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		ExtraCasters(CommandList, CascadeData[Index].ViewProjection, CascadeFrustums[Index], false);
	}
	// 동적 추가 캐스터 (움직이는 2D 스프라이트 등): 모든 캐스케이드에 매 프레임 (Direct면 정적 추가 캐스터와 함께 전부, 캐시면 복사된 정적 깊이 위에)
	for (uint32 Index = 0; bExtraDynamic && Index < ActiveCascades; ++Index)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		ExtraDynamicCasters(CommandList, CascadeData[Index].ViewProjection, CascadeFrustums[Index], false);
	}
}
