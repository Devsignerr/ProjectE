#include "Renderer/LocalLightRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/LightMath.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cstring>
#include <numeric>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr uint32 GCullGroupSize = 64; // ClusterCulling.hlsl numthreads

	constexpr DXGI_FORMAT ShadowResourceFormat = DXGI_FORMAT_R32_TYPELESS;
	constexpr DXGI_FORMAT ShadowDsvFormat      = DXGI_FORMAT_D32_FLOAT;
	constexpr DXGI_FORMAT ShadowSrvFormat      = DXGI_FORMAT_R32_FLOAT;

	enum EComputeRootParameter : uint32
	{
		ComputeParam_Constants = 0, // b0
		ComputeParam_Lights    = 1, // t0
		ComputeParam_Clusters  = 2, // u0
	};

	enum EShadowRootParameter : uint32
	{
		ShadowParam_PassConstants   = 0, // b0 (루트 상수 17개: 장 뷰-투영 + 인스턴스 시작 위치, Shadow.hlsl)
		ShadowParam_SkinPalette     = 1, // t15 (프레임 스킨 팔레트)
		ShadowParam_Instances       = 2, // t13
		ShadowParam_InstanceIndices = 3, // t14
		ShadowParam_MaskConstants   = 4, // b1 (Masked: 알파 팩터, 컷오프)
		ShadowParam_MaskTexture     = 5, // t0 (Masked: 베이스 컬러)
	};
} // namespace

FLocalLightRenderer::~FLocalLightRenderer()
{
	Shutdown();
}

bool FLocalLightRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary)
{
	E_CHECKF(Rhi == nullptr, "로컬 라이트 렌더러가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;

	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	const uint32 ConstantsIndex = ComputeRootSignature.AddConstantBufferView(0);
	const uint32 LightsIndex    = ComputeRootSignature.AddShaderResourceView(0);
	const uint32 ClustersIndex  = ComputeRootSignature.AddUnorderedAccessView(0);
	E_CHECK(ConstantsIndex == ComputeParam_Constants && LightsIndex == ComputeParam_Lights && ClustersIndex == ComputeParam_Clusters);
	if (!ComputeRootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ClusterCullingRootSignature"))
	{
		return false;
	}

	const uint32 PassIndex      = ShadowRootSignature.AddConstants(17, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 PaletteIndex   = ShadowRootSignature.AddShaderResourceView(15, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 InstancesIndex = ShadowRootSignature.AddShaderResourceView(13, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 IndicesIndex   = ShadowRootSignature.AddShaderResourceView(14, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 MaskIndex      = ShadowRootSignature.AddConstants(2, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 MaskTexture    = ShadowRootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) },
	                                                                     D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(PassIndex == ShadowParam_PassConstants && PaletteIndex == ShadowParam_SkinPalette && InstancesIndex == ShadowParam_Instances &&
	        IndicesIndex == ShadowParam_InstanceIndices && MaskIndex == ShadowParam_MaskConstants && MaskTexture == ShadowParam_MaskTexture);
	ShadowRootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR));
	if (!ShadowRootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"LocalShadowRootSignature"))
	{
		return false;
	}

	if (!CreateCullPipeline(CullPipeline, false) || !CreateShadowPipeline(ShadowPipelines[0], false, 0) ||
	    !CreateShadowPipeline(ShadowPipelines[1], false, 1) || !CreateShadowPipeline(ShadowPipelines[2], false, 2) ||
	    !CreateShadowPipeline(ShadowPipelines[3], false, 3))
	{
		return false;
	}

	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   Desc = MakeBufferDesc(sizeof(uint32) * static_cast<uint64>(LightMath::ClusterCount) * LightMath::ClusterStride,
	                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ClusterBuffer))))
	{
		E_LOG(LogRenderer, Error, "클러스터 라이트 버퍼를 만들지 못했습니다");
		return false;
	}
	ClusterBuffer->SetName(L"ClusterLightData");
	ClusterState = D3D12_RESOURCE_STATE_COMMON;

	// 셰이더가 항상 유효한 SRV를 참조하도록 작은 한 장짜리를 미리 만든다 (그림자 라이트가 나오면 다시 만든다)
	if (!EnsureShadowMap(1, 1))
	{
		return false;
	}

	Lights.reserve(64);
	return true;
}

void FLocalLightRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	// 호출자(씬 렌더러)가 GPU Flush 이후 종료한다
	if (ShadowSrv.IsValid())
	{
		Rhi->GetSrvAllocator().Free(ShadowSrv);
		ShadowSrv = FD3D12DescriptorHandle{};
	}
	ShadowMap.Reset();
	ShadowDsvHeap.Shutdown();
	ShadowMapResolution = 0;
	ShadowMapSlices     = 0;
	ClusterBuffer.Reset();
	CullPipeline.Shutdown();
	for (FD3D12PipelineState& Pipeline : ShadowPipelines)
	{
		Pipeline.Shutdown();
	}
	ComputeRootSignature.Shutdown();
	ShadowRootSignature.Shutdown();
	Lights.clear();
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
}

bool FLocalLightRenderer::CreateCullPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile)
{
	FShaderCompileDesc Desc;
	Desc.FileName   = L"ClusterCulling.hlsl";
	Desc.EntryPoint = L"CSMain";
	Desc.Stage      = EShaderStage::Compute;
	if (bForceRecompile && !ShaderLibrary->CookShader(Desc))
	{
		return false;
	}
	const ComPtr<IDxcBlob> Shader = ShaderLibrary->GetShader(Desc);
	if (!Shader)
	{
		return false;
	}
	return OutPipeline.InitCompute(Rhi->GetDevice().GetDevice(), ComputeRootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()),
	                               L"ClusterCullingPipeline");
}

bool FLocalLightRenderer::CreateShadowPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, uint32 Variant)
{
	const bool         bSkinned = (Variant & DepthVariantSkinned) != 0;
	const bool         bMasked  = (Variant & DepthVariantMasked) != 0;
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Shadow.hlsl";
	VertexDesc.EntryPoint = bMasked ? (bSkinned ? L"ShadowSkinnedMaskedVS" : L"ShadowMaskedVS") : (bSkinned ? L"ShadowSkinnedVS" : L"ShadowVS");
	VertexDesc.Stage      = EShaderStage::Vertex;
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
	Desc.RootSignature        = ShadowRootSignature.Get();
	Desc.VertexShader         = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	if (bMasked)
	{
		Desc.PixelShader = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	}
	Desc.InputLayout          = bSkinned ? FStaticMesh::GetSkinnedInputLayout() : FStaticMesh::GetInputLayout();
	Desc.NumRenderTargets     = 0;
	Desc.DepthStencilFormat   = ShadowDsvFormat;
	Desc.bDepthEnable         = true;
	Desc.CullMode             = D3D12_CULL_MODE_NONE; // 한 면짜리 메시도 그림자를 드리운다
	Desc.bDepthClip           = true;                 // 원근: 광원 뒤 지오메트리는 잘라야 한다
	Desc.DepthBias            = BakedDepthBias;
	Desc.SlopeScaledDepthBias = BakedSlopeBias;
	static const wchar_t* const Names[DepthVariantCount] = { L"LocalShadowPipeline", L"LocalShadowSkinnedPipeline", L"LocalShadowMaskedPipeline",
	                                                         L"LocalShadowSkinnedMaskedPipeline" };
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, Names[Variant]);
}

