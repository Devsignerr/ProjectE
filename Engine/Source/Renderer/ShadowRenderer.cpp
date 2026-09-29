#include "Renderer/ShadowRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SkinnedMeshPalette.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr DXGI_FORMAT ShadowResourceFormat = DXGI_FORMAT_R32_TYPELESS;
	constexpr DXGI_FORMAT ShadowDsvFormat      = DXGI_FORMAT_D32_FLOAT;
	constexpr DXGI_FORMAT ShadowSrvFormat      = DXGI_FORMAT_R32_FLOAT;
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

	RootSignature.AddConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	RootSignature.AddConstantBufferView(4, 0, D3D12_SHADER_VISIBILITY_VERTEX); // 1: 스킨 팔레트 (SkinnedMesh.hlsli b4)
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
	                            L"ShadowRootSignature"))
	{
		return false;
	}
	if (!CreatePipeline(Pipeline, false) || !CreatePipeline(SkinnedPipeline, false, true))
	{
		return false;
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
	Pipeline.Shutdown();
	SkinnedPipeline.Shutdown();
	RootSignature.Shutdown();
	MapResolution = 0;
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
}

bool FShadowRenderer::CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, bool bSkinned)
{
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Shadow.hlsl";
	VertexDesc.EntryPoint = bSkinned ? L"ShadowSkinnedVS" : L"ShadowVS";
	VertexDesc.Stage      = EShaderStage::Vertex;
	if (bForceRecompile && !ShaderLibrary->CookShader(VertexDesc))
	{
		return false;
	}
	const ComPtr<IDxcBlob> VertexShader = ShaderLibrary->GetShader(VertexDesc);
	if (!VertexShader)
	{
		return false;
	}

	FGraphicsPipelineDesc Desc;
	Desc.RootSignature        = RootSignature.Get();
	Desc.VertexShader         = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	Desc.InputLayout          = bSkinned ? FStaticMesh::GetSkinnedInputLayout() : FStaticMesh::GetInputLayout(); // POSITION(+스킨)만 사용
	Desc.NumRenderTargets     = 0;
	Desc.DepthStencilFormat   = ShadowDsvFormat;
	Desc.bDepthEnable         = true;
	Desc.CullMode             = D3D12_CULL_MODE_NONE; // 한 면짜리 메시도 그림자를 드리운다
	Desc.bDepthClip           = false;                // 광원 근평면 뒤 캐스터를 근평면에 붙여 그린다 (팬케이킹)
	Desc.DepthBias            = BakedDepthBias;
	Desc.SlopeScaledDepthBias = BakedSlopeBias;
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, bSkinned ? L"ShadowSkinnedPipeline" : L"ShadowPipeline");
}

