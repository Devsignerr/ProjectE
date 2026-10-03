#include "Renderer/PostProcess.h"

#include "Core/FrameTime.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/PostProcessMath.h"

#include <cmath>
#include <format>
#include <iterator>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// 모든 포스트 패스가 공유하는 루트 시그니처 레이아웃
	enum EPostRootParameter : uint32
	{
		PostRoot_Constants = 0, // b0 (루트 상수 8개)
		PostRoot_Source    = 1, // t0
		PostRoot_Source2   = 2, // t1
		PostRoot_Histogram = 3, // u0
		PostRoot_Luminance = 4, // u1
	};

	constexpr uint32 PostRootConstantCount = 16; // 패스별 상수 구조체는 이 크기 이하

	struct FTonemapConstants
	{
		float  ExposureEV        = 0.0f;
		uint32 TonemapOperator   = 1;
		float  BloomIntensity    = 0.0f;
		uint32 bAutoExposure     = 0;
		float  AutoExposureMinEV = -4.0f;
		float  AutoExposureMaxEV = 6.0f;
		float  Sharpness         = 0.0f; // TAA 샤프닝 (0 = 끔)
		float  HdrPeakRatio      = 0.0f; // HDR 출력 하이라이트 상한 (0 = SDR)
	};
	static_assert(sizeof(FTonemapConstants) <= PostRootConstantCount * 4 && sizeof(FTonemapConstants) % 4 == 0);

	struct FBloomConstants
	{
		float  SourceTexelSize[2] = {};
		uint32 bFirstPass         = 0;
		float  Threshold          = 1.0f;
		float  Knee               = 0.5f;
		float  FilterRadius       = 1.0f;
		float  Padding[2]         = {};
	};
	static_assert(sizeof(FBloomConstants) <= PostRootConstantCount * 4 && sizeof(FBloomConstants) % 4 == 0);

	struct FAutoExposureConstants
	{
		float MinLog2Luminance      = 0.0f;
		float InvLog2LuminanceRange = 0.0f;
		float Log2LuminanceRange    = 0.0f;
		float PixelCount            = 0.0f;
		float DeltaSeconds          = 0.0f;
		float AdaptationSpeed       = 0.0f;
		float Padding[2]            = {};
	};
	static_assert(sizeof(FAutoExposureConstants) <= PostRootConstantCount * 4 && sizeof(FAutoExposureConstants) % 4 == 0);

	// PixelArt.hlsl cbuffer와 1:1
	struct FPixelArtConstants
	{
		float  OutputSize[2]     = {};
		float  PixelSize         = 1.0f;
		float  OutlineStrength   = 0.0f;
		float  SubPixelOffset[2] = {};
		float  HighlightStrength = 0.0f;
		float  DepthThreshold    = 1.0f;
		uint32 DitherOrigin[2]   = {};
		float  ColorLevels       = 0.0f;
		float  DitherStrength    = 0.0f;
		uint32 bOrthographic     = 0;
		float  NearZ             = 1.0f;
		float  FarZ              = 2.0f;
		float  PixelViewScale    = 1.0f;
	};
	static_assert(sizeof(FPixelArtConstants) == PostRootConstantCount * 4);

	template <typename T>
	void SetGraphicsConstants(ID3D12GraphicsCommandList* CommandList, const T& Constants)
	{
		CommandList->SetGraphicsRoot32BitConstants(PostRoot_Constants, sizeof(T) / 4, &Constants, 0);
	}

	template <typename T>
	void SetComputeConstants(ID3D12GraphicsCommandList* CommandList, const T& Constants)
	{
		CommandList->SetComputeRoot32BitConstants(PostRoot_Constants, sizeof(T) / 4, &Constants, 0);
	}

	struct FPipelineInfo
	{
		const wchar_t* File;
		const wchar_t* PixelOrComputeEntry;
		const wchar_t* DebugName;
		bool           bCompute;
	};

	const FPipelineInfo GPipelineInfos[] = {
		{ L"Bloom.hlsl", L"PSDownsample", L"BloomDownsamplePipeline", false },
		{ L"Bloom.hlsl", L"PSUpsample", L"BloomUpsamplePipeline", false },
		{ L"AutoExposure.hlsl", L"PSHistogram", L"LuminanceHistogramPipeline", false },
		{ L"AutoExposure.hlsl", L"CSAverage", L"AverageLuminancePipeline", true },
	};

	void SetFullscreenViewport(ID3D12GraphicsCommandList* CommandList, uint32 Width, uint32 Height)
	{
		const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 0.0f, 1.0f };
		const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height) };
		CommandList->RSSetViewports(1, &Viewport);
		CommandList->RSSetScissorRects(1, &Scissor);
	}

	// 셰이더 바이트코드 얻기 (bForceRecompile이면 캐시·쿠킹 파일을 무시하고 다시 컴파일)
	ComPtr<IDxcBlob> LoadShader(FShaderLibrary& Library, const FShaderCompileDesc& Desc, bool bForceRecompile)
	{
		if (bForceRecompile && !Library.CookShader(Desc))
		{
			return nullptr;
		}
		return Library.GetShader(Desc);
	}

	void DrawFullscreen(ID3D12GraphicsCommandList* CommandList)
	{
		CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		CommandList->DrawInstanced(3, 1, 0, 0);
	}
} // namespace

