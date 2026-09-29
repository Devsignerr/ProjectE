#pragma once

#include "RHI/D3D12/D3D12Common.h"

// DXGI 팩토리, 어댑터 선택, ID3D12Device 소유
class FD3D12Device
{
public:
	static constexpr D3D_FEATURE_LEVEL MinFeatureLevel = D3D_FEATURE_LEVEL_11_0;

	bool Init(bool bEnableDebugLayer);
	void Shutdown();

	ID3D12Device*  GetDevice() const { return Device.Get(); }
	IDXGIFactory6* GetFactory() const { return Factory.Get(); }

	bool   IsTearingSupported() const { return bTearingSupported; }
	bool   IsDebugLayerEnabled() const { return bDebugLayerEnabled; }
	uint32 GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE Type) const { return DescriptorSizes[Type]; }

private:
	// 고성능 GPU 우선으로 D3D12 지원 하드웨어 어댑터 선택
	bool SelectAdapter();

	ComPtr<IDXGIFactory6> Factory;
	ComPtr<IDXGIAdapter4> Adapter;
	ComPtr<ID3D12Device>  Device;

	uint32 DescriptorSizes[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES] = {};
	bool   bTearingSupported  = false;
	bool   bDebugLayerEnabled = false;
};
