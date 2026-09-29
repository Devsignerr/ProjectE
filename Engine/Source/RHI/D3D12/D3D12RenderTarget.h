#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DepthBuffer.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12DescriptorHeap.h"

class FD3D12Device;
class FD3D12RHI;

// 렌더 타깃 포맷 구성
struct FRenderTargetDesc
{
	DXGI_FORMAT ResourceFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	DXGI_FORMAT RtvFormat      = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // 선형 → 감마 인코딩
	DXGI_FORMAT SrvFormat      = DXGI_FORMAT_R8G8B8A8_UNORM;      // UI가 인코딩된 값을 그대로 표시
	bool        bWithDepth     = true;

	// 기본(LDR 표시용): 위 값 그대로
	static FRenderTargetDesc MakeLdrDisplay() { return FRenderTargetDesc{}; }

	// HDR 씬 버퍼: 선형 부동소수점
	static FRenderTargetDesc MakeHdr(bool bInWithDepth = true)
	{
		FRenderTargetDesc Desc;
		Desc.ResourceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		Desc.RtvFormat      = DXGI_FORMAT_R16G16B16A16_FLOAT;
		Desc.SrvFormat      = DXGI_FORMAT_R16G16B16A16_FLOAT;
		Desc.bWithDepth     = bInWithDepth;
		return Desc;
	}

	// 단일 채널 마스크 (선택 아웃라인 등)
	static FRenderTargetDesc MakeMask(bool bInWithDepth = false)
	{
		FRenderTargetDesc Desc;
		Desc.ResourceFormat = DXGI_FORMAT_R8_UNORM;
		Desc.RtvFormat      = DXGI_FORMAT_R8_UNORM;
		Desc.SrvFormat      = DXGI_FORMAT_R8_UNORM;
		Desc.bWithDepth     = bInWithDepth;
		return Desc;
	}
};

// 렌더 패스의 출력 대상 (백버퍼 또는 오프스크린 타깃의 RTV)
struct FRenderOutput
{
	D3D12_CPU_DESCRIPTOR_HANDLE Rtv{};
	DXGI_FORMAT                 Format = DXGI_FORMAT_UNKNOWN;
	uint32                      Width  = 0;
	uint32                      Height = 0;

	bool IsValid() const { return Rtv.ptr != 0 && Width > 0 && Height > 0; }
};

// 오프스크린 색상(+깊이) 렌더 타깃. 평상시 상태는 PIXEL_SHADER_RESOURCE이고 Begin/End로 렌더 타깃 상태를 오간다.
class FD3D12RenderTarget
{
public:
	~FD3D12RenderTarget();

	bool Init(FD3D12Device& Device, FD3D12DescriptorAllocator& InSrvAllocator, uint32 InWidth, uint32 InHeight,
	          const wchar_t* DebugName, const FRenderTargetDesc& InDesc = FRenderTargetDesc{});
	void Shutdown();
	void ShutdownDeferred(FD3D12RHI& Rhi);

	// 렌더 타깃 상태로 전이 + 바인딩 + 클리어(ClearColor가 nullptr이면 생략) + 뷰포트/시저
	void Begin(ID3D12GraphicsCommandList* CommandList, const float ClearColor[4]);
	// 픽셀 셰이더 리소스 상태로 전이 (이후 SRV로 샘플링 가능)
	void End(ID3D12GraphicsCommandList* CommandList);

	// 현재 상태 그대로 렌더 타깃으로 쓰일 출력 정보 (Begin 이후 사용)
	FRenderOutput GetOutput() const;

	const FD3D12DescriptorHandle& GetSrv() const { return Srv; }
	D3D12_CPU_DESCRIPTOR_HANDLE   GetRtv() const { return RtvHeap.GetCpuHandle(0); }
	ID3D12Resource*               GetColorResource() const { return ColorResource.Get(); }
	const FRenderTargetDesc&      GetDesc() const { return Desc; }
	uint32                        GetWidth() const { return Width; }
	uint32                        GetHeight() const { return Height; }
	bool                          IsValid() const { return ColorResource != nullptr; }

private:
	ComPtr<ID3D12Resource>     ColorResource;
	FD3D12DescriptorHeap       RtvHeap;
	FD3D12DescriptorHandle     Srv;
	FD3D12DescriptorAllocator* SrvAllocator = nullptr;
	FD3D12DepthBuffer          DepthBuffer;
	FRenderTargetDesc          Desc;
	uint32                     Width          = 0;
	uint32                     Height         = 0;
	bool                       bInRenderState = false;
};
