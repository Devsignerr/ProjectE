#pragma once

#include "Core/CoreTypes.h"

// 렌더 그래프 공용 타입 (GPU/D3D12 비의존 — 컴파일러 순수 로직과 테스트가 함께 쓴다).
// 접근(ERGAccess)은 D3D12 리소스 상태와 1:1로 대응하는 비트이며 실행기(RenderGraph.cpp)만 D3D12 값으로 바꾼다.
//   쓰기 접근(RenderTarget/DepthWrite/Uav/CopyDest)은 서로 배타적이고, 읽기 접근(DepthRead/SrvPixel/SrvNonPixel/CopySource/IndirectArgs)은
//   OR로 합칠 수 있다 (한 상태로 여러 종류 읽기). Present/Common은 단독 상태(D3D12 값 0).
enum class ERGAccess : uint32
{
	None         = 0,
	RenderTarget = 1u << 0,  // RENDER_TARGET (쓰기)
	DepthWrite   = 1u << 1,  // DEPTH_WRITE (쓰기)
	DepthRead    = 1u << 2,  // DEPTH_READ (깊이 테스트만)
	SrvPixel     = 1u << 3,  // PIXEL_SHADER_RESOURCE
	SrvNonPixel  = 1u << 4,  // NON_PIXEL_SHADER_RESOURCE (계산/정점 셰이더)
	Uav          = 1u << 5,  // UNORDERED_ACCESS (읽기·쓰기)
	CopySource   = 1u << 6,  // COPY_SOURCE
	CopyDest     = 1u << 7,  // COPY_DEST (쓰기)
	IndirectArgs = 1u << 8,  // INDIRECT_ARGUMENT
	Present      = 1u << 9,  // PRESENT (= COMMON)
	Common       = 1u << 10, // COMMON

	SrvAll = SrvPixel | SrvNonPixel,
};

constexpr ERGAccess operator|(ERGAccess A, ERGAccess B)
{
	return static_cast<ERGAccess>(static_cast<uint32>(A) | static_cast<uint32>(B));
}
constexpr ERGAccess operator&(ERGAccess A, ERGAccess B)
{
	return static_cast<ERGAccess>(static_cast<uint32>(A) & static_cast<uint32>(B));
}
constexpr ERGAccess& operator|=(ERGAccess& A, ERGAccess B)
{
	A = A | B;
	return A;
}

namespace RGAccess
{
	inline constexpr uint32 WriteMask = static_cast<uint32>(ERGAccess::RenderTarget) | static_cast<uint32>(ERGAccess::DepthWrite) |
	                                    static_cast<uint32>(ERGAccess::Uav) | static_cast<uint32>(ERGAccess::CopyDest);
	inline constexpr uint32 ReadMask = static_cast<uint32>(ERGAccess::DepthRead) | static_cast<uint32>(ERGAccess::SrvPixel) |
	                                   static_cast<uint32>(ERGAccess::SrvNonPixel) | static_cast<uint32>(ERGAccess::CopySource) |
	                                   static_cast<uint32>(ERGAccess::IndirectArgs);
	inline constexpr uint32 SingleMask = static_cast<uint32>(ERGAccess::Present) | static_cast<uint32>(ERGAccess::Common);
	// 계산 큐 명령 목록에서 배리어의 전/후 상태로 쓸 수 있는 상태 (D3D12: 픽셀 셰이더·렌더 타깃·깊이 상태는 그래픽스 큐만)
	inline constexpr uint32 ComputeLegalMask = static_cast<uint32>(ERGAccess::SrvNonPixel) | static_cast<uint32>(ERGAccess::Uav) |
	                                           static_cast<uint32>(ERGAccess::CopySource) | static_cast<uint32>(ERGAccess::CopyDest) |
	                                           static_cast<uint32>(ERGAccess::IndirectArgs) | static_cast<uint32>(ERGAccess::Common) |
	                                           static_cast<uint32>(ERGAccess::Present);

	constexpr uint32 Bits(ERGAccess Access) { return static_cast<uint32>(Access); }
	constexpr bool   HasWrite(ERGAccess Access) { return (Bits(Access) & WriteMask) != 0; }
	constexpr bool   IsReadOnly(ERGAccess Access) { return Bits(Access) != 0 && (Bits(Access) & ~ReadMask) == 0; }
	constexpr bool   IsComputeLegal(ERGAccess Access) { return Bits(Access) != 0 && (Bits(Access) & ~ComputeLegalMask) == 0; }
	// 한 패스 안에서 같은 서브리소스에 같이 선언해도 되는 조합인가 (읽기끼리, 또는 쓰기 하나 단독)
	bool IsValidCombination(ERGAccess Access);
	// 로그용 이름 ("SrvPixel|SrvNonPixel")
	const char* ToString(ERGAccess Access);
} // namespace RGAccess

enum class ERGQueue : uint8
{
	Graphics,     // 프레임 그래픽스(DIRECT) 명령 목록
	AsyncCompute, // 비동기 계산 큐 (끄면 그래픽스 큐에서 등록 순서대로)
};

// 서브리소스 범위 (밉 × 배열 장). D3D12 서브리소스 번호 = 밉 + 장 × 밉 수 (평면 0)
struct FRGSubresourceRange
{
	static constexpr uint32 Remaining = ~0u;

	uint32 FirstMip   = 0;
	uint32 MipCount   = Remaining;
	uint32 FirstSlice = 0;
	uint32 SliceCount = Remaining;

	static FRGSubresourceRange All() { return FRGSubresourceRange{}; }
	static FRGSubresourceRange Mip(uint32 InMip) { return { InMip, 1, 0, Remaining }; }
	static FRGSubresourceRange Slice(uint32 InSlice) { return { 0, Remaining, InSlice, 1 }; }
	bool IsAll() const { return FirstMip == 0 && MipCount == Remaining && FirstSlice == 0 && SliceCount == Remaining; }
};