FPostProcessor::FPostProcessor()  = default;
FPostProcessor::~FPostProcessor() = default;

bool FPostProcessor::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary)
{
	E_CHECKF(Rhi == nullptr, "포스트 프로세서가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;

	// 루트 시그니처 (그래픽스/컴퓨트 공용이므로 가시성은 ALL)
	const uint32 ConstantsIndex = RootSignature.AddConstants(PostRootConstantCount, 0);
	const uint32 SourceIndex    = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
	const uint32 Source2Index   = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
	const uint32 HistogramIndex = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
	const uint32 LuminanceIndex = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
	E_CHECK(ConstantsIndex == PostRoot_Constants && SourceIndex == PostRoot_Source && Source2Index == PostRoot_Source2 &&
	        HistogramIndex == PostRoot_Histogram && LuminanceIndex == PostRoot_Luminance);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
	                                                                      D3D12_SHADER_VISIBILITY_ALL));
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
	                                                                      D3D12_SHADER_VISIBILITY_ALL));
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"PostProcessRootSignature"))
	{
		return false;
	}

	for (uint32 Index = 0; Index < static_cast<uint32>(EPipeline::Count); ++Index)
	{
		if (!CreatePipeline(static_cast<EPipeline>(Index), Pipelines[Index], false))
		{
			return false;
		}
	}

	if (!CreateExposureBuffers())
	{
		return false;
	}

	// 가장 흔한 출력 포맷(sRGB 백버퍼/뷰포트)은 미리 만들어 초기화 실패를 조기에 드러낸다
	return GetOutputPipeline(EOutputPass::Tonemap, FD3D12RHI::RenderTargetFormat) != nullptr;
}

void FPostProcessor::Shutdown()
{
	if (Rhi != nullptr)
	{
		FD3D12DescriptorAllocator& Allocator = Rhi->GetSrvAllocator();
		Allocator.Free(HistogramUav);
		Allocator.Free(LuminanceUav);
	}
	HistogramBuffer.Reset();
	LuminanceBuffer.Reset();
	BloomTargets.clear();
	BloomSourceWidth  = 0;
	BloomSourceHeight = 0;
	for (auto& PassPipelines : OutputPipelines)
	{
		PassPipelines.clear();
	}
	for (FD3D12PipelineState& Pipeline : Pipelines)
	{
		Pipeline.Shutdown();
	}
	RootSignature.Shutdown();
	bHasLastRenderTime = false;
	Rhi                = nullptr;
	ShaderLibrary      = nullptr;
}

// ---------------------------------------------------------------- 파이프라인

