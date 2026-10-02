#include "Renderer/ScreenSpaceReflections.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/ReflectionMath.h"
#include "Renderer/ScreenPass.h"

#include <array>

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
		float      ProjectionScale = 1.0f;
		float      MaxRoughness  = 0.6f;
		float      MaxBlurRadius = 16.0f;
		uint32     bDecals       = 0;
	};
	static_assert(sizeof(FSsrConstants) == 304);

	// SsrResolve.hlsl SsrResolveConstants와 1:1
	struct alignas(16) FSsrResolveConstants
	{
		FVector2 ScreenSize;
		float    CurrentWeight = 0.1f;
		uint32   bHistoryValid = 0;
		float    VarianceGamma = 1.5f;
		uint32   bDecals       = 0;
		float    Padding[2]    = {};
	};
	static_assert(sizeof(FSsrResolveConstants) == 32);

	constexpr float ResolveCurrentWeight = 0.1f;  // 이번 프레임 비중 (정지 화면 ≈ 20프레임에 수렴 — 지터로 반사 윤곽 계단을 평균)
	constexpr float ResolveVarianceGamma = 1.5f; // 움직일 때 번짐(고스팅)과 남는 노이즈 사이의 타협

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
	return CreatePipelines(HizCopyPipeline, HizDownsamplePipeline, TracePipeline, BlurPipeline, ResolvePipeline, false);
}

void FScreenSpaceReflections::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	History[0].reset();
	History[1].reset();
	HizCopyPipeline.Shutdown();
	HizDownsamplePipeline.Shutdown();
	TracePipeline.Shutdown();
	BlurPipeline.Shutdown();
	ResolvePipeline.Shutdown();
	HizRoot.Shutdown();
	Rhi = nullptr;
}

bool FScreenSpaceReflections::CreatePipelines(FD3D12PipelineState& OutCopy, FD3D12PipelineState& OutDownsample, FD3D12PipelineState& OutTrace,
                                              FD3D12PipelineState& OutBlur, FD3D12PipelineState& OutResolve, bool bForceRecompile)
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
	       Root->CreateGraphicsPipeline(OutTrace, Device, *Library, L"SsrTrace.hlsl", L"PSTrace", { ResultFormat, MotionFormat }, EBlendMode::Opaque, bForceRecompile,
	                                    L"SsrTracePipeline") &&
	       Root->CreateGraphicsPipeline(OutBlur, Device, *Library, L"SsrResolve.hlsl", L"PSBlur", { ResultFormat }, EBlendMode::Opaque, bForceRecompile,
	                                    L"SsrBlurPipeline") &&
	       Root->CreateGraphicsPipeline(OutResolve, Device, *Library, L"SsrResolve.hlsl", L"PSResolve", { ResultFormat }, EBlendMode::Opaque,
	                                    bForceRecompile, L"SsrResolvePipeline");
}

bool FScreenSpaceReflections::ReloadShaders(bool bForceRecompile)
{
	FD3D12PipelineState NewCopy;
	FD3D12PipelineState NewDownsample;
	FD3D12PipelineState NewTrace;
	FD3D12PipelineState NewBlur;
	FD3D12PipelineState NewResolve;
	if (!CreatePipelines(NewCopy, NewDownsample, NewTrace, NewBlur, NewResolve, bForceRecompile))
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
	BlurPipeline.Swap(NewBlur);
	Rhi->DeferRelease(NewBlur.Detach());
	ResolvePipeline.Swap(NewResolve);
	Rhi->DeferRelease(NewResolve.Detach());
	return true;
}


