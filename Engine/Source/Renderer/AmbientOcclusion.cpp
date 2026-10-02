#include "Renderer/AmbientOcclusion.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/ScreenPass.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// AmbientOcclusion.hlsl AoConstants와 1:1
	struct alignas(16) FAoConstants
	{
		FMatrix4x4 InvProjection;
		FMatrix4x4 View;
		FVector2   FullSize;
		FVector2   HalfTexelSize;
		float      Radius        = 80.0f;
		float      Intensity     = 1.0f;
		float      PixelsPerUnit = 1.0f;
		uint32     bOrthographic = 0;
		uint32     FrameIndex    = 0;
		float      BlurSharpness = 40.0f;
		FVector2   BlurDirection;
		uint32     ResolutionDivisor = 2;
		uint32     bGridNoise        = 0;
		int32      GridOrigin[2]     = {};
	};
	static_assert(sizeof(FAoConstants) == 192);
} // namespace

FAmbientOcclusion::~FAmbientOcclusion()
{
	Shutdown();
}

bool FAmbientOcclusion::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary, const FScreenPassRootSignature& InRoot)
{
	Rhi     = &InRhi;
	Library = &InLibrary;
	Root    = &InRoot;
	return CreatePipelines(ComputePipeline, BlurPipeline, false);
}

void FAmbientOcclusion::Shutdown()
{
	for (std::unique_ptr<FD3D12RenderTarget>& Target : Targets)
	{
		Target.reset();
	}
	ComputePipeline.Shutdown();
	BlurPipeline.Shutdown();
	Rhi = nullptr;
}

bool FAmbientOcclusion::CreatePipelines(FD3D12PipelineState& OutCompute, FD3D12PipelineState& OutBlur, bool bForceRecompile)
{
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	return Root->CreateGraphicsPipeline(OutCompute, Device, *Library, L"AmbientOcclusion.hlsl", L"PSCompute", { Format }, EBlendMode::Opaque,
	                                    bForceRecompile, L"AmbientOcclusionPipeline") &&
	       Root->CreateGraphicsPipeline(OutBlur, Device, *Library, L"AmbientOcclusion.hlsl", L"PSBlur", { Format }, EBlendMode::Opaque, false,
	                                    L"AmbientOcclusionBlurPipeline");
}

bool FAmbientOcclusion::ReloadShaders(bool bForceRecompile)
{
	FD3D12PipelineState NewCompute;
	FD3D12PipelineState NewBlur;
	if (!CreatePipelines(NewCompute, NewBlur, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "SSAO 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	ComputePipeline.Swap(NewCompute);
	Rhi->DeferRelease(NewCompute.Detach());
	BlurPipeline.Swap(NewBlur);
	Rhi->DeferRelease(NewBlur.Detach());
	return true;
}

void FAmbientOcclusion::EnsureTargets(uint32 FullWidth, uint32 FullHeight, uint32 ResolutionDivisor)
{
	const uint32 Divisor = ResolutionDivisor == 1 ? 1u : 2u;
	const uint32 Width   = FMath::Max(1u, (FullWidth + Divisor - 1) / Divisor);
	const uint32 Height  = FMath::Max(1u, (FullHeight + Divisor - 1) / Divisor);
	if (Targets[0] && Targets[0]->GetWidth() == Width && Targets[0]->GetHeight() == Height)
	{
		return;
	}
	FRenderTargetDesc Desc = FRenderTargetDesc::MakeColor(Format);
	Desc.ClearColor[0]     = 1.0f; // 가림 없음
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		if (Targets[Index])
		{
			Targets[Index]->ShutdownDeferred(*Rhi);
		}
		Targets[Index] = std::make_unique<FD3D12RenderTarget>();
		if (!Targets[Index]->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), Width, Height, Index == 0 ? L"AmbientOcclusion" : L"AmbientOcclusionBlur",
		                          Desc))
		{
			E_LOG(LogRenderer, Fatal, "SSAO 버퍼 생성 실패 ({}x{})", Width, Height);
		}
	}
}

