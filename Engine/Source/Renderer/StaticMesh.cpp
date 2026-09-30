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
	CpuPositions.clear();
	CpuPositions.reserve(MeshData.Vertices.size());
	for (const FVertex& Vertex : MeshData.Vertices)
	{
		LocalBounds.AddPoint(Vertex.Position);
		CpuPositions.push_back(Vertex.Position);
	}
	CpuIndices = MeshData.Indices;

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
	SkinBuffer.Shutdown();
	bSkinned = false;
	LocalBounds = FBox();
	VertexCount = 0;
	IndexCount  = 0;
	CpuPositions.clear();
	CpuIndices.clear();
}

void FStaticMesh::ShutdownDeferred(FD3D12RHI& Rhi)
{
	VertexBuffer.ShutdownDeferred(Rhi);
	IndexBuffer.ShutdownDeferred(Rhi);
	if (bSkinned)
	{
		SkinBuffer.ShutdownDeferred(Rhi);
		bSkinned = false;
	}
	LocalBounds = FBox();
	VertexCount = 0;
	IndexCount  = 0;
	CpuPositions.clear();
	CpuIndices.clear();
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

void FStaticMesh::DrawInstanced(ID3D12GraphicsCommandList* CommandList, uint32 InstanceCount) const
{
	const D3D12_VERTEX_BUFFER_VIEW VertexView = VertexBuffer.GetVertexBufferView(sizeof(FVertex));
	const D3D12_INDEX_BUFFER_VIEW  IndexView  = IndexBuffer.GetIndexBufferView(DXGI_FORMAT_R32_UINT);

	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->IASetVertexBuffers(0, 1, &VertexView);
	CommandList->IASetIndexBuffer(&IndexView);
	CommandList->DrawIndexedInstanced(IndexCount, InstanceCount, 0, 0, 0);
}

const std::vector<D3D12_INPUT_ELEMENT_DESC>& FStaticMesh::GetInputLayout()
{
	static const std::vector<D3D12_INPUT_ELEMENT_DESC> Layout = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(FVertex, Position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(FVertex, Normal),   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(FVertex, UV),       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(FVertex, Color),    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(FVertex, Tangent),  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	return Layout;
}

bool FStaticMesh::InitSkin(FD3D12Device& Device, FD3D12CommandQueue& Queue, const std::vector<FSkinVertex>& SkinVertices, const wchar_t* DebugName)
{
	E_CHECKF(SkinVertices.size() == VertexCount, "스킨 정점 수({})가 정점 수({})와 다릅니다", SkinVertices.size(), VertexCount);
	const std::wstring SkinName = std::wstring(DebugName) + L"_Skin";
	if (!SkinBuffer.InitStatic(Device, Queue, SkinVertices.data(), SkinVertices.size() * sizeof(FSkinVertex), SkinName.c_str()))
	{
		return false;
	}
	bSkinned = true;
	return true;
}

void FStaticMesh::DrawSkinned(ID3D12GraphicsCommandList* CommandList) const
{
	E_CHECKF(bSkinned, "스킨 스트림이 없는 메시입니다");
	const D3D12_VERTEX_BUFFER_VIEW VertexViews[2] = { VertexBuffer.GetVertexBufferView(sizeof(FVertex)),
	                                                  SkinBuffer.GetVertexBufferView(sizeof(FSkinVertex)) };
	const D3D12_INDEX_BUFFER_VIEW  IndexView      = IndexBuffer.GetIndexBufferView(DXGI_FORMAT_R32_UINT);

	CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CommandList->IASetVertexBuffers(0, 2, VertexViews);
	CommandList->IASetIndexBuffer(&IndexView);
	CommandList->DrawIndexedInstanced(IndexCount, 1, 0, 0, 0);
}

const std::vector<D3D12_INPUT_ELEMENT_DESC>& FStaticMesh::GetSkinnedInputLayout()
{
	static const std::vector<D3D12_INPUT_ELEMENT_DESC> Layout = [] {
		std::vector<D3D12_INPUT_ELEMENT_DESC> Elements = GetInputLayout();
		Elements.push_back({ "BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT,  1, offsetof(FSkinVertex, Joints),  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		Elements.push_back({ "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(FSkinVertex, Weights), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
		return Elements;
	}();
	return Layout;
}
