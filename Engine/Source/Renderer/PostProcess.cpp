#include "Renderer/PostProcess.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/PostProcessMath.h"

#include <cmath>
#include <format>

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

	constexpr uint32 PostRootConstantCount = 8;

	struct FTonemapConstants
	{
		float  ExposureEV        = 0.0f;
		uint32 TonemapOperator   = 1;
		float  BloomIntensity    = 0.0f;
		uint32 bAutoExposure     = 0;
		float  AutoExposureMinEV = -4.0f;
		float  AutoExposureMaxEV = 6.0f;
		float  Padding[2]        = {};
	};
	static_assert(sizeof(FTonemapConstants) == PostRootConstantCount * 4);

	struct FBloomConstants
	{
		float  SourceTexelSize[2] = {};
		uint32 bFirstPass         = 0;
		float  Threshold          = 1.0f;
		float  Knee               = 0.5f;
		float  FilterRadius       = 1.0f;
		float  Padding[2]         = {};
	};
	static_assert(sizeof(FBloomConstants) == PostRootConstantCount * 4);

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
	static_assert(sizeof(FAutoExposureConstants) == PostRootConstantCount * 4);

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
	const uint32 SourceIndex    = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) });
	const uint32 Source2Index   = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1) });
	const uint32 HistogramIndex = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0) });
	const uint32 LuminanceIndex = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1) });
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
	return GetTonemapPipeline(FD3D12RHI::RenderTargetFormat) != nullptr;
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
	TonemapPipelines.clear();
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

bool FPostProcessor::CreateTonemapPipeline(FD3D12PipelineState& OutPipeline, DXGI_FORMAT OutputFormat, bool bForceRecompile)
{
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Tonemap.hlsl";
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
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), PsoDesc, L"TonemapPipeline");
}