void FScreenSpaceReflections::EnsureTargets(uint32 Width, uint32 Height)
{
	if (History[0] && TargetWidth == Width && TargetHeight == Height)
	{
		return;
	}
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		if (History[Index])
		{
			History[Index]->ShutdownDeferred(*Rhi);
		}
		History[Index] = std::make_unique<FD3D12RenderTarget>();
		if (!History[Index]->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), Width, Height, Index == 0 ? L"SsrHistory0" : L"SsrHistory1",
		                          FRenderTargetDesc::MakeColor(ResultFormat)))
		{
			E_LOG(LogRenderer, Fatal, "SSR 누적 버퍼 생성 실패 ({}x{})", Width, Height);
		}
	}
	TargetWidth      = Width;
	TargetHeight     = Height;
	LastResolveFrame = 0; // 새 버퍼에는 이력이 없다
}

FRGResourceRef FScreenSpaceReflections::AddPasses(FRenderGraph& Graph, const FScreenSpaceReflectionInputs& Inputs, const FSsrGraphRefs& Refs, int32 Timer)
{
	E_CHECKF(Inputs.SceneColor != nullptr && Inputs.SceneNormal != nullptr && Inputs.SceneColor->GetDesc().bWithDepth && Inputs.DecalNormal != nullptr &&
	             Inputs.DecalMaterial != nullptr && Inputs.Velocity != nullptr,
	         "SSR 입력이 올바르지 않습니다");
	const uint32 Width  = Inputs.SceneColor->GetWidth();
	const uint32 Height = Inputs.SceneColor->GetHeight();
	EnsureTargets(Width, Height);

	// ---- 그래프 풀: Hi-Z 밉 체인 + 추적/흐림 중간 버퍼 (프레임 안에서만 쓴다)
	const uint32   HizMipCount = FReflectionMath::GetHizMipCount(Width, Height);
	FRGTextureDesc HizDesc;
	HizDesc.Width            = Width;
	HizDesc.Height           = Height;
	HizDesc.MipCount         = static_cast<uint16>(HizMipCount);
	HizDesc.Format           = HizFormat;
	HizDesc.bUnorderedAccess = true;
	const FRGResourceRef    HizRef     = Graph.CreateTexture("SsrHiz", HizDesc);
	const FRGResourceRef    ResultRef  = Graph.CreateTexture("SsrResult", FRGTextureDesc::MakeRenderTarget(Width, Height, ResultFormat));
	const FRGResourceRef    MotionRef  = Graph.CreateTexture("SsrReflectMotion", FRGTextureDesc::MakeRenderTarget(Width, Height, MotionFormat));
	const FRGResourceRef    BlurredRef = Graph.CreateTexture("SsrBlurred", FRGTextureDesc::MakeRenderTarget(Width, Height, ResultFormat));
	const FRGPooledTexture* Hiz        = Graph.GetTexture(HizRef);
	const FRGPooledTexture* Result     = Graph.GetTexture(ResultRef);
	const FRGPooledTexture* Motion     = Graph.GetTexture(MotionRef);
	const FRGPooledTexture* Blurred    = Graph.GetTexture(BlurredRef);

	const FD3D12DescriptorHandle DepthSrv     = Inputs.SceneColor->GetDepthSrv();
	const FD3D12DescriptorHandle NormalSrv    = Inputs.SceneNormal->GetSrv();
	const FD3D12DescriptorHandle ColorSrv     = (Inputs.PrevColor != nullptr ? Inputs.PrevColor : Inputs.SceneColor)->GetSrv();
	const FD3D12DescriptorHandle DecalNormal  = Inputs.DecalNormal->GetSrv();
	const FD3D12DescriptorHandle DecalMat     = Inputs.DecalMaterial->GetSrv();
	const FD3D12DescriptorHandle VelocitySrv  = Inputs.Velocity->GetSrv();

	// 1) Hi-Z: 깊이 → 밉 0 → 밉마다 2x2 최소 (밉 k 패스는 밉 k-1을 UAV로 읽고 밉 k를 쓴다 — 서브리소스 단위 전이 + UAV 배리어는 그래프가)
	uint32 SourceWidth  = Width;
	uint32 SourceHeight = Height;
	for (uint32 Mip = 0; Mip < HizMipCount; ++Mip)
	{
		const uint32 DestWidth  = FMath::Max(1u, Width >> Mip);
		const uint32 DestHeight = FMath::Max(1u, Height >> Mip);
		const std::array<uint32, 4> Constants = { SourceWidth, SourceHeight, DestWidth, DestHeight };
		FRenderGraph::FPassBuilder Pass = Graph.AddPass(Mip == 0 ? "SSR Hi-Z 복사" : "SSR Hi-Z 축소");
		if (Mip == 0)
		{
			Pass.Read(Refs.SceneDepth, ERGAccess::SrvNonPixel);
		}
		else
		{
			Pass.Write(HizRef, ERGAccess::Uav, FRGSubresourceRange::Mip(Mip - 1)); // 원본 밉 (UAV로 읽음)
		}
		Pass.Write(HizRef, ERGAccess::Uav, FRGSubresourceRange::Mip(Mip), true)
			.Timer(Timer)
			.Execute([this, Hiz, Mip, Constants, DepthSrv](FRGContext& Context) {
				ID3D12GraphicsCommandList* CommandList = Context.CommandList;
				CommandList->SetComputeRootSignature(HizRoot.Get());
				CommandList->SetPipelineState(Mip == 0 ? HizCopyPipeline.Get() : HizDownsamplePipeline.Get());
				CommandList->SetComputeRootDescriptorTable(HizParam_Depth, DepthSrv.Gpu);
				CommandList->SetComputeRoot32BitConstants(HizParam_Constants, 4, Constants.data(), 0);
				CommandList->SetComputeRootDescriptorTable(HizParam_Dest, Hiz->Uavs[Mip].Gpu);
				CommandList->SetComputeRootDescriptorTable(HizParam_Source, Hiz->Uavs[Mip == 0 ? 0 : Mip - 1].Gpu);
				CommandList->Dispatch((Constants[2] + 7) / 8, (Constants[3] + 7) / 8, 1);
			});
		SourceWidth  = DestWidth;
		SourceHeight = DestHeight;
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
	Constants.ProjectionScale = Inputs.Projection.M[1][1] * static_cast<float>(Height) * 0.5f;
	Constants.MaxRoughness  = Inputs.MaxRoughness;
	Constants.MaxBlurRadius = FMath::Max(Inputs.MaxBlurRadius, 0.0f);
	Constants.bDecals       = Inputs.bDecals ? 1u : 0u;
	const D3D12_GPU_VIRTUAL_ADDRESS Address = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;

	// 색 + 반사 움직임 (전체 화면 삼각형이 모든 픽셀을 쓰므로 지우지 않는다)
	Graph.AddPass("SSR 추적")
		.Read(Refs.SceneDepth, ERGAccess::SrvPixel)
		.Read(HizRef, ERGAccess::SrvPixel)
		.Read(Refs.SceneNormal, ERGAccess::SrvPixel)
		.Read(Refs.ColorSource, ERGAccess::SrvPixel)
		.Read(Refs.DecalNormal, ERGAccess::SrvPixel)
		.Read(Refs.DecalMaterial, ERGAccess::SrvPixel)
		.Write(ResultRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
		.Write(MotionRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, Result, Motion, Hiz, Address, DepthSrv, NormalSrv, ColorSrv, DecalNormal, DecalMat, Width, Height](FRGContext& Context) {
			const D3D12_CPU_DESCRIPTOR_HANDLE TraceTargets[] = { Result->GetRtv(), Motion->GetRtv() };
			Context.CommandList->OMSetRenderTargets(2, TraceTargets, FALSE, nullptr);
			DrawScreenPass(Context.CommandList, *Root, TracePipeline, Address, { DepthSrv, Hiz->Srv, NormalSrv, ColorSrv, DecalNormal, DecalMat }, Width,
			               Height);
		});

	// 3) 거칠기 흐림: 추적이 낸 픽셀별 원뿔 반경 안의 원판 평균 (한 패스, 결정적, 같은 면만 섞음)
	FSsrResolveConstants PassConstants;
	PassConstants.ScreenSize    = Constants.ScreenSize;
	PassConstants.CurrentWeight = ResolveCurrentWeight;
	PassConstants.VarianceGamma = ResolveVarianceGamma;
	PassConstants.bDecals       = Constants.bDecals;
	const D3D12_GPU_VIRTUAL_ADDRESS BlurAddress = Rhi->GetDynamicBuffer().AllocateConstants(PassConstants).GpuAddress;
	Graph.AddPass("SSR 흐림")
		.Read(ResultRef, ERGAccess::SrvPixel)
		.Read(MotionRef, ERGAccess::SrvPixel)
		.Read(Refs.SceneNormal, ERGAccess::SrvPixel)
		.Read(Refs.DecalNormal, ERGAccess::SrvPixel)
		.Read(Refs.DecalMaterial, ERGAccess::SrvPixel)
		.Write(BlurredRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, Result, Motion, Blurred, BlurAddress, NormalSrv, DecalNormal, DecalMat, Width, Height](FRGContext& Context) {
			const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = Blurred->GetRtv();
			Context.CommandList->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
			DrawScreenPass(Context.CommandList, *Root, BlurPipeline, BlurAddress,
			               { Result->Srv, FD3D12DescriptorHandle{}, FD3D12DescriptorHandle{}, Motion->Srv, NormalSrv, DecalNormal, DecalMat }, Width,
			               Height);
		});

	// 4) 시간 누적: 지난 프레임에 연속으로 누적했고 씬 렌더러 이력도 유효할 때만 이력을 쓴다.
	//    깊이 버퍼 픽셀 단위 교차라 반사 윤곽이 계단지고(특히 곡면·스치는 각) TAA 지터마다 계단이 옮겨 다닌다 — 정지 화면은 TAA가 평균내지만
	//    움직이면 TAA가 반사 이력을 버리므로(표면 움직임 ≠ 반사 내용 움직임) 여기서 반사 움직임으로 재투영해 누적한다
	const uint64 FrameNumber   = Rhi->GetFrameNumber();
	const bool   bHistoryValid = Inputs.bHistoryValid && LastResolveFrame != 0 && LastResolveFrame + 1 == FrameNumber;
	const FD3D12RenderTarget& Previous = *History[HistoryIndex];
	HistoryIndex ^= 1u;
	const FD3D12RenderTarget& Current = *History[HistoryIndex];
	PassConstants.bHistoryValid                     = bHistoryValid ? 1u : 0u;
	const D3D12_GPU_VIRTUAL_ADDRESS ResolveAddress = Rhi->GetDynamicBuffer().AllocateConstants(PassConstants).GpuAddress;
	const FRGResourceRef            PreviousRef    = Graph.ImportColor("SsrHistoryPrevious", Previous);
	const FRGResourceRef            CurrentRef     = Graph.ImportColor("SsrHistory", Current);
	const FD3D12DescriptorHandle    PreviousSrv    = Previous.GetSrv();
	Graph.AddPass("SSR 누적")
		.Read(BlurredRef, ERGAccess::SrvPixel)
		.Read(PreviousRef, ERGAccess::SrvPixel)
		.Read(Refs.Velocity, ERGAccess::SrvPixel)
		.Read(MotionRef, ERGAccess::SrvPixel)
		.Write(CurrentRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, &Current, Blurred, Motion, ResolveAddress, PreviousSrv, VelocitySrv, Width, Height](FRGContext& Context) {
			Current.Bind(Context.CommandList, nullptr);
			DrawScreenPass(Context.CommandList, *Root, ResolvePipeline, ResolveAddress, { Blurred->Srv, PreviousSrv, VelocitySrv, Motion->Srv }, Width,
			               Height);
		});
	LastResolveFrame = FrameNumber;
	return CurrentRef;
}