bool FLocalLightRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewCull;
	FD3D12PipelineState NewShadow[DepthVariantCount];
	bool                bOk = CreateCullPipeline(NewCull, bForceRecompile);
	for (uint32 Variant = 0; bOk && Variant < DepthVariantCount; ++Variant)
	{
		bOk = CreateShadowPipeline(NewShadow[Variant], bForceRecompile, Variant);
	}
	if (!bOk)
	{
		E_LOG(LogRenderer, Error, "로컬 라이트 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	CullPipeline.Swap(NewCull);
	Rhi->DeferRelease(NewCull.Detach());
	for (uint32 Variant = 0; Variant < DepthVariantCount; ++Variant)
	{
		ShadowPipelines[Variant].Swap(NewShadow[Variant]);
		Rhi->DeferRelease(NewShadow[Variant].Detach());
	}
	return true;
}

D3D12_GPU_VIRTUAL_ADDRESS FLocalLightRenderer::GetClusterData() const
{
	return ClusterBuffer ? ClusterBuffer->GetGPUVirtualAddress() : 0;
}

FRGResourceRef FLocalLightRenderer::ImportShadowMap(FRenderGraph& Graph) const
{
	return ShadowMap ? Graph.Import("LocalShadowMap", ShadowMap.Get(), ERGAccess::SrvPixel, ERGAccess::SrvPixel, 1, ShadowMapSlices) : FRGResourceRef{};
}

FRGResourceRef FLocalLightRenderer::ImportClusters(FRenderGraph& Graph)
{
	return Graph.ImportTracked("ClusterBuffer", ClusterBuffer.Get(), &ClusterState);
}

bool FLocalLightRenderer::EnsureShadowMap(uint32 Resolution, uint32 Slices)
{
	if (ShadowMap && ShadowMapResolution == Resolution && ShadowMapSlices >= Slices)
	{
		return true;
	}
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	ComPtr<ID3D12Resource> NewShadowMap;
	FD3D12DescriptorHeap   NewDsvHeap;

	D3D12_RESOURCE_DESC Desc = MakeTexture2DDesc(Resolution, Resolution, ShadowResourceFormat, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	Desc.DepthOrArraySize    = static_cast<UINT16>(Slices);

	D3D12_CLEAR_VALUE ClearValue{};
	ClearValue.Format             = ShadowDsvFormat;
	ClearValue.DepthStencil.Depth = 1.0f;

	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
	                                           IID_PPV_ARGS(&NewShadowMap))))
	{
		E_LOG(LogRenderer, Error, "로컬 그림자 맵 생성 실패 ({}x{} x {})", Resolution, Resolution, Slices);
		return false;
	}
	NewShadowMap->SetName(L"LocalShadowMap");

	if (!NewDsvHeap.Init(Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, Slices, false, L"LocalShadowDsvHeap"))
	{
		return false;
	}
	for (uint32 Index = 0; Index < Slices; ++Index)
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
	SrvDesc.Texture2DArray.ArraySize = Slices;
	const FD3D12DescriptorHandle NewSrv = Rhi->GetSrvAllocator().Allocate();
	Device->CreateShaderResourceView(NewShadowMap.Get(), &SrvDesc, NewSrv.Cpu);

	// 새 리소스가 모두 준비된 뒤 기존 리소스를 지연 해제한다
	ReleaseShadowMap();
	ShadowMap           = std::move(NewShadowMap);
	ShadowDsvHeap       = std::move(NewDsvHeap);
	ShadowSrv           = NewSrv;
	ShadowMapResolution = Resolution;
	ShadowMapSlices     = Slices;
	if (Slices > 1)
	{
		E_LOG(LogRenderer, Log, "로컬 그림자 맵 생성: {}x{} x {}장", Resolution, Resolution, Slices);
	}
	return true;
}

void FLocalLightRenderer::ReleaseShadowMap()
{
	if (!ShadowMap)
	{
		return;
	}
	// 진행 중인 프레임이 참조할 수 있으므로 지연 해제
	Rhi->DeferRelease(ShadowMap);
	Rhi->DeferFreeDescriptor(ShadowSrv);
	ShadowSrv = FD3D12DescriptorHandle{};
	ShadowMap.Reset();
	ShadowDsvHeap.Shutdown(); // DSV 힙은 기록 시점에만 읽히므로 즉시 해제
	ShadowMapResolution = 0;
	ShadowMapSlices     = 0;
}