bool FPostProcessor::CreatePipeline(EPipeline Pipeline, FD3D12PipelineState& OutPipeline, bool bForceRecompile)
{
	const FPipelineInfo& Info = GPipelineInfos[static_cast<size_t>(Pipeline)];
	ID3D12Device*        Device = Rhi->GetDevice().GetDevice();

	FShaderCompileDesc MainDesc;
	MainDesc.FileName   = Info.File;
	MainDesc.EntryPoint = Info.PixelOrComputeEntry;
	MainDesc.Stage      = Info.bCompute ? EShaderStage::Compute : EShaderStage::Pixel;

	const ComPtr<IDxcBlob> MainShader = LoadShader(*ShaderLibrary, MainDesc, bForceRecompile);
	if (!MainShader)
	{
		return false;
	}

	if (Info.bCompute)
	{
		return OutPipeline.InitCompute(Device, RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(MainShader.Get()), Info.DebugName);
	}

	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = Info.File;
	VertexDesc.EntryPoint = L"VSMain";
	VertexDesc.Stage      = EShaderStage::Vertex;
	const ComPtr<IDxcBlob> VertexShader = LoadShader(*ShaderLibrary, VertexDesc, bForceRecompile);
	if (!VertexShader)
	{
		return false;
	}

	FGraphicsPipelineDesc PsoDesc;
	PsoDesc.RootSignature = RootSignature.Get();
	PsoDesc.VertexShader  = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	PsoDesc.PixelShader   = FD3D12ShaderCompiler::ToBytecode(MainShader.Get());
	PsoDesc.CullMode      = D3D12_CULL_MODE_NONE;
	PsoDesc.bDepthEnable  = false;

	switch (Pipeline)
	{
	case EPipeline::BloomDownsample:
		PsoDesc.RenderTargetFormats[0] = FRenderTargetDesc::MakeHdr(false).RtvFormat;
		break;
	case EPipeline::BloomUpsample:
		PsoDesc.RenderTargetFormats[0] = FRenderTargetDesc::MakeHdr(false).RtvFormat;
		PsoDesc.BlendMode              = EBlendMode::Additive; // 한 단계 큰 레벨에 누적
		break;
	case EPipeline::Histogram:
		PsoDesc.NumRenderTargets = 0; // UAV에만 기록
		break;
	default:
		break;
	}
	return OutPipeline.InitGraphics(Device, PsoDesc, Info.DebugName);
}

bool FPostProcessor::CreateOutputPipeline(EOutputPass Pass, FD3D12PipelineState& OutPipeline, DXGI_FORMAT OutputFormat, bool bForceRecompile)
{
	const wchar_t* const Files[]      = { L"Tonemap.hlsl", L"PixelArt.hlsl", L"ScreenDebug.hlsl" };
	const wchar_t* const DebugNames[] = { L"TonemapPipeline", L"PixelArtCompositePipeline", L"ScreenDebugPipeline" };
	static_assert(std::size(Files) == static_cast<size_t>(EOutputPass::Count));
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = Files[static_cast<size_t>(Pass)];
	VertexDesc.EntryPoint = L"VSMain";
	VertexDesc.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc PixelDesc = VertexDesc;
	PixelDesc.EntryPoint         = L"PSMain";
	PixelDesc.Stage              = EShaderStage::Pixel;

	const ComPtr<IDxcBlob> VertexShader = LoadShader(*ShaderLibrary, VertexDesc, bForceRecompile);
	const ComPtr<IDxcBlob> PixelShader  = LoadShader(*ShaderLibrary, PixelDesc, bForceRecompile);
	if (!VertexShader || !PixelShader)
	{
		return false;
	}

	FGraphicsPipelineDesc PsoDesc;
	PsoDesc.RootSignature          = RootSignature.Get();
	PsoDesc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	PsoDesc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	PsoDesc.RenderTargetFormats[0] = OutputFormat;
	PsoDesc.CullMode               = D3D12_CULL_MODE_NONE;
	PsoDesc.bDepthEnable           = false;
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), PsoDesc, DebugNames[static_cast<size_t>(Pass)]);
}

FD3D12PipelineState* FPostProcessor::GetOutputPipeline(EOutputPass Pass, DXGI_FORMAT OutputFormat)
{
	auto& PassPipelines = OutputPipelines[static_cast<size_t>(Pass)];
	if (auto Found = PassPipelines.find(OutputFormat); Found != PassPipelines.end())
	{
		return &Found->second;
	}
	FD3D12PipelineState& Pipeline = PassPipelines[OutputFormat];
	if (!CreateOutputPipeline(Pass, Pipeline, OutputFormat, false))
	{
		PassPipelines.erase(OutputFormat);
		E_LOG(LogRenderer, Error, "출력 패스 {} 파이프라인 생성 실패 (포맷 {})", static_cast<int32>(Pass), static_cast<int32>(OutputFormat));
		return nullptr;
	}
	return &Pipeline;
}

bool FPostProcessor::ReloadShaders(bool bForceRecompile)
{
	bool bAllOk = true;

	for (uint32 Index = 0; Index < static_cast<uint32>(EPipeline::Count); ++Index)
	{
		FD3D12PipelineState NewPipeline;
		if (!CreatePipeline(static_cast<EPipeline>(Index), NewPipeline, bForceRecompile))
		{
			bAllOk = false;
			continue;
		}
		Pipelines[Index].Swap(NewPipeline);
		Rhi->DeferRelease(NewPipeline.Detach());
	}

	for (size_t PassIndex = 0; PassIndex < static_cast<size_t>(EOutputPass::Count); ++PassIndex)
	{
		bool bForcePass = bForceRecompile;
		for (auto& [Format, Pipeline] : OutputPipelines[PassIndex])
		{
			FD3D12PipelineState NewPipeline;
			if (!CreateOutputPipeline(static_cast<EOutputPass>(PassIndex), NewPipeline, Format, bForcePass))
			{
				bAllOk = false;
				continue;
			}
			Pipeline.Swap(NewPipeline);
			Rhi->DeferRelease(NewPipeline.Detach());
			bForcePass = false; // 같은 셰이더를 포맷마다 다시 쿠킹하지 않는다
		}
	}
	return bAllOk;
}

