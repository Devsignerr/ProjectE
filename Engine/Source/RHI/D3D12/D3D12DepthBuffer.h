#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"

// 스왑체인 크기에 맞춘 깊이 버퍼 (D32_FLOAT)
class FD3D12DepthBuffer
{
public:
	static constexpr DXGI_FORMAT Format     = DXGI_FORMAT_D32_FLOAT;
	static constexpr float       ClearDepth = 1.0f;

	~FD3D12DepthBuffer();

	bool Init(ID3D12Device* Device, uint32 InWidth, uint32 InHeight);
	void Shutdown();

	// 호출 전에 GPU가 깊이 버퍼 사용을 끝냈음을 보장해야 한다
	bool Resize(ID3D12Device* Device, uint32 InWidth, uint32 InHeight);

	D3D12_CPU_DESCRIPTOR_HANDLE GetDepthStencilView() const { return DsvHeap.GetCpuHandle(0); }
	ID3D12Resource*             GetResource() const { return Resource.Get(); }

	uint32 GetWidth() const { return Width; }
	uint32 GetHeight() const { return Height; }

private:
	bool CreateResource(ID3D12Device* Device);

	ComPtr<ID3D12Resource> Resource;
	FD3D12DescriptorHeap   DsvHeap;
	uint32                 Width  = 0;
	uint32                 Height = 0;
};