void FLocalLightRenderer::CollectLights(FScene& Scene, const FCamera& Camera)
{
	Lights.clear();
	LightScores.clear();
	LightOuterAngles.clear();
	LightWantsShadow.clear();

	const FFrustum Frustum        = FFrustum::FromViewProjection(Camera.GetViewProjectionMatrix());
	const FVector3 CameraPosition = Camera.GetPosition();
	FRegistry&     Registry       = Scene.GetRegistry();

	auto AddLight = [&](const FVector3& Position, const FVector3& SrgbColor, float Intensity, float Radius, bool bShadow) -> FLocalLightGpuData* {
		if (Intensity <= 0.0f || Radius <= 0.0f)
		{
			return nullptr;
		}
		if (!Frustum.Intersects(FBox(Position - FVector3(Radius), Position + FVector3(Radius))))
		{
			return nullptr;
		}
		FLocalLightGpuData& Light = Lights.emplace_back();
		Light.Position            = Position;
		Light.Radius              = Radius;
		Light.Color               = LightMath::SrgbToLinear(SrgbColor) * Intensity;
		LightScores.push_back(FMath::Max(FVector3::Distance(Position, CameraPosition) - Radius, 0.0f));
		LightOuterAngles.push_back(0.0f);
		LightWantsShadow.push_back(bShadow ? 1 : 0);
		return &Light;
	};

	Registry.View<FTransformComponent, FPointLightComponent>().Each([&](FEntity, FTransformComponent& Transform, FPointLightComponent& Point) {
		if (FLocalLightGpuData* Light = AddLight(Transform.GetWorldPosition(), Point.Color, Point.Intensity, Point.Radius, Point.bCastShadows))
		{
			Light->Type = static_cast<uint32>(LightMath::ELocalLightType::Point);
		}
	});
	Registry.View<FTransformComponent, FSpotLightComponent>().Each([&](FEntity, FTransformComponent& Transform, FSpotLightComponent& Spot) {
		if (FLocalLightGpuData* Light = AddLight(Transform.GetWorldPosition(), Spot.Color, Spot.Intensity, Spot.Radius, Spot.bCastShadows))
		{
			const LightMath::FConeParams Cone = LightMath::ComputeConeParams(Spot.InnerConeAngle, Spot.OuterConeAngle);
			Light->Type            = static_cast<uint32>(LightMath::ELocalLightType::Spot);
			Light->Direction       = Transform.GetWorldForward();
			Light->ConeScale       = Cone.Scale;
			Light->ConeOffset      = Cone.Offset;
			LightOuterAngles.back() = Spot.OuterConeAngle;
		}
	});

	// 상한을 넘으면 카메라에 가까운(영향 구 기준) 라이트만 남긴다
	if (Lights.size() > LightMath::MaxLocalLights)
	{
		std::vector<uint32> Order(Lights.size());
		std::iota(Order.begin(), Order.end(), 0u);
		std::nth_element(Order.begin(), Order.begin() + LightMath::MaxLocalLights, Order.end(),
		                 [&](uint32 A, uint32 B) { return LightScores[A] < LightScores[B]; });
		Order.resize(LightMath::MaxLocalLights);
		auto Keep = [&Order](auto& Values) {
			std::remove_reference_t<decltype(Values)> Kept;
			Kept.reserve(Order.size());
			for (const uint32 Index : Order)
			{
				Kept.push_back(Values[Index]);
			}
			Values.swap(Kept);
		};
		Keep(Lights);
		Keep(LightScores);
		Keep(LightOuterAngles);
		Keep(LightWantsShadow);
	}
}

