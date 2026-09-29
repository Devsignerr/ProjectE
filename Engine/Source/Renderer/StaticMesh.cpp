#include "Renderer/StaticMesh.h"

#include <cstddef>
#include <string>

bool FStaticMesh::Init(FD3D12Device& Device, FD3D12CommandQueue& Queue, const FMeshData& MeshData, const wchar_t* DebugName)
{
	E_CHECKF(!MeshData.Vertices.empty() && !MeshData.Indices.empty(), "메시 데이터가 비어 있습니다");
	E_CHECKF(MeshData.Indices.size() % 3 == 0, "인덱스 수가 3의 배수가 아닙니다");

	VertexCount = static_cast<uint32>(MeshData.Vertices.size());
	IndexCount  = static_cast<uint32>(MeshData.Indices.size());

	LocalBounds = FBox();
	for (const FVertex& Vertex : MeshData.Vertices)
	{
		LocalBounds.AddPoint(Vertex.Position);
	}

	const std::wstring VertexName = std::wstring(DebugName) + L"_VB";
	const std::wstring IndexName  = std::wstring(DebugName) + L"_IB";

	if (!VertexBuffer.InitStatic(Device, Queue, MeshData.Vertices.data(), MeshData.Vertices.size() * sizeof(FVertex),
	                             VertexName.c_str()))
	{
		return false;
	}
	if (!IndexBuffer.InitStatic(Device, Queue, MeshData.Indices.data(), MeshData.Indices.size() * sizeof(uint32),
	                            IndexName.c_str()))
	{
		return false;
	}
	return true;
}

void FStaticMesh::Shutdown()
{
	VertexBuffer.Shutdown();
	IndexBuffer.Shutdown();
	LocalBounds = FBox();
	VertexCount = 0;
	IndexCount  = 0;
}

void FStaticMesh::ShutdownDeferred(FD3D12RHI& Rhi)
{
	VertexBuffer.ShutdownDeferred(Rhi);
	IndexBuffer.ShutdownDeferred(Rhi);
	LocalBounds = FBox();
	VertexCount = 0;
	IndexCount  = 0;
}

void FStaticMesh::Draw(ID3D12GraphicsCommandList* CommandList) const
{
	const D3D12_VERTEX_BUFFER_VIEW VertexView = VertexBuffer.GetVertexBufferView(sizeof(FVertex));
	const D3D12_INDEX_BUFFER_VIEW  IndexView  = IndexBuffer.GetIndexBufferView(DXGI_FORMAT_R32_UINT);

	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->IASetVertexBuffers(0, 1, &VertexView);
	CommandList->IASetIndexBuffer(&IndexView);
	CommandList->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);
}

const std::vector<D3D12_INPUT_ELEMENT_DESC>& FStaticMesh::GetInputLayout()
{
	static const std::vector<D3D12_INPUT_ELEMENT_DESC> Layout = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(FVertex, Position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(FVertex, Normal),   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(FVertex, UV),       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(FVertex, Color),    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	return Layout;
}
