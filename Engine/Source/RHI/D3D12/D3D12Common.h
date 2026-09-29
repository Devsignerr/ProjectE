#pragma once

#include "Core/CoreMinimal.h"
#include "Core/Platform/WindowsHeaders.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

using Microsoft::WRL::ComPtr;

E_DECLARE_ENGINE_LOG_CATEGORY(LogD3D12)

// HRESULT를 16진수 문자열로 변환
inline std::string HResultToString(HRESULT Result)
{
	return std::format("0x{:08X}", static_cast<uint32>(Result));
}

// 리소스 상태 전이 배리어 생성 (d3dx12.h 미사용). Subresource로 특정 밉/슬라이스만 전이할 수 있다.
inline D3D12_RESOURCE_BARRIER MakeTransitionBarrier(ID3D12Resource* Resource,
                                                    D3D12_RESOURCE_STATES Before,
                                                    D3D12_RESOURCE_STATES After,
                                                    uint32 Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
	D3D12_RESOURCE_BARRIER Barrier{};
	Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	Barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	Barrier.Transition.pResource   = Resource;
	Barrier.Transition.Subresource = Subresource;
	Barrier.Transition.StateBefore = Before;
	Barrier.Transition.StateAfter  = After;
	return Barrier;
}

// UAV 쓰기 완료 대기 배리어
inline D3D12_RESOURCE_BARRIER MakeUavBarrier(ID3D12Resource* Resource)
{
	D3D12_RESOURCE_BARRIER Barrier{};
	Barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	Barrier.Flags         = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	Barrier.UAV.pResource = Resource;
	return Barrier;
}

// ---- 포맷 유틸리티 (밉 생성용 TYPELESS/UNORM/sRGB 변환). 지원 밖의 포맷은 UNKNOWN 반환.

inline DXGI_FORMAT GetTypelessFormat(DXGI_FORMAT Format)
{
	switch (Format)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	default:                            return DXGI_FORMAT_UNKNOWN;
	}
}

inline DXGI_FORMAT GetUnormFormat(DXGI_FORMAT Format)
{
	switch (Format)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	default:                            return DXGI_FORMAT_UNKNOWN;
	}
}

inline bool IsSrgbFormat(DXGI_FORMAT Format)
{
	return Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

inline D3D12_HEAP_PROPERTIES MakeHeapProperties(D3D12_HEAP_TYPE Type)
{
	D3D12_HEAP_PROPERTIES Properties{};
	Properties.Type                 = Type;
	Properties.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	Properties.CreationNodeMask     = 1;
	Properties.VisibleNodeMask      = 1;
	return Properties;
}

inline D3D12_RESOURCE_DESC MakeBufferDesc(uint64 SizeInBytes, D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE)
{
	D3D12_RESOURCE_DESC Desc{};
	Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	Desc.Alignment        = 0;
	Desc.Width            = SizeInBytes;
	Desc.Height           = 1;
	Desc.DepthOrArraySize = 1;
	Desc.MipLevels        = 1;
	Desc.Format           = DXGI_FORMAT_UNKNOWN;
	Desc.SampleDesc       = { 1, 0 };
	Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	Desc.Flags            = Flags;
	return Desc;
}

inline D3D12_RESOURCE_DESC MakeTexture2DDesc(uint32 Width, uint32 Height, DXGI_FORMAT Format,
                                             D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE, uint16 MipLevels = 1)
{
	D3D12_RESOURCE_DESC Desc{};
	Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	Desc.Alignment        = 0;
	Desc.Width            = Width;
	Desc.Height           = Height;
	Desc.DepthOrArraySize = 1;
	Desc.MipLevels        = MipLevels;
	Desc.Format           = Format;
	Desc.SampleDesc       = { 1, 0 };
	Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	Desc.Flags            = Flags;
	return Desc;
}

template <typename T>
constexpr T AlignUp(T Value, T Alignment)
{
	return (Value + Alignment - 1) & ~(Alignment - 1);
}

// HRESULT 실패 시 Error 로그를 남기고 현재 함수에서 false를 반환 (초기화 코드용)
#define E_D3D_VERIFY(Call)                                                                            \
	do                                                                                                \
	{                                                                                                 \
		const HRESULT VerifyResult_ = (Call);                                                         \
		if (FAILED(VerifyResult_))                                                                    \
		{                                                                                             \
			E_LOG(LogD3D12, Error, "D3D12 호출 실패: {} -> {} ({}:{})", #Call,                       \
			      HResultToString(VerifyResult_), __FILE__, __LINE__);                                \
			return false;                                                                             \
		}                                                                                             \
	} while (0)

// HRESULT 실패 시 Fatal (프레임 루프 등 복구 불가능한 지점용)
#define E_D3D_CHECK(Call)                                                                             \
	do                                                                                                \
	{                                                                                                 \
		const HRESULT CheckResult_ = (Call);                                                          \
		if (FAILED(CheckResult_))                                                                     \
		{                                                                                             \
			E_LOG(LogD3D12, Fatal, "D3D12 호출 실패: {} -> {} ({}:{})", #Call,                       \
			      HResultToString(CheckResult_), __FILE__, __LINE__);                                 \
		}                                                                                             \
	} while (0)