// ---------------------------------------------------------------- 리소스

bool FPostProcessor::CreateExposureBuffers()
{
	ID3D12Device*               Device      = Rhi->GetDevice().GetDevice();
	const D3D12_HEAP_PROPERTIES DefaultHeap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);

	// 커밋 리소스는 0으로 초기화되어 생성된다 (히스토그램 초기 비움, 휘도 0 = "이전 값 없음")
	const D3D12_RESOURCE_DESC HistogramDesc = MakeBufferDesc(FPostProcessMath::HistogramBinCount * sizeof(uint32), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	E_D3D_VERIFY(Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &HistogramDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
	                                             IID_PPV_ARGS(&HistogramBuffer)));
	HistogramBuffer->SetName(L"LuminanceHistogram");

	const D3D12_RESOURCE_DESC LuminanceDesc = MakeBufferDesc(sizeof(float), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	E_D3D_VERIFY(Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &LuminanceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
	                                             IID_PPV_ARGS(&LuminanceBuffer)));
	LuminanceBuffer->SetName(L"AdaptedLuminance");

	FD3D12DescriptorAllocator& Allocator = Rhi->GetSrvAllocator();

	D3D12_UNORDERED_ACCESS_VIEW_DESC HistogramUavDesc{};
	HistogramUavDesc.Format              = DXGI_FORMAT_R32_TYPELESS;
	HistogramUavDesc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
	HistogramUavDesc.Buffer.NumElements  = FPostProcessMath::HistogramBinCount;
	HistogramUavDesc.Buffer.Flags        = D3D12_BUFFER_UAV_FLAG_RAW;
	HistogramUav                         = Allocator.Allocate();
	Device->CreateUnorderedAccessView(HistogramBuffer.Get(), nullptr, &HistogramUavDesc, HistogramUav.Cpu);

	D3D12_UNORDERED_ACCESS_VIEW_DESC LuminanceUavDesc{};
	LuminanceUavDesc.Format                     = DXGI_FORMAT_UNKNOWN;
	LuminanceUavDesc.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;
	LuminanceUavDesc.Buffer.NumElements         = 1;
	LuminanceUavDesc.Buffer.StructureByteStride = sizeof(float);
	LuminanceUav                                = Allocator.Allocate();
	Device->CreateUnorderedAccessView(LuminanceBuffer.Get(), nullptr, &LuminanceUavDesc, LuminanceUav.Cpu);
	return true;
}

void FPostProcessor::EnsureBloomTargets(uint32 Width, uint32 Height)
{
	const uint32 MipCount = FPostProcessMath::GetBloomMipCount(Width, Height);
	if (BloomSourceWidth == Width && BloomSourceHeight == Height && BloomTargets.size() == MipCount)
	{
		return;
	}

	// 이전 레벨은 진행 중인 프레임이 참조할 수 있으므로 지연 해제
	for (std::unique_ptr<FD3D12RenderTarget>& Target : BloomTargets)
	{
		Target->ShutdownDeferred(*Rhi);
	}
	BloomTargets.clear();

	for (uint32 Level = 0; Level < MipCount; ++Level)
	{
		auto Target = std::make_unique<FD3D12RenderTarget>();
		if (!Target->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), FPostProcessMath::GetBloomMipDimension(Width, Level),
		                  FPostProcessMath::GetBloomMipDimension(Height, Level), std::format(L"BloomMip_{}", Level).c_str(),
		                  FRenderTargetDesc::MakeHdr(false)))
		{
			E_LOG(LogRenderer, Error, "블룸 버퍼 생성 실패 (레벨 {})", Level);
			BloomTargets.clear();
			break;
		}
		BloomTargets.push_back(std::move(Target));
	}
	BloomSourceWidth  = Width;
	BloomSourceHeight = Height;
}

// ---------------------------------------------------------------- 렌더 그래프 패스