void FLocalLightRenderer::AssignShadows(const FLocalShadowSettings& Settings)
{
	ShadowSlices.clear();
	ShadowMatrices.clear();
	if (!Settings.bEnabled)
	{
		return;
	}

	// 카메라에 가까운 그림자 라이트부터 장을 배정 (스포트 1장, 점광원 6장). 모자라면 그 라이트는 그림자 없이
	std::vector<uint32> Candidates;
	for (uint32 Index = 0; Index < Lights.size(); ++Index)
	{
		if (LightWantsShadow[Index] != 0)
		{
			Candidates.push_back(Index);
		}
	}
	std::sort(Candidates.begin(), Candidates.end(), [&](uint32 A, uint32 B) { return LightScores[A] < LightScores[B]; });

	const uint32 Resolution = FMath::Clamp<uint32>(Settings.Resolution, 64, 4096);
	const uint32 MaxSlices  = FMath::Clamp<uint32>(Settings.MaxSlices, 1, 2048);
	const float  NearZ      = FMath::Max(Settings.NearZ, 0.1f);
	for (const uint32 Index : Candidates)
	{
		FLocalLightGpuData& Light  = Lights[Index];
		const bool          bPoint = Light.Type == static_cast<uint32>(LightMath::ELocalLightType::Point);
		const uint32        Needed = bPoint ? LightMath::CubeFaceCount : 1u;
		if (ShadowSlices.size() + Needed > MaxSlices)
		{
			continue; // 더 작은 스포트는 들어갈 수 있다
		}
		Light.ShadowIndex = static_cast<int32>(ShadowSlices.size());
		if (bPoint)
		{
			Light.ShadowTexelFactor = LightMath::ShadowTexelWorldFactor(LightMath::CubeFaceTanHalfFov(Resolution), Resolution);
			for (uint32 Face = 0; Face < LightMath::CubeFaceCount; ++Face)
			{
				ShadowSlices.push_back({ LightMath::ComputeCubeFaceViewProjection(Light.Position, Face, Light.Radius, NearZ, Resolution),
				                         Light.Position, Light.Radius });
			}
		}
		else
		{
			const float Outer = FMath::Clamp(LightOuterAngles[Index], 1.0f, LightMath::MaxSpotConeAngle);
			const float Tan   = FMath::Tan(FMath::DegreesToRadians(Outer)) * LightMath::CubeFaceTanHalfFov(Resolution);
			Light.ShadowTexelFactor = LightMath::ShadowTexelWorldFactor(Tan, Resolution);
			ShadowSlices.push_back({ LightMath::ComputeSpotViewProjection(Light.Position, Light.Direction, Outer, Light.Radius, NearZ, Resolution),
			                         Light.Position, Light.Radius });
		}
	}
	for (FShadowSlice& Slice : ShadowSlices)
	{
		Slice.Frustum     = FFrustum::FromViewProjection(Slice.ViewProjection);
		Slice.LightBounds = FBox(Slice.LightPosition - FVector3(Slice.Radius), Slice.LightPosition + FVector3(Slice.Radius));
		ShadowMatrices.push_back(Slice.ViewProjection);
	}
}

bool FLocalLightRenderer::IntersectsShadowCaster(const FBox& WorldBounds) const
{
	for (const FShadowSlice& Slice : ShadowSlices)
	{
		if (Slice.IsCaster(WorldBounds))
		{
			return true;
		}
	}
	return false;
}

