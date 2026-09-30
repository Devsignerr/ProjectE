#include "Editor/NavMeshDebugRenderer.h"

#include "RHI/D3D12/D3D12DepthBuffer.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"

#include <cstring>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	constexpr float Lift = 2.0f; // 바닥면과 같은 높이에서 깜빡이지 않도록 위로 (cm)

	constexpr uint32 PackColor(uint32 R, uint32 G, uint32 B, uint32 A) { return R | (G << 8) | (B << 16) | (A << 24); }

	constexpr uint32 FaceColor = PackColor(40, 200, 230, 80);
	constexpr uint32 EdgeColor = PackColor(220, 250, 255, 110);
	constexpr uint32 PathColor = PackColor(255, 210, 60, 255);

	FShaderCompileDesc MakeDesc(const wchar_t* Entry, EShaderStage Stage)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = L"NavMeshDebug.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		return Desc;
	}

	FVector3 Lifted(const FVector3& Point) { return FVector3(Point.X, Point.Y, Point.Z + Lift); }
} // namespace

FNavMeshDebugRenderer::~FNavMeshDebugRenderer()
{
	Shutdown();
}

bool FNavMeshDebugRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary)
{
	E_CHECKF(Rhi == nullptr, "내비메시 디버그 렌더러가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;

	RootSignature.AddConstants(sizeof(FMatrix4x4) / 4, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"NavMeshDebugRootSignature"))
	{
		return false;
	}
	return CreatePipelines(FacePipeline, LinePipeline, false);
}

void FNavMeshDebugRenderer::Shutdown()
{
	if (Rhi != nullptr)
	{
		FaceBuffer.ShutdownDeferred(*Rhi);
		EdgeBuffer.ShutdownDeferred(*Rhi);
	}
	FaceVertexCount = 0;
	EdgeVertexCount = 0;
	FacePipeline.Shutdown();
	LinePipeline.Shutdown();
	RootSignature.Shutdown();
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
}

bool FNavMeshDebugRenderer::CreatePipelines(FD3D12PipelineState& OutFaces, FD3D12PipelineState& OutLines, bool bForceRecompile)
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
	Desc.DepthStencilFormat     = FD3D12DepthBuffer::Format;
	Desc.CullMode               = D3D12_CULL_MODE_NONE;
	Desc.bDepthEnable           = true;
	Desc.bDepthWrite            = false; // 씬 깊이로 가려지기만 한다
	Desc.DepthFunc              = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	Desc.BlendMode              = EBlendMode::Alpha;
	if (!OutFaces.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, L"NavMeshDebugFacePipeline"))
	{
		return false;
	}
	Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	return OutLines.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, L"NavMeshDebugLinePipeline");
}

