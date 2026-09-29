#pragma once

#include "Core/Math/Box.h"
#include "RHI/D3D12/D3D12Buffer.h"
#include "Renderer/MeshData.h"

#include <vector>

class FD3D12CommandQueue;
class FD3D12Device;
class FD3D12RHI;

// GPU에 올라간 정적 메시 (정점/인덱스 버퍼 + 로컬 경계)
class FStaticMesh
{
public:
	bool Init(FD3D12Device& Device, FD3D12CommandQueue& Queue, const FMeshData& MeshData, const wchar_t* DebugName);
	void Shutdown();
	void ShutdownDeferred(FD3D12RHI& Rhi);

	// 토폴로지/버퍼 바인딩 후 인덱스 드로우
	void Draw(ID3D12GraphicsCommandList* CommandList) const;

	uint32      GetIndexCount() const { return IndexCount; }
	uint32      GetVertexCount() const { return VertexCount; }
	const FBox& GetLocalBounds() const { return LocalBounds; }

	// FVertex에 대응하는 입력 레이아웃
	static const std::vector<D3D12_INPUT_ELEMENT_DESC>& GetInputLayout();

private:
	FD3D12Buffer VertexBuffer;
	FD3D12Buffer IndexBuffer;
	FBox         LocalBounds;
	uint32       VertexCount = 0;
	uint32       IndexCount  = 0;
};