namespace
{
	// 출력이 그래프 리소스면 쓰기 선언, 아니면(추적 안 하는 외부 RTV) 부수 효과 패스
	void DeclareOutput(FRenderGraph::FPassBuilder& Pass, const FPostProcessGraphOutput& Output)
	{
		if (Output.Ref.IsValid())
		{
			Pass.Write(Output.Ref, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true);
		}
		else
		{
			Pass.NeverCull();
		}
	}
} // namespace

void FPostProcessor::AddBloomPasses(FRenderGraph& Graph, const FPostProcessGraphInput& SceneColor, uint32 Width, uint32 Height,
                                    const FPostProcessSettings& Settings, int32 Timer, FRGResourceRef& OutBloom)
{
	EnsureBloomTargets(Width, Height);
	if (BloomTargets.empty())
	{
		return;
	}
	std::vector<FRGResourceRef> Levels(BloomTargets.size());
	for (size_t Level = 0; Level < BloomTargets.size(); ++Level)
	{
		Levels[Level] = Graph.ImportColor("BloomMip", *BloomTargets[Level]);
	}

	// 다운샘플: 씬 → 레벨 0 (Karis + 임계값) → 레벨 1 → ...
	for (size_t Level = 0; Level < BloomTargets.size(); ++Level)
	{
		const bool                   bFirst       = Level == 0;
		const FD3D12DescriptorHandle Source       = bFirst ? SceneColor.Srv : BloomTargets[Level - 1]->GetSrv();
		const uint32                 SourceWidth  = bFirst ? Width : BloomTargets[Level - 1]->GetWidth();
		const uint32                 SourceHeight = bFirst ? Height : BloomTargets[Level - 1]->GetHeight();

		FBloomConstants Constants;
		Constants.SourceTexelSize[0] = 1.0f / static_cast<float>(SourceWidth);
		Constants.SourceTexelSize[1] = 1.0f / static_cast<float>(SourceHeight);
		Constants.bFirstPass         = bFirst ? 1u : 0u;
		Constants.Threshold          = FMath::Max(Settings.BloomThreshold, 0.0f);
		Constants.Knee               = FMath::Clamp(Settings.BloomKnee, 0.0f, 1.0f);

		FD3D12RenderTarget* Target = BloomTargets[Level].get();
		Graph.AddPass("블룸 다운샘플")
			.Read(bFirst ? SceneColor.Ref : Levels[Level - 1], ERGAccess::SrvPixel)
			.Write(Levels[Level], ERGAccess::RenderTarget, FRGSubresourceRange::All(), true) // 전체를 덮어쓴다
			.Timer(Timer)
			.Execute([this, Target, Constants, Source](FRGContext& Context) {
				ID3D12GraphicsCommandList* CommandList = Context.CommandList;
				Target->Bind(CommandList, nullptr);
				CommandList->SetGraphicsRootSignature(RootSignature.Get());
				CommandList->SetPipelineState(Pipelines[static_cast<size_t>(EPipeline::BloomDownsample)].Get());
				SetGraphicsConstants(CommandList, Constants);
				CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source, Source.Gpu);
				DrawFullscreen(CommandList);
			});
	}

	// 업샘플: 가장 작은 레벨부터 한 단계 큰 레벨에 텐트 필터로 가산 (다운샘플 결과 위에 누적)
	for (size_t Level = BloomTargets.size() - 1; Level > 0; --Level)
	{
		const FD3D12RenderTarget& Source = *BloomTargets[Level];
		FBloomConstants           Constants;
		Constants.SourceTexelSize[0] = 1.0f / static_cast<float>(Source.GetWidth());
		Constants.SourceTexelSize[1] = 1.0f / static_cast<float>(Source.GetHeight());
		Constants.FilterRadius       = 1.0f;

		FD3D12RenderTarget*          Target    = BloomTargets[Level - 1].get();
		const FD3D12DescriptorHandle SourceSrv = Source.GetSrv();
		Graph.AddPass("블룸 업샘플")
			.Read(Levels[Level], ERGAccess::SrvPixel)
			.Write(Levels[Level - 1], ERGAccess::RenderTarget)
			.Timer(Timer)
			.Execute([this, Target, Constants, SourceSrv](FRGContext& Context) {
				ID3D12GraphicsCommandList* CommandList = Context.CommandList;
				Target->Bind(CommandList, nullptr);
				CommandList->SetGraphicsRootSignature(RootSignature.Get());
				CommandList->SetPipelineState(Pipelines[static_cast<size_t>(EPipeline::BloomUpsample)].Get());
				SetGraphicsConstants(CommandList, Constants);
				CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source, SourceSrv.Gpu);
				DrawFullscreen(CommandList);
			});
	}
	OutBloom = Levels[0];
}