bool FNavMeshDebugRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewFaces;
	FD3D12PipelineState NewLines;
	if (!CreatePipelines(NewFaces, NewLines, bForceRecompile))
	{
		E_LOG(LogEditor, Error, "내비메시 디버그 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	FacePipeline.Swap(NewFaces);
	LinePipeline.Swap(NewLines);
	Rhi->DeferRelease(NewFaces.Detach());
	Rhi->DeferRelease(NewLines.Detach());
	return true;
}

void FNavMeshDebugRenderer::SetNavMeshTriangles(const std::vector<FVector3>& Triangles)
{
	if (Rhi == nullptr)
	{
		return;
	}
	// 이전 버퍼는 진행 중인 프레임이 쓸 수 있으므로 지연 해제
	FaceBuffer.ShutdownDeferred(*Rhi);
	EdgeBuffer.ShutdownDeferred(*Rhi);
	FaceVertexCount = 0;
	EdgeVertexCount = 0;
	if (Triangles.size() < 3)
	{
		return;
	}

	std::vector<FDebugVertex> Faces;
	std::vector<FDebugVertex> Edges;
	Faces.reserve(Triangles.size());
	Edges.reserve(Triangles.size() * 2);
	for (size_t Index = 0; Index + 2 < Triangles.size(); Index += 3)
	{
		const FVector3 Corner[3] = { Lifted(Triangles[Index]), Lifted(Triangles[Index + 1]), Lifted(Triangles[Index + 2]) };
		for (int32 Edge = 0; Edge < 3; ++Edge)
		{
			Faces.push_back({ Corner[Edge], FaceColor });
			Edges.push_back({ Corner[Edge], EdgeColor });
			Edges.push_back({ Corner[(Edge + 1) % 3], EdgeColor });
		}
	}
	if (FaceBuffer.InitStatic(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Faces.data(), Faces.size() * sizeof(FDebugVertex), L"NavMeshDebugFaces") &&
	    EdgeBuffer.InitStatic(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Edges.data(), Edges.size() * sizeof(FDebugVertex), L"NavMeshDebugEdges"))
	{
		FaceVertexCount = static_cast<uint32>(Faces.size());
		EdgeVertexCount = static_cast<uint32>(Edges.size());
	}
}

void FNavMeshDebugRenderer::AddPath(const std::vector<FVector3>& Points)
{
	for (size_t Index = 1; Index < Points.size() && PathVertices.size() + 2 <= MaxPathVertices; ++Index)
	{
		PathVertices.push_back({ Lifted(Points[Index - 1]) + FVector3(0.0f, 0.0f, Lift), PathColor });
		PathVertices.push_back({ Lifted(Points[Index]) + FVector3(0.0f, 0.0f, Lift), PathColor });
	}
}

void FNavMeshDebugRenderer::Render(const FCamera& Camera, const FRenderOutput& Output, D3D12_CPU_DESCRIPTOR_HANDLE SceneDepthDsv, bool bDrawNavMesh)
{
	const bool bHasWork = (bDrawNavMesh && FaceVertexCount > 0) || !PathVertices.empty();
	if (Rhi == nullptr || !bHasWork || !Output.IsValid() || Output.Format != FD3D12RHI::RenderTargetFormat || SceneDepthDsv.ptr == 0)
	{
		PathVertices.clear();
		return;
	}
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	CommandList->OMSetRenderTargets(1, &Output.Rtv, FALSE, &SceneDepthDsv);
	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Output.Width), static_cast<float>(Output.Height), 0.0f, 1.0f };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Output.Width), static_cast<LONG>(Output.Height) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);

	const FMatrix4x4 ViewProjection = Camera.GetViewProjectionMatrix();
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetGraphicsRoot32BitConstants(0, sizeof(ViewProjection) / 4, &ViewProjection, 0);

	if (bDrawNavMesh && FaceVertexCount > 0)
	{
		const D3D12_VERTEX_BUFFER_VIEW Faces = FaceBuffer.GetVertexBufferView(sizeof(FDebugVertex));
		CommandList->SetPipelineState(FacePipeline.Get());
		CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		CommandList->IASetVertexBuffers(0, 1, &Faces);
		CommandList->DrawInstanced(FaceVertexCount, 1, 0, 0);

		const D3D12_VERTEX_BUFFER_VIEW Edges = EdgeBuffer.GetVertexBufferView(sizeof(FDebugVertex));
		CommandList->SetPipelineState(LinePipeline.Get());
		CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
		CommandList->IASetVertexBuffers(0, 1, &Edges);
		CommandList->DrawInstanced(EdgeVertexCount, 1, 0, 0);
	}

	if (!PathVertices.empty())
	{
		const uint64                  Bytes      = PathVertices.size() * sizeof(FDebugVertex);
		const FD3D12DynamicAllocation Allocation = Rhi->GetDynamicBuffer().Allocate(Bytes, 16);
		std::memcpy(Allocation.CpuAddress, PathVertices.data(), Bytes);
		D3D12_VERTEX_BUFFER_VIEW Paths{};
		Paths.BufferLocation = Allocation.GpuAddress;
		Paths.SizeInBytes    = static_cast<UINT>(Bytes);
		Paths.StrideInBytes  = sizeof(FDebugVertex);
		CommandList->SetPipelineState(LinePipeline.Get());
		CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
		CommandList->IASetVertexBuffers(0, 1, &Paths);
		CommandList->DrawInstanced(static_cast<uint32>(PathVertices.size()), 1, 0, 0);
		PathVertices.clear();
	}
}
