#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "Renderer/RenderGraph/RenderGraph.h"

#include <vector>

class FD3D12RHI;

// 렌더러가 프레임을 넘어 들고 있는 계산용 텍스처 (LUT·이력·노이즈 볼륨 등, Phase 49): 리소스 + 전체 SRV(셰이더 가시 힙) + 밉별 UAV +
// 추적 상태 하나. 렌더 그래프에는 ImportTracked로 들어간다 (끝 상태는 서브리소스 0 상태로 맞춰짐). 해제는 지연 (진행 중 프레임 보호)
struct FPersistentTextureDesc
{
	uint32      Width     = 1;
	uint32      Height    = 1;
	uint32      Depth     = 1;     // 3D 깊이 또는 배열 장 수 (큐브는 6)
	uint32      MipCount  = 1;
	DXGI_FORMAT Format    = DXGI_FORMAT_R16G16B16A16_FLOAT;
	bool        b3D       = false;
	bool        bCube     = false; // SRV = TextureCube, UAV = Texture2DArray(6)
	bool        bUnorderedAccess = true;
	bool        bRenderTarget    = false;
};

struct FPersistentTexture
{
	ComPtr<ID3D12Resource>              Resource;
	FD3D12DescriptorHandle              Srv;
	std::vector<FD3D12DescriptorHandle> Uavs; // 밉별
	D3D12_RESOURCE_STATES               State = D3D12_RESOURCE_STATE_COMMON;
	FPersistentTextureDesc              Desc;

	bool IsValid() const { return Resource != nullptr; }
	bool Matches(const FPersistentTextureDesc& Other) const;
	// 같은 설명이면 아무것도 하지 않는다. 다르면 이전 것을 지연 해제하고 다시 만든다 (내용은 비어 있음 — 쓰는 쪽이 다시 채운다)
	bool Ensure(FD3D12RHI& Rhi, const FPersistentTextureDesc& InDesc, const wchar_t* DebugName);
	void Release(FD3D12RHI& Rhi);
	uint32 GetArraySize() const { return Desc.b3D ? 1u : Desc.Depth; }
	FRGResourceRef Import(FRenderGraph& Graph, const char* Name) { return Graph.ImportTracked(Name, Resource.Get(), &State, Desc.MipCount, GetArraySize()); }
};