void FAmbientOcclusion::Render(const FAmbientOcclusionInputs& Inputs)
{
	E_CHECKF(Inputs.SceneDepth != nullptr && Inputs.SceneNormal != nullptr && Inputs.SceneDepth->GetDesc().bWithDepth, "SSAO 입력이 올바르지 않습니다");
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	const uint32               FullWidth   = Inputs.SceneDepth->GetWidth();
	const uint32               FullHeight  = Inputs.SceneDepth->GetHeight();
	EnsureTargets(FullWidth, FullHeight, Inputs.ResolutionDivisor);
	const uint32 Width  = Targets[0]->GetWidth();
	const uint32 Height = Targets[0]->GetHeight();

	FAoConstants Constants;
	Constants.InvProjection = Inputs.Projection.GetInverse();
	Constants.View          = Inputs.View;
	Constants.FullSize      = FVector2(static_cast<float>(FullWidth), static_cast<float>(FullHeight));
	Constants.HalfTexelSize = FVector2(1.0f / static_cast<float>(Width), 1.0f / static_cast<float>(Height));
	Constants.Radius        = FMath::Max(Inputs.Radius, 1.0f);
	Constants.Intensity     = FMath::Max(Inputs.Intensity, 0.0f);
	Constants.PixelsPerUnit = 0.5f * static_cast<float>(FullHeight) * Inputs.Projection.M[1][1];
	Constants.bOrthographic = Inputs.bOrthographic ? 1u : 0u;
	Constants.FrameIndex    = Inputs.FrameIndex;
	Constants.ResolutionDivisor = Inputs.ResolutionDivisor == 1 ? 1u : 2u;
	Constants.bGridNoise        = Inputs.bGridNoise ? 1u : 0u;
	Constants.GridOrigin[0]     = Inputs.GridOrigin[0];
	Constants.GridOrigin[1]     = Inputs.GridOrigin[1];

	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	const D3D12_GPU_VIRTUAL_ADDRESS ComputeConstants = DynamicBuffer.AllocateConstants(Constants).GpuAddress;
	Constants.BlurDirection                          = FVector2(1.0f, 0.0f);
	const D3D12_GPU_VIRTUAL_ADDRESS HorizontalConstants = DynamicBuffer.AllocateConstants(Constants).GpuAddress;
	Constants.BlurDirection                             = FVector2(0.0f, 1.0f);
	const D3D12_GPU_VIRTUAL_ADDRESS VerticalConstants   = DynamicBuffer.AllocateConstants(Constants).GpuAddress;

	ID3D12Resource*              DepthResource = Inputs.SceneDepth->GetDepthResource();
	const D3D12_RESOURCE_BARRIER ToRead =
		MakeTransitionBarrier(DepthResource, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	CommandList->ResourceBarrier(1, &ToRead);

	// 1) 계산 → [0], 2) 가로 블러 [0] → [1], 3) 세로 블러 [1] → [0]
	Targets[0]->Begin(CommandList, nullptr);
	DrawScreenPass(CommandList, *Root, ComputePipeline, ComputeConstants, { Inputs.SceneDepth->GetDepthSrv(), Inputs.SceneNormal->GetSrv() }, Width,
	               Height);
	Targets[0]->End(CommandList);

	const D3D12_RESOURCE_BARRIER ToWrite =
		MakeTransitionBarrier(DepthResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	CommandList->ResourceBarrier(1, &ToWrite);

	const FD3D12DescriptorHandle None;
	Targets[1]->Begin(CommandList, nullptr);
	DrawScreenPass(CommandList, *Root, BlurPipeline, HorizontalConstants, { None, None, Targets[0]->GetSrv() }, Width, Height);
	Targets[1]->End(CommandList);
	Targets[0]->Begin(CommandList, nullptr);
	DrawScreenPass(CommandList, *Root, BlurPipeline, VerticalConstants, { None, None, Targets[1]->GetSrv() }, Width, Height);
	Targets[0]->End(CommandList);
}
