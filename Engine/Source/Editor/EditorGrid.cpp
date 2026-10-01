#include "Editor/EditorGrid.h"

#include "RHI/D3D12/D3D12DepthBuffer.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	// Grid.hlsl GridConstants와 1:1
	struct FGridConstants
	{
		FMatrix4x4 ViewProjection;
		FVector3   CameraPosition;
		float      Extent       = 0.0f;
		float      MinorStep    = 0.0f;
		float      MajorStep    = 0.0f;
		float      FadeDistance = 0.0f;
		float      Padding0     = 0.0f;
	};
	static_assert(sizeof(FGridConstants) == 96);

	FShaderCompileDesc MakeDesc(const wchar_t* Entry, EShaderStage Stage)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = L"Grid.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		return Desc;
	}
} // namespace

FEditorGrid::~FEditorGrid()
{
	Shutdown();
}

bool FEditorGrid::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary)
{
	E_CHECKF(Rhi == nullptr, "에디터 그리드가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;

	RootSignature.AddConstants(sizeof(FGridConstants) / 4, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"EditorGridRootSignature"))
	{
		return false;
	}
	return CreatePipeline(Pipeline, false);
}

void FEditorGrid::Shutdown()
{
	Pipeline.Shutdown();
	RootSignature.Shutdown();
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
}

bool FEditorGrid::CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile)
{
	const FShaderCompileDesc Descs[] = { MakeDesc(L"GridVS", EShaderStage::Vertex), MakeDesc(L"GridPS", EShaderStage::Pixel) };
	ComPtr<IDxcBlob>         Blobs[2];
	for (int32 Index = 0; Index < 2; ++Index)
	{
		if (bForceRecompile && !ShaderLibrary->CookShader(Descs[Index]))
		{
			return false;
		}
		Blobs[Index] = ShaderLibrary->GetShader(Descs[Index]);
		if (!Blobs[Index])
		{
			return false;
		}
	}

	FGraphicsPipelineDesc Desc;
	Desc.RootSignature          = RootSignature.Get();
	Desc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(Blobs[0].Get());
	Desc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(Blobs[1].Get());
	Desc.RenderTargetFormats[0] = FD3D12RHI::RenderTargetFormat;
	Desc.DepthStencilFormat     = FD3D12DepthBuffer::Format;
	Desc.CullMode               = D3D12_CULL_MODE_NONE;
	Desc.bDepthEnable           = true;
	Desc.bDepthWrite            = false; // 씬 깊이로 가려지기만 한다
	Desc.DepthFunc              = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	// 씬 깊이는 TAA 지터(최대 반 픽셀)로 그려지고 그리드는 지터 없이 그리므로, Z=0 바닥과 겹치면 매 프레임 깊이 비교가 뒤집혀 선이 깜빡인다.
	// 카메라 쪽으로 기울기 비례 바이어스(반 픽셀 × 두 축 + 여유)를 줘 바닥과 같은 평면에서는 항상 보이게 한다
	Desc.DepthBias              = -8;
	Desc.SlopeScaledDepthBias   = -1.5f;
	Desc.BlendMode              = EBlendMode::Alpha;
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, L"EditorGridPipeline");
}

bool FEditorGrid::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewPipeline;
	if (!CreatePipeline(NewPipeline, bForceRecompile))
	{
		E_LOG(LogEditor, Error, "그리드 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	Pipeline.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());
	return true;
}

void FEditorGrid::Render(const FCamera& Camera, const FRenderOutput& Output, D3D12_CPU_DESCRIPTOR_HANDLE SceneDepthDsv)
{
	if (Rhi == nullptr || !Output.IsValid() || Output.Format != FD3D12RHI::RenderTargetFormat || SceneDepthDsv.ptr == 0)
	{
		return;
	}
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();

	CommandList->OMSetRenderTargets(1, &Output.Rtv, FALSE, &SceneDepthDsv);
	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Output.Width), static_cast<float>(Output.Height), 0.0f, 1.0f };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Output.Width), static_cast<LONG>(Output.Height) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);

	FGridConstants Constants;
	Constants.ViewProjection = Camera.GetViewProjectionMatrix();
	Constants.CameraPosition = Camera.GetPosition();
	Constants.Extent         = FMath::Min(FadeDistance, Camera.GetFarZ() * 0.9f);
	Constants.MinorStep      = FMath::Max(MinorStep, 0.01f);
	Constants.MajorStep      = FMath::Max(MajorStep, Constants.MinorStep);
	Constants.FadeDistance   = FMath::Max(FadeDistance, 1.0f);

	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(Pipeline.Get());
	CommandList->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &Constants, 0);
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->DrawInstanced(6, 1, 0, 0);
}