bool FShadowRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewPipeline;
	if (!CreatePipeline(NewPipeline, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "섀도우 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	Pipeline.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());

	FD3D12PipelineState NewSkinnedPipeline;
	if (!CreatePipeline(NewSkinnedPipeline, bForceRecompile, true))
	{
		E_LOG(LogRenderer, Error, "스킨 섀도우 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	SkinnedPipeline.Swap(NewSkinnedPipeline);
	Rhi->DeferRelease(NewSkinnedPipeline.Detach());
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

void FShadowRenderer::Render(FScene& Scene, FResourceManager& Resources, const FCamera& Camera, const FVector3& LightDirection,
                             const FShadowSettings& Settings, const FSkinnedMeshPalette* SkinPalettes)
{
	E_CHECKF(Rhi != nullptr, "섀도우 렌더러가 초기화되지 않았습니다");

	Constants               = FShadowConstants{};
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

	ShadowMath::FCascade Cascades[ShadowMath::MaxCascades];
	float                SliceNear = NearZ;
	for (uint32 Index = 0; Index < CascadeCount; ++Index)
	{
		const auto Corners = ShadowMath::ComputeFrustumSliceCorners(Camera.GetPosition(), Camera.GetForwardVector(), Camera.GetRightVector(),
		                                                            Camera.GetUpVector(), FMath::DegreesToRadians(Camera.GetFovYDegrees()),
		                                                            Camera.GetAspectRatio(), SliceNear, Splits[Index]);
		Cascades[Index] = ShadowMath::ComputeCascade(Corners, LightDirection, Resolution, Settings.CasterExtension);
		Constants.CascadeViewProjection[Index] = Cascades[Index].ViewProjection;
		Constants.CascadeSplits[Index]         = Splits[Index];
		Constants.CascadeTexelWorld[Index]     = Cascades[Index].WorldTexelSize;
		SliceNear                              = Splits[Index];
	}
	Constants.ShadowEnabled     = 1.0f;
	Constants.TexelSize         = 1.0f / static_cast<float>(Resolution);
	Constants.NormalOffset      = Settings.NormalOffset;
	Constants.CascadeCount      = CascadeCount;
	Constants.VisualizeCascades = Settings.bVisualizeCascades ? 1u : 0u;

	// ---- 깊이 패스
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();

	const D3D12_RESOURCE_BARRIER ToDepth =
		MakeTransitionBarrier(ShadowMap.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	CommandList->ResourceBarrier(1, &ToDepth);

	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Resolution), static_cast<float>(Resolution), 0.0f, 1.0f };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Resolution), static_cast<LONG>(Resolution) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(Pipeline.Get());

	FRegistry& Registry = Scene.GetRegistry();
	for (uint32 Index = 0; Index < CascadeCount; ++Index)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DsvHeap.GetCpuHandle(Index);
		CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
		CommandList->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		const FFrustum CascadeFrustum = FFrustum::FromViewProjection(Cascades[Index].ViewProjection);
		Registry.View<FTransformComponent, FStaticMeshComponent>().Each(
			[&](FEntity Entity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
				if (!MeshComponent.bVisible)
				{
					return;
				}
				// 스킨 메시는 아래에서 스킨 PSO로 따로 그린다
				if (SkinPalettes != nullptr && SkinPalettes->Find(Entity) != nullptr)
				{
					return;
				}
				const FStaticMesh* Mesh = Resources.GetMesh(MeshComponent.Mesh);
				if (Mesh == nullptr || !CascadeFrustum.Intersects(Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix)))
				{
					return;
				}
				const FMatrix4x4 WorldLightViewProjection = Transform.WorldMatrix * Cascades[Index].ViewProjection;
				CommandList->SetGraphicsRoot32BitConstants(0, 16, &WorldLightViewProjection.M[0][0], 0);
				Mesh->Draw(CommandList);
			});

		// 스킨 메시 캐스터: 팔레트가 바로 월드로 보내므로 상수는 캐스케이드 뷰-투영 그대로
		if (SkinPalettes != nullptr && SkinPalettes->GetCount() > 0)
		{
			bool bSkinnedPipelineBound = false;
			Registry.View<FSkinComponent, FStaticMeshComponent>().Each([&](FEntity Entity, FSkinComponent&, FStaticMeshComponent& MeshComponent) {
				const FSkinnedDrawInfo* Skinned = SkinPalettes->Find(Entity);
				const FStaticMesh*      Mesh    = Resources.GetMesh(MeshComponent.Mesh);
				if (!MeshComponent.bVisible || Skinned == nullptr || Mesh == nullptr || !CascadeFrustum.Intersects(Skinned->WorldBounds))
				{
					return;
				}
				if (!bSkinnedPipelineBound)
				{
					CommandList->SetPipelineState(SkinnedPipeline.Get());
					CommandList->SetGraphicsRoot32BitConstants(0, 16, &Cascades[Index].ViewProjection.M[0][0], 0);
					bSkinnedPipelineBound = true;
				}
				CommandList->SetGraphicsRootConstantBufferView(1, Skinned->Palette);
				Mesh->DrawSkinned(CommandList);
			});
			if (bSkinnedPipelineBound)
			{
				CommandList->SetPipelineState(Pipeline.Get());
			}
		}
	}

	const D3D12_RESOURCE_BARRIER ToShaderResource =
		MakeTransitionBarrier(ShadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	CommandList->ResourceBarrier(1, &ToShaderResource);
}
