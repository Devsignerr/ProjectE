#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"

// 스왑체인 크기에 맞춘 깊이 버퍼 (D32_FLOAT, 리소스는 R32_TYPELESS라 R32_FLOAT SRV 생성 가능)
class FD3D12DepthBuffer
{
public:
	static constexpr DXGI_FORMAT Format         = DXGI_FORMAT_D32_FLOAT;
	static constexpr DXGI_FORMAT ResourceFormat = DXGI_FORMAT_R32_TYPELESS; // SRV로도 읽을 수 있도록
	static constexpr DXGI_FORMAT SrvFormat      = DXGI_FORMAT_R32_FLOAT;
	static constexpr float       ClearDepth     = 1.0f;

	~FD3D12DepthBuffer();

	bool Init(ID3D12Device* Device, uint32 InWidth, uint32 InHeight);
	void Shutdown();
	// 리소스를 지연 해제 (DSV 힙은 기록 시점에만 읽히므로 즉시 해제)
	void ShutdownDeferred(class FD3D12RHI& Rhi);

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
