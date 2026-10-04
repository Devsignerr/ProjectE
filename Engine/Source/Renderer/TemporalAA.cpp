#include "Renderer/TemporalAA.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/RendererConsoleVariables.h"
#include "Renderer/ScreenPass.h"
#include "Renderer/UpscaleMath.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// TemporalAA.hlsl TaaConstants와 1:1 (앞 88바이트는 Phase 33 그대로 — PSResolve 결과 불변)
	struct alignas(16) FTaaConstants
	{
		FMatrix4x4 Reprojection;
		FVector2   TexelSize;
		float      CurrentWeight  = 0.1f;
		uint32     bHistoryValid  = 0;
		float      ReactiveWeight = 0.6f;
		float      VarianceGamma  = 1.25f;
		FVector2   InputSize;
		FVector2   InputTexelSize;
		FVector2   JitterUv;
		float      UpsampleScale = 1.0f;
		float      StaticWeight  = 0.1f; // 움직임 0일 때 현재 비중 (재구성 경로만, 2px 움직임까지 CurrentWeight로)
		float      Padding[2]    = {};
	};
	static_assert(sizeof(FTaaConstants) == 128);

	constexpr DXGI_FORMAT HistoryFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
} // namespace

FTemporalAA::~FTemporalAA()
{
	Shutdown();
}

bool FTemporalAA::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary, const FScreenPassRootSignature& InRoot)
{
	Rhi     = &InRhi;
	Library = &InLibrary;
	Root    = &InRoot;
	return CreatePipelines(Pipeline, UpsamplePipeline, DepthPipeline, false);
}

void FTemporalAA::Shutdown()
{
	for (std::unique_ptr<FD3D12RenderTarget>& Target : HistoryTargets)
	{
		Target.reset();
	}
	OverlayDepth.reset();
	Pipeline.Shutdown();
	UpsamplePipeline.Shutdown();
	DepthPipeline.Shutdown();
	bHasHistory = false;
	Rhi         = nullptr;
}

bool FTemporalAA::CreatePipelines(FD3D12PipelineState& OutResolve, FD3D12PipelineState& OutUpsample, FD3D12PipelineState& OutDepth, bool bForceRecompile)
{
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	return Root->CreateGraphicsPipeline(OutResolve, Device, *Library, L"TemporalAA.hlsl", L"PSResolve", { HistoryFormat }, EBlendMode::Opaque,
	                                    bForceRecompile, L"TemporalAAPipeline") &&
	       Root->CreateGraphicsPipeline(OutUpsample, Device, *Library, L"TemporalAA.hlsl", L"PSResolveUpsample", { HistoryFormat }, EBlendMode::Opaque,
	                                    bForceRecompile, L"TemporalUpsamplePipeline") &&
	       Root->CreateDepthOutputPipeline(OutDepth, Device, *Library, L"TemporalAA.hlsl", L"PSUpscaleDepth", FD3D12RHI::DepthBufferFormat, bForceRecompile,
	                                       L"UpscaleDepthPipeline");
}

bool FTemporalAA::ReloadShaders(bool bForceRecompile)
{
	FD3D12PipelineState NewResolve;
	FD3D12PipelineState NewUpsample;
	FD3D12PipelineState NewDepth;
	if (!CreatePipelines(NewResolve, NewUpsample, NewDepth, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "TAA 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	Pipeline.Swap(NewResolve);
	UpsamplePipeline.Swap(NewUpsample);
	DepthPipeline.Swap(NewDepth);
	Rhi->DeferRelease(NewResolve.Detach());
	Rhi->DeferRelease(NewUpsample.Detach());
	Rhi->DeferRelease(NewDepth.Detach());
	return true;
}

void FTemporalAA::EnsureTargets(uint32 Width, uint32 Height)
{
	if (HistoryTargets[0] && HistoryTargets[0]->GetWidth() == Width && HistoryTargets[0]->GetHeight() == Height)
	{
		return;
	}
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		if (HistoryTargets[Index])
		{
			HistoryTargets[Index]->ShutdownDeferred(*Rhi); // 진행 중인 프레임이 참조할 수 있다
		}
		HistoryTargets[Index] = std::make_unique<FD3D12RenderTarget>();
		if (!HistoryTargets[Index]->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), Width, Height, Index == 0 ? L"TaaHistory0" : L"TaaHistory1",
		                                 FRenderTargetDesc::MakeHdr(false)))
		{
			E_LOG(LogRenderer, Fatal, "TAA 이력 버퍼 생성 실패 ({}x{})", Width, Height);
		}
	}
	bHasHistory = false; // 새 버퍼는 비어 있다
}

