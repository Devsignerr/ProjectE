#pragma once

#include "Core/Math/Box.h"
#include "RHI/D3D12/D3D12Buffer.h"
#include "Renderer/MeshData.h"
#include "Renderer/SkinnedMeshData.h"

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
	// 같은 메시를 InstanceCount번 (인스턴스 데이터는 셰이더가 SV_InstanceID로 읽는다)
	void DrawInstanced(ID3D12GraphicsCommandList* CommandList, uint32 InstanceCount) const;

	// 스킨 정점 스트림(슬롯 1) 추가. Init 이후 호출, 정점 수가 같아야 한다
	bool InitSkin(FD3D12Device& Device, FD3D12CommandQueue& Queue, const std::vector<FSkinVertex>& SkinVertices, const wchar_t* DebugName);
	bool IsSkinned() const { return bSkinned; }
	// 슬롯 0 + 슬롯 1(스킨) 바인딩 후 드로우 (IsSkinned일 때만)
	void DrawSkinned(ID3D12GraphicsCommandList* CommandList) const;

	uint32      GetIndexCount() const { return IndexCount; }
	uint32      GetVertexCount() const { return VertexCount; }
	const FBox& GetLocalBounds() const { return LocalBounds; }
	// CPU 사본 (로컬 위치 + 삼각형 인덱스, CW 앞면): 내비메시 굽기 등 CPU 지오메트리 처리용
	const std::vector<FVector3>& GetCpuPositions() const { return CpuPositions; }
	const std::vector<uint32>&   GetCpuIndices() const { return CpuIndices; }

	// FVertex에 대응하는 입력 레이아웃
	static const std::vector<D3D12_INPUT_ELEMENT_DESC>& GetInputLayout();
	// FVertex + FSkinVertex(슬롯 1: BLENDINDICES, BLENDWEIGHT)
	static const std::vector<D3D12_INPUT_ELEMENT_DESC>& GetSkinnedInputLayout();

private:
	FD3D12Buffer          VertexBuffer;
	FD3D12Buffer          IndexBuffer;
	FD3D12Buffer          SkinBuffer;
	FBox                  LocalBounds;
	std::vector<FVector3> CpuPositions;
	std::vector<uint32>   CpuIndices;
	uint32                VertexCount = 0;
	uint32                IndexCount  = 0;
	bool                  bSkinned    = false;
};
