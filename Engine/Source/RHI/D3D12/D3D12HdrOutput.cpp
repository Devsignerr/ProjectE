#include "RHI/D3D12/D3D12HdrOutput.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"

namespace
{
	struct FCompositeConstants
	{
		uint32 Mode           = 0;
		float  PaperWhiteNits = 200.0f;
		float  Padding[2]     = {};
	};
	static_assert(sizeof(FCompositeConstants) == 16);

	DXGI_FORMAT GetSwapChainFormat(EHdrSwapChainMode Mode)
	{
		return Mode == EHdrSwapChainMode::Hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
} // namespace

FD3D12HdrOutput::~FD3D12HdrOutput()
{
	Shutdown();
}

bool FD3D12HdrOutput::CreatePipelines(FD3D12RHI& Rhi)
{
	ID3D12Device* Device = Rhi.GetDevice().GetDevice();
	if (!Compiler)
	{
		Compiler = std::make_unique<FD3D12ShaderCompiler>();
		Library  = std::make_unique<FShaderLibrary>();
		if (!Compiler->Init() || !Library->Init(*Compiler))
		{
			E_LOG(LogD3D12, Error, "HDR 합성 셰이더 컴파일러 초기화 실패");
			Library.reset();
			Compiler.reset();
			return false;
		}
		RootSignature.AddConstants(4, 0);
		RootSignature.AddDescriptorTable(
			{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) }, D3D12_SHADER_VISIBILITY_PIXEL);
		RootSignature.AddDescriptorTable(
			{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) }, D3D12_SHADER_VISIBILITY_PIXEL);
		if (!RootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"HdrCompositeRoot"))
		{
			return false;
		}
	}
	const DXGI_FORMAT Format = GetSwapChainFormat(Mode);
	if (Pipelines[0].IsInitialized() && PipelineFormat == Format)
	{
		return true;
	}
	FShaderCompileDesc Vs;
	Vs.FileName   = L"HdrComposite.hlsl";
	Vs.EntryPoint = L"VSMain";
	Vs.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc Ps = Vs;
	Ps.EntryPoint         = L"PSMain";
	Ps.Stage              = EShaderStage::Pixel;
	const ComPtr<IDxcBlob> VsBlob = Library->GetShader(Vs);
	const ComPtr<IDxcBlob> PsBlob = Library->GetShader(Ps);
	if (!VsBlob || !PsBlob)
	{
		return false;
	}
	FGraphicsPipelineDesc Desc;
	Desc.RootSignature          = RootSignature.Get();
	Desc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VsBlob.Get());
	Desc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PsBlob.Get());
	Desc.RenderTargetFormats[0] = Format;
	Desc.CullMode               = D3D12_CULL_MODE_NONE;
	FD3D12PipelineState Swap;
	FD3D12PipelineState PreviewPipeline;
	if (!Swap.InitGraphics(Device, Desc, L"HdrCompositePipeline"))
	{
		return false;
	}
	Desc.RenderTargetFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (!PreviewPipeline.InitGraphics(Device, Desc, L"HdrCompositePreviewPipeline"))
	{
		return false;
	}
	Pipelines[0].Swap(Swap);
	Pipelines[1].Swap(PreviewPipeline);
	if (Swap.Get() != nullptr)
	{
		Rhi.DeferRelease(Swap.Detach());
	}
	if (PreviewPipeline.IsInitialized())
	{
		Rhi.DeferRelease(PreviewPipeline.Detach());
	}
	PipelineFormat = Format;
	return true;
}

bool FD3D12HdrOutput::CreateTargets(FD3D12RHI& Rhi, uint32 Width, uint32 Height)
{
	ReleaseTargets(&Rhi);
	ID3D12Device*               Device = Rhi.GetDevice().GetDevice();
	const D3D12_HEAP_PROPERTIES Heap   = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);

	// 겹침 층: R8G8B8A8 TYPELESS (sRGB/UNORM RTV, UNORM SRV), 투명 0 클리어
	D3D12_RESOURCE_DESC OverlayDesc = MakeTexture2DDesc(Width, Height, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	D3D12_CLEAR_VALUE   Clear{};
	Clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	E_D3D_VERIFY(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &OverlayDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &Clear,
	                                             IID_PPV_ARGS(&Overlay)));
	Overlay->SetName(L"HdrOverlay");
	if (!OverlayRtvHeap.Init(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, false, L"HdrOverlayRtv"))
	{
		return false;
	}
	D3D12_RENDER_TARGET_VIEW_DESC Rtv{};
	Rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	Rtv.Format        = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	Device->CreateRenderTargetView(Overlay.Get(), &Rtv, OverlayRtvHeap.GetCpuHandle(0));
	Rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	Device->CreateRenderTargetView(Overlay.Get(), &Rtv, OverlayRtvHeap.GetCpuHandle(1));
	D3D12_SHADER_RESOURCE_VIEW_DESC Srv{};
	Srv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
	Srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
	Srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	Srv.Texture2D.MipLevels     = 1;
	OverlaySrv                  = Rhi.GetSrvAllocator().Allocate();
	Device->CreateShaderResourceView(Overlay.Get(), &Srv, OverlaySrv.Cpu);

	// 씬 타깃 (선형 FP16, 깊이 없음 — 씬 렌더러는 자기 깊이를 쓴다). 평소 상태 PIXEL_SHADER_RESOURCE
	SceneTarget = std::make_unique<FD3D12RenderTarget>();
	if (!SceneTarget->Init(Rhi.GetDevice(), Rhi.GetSrvAllocator(), Width, Height, L"HdrSceneTarget", FRenderTargetDesc::MakeHdr(false)))
	{
		return false;
	}

	// SDR 미리보기 (스크린샷)
	D3D12_RESOURCE_DESC PreviewDesc = MakeTexture2DDesc(Width, Height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	E_D3D_VERIFY(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &PreviewDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&Preview)));
	Preview->SetName(L"HdrPreview");
	if (!PreviewRtvHeap.Init(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, L"HdrPreviewRtv"))
	{
		return false;
	}
	Device->CreateRenderTargetView(Preview.Get(), nullptr, PreviewRtvHeap.GetCpuHandle(0));
	return true;
}