const FD3D12RenderTarget& FTemporalAA::AddPass(FRenderGraph& Graph, const FTemporalAAInputs& Inputs, const FTemporalAAGraphRefs& Refs, int32 Timer,
                                               FRGResourceRef& OutResult)
{
	E_CHECKF(Inputs.SceneColor != nullptr && Inputs.Velocity != nullptr && Inputs.SceneColor->GetDesc().bWithDepth, "TAA 입력이 올바르지 않습니다");
	const uint32 InputWidth  = Inputs.SceneColor->GetWidth();
	const uint32 InputHeight = Inputs.SceneColor->GetHeight();
	const uint32 Width       = Inputs.OutputWidth > 0 ? Inputs.OutputWidth : InputWidth;
	const uint32 Height      = Inputs.OutputHeight > 0 ? Inputs.OutputHeight : InputHeight;
	const bool   bUpsample   = Width != InputWidth || Height != InputHeight;
	// 네이티브도 지터 보정 재구성(PSResolveUpsample, 배율 1): 원본 픽셀 하나를 그대로 섞으면 지터마다 값이 달라 정지 화면에서도 수렴하지 않는다
	const bool   bReconstruct = bUpsample || RendererCVars::TaaReconstruct.Get();
	EnsureTargets(Width, Height);

	FD3D12RenderTarget& Read  = *HistoryTargets[WriteIndex ^ 1];
	FD3D12RenderTarget& Write = *HistoryTargets[WriteIndex];

	FTaaConstants Constants;
	Constants.Reprojection   = Inputs.Reprojection;
	Constants.TexelSize      = FVector2(1.0f / static_cast<float>(Width), 1.0f / static_cast<float>(Height));
	Constants.CurrentWeight  = FMath::Clamp(Inputs.CurrentWeight, 0.01f, 1.0f);
	Constants.bHistoryValid  = (Inputs.bHistoryValid && bHasHistory) ? 1u : 0u;
	Constants.ReactiveWeight = FMath::Clamp(Inputs.ReactiveWeight, Constants.CurrentWeight, 1.0f);
	Constants.VarianceGamma  = FMath::Max(Inputs.VarianceGamma, 0.1f);
	Constants.StaticWeight   = FMath::Clamp(RendererCVars::TaaStaticWeight.Get(), 0.01f, Constants.CurrentWeight);
	if (bReconstruct)
	{
		Constants.InputSize      = FVector2(static_cast<float>(InputWidth), static_cast<float>(InputHeight));
		Constants.InputTexelSize = FVector2(1.0f / Constants.InputSize.X, 1.0f / Constants.InputSize.Y);
		Constants.JitterUv       = FUpscaleMath::JitterNdcToUv(Inputs.JitterNdc);
		Constants.UpsampleScale  = static_cast<float>(Height) / static_cast<float>(InputHeight);
	}
	const D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;

	const FRGResourceRef ReadRef  = Graph.ImportColor("TaaHistoryRead", Read);
	const FRGResourceRef WriteRef = Graph.ImportColor("TaaHistoryWrite", Write);
	const FD3D12DescriptorHandle SceneSrv    = Inputs.SceneColor->GetSrv();
	const FD3D12DescriptorHandle VelocitySrv = Inputs.Velocity->GetSrv();
	const FD3D12DescriptorHandle DepthSrv    = Inputs.SceneColor->GetDepthSrv();
	const FD3D12DescriptorHandle HistorySrv  = Read.GetSrv();
	const FD3D12PipelineState*   UsedPipeline = bReconstruct ? &UpsamplePipeline : &Pipeline;
	Graph.AddPass(bUpsample ? "TAAU" : "TAA")
		.Read(Refs.SceneColor, ERGAccess::SrvPixel)
		.Read(Refs.Velocity, ERGAccess::SrvPixel)
		.Read(Refs.SceneDepth, ERGAccess::SrvPixel)
		.Read(ReadRef, ERGAccess::SrvPixel)
		.Write(WriteRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true) // 전체를 덮어쓴다
		.Timer(Timer)
		.Execute([this, &Write, UsedPipeline, ConstantsAddress, SceneSrv, VelocitySrv, DepthSrv, HistorySrv, Width, Height](FRGContext& Context) {
			Write.Bind(Context.CommandList, nullptr);
			DrawScreenPass(Context.CommandList, *Root, *UsedPipeline, ConstantsAddress, { SceneSrv, HistorySrv, VelocitySrv, DepthSrv }, Width, Height);
		});

	bHasHistory = true;
	WriteIndex ^= 1;
	OutResult = WriteRef;
	return Write;
}