void FPostProcessor::AddAutoExposurePasses(FRenderGraph& Graph, const FPostProcessGraphInput& SceneColor, uint32 Width, uint32 Height, float DeltaSeconds,
                                           const FPostProcessSettings& Settings, FRGResourceRef Histogram, FRGResourceRef Luminance, int32 Timer)
{
	const uint32 HistogramWidth  = FMath::Max(1u, Width / FPostProcessMath::HistogramDownscale);
	const uint32 HistogramHeight = FMath::Max(1u, Height / FPostProcessMath::HistogramDownscale);

	FAutoExposureConstants Constants;
	Constants.MinLog2Luminance      = FPostProcessMath::HistogramMinLog2Luminance;
	Constants.Log2LuminanceRange    = FPostProcessMath::HistogramMaxLog2Luminance - FPostProcessMath::HistogramMinLog2Luminance;
	Constants.InvLog2LuminanceRange = 1.0f / Constants.Log2LuminanceRange;
	Constants.PixelCount            = static_cast<float>(HistogramWidth) * static_cast<float>(HistogramHeight);
	Constants.DeltaSeconds          = DeltaSeconds;
	Constants.AdaptationSpeed       = FMath::Max(Settings.AdaptationSpeed, 0.0f);

	// 1) 히스토그램: 렌더 타깃 없이 1/4 해상도로 픽셀 셰이더 실행 (UAV 쓰기)
	const FD3D12DescriptorHandle Source = SceneColor.Srv;
	Graph.AddPass("자동 노출 히스토그램")
		.Read(SceneColor.Ref, ERGAccess::SrvPixel)
		.Write(Histogram, ERGAccess::Uav)
		.Write(Luminance, ERGAccess::Uav)
		.Timer(Timer)
		.Execute([this, Constants, Source, HistogramWidth, HistogramHeight](FRGContext& Context) {
			ID3D12GraphicsCommandList* CommandList = Context.CommandList;
			CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
			SetFullscreenViewport(CommandList, HistogramWidth, HistogramHeight);
			CommandList->SetGraphicsRootSignature(RootSignature.Get());
			CommandList->SetPipelineState(Pipelines[static_cast<size_t>(EPipeline::Histogram)].Get());
			SetGraphicsConstants(CommandList, Constants);
			CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source, Source.Gpu);
			CommandList->SetGraphicsRootDescriptorTable(PostRoot_Histogram, HistogramUav.Gpu);
			CommandList->SetGraphicsRootDescriptorTable(PostRoot_Luminance, LuminanceUav.Gpu);
			DrawFullscreen(CommandList);
		});

	// 2) 평균 + 시간 적응 (히스토그램은 컴퓨트가 다시 0으로 비운다). 앞 패스와의 UAV 배리어는 그래프가
	Graph.AddPass("자동 노출 평균")
		.Write(Histogram, ERGAccess::Uav)
		.Write(Luminance, ERGAccess::Uav)
		.Timer(Timer)
		.Execute([this, Constants](FRGContext& Context) {
			ID3D12GraphicsCommandList* CommandList = Context.CommandList;
			CommandList->SetComputeRootSignature(RootSignature.Get());
			CommandList->SetPipelineState(Pipelines[static_cast<size_t>(EPipeline::AverageLuminance)].Get());
			SetComputeConstants(CommandList, Constants);
			CommandList->SetComputeRootDescriptorTable(PostRoot_Histogram, HistogramUav.Gpu);
			CommandList->SetComputeRootDescriptorTable(PostRoot_Luminance, LuminanceUav.Gpu);
			CommandList->Dispatch(1, 1, 1);
		});
}