void FD3D12HdrOutput::ReleaseTargets(FD3D12RHI* Rhi)
{
	if (Rhi != nullptr)
	{
		if (Overlay)
		{
			Rhi->DeferRelease(Overlay);
		}
		if (Preview)
		{
			Rhi->DeferRelease(Preview);
		}
		if (OverlaySrv.IsValid())
		{
			Rhi->DeferFreeDescriptor(OverlaySrv);
		}
		if (SceneTarget)
		{
			SceneTarget->ShutdownDeferred(*Rhi);
		}
	}
	Overlay.Reset();
	Preview.Reset();
	OverlaySrv = FD3D12DescriptorHandle{};
	SceneTarget.reset();
	OverlayRtvHeap.Shutdown();
	PreviewRtvHeap.Shutdown();
}

bool FD3D12HdrOutput::Enable(FD3D12RHI& Rhi, EHdrSwapChainMode InMode, float InPaperWhiteNits, uint32 Width, uint32 Height)
{
	RhiPtr         = &Rhi;
	PaperWhiteNits = InPaperWhiteNits;
	const EHdrSwapChainMode Previous = Mode;
	Mode                             = InMode;
	if (InMode == EHdrSwapChainMode::Off)
	{
		Disable(Rhi);
		return true;
	}
	if (!CreatePipelines(Rhi) || ((Previous == EHdrSwapChainMode::Off || !Overlay) && !CreateTargets(Rhi, Width, Height)))
	{
		Mode = EHdrSwapChainMode::Off;
		ReleaseTargets(&Rhi);
		return false;
	}
	return true;
}

void FD3D12HdrOutput::Disable(FD3D12RHI& Rhi)
{
	Mode = EHdrSwapChainMode::Off;
	ReleaseTargets(&Rhi);
}

void FD3D12HdrOutput::Shutdown()
{
	if (RhiPtr != nullptr && OverlaySrv.IsValid())
	{
		RhiPtr->GetSrvAllocator().Free(OverlaySrv); // RHI 종료 경로 (GPU 비운 뒤) — 즉시 반환
	}
	ReleaseTargets(nullptr);
	Pipelines[0].Shutdown();
	Pipelines[1].Shutdown();
	RootSignature.Shutdown();
	if (Library)
	{
		Library->Shutdown();
	}
	Library.reset();
	if (Compiler)
	{
		Compiler->Shutdown();
	}
	Compiler.reset();
	Mode = EHdrSwapChainMode::Off;
}

bool FD3D12HdrOutput::Resize(FD3D12RHI& Rhi, uint32 Width, uint32 Height)
{
	if (!IsActive())
	{
		return true;
	}
	return CreateTargets(Rhi, Width, Height);
}

void FD3D12HdrOutput::BeginFrame(ID3D12GraphicsCommandList* List, const float ClearColor[4])
{
	const D3D12_RESOURCE_BARRIER Barriers[] = {
		MakeTransitionBarrier(Overlay.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
		MakeTransitionBarrier(SceneTarget->GetColorResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
	};
	List->ResourceBarrier(2, Barriers);
	const float Transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	List->ClearRenderTargetView(GetOverlayRtv(false), Transparent, 0, nullptr);
	List->ClearRenderTargetView(SceneTarget->GetRtv(), SceneTarget->GetDesc().ClearColor, 0, nullptr); // 최적 클리어 값 (씬 렌더러가 전부 덮는다)
	(void)ClearColor;
}

void FD3D12HdrOutput::Composite(ID3D12GraphicsCommandList* List, D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv, uint32 Width, uint32 Height, bool bPreview)
{
	const D3D12_RESOURCE_BARRIER ToRead[] = {
		MakeTransitionBarrier(Overlay.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
		MakeTransitionBarrier(SceneTarget->GetColorResource(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
	};
	List->ResourceBarrier(2, ToRead);
	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height) };
	List->RSSetViewports(1, &Viewport);
	List->RSSetScissorRects(1, &Scissor);
	List->SetGraphicsRootSignature(RootSignature.Get());
	List->SetGraphicsRootDescriptorTable(1, SceneTarget->GetSrv().Gpu);
	List->SetGraphicsRootDescriptorTable(2, OverlaySrv.Gpu);
	List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	FCompositeConstants Constants;
	Constants.Mode           = Mode == EHdrSwapChainMode::Hdr10 ? 0u : 1u;
	Constants.PaperWhiteNits = PaperWhiteNits;
	List->OMSetRenderTargets(1, &BackBufferRtv, FALSE, nullptr);
	List->SetPipelineState(Pipelines[0].Get());
	List->SetGraphicsRoot32BitConstants(0, 4, &Constants, 0);
	List->DrawInstanced(3, 1, 0, 0);
	if (bPreview)
	{
		Constants.Mode                          = 2u;
		const D3D12_CPU_DESCRIPTOR_HANDLE Target = PreviewRtvHeap.GetCpuHandle(0);
		List->OMSetRenderTargets(1, &Target, FALSE, nullptr);
		List->SetPipelineState(Pipelines[1].Get());
		List->SetGraphicsRoot32BitConstants(0, 4, &Constants, 0);
		List->DrawInstanced(3, 1, 0, 0);
	}
}
