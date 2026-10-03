#pragma once

#include "RHI/D3D12/D3D12Common.h"

#include <memory>

class FD3D12MipGenerator;

// DXGI 팩토리, 어댑터 선택, ID3D12Device 소유
class FD3D12Device
{
public:
	static constexpr D3D_FEATURE_LEVEL MinFeatureLevel = D3D_FEATURE_LEVEL_11_0;

	FD3D12Device();
	~FD3D12Device();

	bool Init(bool bEnableDebugLayer);
	void Shutdown();

	// 밉맵 생성기 (첫 사용 시 생성). 초기화 실패 시 nullptr — 호출자는 밉 없이 진행한다.
	FD3D12MipGenerator* GetMipGenerator();

	ID3D12Device*  GetDevice() const { return Device.Get(); }
	IDXGIFactory6* GetFactory() const { return Factory.Get(); }

	// 어댑터 비디오 메모리 사용량/예산 (IDXGIAdapter3::QueryVideoMemoryInfo, 바이트). Local = 전용 VRAM, NonLocal = 공유 시스템 메모리
	struct FVideoMemoryInfo
	{
		uint64 LocalUsage     = 0;
		uint64 LocalBudget    = 0;
		uint64 NonLocalUsage  = 0;
		uint64 NonLocalBudget = 0;
	};
	bool QueryVideoMemory(FVideoMemoryInfo& OutInfo) const;

	// 레이 트레이싱 (Phase 50): DXR 1.1(인라인 RayQuery) 이상이면 true. 가속 구조 빌드는 ID3D12Device5/ID3D12GraphicsCommandList4
	bool                  SupportsRayTracing() const { return Device5 != nullptr; }
	D3D12_RAYTRACING_TIER GetRaytracingTier() const { return RaytracingTier; }
	ID3D12Device5*        GetDevice5() const { return Device5.Get(); } // 미지원이면 nullptr

	bool   IsTearingSupported() const { return bTearingSupported; }
	bool   IsDebugLayerEnabled() const { return bDebugLayerEnabled; }
	uint32 GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE Type) const { return DescriptorSizes[Type]; }

private:
	// 고성능 GPU 우선으로 D3D12 지원 하드웨어 어댑터 선택
	bool SelectAdapter();

	ComPtr<IDXGIFactory6> Factory;
	ComPtr<IDXGIAdapter4> Adapter;
	ComPtr<ID3D12Device>  Device;
	ComPtr<ID3D12Device5> Device5; // DXR 1.1 지원 시에만
	D3D12_RAYTRACING_TIER RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;

	std::unique_ptr<FD3D12MipGenerator> MipGenerator;
	bool                                bMipGeneratorFailed = false;

	uint32 DescriptorSizes[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES] = {};
	bool   bTearingSupported  = false;
	bool   bDebugLayerEnabled = false;
};