void FLocalLightRenderer::RecordShadows(ID3D12GraphicsCommandList* CommandList, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes)
{
	const uint32 Resolution = ShadowMapResolution;

	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Resolution), static_cast<float>(Resolution), 0.0f, 1.0f };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Resolution), static_cast<LONG>(Resolution) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);
	CommandList->SetGraphicsRootSignature(ShadowRootSignature.Get());
	CommandList->SetPipelineState(ShadowPipelines[0].Get());

	CommandList->SetGraphicsRootShaderResourceView(ShadowParam_Instances, Instances.GetGpuData());
	CommandList->SetGraphicsRootShaderResourceView(ShadowParam_SkinPalette, SkinPalettes);
	FD3D12DynamicUploadBuffer&        DynamicBuffer = Rhi->GetDynamicBuffer();
	const std::vector<FMeshInstance>& List          = Instances.GetInstances();
	FDepthPassBindings                Bindings;
	for (uint32 Variant = 0; Variant < DepthVariantCount; ++Variant)
	{
		Bindings.Pipelines[Variant] = ShadowPipelines[Variant].Get();
	}
	Bindings.InstanceRootIndex  = ShadowParam_PassConstants;
	Bindings.InstanceDestOffset = 16;
	Bindings.MaskRootIndex      = ShadowParam_MaskConstants;
	Bindings.MaskTextureRoot    = ShadowParam_MaskTexture;
	for (uint32 Index = 0; Index < ShadowSlices.size(); ++Index)
	{
		const FShadowSlice&               Slice = ShadowSlices[Index];
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv   = ShadowDsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		CommandList->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		// (정적/스킨 × 불투명/Masked)·메시·LOD별 묶음 (Masked만 머티리얼별, 반투명 제외), 스킨은 팔레트가 바로 월드로 보내므로 상수는 뷰-투영 그대로
		ShadowBatches.Reset();
		for (uint32 InstanceIndex = 0; InstanceIndex < static_cast<uint32>(List.size()); ++InstanceIndex)
		{
			const FMeshInstance& Instance = List[InstanceIndex];
			if (Instance.CastsShadow() && Slice.IsCaster(Instance.WorldBounds))
			{
				ShadowBatches.Add(MakeDepthBatchKey(Instance), 0.0f, InstanceIndex);
			}
		}
		ShadowBatches.Finalize(DynamicBuffer);

		CommandList->SetGraphicsRoot32BitConstants(ShadowParam_PassConstants, 16, &Slice.ViewProjection.M[0][0], 0);
		CommandList->SetGraphicsRootShaderResourceView(ShadowParam_InstanceIndices, ShadowBatches.GetIndexBuffer());
		DrawDepthBatches(CommandList, ShadowBatches, Instances, Bindings, ShadowDrawCalls, ShadowTriangles);
	}
	// 추가 캐스터 (지형 등): 장마다 DSV를 다시 바인딩해 그린다
	for (uint32 Index = 0; ExtraCasters && Index < ShadowSlices.size(); ++Index)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = ShadowDsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		ExtraCasters(CommandList, ShadowSlices[Index].ViewProjection, ShadowSlices[Index].Frustum, true);
	}
}

void FLocalLightRenderer::PrepareLights(FScene& Scene, const FCamera& Camera, const FLocalShadowSettings& ShadowSettings)
{
	E_CHECKF(Rhi != nullptr, "로컬 라이트 렌더러가 초기화되지 않았습니다");

	CollectLights(Scene, Camera);

	// ---- 그림자: 바이어스는 PSO에 고정되므로 바뀌면 재생성, 타일 배열은 필요한 만큼 (8장 단위로 키운다)
	if (ShadowSettings.DepthBias != BakedDepthBias || ShadowSettings.SlopeBias != BakedSlopeBias)
	{
		BakedDepthBias = ShadowSettings.DepthBias;
		BakedSlopeBias = ShadowSettings.SlopeBias;
		ReloadShaders(false);
	}
	AssignShadows(ShadowSettings);
	if (!ShadowSlices.empty())
	{
		const uint32 Resolution = FMath::Clamp<uint32>(ShadowSettings.Resolution, 64, 4096);
		const uint32 Needed     = static_cast<uint32>(ShadowSlices.size());
		const uint32 Capacity   = FMath::Max(ShadowMapResolution == Resolution ? ShadowMapSlices : 0u, (Needed + 7u) / 8u * 8u);
		if (!EnsureShadowMap(Resolution, Capacity))
		{
			// 만들지 못하면 이번 프레임은 그림자 없이
			ShadowSlices.clear();
			ShadowMatrices.clear();
			for (FLocalLightGpuData& Light : Lights)
			{
				Light.ShadowIndex = -1;
			}
			EnsureShadowMap(1, 1);
		}
	}
}

