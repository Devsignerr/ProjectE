#include "Renderer/ScreenSpaceReflections.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/ReflectionMath.h"
#include "Renderer/ScreenPass.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	enum EHizRootParameter : uint32
	{
		HizParam_Constants = 0, // b0 (상수 4개)
		HizParam_Depth     = 1, // t0
		HizParam_Dest      = 2, // u0
		HizParam_Source    = 3, // u1
	};

	// SsrTrace.hlsl SsrConstants와 1:1
	struct alignas(16) FSsrConstants
	{
		FMatrix4x4 Projection;
		FMatrix4x4 InvProjection;
		FMatrix4x4 View;
		FMatrix4x4 Reprojection;
		FVector2   ScreenSize;
		uint32     HizMipCount   = 1;
		uint32     MaxIterations = 64;
		float      MaxDistance   = 2000.0f;
		float      Thickness     = 40.0f;
		float      NearZ         = 10.0f;
		uint32     bOrthographic = 0;
		uint32     FrameIndex    = 0;
		float      MaxRoughness  = 0.6f;
		uint32     bStochastic   = 0;
		float      Padding       = 0.0f;
	};
	static_assert(sizeof(FSsrConstants) == 304);

	constexpr DXGI_FORMAT HizFormat = DXGI_FORMAT_R32_FLOAT;
} // namespace

FScreenSpaceReflections::~FScreenSpaceReflections()
{
	Shutdown();
}

bool FScreenSpaceReflections::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary, const FScreenPassRootSignature& InRoot)
{
	Rhi     = &InRhi;
	Library = &InLibrary;
	Root    = &InRoot;

	const uint32 ConstantsIndex = HizRoot.AddConstants(4, 0);
	const uint32 DepthIndex     = HizRoot.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
	const uint32 DestIndex = HizRoot.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
	const uint32 SourceIndex = HizRoot.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
	E_CHECK(ConstantsIndex == HizParam_Constants && DepthIndex == HizParam_Depth && DestIndex == HizParam_Dest && SourceIndex == HizParam_Source);
	if (!HizRoot.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"SsrHizRootSignature"))
	{
		return false;
	}
	return CreatePipelines(HizCopyPipeline, HizDownsamplePipeline, TracePipeline, false);
}

void FScreenSpaceReflections::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	ReleaseHiz();
	Result.reset();
	HizCopyPipeline.Shutdown();
	HizDownsamplePipeline.Shutdown();
	TracePipeline.Shutdown();
	HizRoot.Shutdown();
	Rhi = nullptr;
}

bool FScreenSpaceReflections::CreatePipelines(FD3D12PipelineState& OutCopy, FD3D12PipelineState& OutDownsample, FD3D12PipelineState& OutTrace,
                                              bool bForceRecompile)
{
	ID3D12Device* Device  = Rhi->GetDevice().GetDevice();
	const auto    Compute = [&](const wchar_t* Entry) {
		FShaderCompileDesc Desc;
		Desc.FileName   = L"ScreenSpaceReflections.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = EShaderStage::Compute;
		if (bForceRecompile && !Library->CookShader(Desc))
		{
			return ComPtr<IDxcBlob>();
		}
		return Library->GetShader(Desc);
	};
	const ComPtr<IDxcBlob> Copy       = Compute(L"CSHizCopy");
	const ComPtr<IDxcBlob> Downsample = Compute(L"CSHizDownsample");
	if (!Copy || !Downsample)
	{
		return false;
	}
	return OutCopy.InitCompute(Device, HizRoot.Get(), FD3D12ShaderCompiler::ToBytecode(Copy.Get()), L"SsrHizCopy") &&
	       OutDownsample.InitCompute(Device, HizRoot.Get(), FD3D12ShaderCompiler::ToBytecode(Downsample.Get()), L"SsrHizDownsample") &&
	       Root->CreateGraphicsPipeline(OutTrace, Device, *Library, L"SsrTrace.hlsl", L"PSTrace", { ResultFormat }, EBlendMode::Opaque, bForceRecompile,
	                                    L"SsrTracePipeline");
}

bool FScreenSpaceReflections::ReloadShaders(bool bForceRecompile)
{
	FD3D12PipelineState NewCopy;
	FD3D12PipelineState NewDownsample;
	FD3D12PipelineState NewTrace;
	if (!CreatePipelines(NewCopy, NewDownsample, NewTrace, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "SSR 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	HizCopyPipeline.Swap(NewCopy);
	Rhi->DeferRelease(NewCopy.Detach());
	HizDownsamplePipeline.Swap(NewDownsample);
	Rhi->DeferRelease(NewDownsample.Detach());
	TracePipeline.Swap(NewTrace);
	Rhi->DeferRelease(NewTrace.Detach());
	return true;
}

void FScreenSpaceReflections::ReleaseHiz()
{
	if (Hiz)
	{
		Rhi->DeferRelease(Hiz);
		Rhi->DeferFreeDescriptor(HizSrv);
		for (const FD3D12DescriptorHandle& Uav : HizUavs)
		{
			Rhi->DeferFreeDescriptor(Uav);
		}
	}
	Hiz.Reset();
	HizSrv = FD3D12DescriptorHandle{};
	HizUavs.clear();
	HizWidth = HizHeight = HizMipCount = 0;
}

void FScreenSpaceReflections::EnsureTargets(uint32 Width, uint32 Height)
{
	if (Result && Result->GetWidth() == Width && Result->GetHeight() == Height)
	{
		return;
	}
	if (Result)
	{
		Result->ShutdownDeferred(*Rhi);
	}
	Result = std::make_unique<FD3D12RenderTarget>();
	if (!Result->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), Width, Height, L"SsrResult", FRenderTargetDesc::MakeColor(ResultFormat)))
	{
		E_LOG(LogRenderer, Fatal, "SSR 버퍼 생성 실패 ({}x{})", Width, Height);
	}

	ReleaseHiz();
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	HizWidth             = Width;
	HizHeight            = Height;
	HizMipCount          = FReflectionMath::GetHizMipCount(Width, Height);
	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   Desc = MakeTexture2DDesc(Width, Height, HizFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, static_cast<uint16>(HizMipCount));
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&Hiz))))
	{
		E_LOG(LogRenderer, Fatal, "SSR Hi-Z 생성 실패 ({}x{})", Width, Height);
	}
	Hiz->SetName(L"SsrHiz");
	FD3D12DescriptorAllocator&      Allocator = Rhi->GetSrvAllocator();
	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                  = HizFormat;
	SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Texture2D.MipLevels     = HizMipCount;
	HizSrv                          = Allocator.Allocate();
	Device->CreateShaderResourceView(Hiz.Get(), &SrvDesc, HizSrv.Cpu);
	for (uint32 Mip = 0; Mip < HizMipCount; ++Mip)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc{};
		UavDesc.Format             = HizFormat;
		UavDesc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
		UavDesc.Texture2D.MipSlice = Mip;
		const FD3D12DescriptorHandle Uav = Allocator.Allocate();
		Device->CreateUnorderedAccessView(Hiz.Get(), nullptr, &UavDesc, Uav.Cpu);
		HizUavs.push_back(Uav);
	}
}