void FPostProcessor::AddPasses(FRenderGraph& Graph, const FPostProcessGraphInput& SceneColor, const FPostProcessGraphOutput& Output,
                               const FPostProcessSettings& Settings, float Sharpness, int32 Timer)
{
	E_CHECKF(Output.Output.IsValid(), "포스트 프로세스 출력 대상이 유효하지 않습니다");

	FD3D12PipelineState* TonemapPipeline = GetOutputPipeline(EOutputPass::Tonemap, Output.Output.Format);
	if (TonemapPipeline == nullptr)
	{
		return;
	}

	// 자동 노출 적응용 경과 시간 = 앱 프레임 시간 (실제 시각이 아님 — --fixed-delta 재현성). 첫 호출/긴 정지 후에는 과도한 점프를 막기 위해 제한
	const double Now          = FFrameTime::GetTotalSeconds();
	const float  DeltaSeconds = bHasLastRenderTime ? FMath::Clamp(static_cast<float>(Now - LastRenderTime), 0.0f, 0.25f) : 0.0f;
	LastRenderTime           = Now;
	bHasLastRenderTime       = true;

	// 노출 버퍼는 명령 목록 사이에서 COMMON으로 돌아가므로(버퍼 감쇠) 그래프 시작·끝을 COMMON으로 둔다
	const FRGResourceRef Histogram = Graph.Import("LuminanceHistogram", HistogramBuffer.Get(), ERGAccess::Common, ERGAccess::Common);
	const FRGResourceRef Luminance = Graph.Import("AdaptedLuminance", LuminanceBuffer.Get(), ERGAccess::Common, ERGAccess::Common);

	const uint32   Width  = Output.Output.Width;
	const uint32   Height = Output.Output.Height;
	const bool     bBloom = Settings.bBloomEnabled && Settings.BloomIntensity > 0.0f;
	FRGResourceRef BloomRef;
	if (bBloom)
	{
		AddBloomPasses(Graph, SceneColor, Width, Height, Settings, Timer, BloomRef);
	}
	if (Settings.bAutoExposure)
	{
		AddAutoExposurePasses(Graph, SceneColor, Width, Height, DeltaSeconds, Settings, Histogram, Luminance, Timer);
	}

	// 톤매핑 → 출력
	FTonemapConstants Constants;
	Constants.ExposureEV        = Settings.ExposureEV;
	Constants.TonemapOperator   = static_cast<uint32>(Settings.Tonemapper);
	Constants.BloomIntensity    = (bBloom && BloomRef.IsValid()) ? Settings.BloomIntensity : 0.0f;
	Constants.bAutoExposure     = Settings.bAutoExposure ? 1u : 0u;
	Constants.AutoExposureMinEV = FMath::Min(Settings.AutoExposureMinEV, Settings.AutoExposureMaxEV);
	Constants.AutoExposureMaxEV = FMath::Max(Settings.AutoExposureMinEV, Settings.AutoExposureMaxEV);
	Constants.Sharpness         = FMath::Clamp(Sharpness, 0.0f, 1.0f);
	Constants.HdrPeakRatio      = HdrPeakRatio;

	// 블룸이 없으면 t1에 씬을 바인딩해 둔다 (강도 0이라 샘플링되지 않음)
	const FD3D12DescriptorHandle BloomSource = Constants.BloomIntensity > 0.0f ? BloomTargets[0]->GetSrv() : SceneColor.Srv;
	const FD3D12DescriptorHandle SceneSrv    = SceneColor.Srv;
	const FRenderOutput          Target      = Output.Output;
	ID3D12PipelineState* const   Pipeline    = TonemapPipeline->Get();

	FRenderGraph::FPassBuilder Pass = Graph.AddPass("톤매핑");
	Pass.Read(SceneColor.Ref, ERGAccess::SrvPixel).Write(Histogram, ERGAccess::Uav).Write(Luminance, ERGAccess::Uav).Timer(Timer);
	if (Constants.BloomIntensity > 0.0f)
	{
		Pass.Read(BloomRef, ERGAccess::SrvPixel);
	}
	DeclareOutput(Pass, Output);
	Pass.Execute([this, Constants, BloomSource, SceneSrv, Target, Pipeline](FRGContext& Context) {
		ID3D12GraphicsCommandList* CommandList = Context.CommandList;
		CommandList->OMSetRenderTargets(1, &Target.Rtv, FALSE, nullptr);
		SetFullscreenViewport(CommandList, Target.Width, Target.Height);
		CommandList->SetGraphicsRootSignature(RootSignature.Get());
		CommandList->SetPipelineState(Pipeline);
		SetGraphicsConstants(CommandList, Constants);
		CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source, SceneSrv.Gpu);
		CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source2, BloomSource.Gpu);
		CommandList->SetGraphicsRootDescriptorTable(PostRoot_Histogram, HistogramUav.Gpu);
		CommandList->SetGraphicsRootDescriptorTable(PostRoot_Luminance, LuminanceUav.Gpu);
		DrawFullscreen(CommandList);
	});
}

