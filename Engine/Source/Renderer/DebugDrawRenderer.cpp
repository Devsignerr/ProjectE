#include "Renderer/DebugDrawRenderer.h"

#include "Core/Log.h"
#include "RHI/D3D12/D3D12DepthBuffer.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/DebugDraw.h"

#include <cstring>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// 위치 + 색 선 셰이더 (에디터 내비메시 표시와 공용 — 새 셰이더/쿠킹 항목 없이 재사용)
	FShaderCompileDesc MakeDesc(const wchar_t* Entry, EShaderStage Stage)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = L"NavMeshDebug.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		return Desc;
	}
} // namespace

FDebugDrawRenderer::~FDebugDrawRenderer()
{
	Shutdown();
}

bool FDebugDrawRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary)
{
	E_CHECKF(Rhi == nullptr, "디버그 선 렌더러가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;

	RootSignature.AddConstants(sizeof(FMatrix4x4) / 4, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"DebugDrawRootSignature") ||
	    !CreatePipelines(DepthTestedPipeline, OnTopPipeline, false))
	{
		Shutdown();
		return false;
	}
	return true;
}

void FDebugDrawRenderer::Shutdown()
{
	DepthTestedPipeline.Shutdown();
	OnTopPipeline.Shutdown();
	RootSignature.Shutdown();
	DepthTestedVertices.clear();
	OnTopVertices.clear();
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
}

bool FDebugDrawRenderer::CreatePipelines(FD3D12PipelineState& OutDepthTested, FD3D12PipelineState& OutOnTop, bool bForceRecompile)
{
	const FShaderCompileDesc Descs[] = { MakeDesc(L"NavDebugVS", EShaderStage::Vertex), MakeDesc(L"NavDebugPS", EShaderStage::Pixel) };
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
	Desc.InputLayout            = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	Desc.RenderTargetFormats[0] = FD3D12RHI::RenderTargetFormat;
	Desc.PrimitiveTopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	Desc.CullMode               = D3D12_CULL_MODE_NONE;
	Desc.BlendMode              = EBlendMode::Alpha;
	Desc.DepthStencilFormat     = FD3D12DepthBuffer::Format;
	Desc.bDepthEnable           = true;
	Desc.bDepthWrite            = false; // 씬 깊이로 가려지기만 한다
	Desc.DepthFunc              = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	if (!OutDepthTested.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, L"DebugDrawDepthTestedPipeline"))
	{
		return false;
	}
	// 항상 위: 깊이 버퍼 없이 (씬 깊이를 쓸 수 없는 픽셀 아트 모드에서도 같은 PSO)
	Desc.DepthStencilFormat = DXGI_FORMAT_UNKNOWN;
	Desc.bDepthEnable       = false;
	return OutOnTop.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, L"DebugDrawOnTopPipeline");
}

bool FDebugDrawRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewDepthTested;
	FD3D12PipelineState NewOnTop;
	if (!CreatePipelines(NewDepthTested, NewOnTop, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "디버그 선 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	DepthTestedPipeline.Swap(NewDepthTested);
	OnTopPipeline.Swap(NewOnTop);
	Rhi->DeferRelease(NewDepthTested.Detach());
	Rhi->DeferRelease(NewOnTop.Detach());
	return true;
}

void FDebugDrawRenderer::DrawBatch(const std::vector<FLineVertex>& Vertices, const FD3D12PipelineState& Pipeline)
{
	ID3D12GraphicsCommandList*    CommandList = Rhi->GetCommandList();
	const uint64                  Bytes       = Vertices.size() * sizeof(FLineVertex);
	const FD3D12DynamicAllocation Allocation  = Rhi->GetDynamicBuffer().Allocate(Bytes, 16);
	std::memcpy(Allocation.CpuAddress, Vertices.data(), Bytes);
	D3D12_VERTEX_BUFFER_VIEW View{};
	View.BufferLocation = Allocation.GpuAddress;
	View.SizeInBytes    = static_cast<UINT>(Bytes);
	View.StrideInBytes  = sizeof(FLineVertex);
	CommandList->SetPipelineState(Pipeline.Get());
	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
	CommandList->IASetVertexBuffers(0, 1, &View);
	CommandList->DrawInstanced(static_cast<uint32>(Vertices.size()), 1, 0, 0);
}

void FDebugDrawRenderer::Render(const FDebugDraw& Lines, const FCamera& Camera, const FRenderOutput& Output, D3D12_CPU_DESCRIPTOR_HANDLE SceneDepthDsv)
{
	Render(Lines.GetLines(), Camera, Output, SceneDepthDsv);
}

void FDebugDrawRenderer::Render(const std::vector<FDebugLine>& Source, const FCamera& Camera, const FRenderOutput& Output,
                                D3D12_CPU_DESCRIPTOR_HANDLE SceneDepthDsv)
{
	if (Rhi == nullptr || Source.empty() || !Output.IsValid() || Output.Format != FD3D12RHI::RenderTargetFormat)
	{
		return;
	}
	const bool bHasDepth = SceneDepthDsv.ptr != 0;
	DepthTestedVertices.clear();
	OnTopVertices.clear();
	for (const FDebugLine& Line : Source) // 개수는 FDebugDraw::MaxLines로 이미 제한된다
	{
		if (Line.bDepthTest && !bHasDepth)
		{
			continue; // 씬 깊이 없음 (픽셀 아트 모드)
		}
		std::vector<FLineVertex>& Target = Line.bDepthTest ? DepthTestedVertices : OnTopVertices;
		Target.push_back({ { Line.Start.X, Line.Start.Y, Line.Start.Z }, Line.Color });
		Target.push_back({ { Line.End.X, Line.End.Y, Line.End.Z }, Line.Color });
	}
	if (DepthTestedVertices.empty() && OnTopVertices.empty())
	{
		return;
	}

	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	const D3D12_VIEWPORT       Viewport{ 0.0f, 0.0f, static_cast<float>(Output.Width), static_cast<float>(Output.Height), 0.0f, 1.0f };
	const D3D12_RECT           Scissor{ 0, 0, static_cast<LONG>(Output.Width), static_cast<LONG>(Output.Height) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);
	const FMatrix4x4 ViewProjection = Camera.GetViewProjectionMatrix(); // 지터 없음
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetGraphicsRoot32BitConstants(0, sizeof(ViewProjection) / 4, &ViewProjection, 0);

	if (!DepthTestedVertices.empty())
	{
		CommandList->OMSetRenderTargets(1, &Output.Rtv, FALSE, &SceneDepthDsv);
		DrawBatch(DepthTestedVertices, DepthTestedPipeline);
	}
	CommandList->OMSetRenderTargets(1, &Output.Rtv, FALSE, nullptr);
	if (!OnTopVertices.empty())
	{
		DrawBatch(OnTopVertices, OnTopPipeline);
	}
}
