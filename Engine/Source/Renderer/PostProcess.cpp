#include "Renderer/PostProcess.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"

#include <cmath>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	enum ETonemapRootParameter : uint32
	{
		TonemapRoot_Constants = 0, // b0 (루트 상수 4개)
		TonemapRoot_Scene     = 1, // t0
	};

	struct FTonemapConstants
	{
		float  Exposure        = 1.0f;
		uint32 TonemapOperator = 1;
		float  Padding[2]      = {};
	};
	static_assert(sizeof(FTonemapConstants) == 16);
} // namespace

bool FPostProcessor::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary)
{
	E_CHECKF(Rhi == nullptr, "포스트 프로세서가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;

	const uint32 ConstantsIndex = RootSignature.AddConstants(sizeof(FTonemapConstants) / 4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 SceneIndex     = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(ConstantsIndex == TonemapRoot_Constants && SceneIndex == TonemapRoot_Scene);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"PostProcessRootSignature"))
	{
		return false;
	}

	// 가장 흔한 출력 포맷(sRGB 백버퍼/뷰포트)은 미리 만들어 초기화 실패를 조기에 드러낸다
	return GetTonemapPipeline(FD3D12RHI::RenderTargetFormat) != nullptr;
}

void FPostProcessor::Shutdown()
{
	TonemapPipelines.clear();
	RootSignature.Shutdown();
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
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

	if (bForceRecompile && (!ShaderLibrary->CookShader(VertexDesc) || !ShaderLibrary->CookShader(PixelDesc)))
	{
		return false;
	}
	const ComPtr<IDxcBlob> VertexShader = ShaderLibrary->GetShader(VertexDesc);
	const ComPtr<IDxcBlob> PixelShader  = ShaderLibrary->GetShader(PixelDesc);
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
	for (auto& [Format, Pipeline] : TonemapPipelines)
	{
		FD3D12PipelineState NewPipeline;
		if (!CreateTonemapPipeline(NewPipeline, Format, bForceRecompile))
		{
			bAllOk = false;
			continue;
		}
		Pipeline.Swap(NewPipeline);
		Rhi->DeferRelease(NewPipeline.Detach());
		bForceRecompile = false; // 같은 셰이더를 포맷마다 다시 쿠킹하지 않는다
	}
	return bAllOk;
}

void FPostProcessor::Render(ID3D12GraphicsCommandList* CommandList, const FD3D12DescriptorHandle& HdrSceneColor, const FRenderOutput& Output,
                            const FPostProcessSettings& Settings)
{
	E_CHECKF(Output.IsValid(), "포스트 프로세스 출력 대상이 유효하지 않습니다");

	FD3D12PipelineState* Pipeline = GetTonemapPipeline(Output.Format);
	if (Pipeline == nullptr)
	{
		return;
	}

	CommandList->OMSetRenderTargets(1, &Output.Rtv, FALSE, nullptr);
	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Output.Width), static_cast<float>(Output.Height), 0.0f, 1.0f };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Output.Width), static_cast<LONG>(Output.Height) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);

	FTonemapConstants Constants;
	Constants.Exposure        = std::exp2(Settings.ExposureEV);
	Constants.TonemapOperator = static_cast<uint32>(Settings.Tonemapper);

	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(Pipeline->Get());
	CommandList->SetGraphicsRoot32BitConstants(TonemapRoot_Constants, sizeof(Constants) / 4, &Constants, 0);
	CommandList->SetGraphicsRootDescriptorTable(TonemapRoot_Scene, HdrSceneColor.Gpu);
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->DrawInstanced(3, 1, 0, 0);
}