void FPostProcessor::AddPixelArtCompositePass(FRenderGraph& Graph, const FPostProcessGraphInput& SourceColor, const FPostProcessGraphInput& SourceDepth,
                                              const FPostProcessGraphOutput& Output, const FPixelArtCompositeParams& Params, int32 Timer)
{
	E_CHECKF(Output.Output.IsValid(), "픽셀 아트 합성 출력 대상이 유효하지 않습니다");
	FD3D12PipelineState* Pipeline = GetOutputPipeline(EOutputPass::PixelArtComposite, Output.Output.Format);
	if (Pipeline == nullptr)
	{
		return;
	}

	FPixelArtConstants Constants;
	Constants.OutputSize[0]     = static_cast<float>(Output.Output.Width);
	Constants.OutputSize[1]     = static_cast<float>(Output.Output.Height);
	Constants.PixelSize         = static_cast<float>(FMath::Max(Params.PixelSize, 1u));
	Constants.OutlineStrength   = FMath::Clamp(Params.OutlineStrength, 0.0f, 1.0f);
	Constants.SubPixelOffset[0] = Params.SubPixelOffset.X;
	Constants.SubPixelOffset[1] = Params.SubPixelOffset.Y;
	Constants.HighlightStrength = FMath::Max(Params.HighlightStrength, 0.0f);
	Constants.DepthThreshold    = FMath::Max(Params.DepthThreshold, 0.01f);
	Constants.DitherOrigin[0]   = Params.DitherOrigin[0];
	Constants.DitherOrigin[1]   = Params.DitherOrigin[1];
	Constants.ColorLevels       = Params.ColorLevels >= 2 ? static_cast<float>(Params.ColorLevels) : 0.0f;
	Constants.DitherStrength    = FMath::Clamp(Params.DitherStrength, 0.0f, 1.0f);
	Constants.bOrthographic     = Params.bOrthographic ? 1u : 0u;
	Constants.NearZ             = Params.NearZ;
	Constants.FarZ              = Params.FarZ;
	Constants.PixelViewScale    = Params.PixelViewScale;

	const FD3D12DescriptorHandle ColorSrv = SourceColor.Srv;
	const FD3D12DescriptorHandle DepthSrv = SourceDepth.Srv;
	const FRenderOutput          Target   = Output.Output;
	ID3D12PipelineState* const   State    = Pipeline->Get();
	FRenderGraph::FPassBuilder   Pass     = Graph.AddPass("픽셀 아트 합성");
	Pass.Read(SourceColor.Ref, ERGAccess::SrvPixel).Read(SourceDepth.Ref, ERGAccess::SrvPixel).Timer(Timer);
	DeclareOutput(Pass, Output);
	Pass.Execute([this, Constants, ColorSrv, DepthSrv, Target, State](FRGContext& Context) {
		ID3D12GraphicsCommandList* CommandList = Context.CommandList;
		CommandList->OMSetRenderTargets(1, &Target.Rtv, FALSE, nullptr);
		SetFullscreenViewport(CommandList, Target.Width, Target.Height);
		CommandList->SetGraphicsRootSignature(RootSignature.Get());
		CommandList->SetPipelineState(State);
		SetGraphicsConstants(CommandList, Constants);
		CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source, ColorSrv.Gpu);
		CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source2, DepthSrv.Gpu);
		DrawFullscreen(CommandList);
	});
}

void FPostProcessor::AddDebugViewPass(FRenderGraph& Graph, const FPostProcessGraphInput& Source, const FPostProcessGraphOutput& Output, uint32 Mode, int32 Timer)
{
	FD3D12PipelineState* Pipeline = GetOutputPipeline(EOutputPass::DebugView, Output.Output.Format);
	if (Pipeline == nullptr)
	{
		return;
	}
	struct FDebugConstants
	{
		uint32 Mode          = 0;
		float  VelocityScale = 50.0f;
		float  Padding[2]    = {};
	} Constants;
	Constants.Mode = Mode;

	const FD3D12DescriptorHandle SourceSrv = Source.Srv;
	const FRenderOutput          Target    = Output.Output;
	ID3D12PipelineState* const   State     = Pipeline->Get();
	FRenderGraph::FPassBuilder   Pass      = Graph.AddPass("화면 버퍼 확인");
	Pass.Read(Source.Ref, ERGAccess::SrvPixel).Timer(Timer);
	DeclareOutput(Pass, Output);
	Pass.Execute([this, Constants, SourceSrv, Target, State](FRGContext& Context) {
		ID3D12GraphicsCommandList* CommandList = Context.CommandList;
		CommandList->OMSetRenderTargets(1, &Target.Rtv, FALSE, nullptr);
		SetFullscreenViewport(CommandList, Target.Width, Target.Height);
		CommandList->SetGraphicsRootSignature(RootSignature.Get());
		CommandList->SetPipelineState(State);
		SetGraphicsConstants(CommandList, Constants);
		CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source, SourceSrv.Gpu);
		DrawFullscreen(CommandList);
	});
}