void FTemporalAA::AddOverlayDepthPass(FRenderGraph& Graph, const FD3D12RenderTarget& SceneColor, FRGResourceRef SceneDepth, const FVector2& JitterNdc,
                                      uint32 OutputWidth, uint32 OutputHeight, int32 Timer)
{
	if (!OverlayDepth || OverlayDepth->GetWidth() != OutputWidth || OverlayDepth->GetHeight() != OutputHeight)
	{
		if (OverlayDepth)
		{
			OverlayDepth->ShutdownDeferred(*Rhi);
		}
		OverlayDepth = std::make_unique<FD3D12RenderTarget>();
		if (!OverlayDepth->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), OutputWidth, OutputHeight, L"UpscaledOverlayDepth",
		                        FRenderTargetDesc::MakeMask(true)))
		{
			E_LOG(LogRenderer, Fatal, "오버레이 깊이 버퍼 생성 실패 ({}x{})", OutputWidth, OutputHeight);
		}
	}

	FTaaConstants Constants;
	Constants.InputSize      = FVector2(static_cast<float>(SceneColor.GetWidth()), static_cast<float>(SceneColor.GetHeight()));
	Constants.InputTexelSize = FVector2(1.0f / Constants.InputSize.X, 1.0f / Constants.InputSize.Y);
	Constants.JitterUv       = FUpscaleMath::JitterNdcToUv(JitterNdc);
	const D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;

	const FRGResourceRef         OverlayRef = Graph.ImportDepth("OverlayDepth", *OverlayDepth);
	const FD3D12DescriptorHandle DepthSrv   = SceneColor.GetDepthSrv();
	const D3D12_CPU_DESCRIPTOR_HANDLE Dsv   = OverlayDepth->GetDsv();
	Graph.AddPass("오버레이 깊이 업스케일")
		.Read(SceneDepth, ERGAccess::SrvPixel)
		.Write(OverlayRef, ERGAccess::DepthWrite, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.NeverCull() // 그래프 밖(에디터 오버레이·디버그 선)이 읽는다
		.Execute([this, ConstantsAddress, DepthSrv, Dsv, OutputWidth, OutputHeight](FRGContext& Context) {
			Context.CommandList->OMSetRenderTargets(0, nullptr, FALSE, &Dsv);
			DrawScreenPass(Context.CommandList, *Root, DepthPipeline, ConstantsAddress, { FD3D12DescriptorHandle{}, FD3D12DescriptorHandle{},
			                                                                              FD3D12DescriptorHandle{}, DepthSrv }, OutputWidth, OutputHeight);
		});
}
