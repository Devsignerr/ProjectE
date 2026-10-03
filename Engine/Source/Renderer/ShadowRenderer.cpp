#include "Renderer/ShadowRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/StaticMesh.h"

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
	Desc.InputLayout          = bSkinned ? FStaticMesh::GetSkinnedInputLayout() : FStaticMesh::GetInputLayout(); // POSITION(+UV/COLOR, 스킨)만 사용
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
}

void FShadowRenderer::PrepareCascades(const FCamera& Camera, const FVector3& LightDirection, const FShadowSettings& Settings)
{
	E_CHECKF(Rhi != nullptr, "섀도우 렌더러가 초기화되지 않았습니다");

	Constants               = FShadowConstants{};
	ActiveCascades          = 0;
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
		Cascades[Index] = ShadowMath::ComputeCascade(Corners, LightDirection, Resolution, Settings.CasterExtension);
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

void FShadowRenderer::AddPass(FRenderGraph& Graph, FRGResourceRef ShadowMapRef, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes,
                              int32 Timer)
{
	E_CHECKF(Rhi != nullptr, "섀도우 렌더러가 초기화되지 않았습니다");
	DrawCalls = 0;
	Triangles = 0;
	if (ActiveCascades == 0 || !ShadowMap || !ShadowMapRef.IsValid())
	{
		return;
	}
	// 캐스케이드마다 지우고 그리므로 덮어쓰기 (쓰지 않는 캐스케이드 장은 셰이더가 읽지 않는다)
	Graph.AddPass("방향광 그림자")
		.Write(ShadowMapRef, ERGAccess::DepthWrite, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, &Instances, SkinPalettes](FRGContext& Context) { Record(Context.CommandList, Instances, SkinPalettes); });
}

void FShadowRenderer::Record(ID3D12GraphicsCommandList* CommandList, const FMeshInstanceList& Instances, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes)
{
	const uint32                Resolution   = MapResolution;
	const uint32                CascadeCount = ActiveCascades;
	const ShadowMath::FCascade* Cascades     = CascadeData;

	// ---- 깊이 패스 (섀도우 맵은 그래프가 DEPTH_WRITE로 전이)
	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Resolution), static_cast<float>(Resolution), 0.0f, 1.0f };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Resolution), static_cast<LONG>(Resolution) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(Pipelines[0].Get());

	CommandList->SetGraphicsRootShaderResourceView(ShadowParam_Instances, Instances.GetGpuData());
	CommandList->SetGraphicsRootShaderResourceView(ShadowParam_SkinPalette, SkinPalettes);
	FD3D12DynamicUploadBuffer&        DynamicBuffer = Rhi->GetDynamicBuffer();
	const std::vector<FMeshInstance>& List          = Instances.GetInstances();
	FDepthPassBindings                Bindings;
	for (uint32 Variant = 0; Variant < DepthVariantCount; ++Variant)
	{
		Bindings.Pipelines[Variant] = Pipelines[Variant].Get();
	}
	Bindings.InstanceRootIndex  = ShadowParam_PassConstants;
	Bindings.InstanceDestOffset = 16;
	Bindings.MaskRootIndex      = ShadowParam_MaskConstants;
	Bindings.MaskTextureRoot    = ShadowParam_MaskTexture;
	Bindings.MaterialPipelines    = &MaterialPipelines;
	Bindings.DynamicBuffer        = &DynamicBuffer;
	Bindings.MaterialConstantRoot = ShadowParam_MaterialConstants;
	Bindings.MaterialTextureRoot  = ShadowParam_MaterialTextures;
	for (uint32 Index = 0; Index < CascadeCount; ++Index)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		CommandList->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		// 캐스케이드 프러스텀 컬링 → (정적/스킨 × 불투명/Masked)·메시·LOD별 묶음 (Masked만 머티리얼별), 스킨은 팔레트가 바로 월드로 보낸다
		// 반투명 머티리얼은 그림자를 드리우지 않는다
		const FFrustum& CascadeFrustum = CascadeFrustums[Index];
		Batches.Reset();
		for (uint32 InstanceIndex = 0; InstanceIndex < static_cast<uint32>(List.size()); ++InstanceIndex)
		{
			const FMeshInstance& Instance = List[InstanceIndex];
			if (Instance.CastsShadow() && CascadeFrustum.Intersects(Instance.WorldBounds))
			{
				Batches.Add(MakeDepthBatchKey(Instance), 0.0f, InstanceIndex);
			}
		}
		Batches.Finalize(DynamicBuffer);

		CommandList->SetGraphicsRoot32BitConstants(ShadowParam_PassConstants, 16, &Cascades[Index].ViewProjection.M[0][0], 0);
		CommandList->SetGraphicsRootShaderResourceView(ShadowParam_InstanceIndices, Batches.GetIndexBuffer());
		DrawDepthBatches(CommandList, Batches, Instances, Bindings, DrawCalls, Triangles);
	}
	// 추가 캐스터 (지형 등): 캐스케이드마다 DSV를 다시 바인딩해 그린다
	for (uint32 Index = 0; ExtraCasters && Index < CascadeCount; ++Index)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		ExtraCasters(CommandList, Cascades[Index].ViewProjection, CascadeFrustums[Index], false);
	}
}