FD3D12PipelineState* FPostProcessor::GetTonemapPipeline(DXGI_FORMAT OutputFormat)
{
	if (auto Found = TonemapPipelines.find(OutputFormat); Found != TonemapPipelines.end())
	{
		return &Found->second;
	}
	FD3D12PipelineState& Pipeline = TonemapPipelines[OutputFormat];
	if (!CreateTonemapPipeline(Pipeline, OutputFormat, false))
	{
		TonemapPipelines.erase(OutputFormat);
		E_LOG(LogRenderer, Error, "톤매핑 파이프라인 생성 실패 (포맷 {})", static_cast<int32>(OutputFormat));
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

	bool bForceTonemap = bForceRecompile;
	for (auto& [Format, Pipeline] : TonemapPipelines)
	{
		FD3D12PipelineState NewPipeline;
		if (!CreateTonemapPipeline(NewPipeline, Format, bForceTonemap))
		{
			bAllOk = false;
			continue;
		}
		Pipeline.Swap(NewPipeline);
		Rhi->DeferRelease(NewPipeline.Detach());
		bForceTonemap = false; // 같은 셰이더를 포맷마다 다시 쿠킹하지 않는다
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

void FPostProcessor::TransitionExposureBuffers(ID3D12GraphicsCommandList* CommandList, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
{
	const D3D12_RESOURCE_BARRIER Barriers[] = {
		MakeTransitionBarrier(HistogramBuffer.Get(), Before, After),
		MakeTransitionBarrier(LuminanceBuffer.Get(), Before, After),
	};
	CommandList->ResourceBarrier(2, Barriers);
}

// ---------------------------------------------------------------- 렌더링

void FPostProcessor::RenderBloom(ID3D12GraphicsCommandList* CommandList, const FD3D12DescriptorHandle& SceneColor, uint32 Width, uint32 Height,
                                 const FPostProcessSettings& Settings)
{
	EnsureBloomTargets(Width, Height);
	if (BloomTargets.empty())
	{
		return;
	}

	CommandList->SetGraphicsRootSignature(RootSignature.Get());

	// 다운샘플: 씬 → 레벨 0 (Karis + 임계값) → 레벨 1 → ...
	CommandList->SetPipelineState(Pipelines[static_cast<size_t>(EPipeline::BloomDownsample)].Get());
	for (size_t Level = 0; Level < BloomTargets.size(); ++Level)
	{
		const bool                    bFirst       = Level == 0;
		const FD3D12DescriptorHandle& Source       = bFirst ? SceneColor : BloomTargets[Level - 1]->GetSrv();
		const uint32                  SourceWidth  = bFirst ? Width : BloomTargets[Level - 1]->GetWidth();
		const uint32                  SourceHeight = bFirst ? Height : BloomTargets[Level - 1]->GetHeight();

		FBloomConstants Constants;
		Constants.SourceTexelSize[0] = 1.0f / static_cast<float>(SourceWidth);
		Constants.SourceTexelSize[1] = 1.0f / static_cast<float>(SourceHeight);
		Constants.bFirstPass         = bFirst ? 1u : 0u;
		Constants.Threshold          = FMath::Max(Settings.BloomThreshold, 0.0f);
		Constants.Knee               = FMath::Clamp(Settings.BloomKnee, 0.0f, 1.0f);

		BloomTargets[Level]->Begin(CommandList, nullptr); // 전체를 덮어쓰므로 클리어 불필요
		CommandList->SetGraphicsRoot32BitConstants(PostRoot_Constants, PostRootConstantCount, &Constants, 0);
		CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source, Source.Gpu);
		DrawFullscreen(CommandList);
		BloomTargets[Level]->End(CommandList);
	}

	// 업샘플: 가장 작은 레벨부터 한 단계 큰 레벨에 텐트 필터로 가산
	CommandList->SetPipelineState(Pipelines[static_cast<size_t>(EPipeline::BloomUpsample)].Get());
	for (size_t Level = BloomTargets.size() - 1; Level > 0; --Level)
	{
		const FD3D12RenderTarget& Source = *BloomTargets[Level];

		FBloomConstants Constants;
		Constants.SourceTexelSize[0] = 1.0f / static_cast<float>(Source.GetWidth());
		Constants.SourceTexelSize[1] = 1.0f / static_cast<float>(Source.GetHeight());
		Constants.FilterRadius       = 1.0f;

		BloomTargets[Level - 1]->Begin(CommandList, nullptr); // 다운샘플 결과 위에 누적
		CommandList->SetGraphicsRoot32BitConstants(PostRoot_Constants, PostRootConstantCount, &Constants, 0);
		CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source, Source.GetSrv().Gpu);
		DrawFullscreen(CommandList);
		BloomTargets[Level - 1]->End(CommandList);
	}
}

void FPostProcessor::RenderAutoExposure(ID3D12GraphicsCommandList* CommandList, const FD3D12DescriptorHandle& SceneColor, uint32 Width,
                                        uint32 Height, float DeltaSeconds, const FPostProcessSettings& Settings)
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

	// 1) 히스토그램: 렌더 타깃 없이 1/4 해상도로 픽셀 셰이더 실행
	CommandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
	SetFullscreenViewport(CommandList, HistogramWidth, HistogramHeight);
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(Pipelines[static_cast<size_t>(EPipeline::Histogram)].Get());
	CommandList->SetGraphicsRoot32BitConstants(PostRoot_Constants, PostRootConstantCount, &Constants, 0);
	CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source, SceneColor.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(PostRoot_Histogram, HistogramUav.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(PostRoot_Luminance, LuminanceUav.Gpu);
	DrawFullscreen(CommandList);

	const D3D12_RESOURCE_BARRIER HistogramBarrier = MakeUavBarrier(HistogramBuffer.Get());
	CommandList->ResourceBarrier(1, &HistogramBarrier);

	// 2) 평균 + 시간 적응 (히스토그램은 컴퓨트가 다시 0으로 비운다)
	CommandList->SetComputeRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(Pipelines[static_cast<size_t>(EPipeline::AverageLuminance)].Get());
	CommandList->SetComputeRoot32BitConstants(PostRoot_Constants, PostRootConstantCount, &Constants, 0);
	CommandList->SetComputeRootDescriptorTable(PostRoot_Histogram, HistogramUav.Gpu);
	CommandList->SetComputeRootDescriptorTable(PostRoot_Luminance, LuminanceUav.Gpu);
	CommandList->Dispatch(1, 1, 1);

	const D3D12_RESOURCE_BARRIER UavBarriers[] = { MakeUavBarrier(HistogramBuffer.Get()), MakeUavBarrier(LuminanceBuffer.Get()) };
	CommandList->ResourceBarrier(2, UavBarriers);
}

void FPostProcessor::Render(ID3D12GraphicsCommandList* CommandList, const FD3D12DescriptorHandle& HdrSceneColor, const FRenderOutput& Output,
                            const FPostProcessSettings& Settings)
{
	E_CHECKF(Output.IsValid(), "포스트 프로세스 출력 대상이 유효하지 않습니다");

	FD3D12PipelineState* TonemapPipeline = GetTonemapPipeline(Output.Format);
	if (TonemapPipeline == nullptr)
	{
		return;
	}

	// 자동 노출 적응용 경과 시간 (첫 호출/긴 정지 후에는 과도한 점프를 막기 위해 제한)
	const auto  Now          = std::chrono::steady_clock::now();
	const float DeltaSeconds = bHasLastRenderTime ? FMath::Clamp(std::chrono::duration<float>(Now - LastRenderTime).count(), 0.0f, 0.25f) : 0.0f;
	LastRenderTime           = Now;
	bHasLastRenderTime       = true;

	// 버퍼는 명령 목록 사이에서 COMMON으로 돌아가므로 매번 UAV로 전이했다가 끝에 복귀시킨다
	TransitionExposureBuffers(CommandList, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	const bool bBloom = Settings.bBloomEnabled && Settings.BloomIntensity > 0.0f;
	if (bBloom)
	{
		RenderBloom(CommandList, HdrSceneColor, Output.Width, Output.Height, Settings);
	}
	if (Settings.bAutoExposure)
	{
		RenderAutoExposure(CommandList, HdrSceneColor, Output.Width, Output.Height, DeltaSeconds, Settings);
	}

	// 톤매핑 → 출력
	FTonemapConstants Constants;
	Constants.ExposureEV        = Settings.ExposureEV;
	Constants.TonemapOperator   = static_cast<uint32>(Settings.Tonemapper);
	Constants.BloomIntensity    = (bBloom && !BloomTargets.empty()) ? Settings.BloomIntensity : 0.0f;
	Constants.bAutoExposure     = Settings.bAutoExposure ? 1u : 0u;
	Constants.AutoExposureMinEV = FMath::Min(Settings.AutoExposureMinEV, Settings.AutoExposureMaxEV);
	Constants.AutoExposureMaxEV = FMath::Max(Settings.AutoExposureMinEV, Settings.AutoExposureMaxEV);

	// 블룸이 없으면 t1에 씬을 바인딩해 둔다 (강도 0이라 샘플링되지 않음)
	const FD3D12DescriptorHandle& BloomSource = Constants.BloomIntensity > 0.0f ? BloomTargets[0]->GetSrv() : HdrSceneColor;

	CommandList->OMSetRenderTargets(1, &Output.Rtv, FALSE, nullptr);
	SetFullscreenViewport(CommandList, Output.Width, Output.Height);
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(TonemapPipeline->Get());
	CommandList->SetGraphicsRoot32BitConstants(PostRoot_Constants, PostRootConstantCount, &Constants, 0);
	CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source, HdrSceneColor.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(PostRoot_Source2, BloomSource.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(PostRoot_Histogram, HistogramUav.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(PostRoot_Luminance, LuminanceUav.Gpu);
	DrawFullscreen(CommandList);

	TransitionExposureBuffers(CommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
}