void FScreenSpaceReflections::Render(const FScreenSpaceReflectionInputs& Inputs)
{
	E_CHECKF(Inputs.SceneColor != nullptr && Inputs.SceneNormal != nullptr && Inputs.SceneColor->GetDesc().bWithDepth, "SSR 입력이 올바르지 않습니다");
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	const uint32               Width       = Inputs.SceneColor->GetWidth();
	const uint32               Height      = Inputs.SceneColor->GetHeight();
	EnsureTargets(Width, Height);

	ID3D12Resource*             Depth     = Inputs.SceneColor->GetDepthResource();
	const D3D12_RESOURCE_STATES DepthRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	{
		const D3D12_RESOURCE_BARRIER Barriers[] = {
			MakeTransitionBarrier(Depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, DepthRead),
			MakeTransitionBarrier(Hiz.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		CommandList->ResourceBarrier(2, Barriers);
	}

	// 1) Hi-Z: 깊이 → 밉 0 → 밉마다 2x2 최소
	CommandList->SetComputeRootSignature(HizRoot.Get());
	CommandList->SetComputeRootDescriptorTable(HizParam_Depth, Inputs.SceneColor->GetDepthSrv().Gpu);
	uint32 SourceWidth  = Width;
	uint32 SourceHeight = Height;
	for (uint32 Mip = 0; Mip < HizMipCount; ++Mip)
	{
		const uint32 DestWidth  = FMath::Max(1u, Width >> Mip);
		const uint32 DestHeight = FMath::Max(1u, Height >> Mip);
		const uint32 Constants[4] = { SourceWidth, SourceHeight, DestWidth, DestHeight };
		CommandList->SetPipelineState(Mip == 0 ? HizCopyPipeline.Get() : HizDownsamplePipeline.Get());
		CommandList->SetComputeRoot32BitConstants(HizParam_Constants, 4, Constants, 0);
		CommandList->SetComputeRootDescriptorTable(HizParam_Dest, HizUavs[Mip].Gpu);
		CommandList->SetComputeRootDescriptorTable(HizParam_Source, HizUavs[Mip == 0 ? 0 : Mip - 1].Gpu);
		CommandList->Dispatch((DestWidth + 7) / 8, (DestHeight + 7) / 8, 1);
		const D3D12_RESOURCE_BARRIER UavBarrier = MakeUavBarrier(Hiz.Get());
		CommandList->ResourceBarrier(1, &UavBarrier);
		SourceWidth  = DestWidth;
		SourceHeight = DestHeight;
	}
	{
		const D3D12_RESOURCE_BARRIER Barrier = MakeTransitionBarrier(Hiz.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		CommandList->ResourceBarrier(1, &Barrier);
	}

	// 2) 추적 (전체 화면)
	FSsrConstants Constants;
	Constants.Projection    = Inputs.Projection;
	Constants.InvProjection = Inputs.Projection.GetInverse();
	Constants.View          = Inputs.View;
	Constants.Reprojection  = Inputs.Reprojection;
	Constants.ScreenSize    = FVector2(static_cast<float>(Width), static_cast<float>(Height));
	Constants.HizMipCount   = HizMipCount;
	Constants.MaxDistance   = FMath::Max(Inputs.MaxDistance, 10.0f);
	Constants.Thickness     = FMath::Max(Inputs.Thickness, 1.0f);
	Constants.NearZ         = Inputs.NearZ;
	Constants.bOrthographic = Inputs.bOrthographic ? 1u : 0u;
	Constants.FrameIndex    = Inputs.FrameIndex;
	Constants.MaxRoughness  = Inputs.MaxRoughness;
	Constants.bStochastic   = Inputs.bStochastic ? 1u : 0u;
	const D3D12_GPU_VIRTUAL_ADDRESS Address = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;

	Result->Begin(CommandList, nullptr);
	DrawScreenPass(CommandList, *Root, TracePipeline, Address,
	               { Inputs.SceneColor->GetDepthSrv(), HizSrv, Inputs.SceneNormal->GetSrv(), Inputs.SceneColor->GetSrv() }, Width, Height);
	Result->End(CommandList);

	const D3D12_RESOURCE_BARRIER ToWrite = MakeTransitionBarrier(Depth, DepthRead, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	CommandList->ResourceBarrier(1, &ToWrite);
}
