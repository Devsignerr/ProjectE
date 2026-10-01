#include "Renderer/TemporalAA.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/ScreenPass.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// TemporalAA.hlsl TaaConstants와 1:1
	struct alignas(16) FTaaConstants
	{
		FMatrix4x4 Reprojection;
		FVector2   TexelSize;
		float      CurrentWeight  = 0.1f;
		uint32     bHistoryValid  = 0;
		float      ReactiveWeight = 0.6f;
		float      VarianceGamma  = 1.25f;
		float      Padding[2]     = {};
	};
	static_assert(sizeof(FTaaConstants) == 96);

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
	return CreatePipeline(Pipeline, false);
}

void FTemporalAA::Shutdown()
{
	for (std::unique_ptr<FD3D12RenderTarget>& Target : HistoryTargets)
	{
		Target.reset();
	}
	Pipeline.Shutdown();
	bHasHistory = false;
	Rhi         = nullptr;
}

bool FTemporalAA::CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile)
{
	return Root->CreateGraphicsPipeline(OutPipeline, Rhi->GetDevice().GetDevice(), *Library, L"TemporalAA.hlsl", L"PSResolve", { HistoryFormat },
	                                    EBlendMode::Opaque, bForceRecompile, L"TemporalAAPipeline");
}

bool FTemporalAA::ReloadShaders(bool bForceRecompile)
{
	FD3D12PipelineState NewPipeline;
	if (!CreatePipeline(NewPipeline, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "TAA 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	Pipeline.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());
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

const FD3D12RenderTarget& FTemporalAA::Resolve(const FTemporalAAInputs& Inputs)
{
	E_CHECKF(Inputs.SceneColor != nullptr && Inputs.Velocity != nullptr && Inputs.SceneColor->GetDesc().bWithDepth, "TAA 입력이 올바르지 않습니다");
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	const uint32               Width       = Inputs.SceneColor->GetWidth();
	const uint32               Height      = Inputs.SceneColor->GetHeight();
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
	const D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;

	ID3D12Resource*              DepthResource = Inputs.SceneColor->GetDepthResource();
	const D3D12_RESOURCE_BARRIER ToRead =
		MakeTransitionBarrier(DepthResource, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	CommandList->ResourceBarrier(1, &ToRead);

	Write.Begin(CommandList, nullptr); // 전체를 덮어쓴다
	DrawScreenPass(CommandList, *Root, Pipeline, ConstantsAddress,
	               { Inputs.SceneColor->GetSrv(), Read.GetSrv(), Inputs.Velocity->GetSrv(), Inputs.SceneColor->GetDepthSrv() }, Width, Height);
	Write.End(CommandList);

	const D3D12_RESOURCE_BARRIER ToWrite =
		MakeTransitionBarrier(DepthResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	CommandList->ResourceBarrier(1, &ToWrite);

	bHasHistory = true;
	WriteIndex ^= 1;
	return Write;
}