void FLocalLightRenderer::PrepareFrame(const FCamera& Camera, uint32 Width, uint32 Height, const FLocalShadowSettings& ShadowSettings)
{
	E_CHECKF(Rhi != nullptr, "로컬 라이트 렌더러가 초기화되지 않았습니다");

	// ---- 상수
	const float NearZ = Camera.GetNearZ();
	const float FarZ  = FMath::Max(Camera.GetFarZ(), NearZ * 2.0f);
	const LightMath::FSliceParams Slices = LightMath::ComputeSliceParams(NearZ, FarZ, LightMath::ClusterGridZ);

	Constants                    = FClusterConstants{};
	Constants.View               = Camera.GetViewMatrix();
	Constants.GridX              = LightMath::ClusterGridX;
	Constants.GridY              = LightMath::ClusterGridY;
	Constants.GridZ              = LightMath::ClusterGridZ;
	Constants.LightCount         = static_cast<uint32>(Lights.size());
	Constants.ScreenSize         = FVector2(static_cast<float>(FMath::Max<uint32>(Width, 1)), static_cast<float>(FMath::Max<uint32>(Height, 1)));
	Constants.SliceScale         = Slices.Scale;
	Constants.SliceBias          = Slices.Bias;
	Constants.NearZ              = NearZ;
	Constants.FarZ               = FarZ;
	Constants.bOrthographic      = Camera.IsOrthographic() ? 1u : 0u;
	Constants.ShadowNormalOffset = ShadowSettings.NormalOffset;
	Constants.ShadowTexelSize    = 1.0f / static_cast<float>(FMath::Max<uint32>(ShadowMapResolution, 1));
	if (Camera.IsOrthographic())
	{
		Constants.ProjScaleY = Camera.GetOrthoHeight() * 0.5f;
		Constants.ProjScaleX = Constants.ProjScaleY * Camera.GetAspectRatio();
	}
	else
	{
		Constants.ProjScaleY = FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f);
		Constants.ProjScaleX = Constants.ProjScaleY * Camera.GetAspectRatio();
	}

	// ---- 업로드 (빈 목록이어도 루트 SRV가 유효한 주소를 가리키게 한 칸은 잡는다)
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	auto Upload = [&DynamicBuffer](const void* Data, size_t Bytes, size_t MinBytes) {
		const size_t                  Size       = FMath::Max(Bytes, MinBytes);
		const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(Size, 16);
		std::memset(Allocation.CpuAddress, 0, Size);
		if (Bytes > 0)
		{
			std::memcpy(Allocation.CpuAddress, Data, Bytes);
		}
		return Allocation.GpuAddress;
	};
	LightListAddress      = Upload(Lights.data(), sizeof(FLocalLightGpuData) * Lights.size(), sizeof(FLocalLightGpuData));
	ShadowMatricesAddress = Upload(ShadowMatrices.data(), sizeof(FMatrix4x4) * ShadowMatrices.size(), sizeof(FMatrix4x4));
	ConstantsAddress      = DynamicBuffer.AllocateConstants(Constants).GpuAddress;
}

void FLocalLightRenderer::AddPasses(FRenderGraph& Graph, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes,
                                    ID3D12RootSignature* BreakRootSignature, int32 Timer)
{
	ShadowDrawCalls = 0;
	ShadowTriangles = 0;
	if (!ShadowSlices.empty())
	{
		// 장마다 지우고 그린다 (쓰지 않는 장은 셰이더가 읽지 않음)
		Graph.AddPass("로컬 그림자")
			.Write(ImportShadowMap(Graph), ERGAccess::DepthWrite, FRGSubresourceRange::All(), true)
			.Timer(Timer)
			.Execute([this, &Instances, SkinPalettes](FRGContext& Context) { RecordShadows(Context.CommandList, Instances, SkinPalettes); });
	}

	// ---- 클러스터 컬링 (전체 클러스터를 다시 쓴다)
	Graph.AddPass("클러스터 컬링")
		.Write(ImportClusters(Graph), ERGAccess::Uav, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, BreakRootSignature](FRGContext& Context) {
			ID3D12GraphicsCommandList* CommandList = Context.CommandList;
			if (BreakRootSignature != nullptr && !Context.bAsyncCompute)
			{
				CommandList->SetGraphicsRootSignature(BreakRootSignature);
			}
			CommandList->SetComputeRootSignature(ComputeRootSignature.Get());
			CommandList->SetPipelineState(CullPipeline.Get());
			CommandList->SetComputeRootConstantBufferView(ComputeParam_Constants, ConstantsAddress);
			CommandList->SetComputeRootShaderResourceView(ComputeParam_Lights, LightListAddress);
			CommandList->SetComputeRootUnorderedAccessView(ComputeParam_Clusters, ClusterBuffer->GetGPUVirtualAddress());
			CommandList->Dispatch((LightMath::ClusterCount + GCullGroupSize - 1) / GCullGroupSize, 1, 1);
		});
}
