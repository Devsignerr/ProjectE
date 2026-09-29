#pragma once

#include "Core/CoreTypes.h"

#include <functional>

// 세대 검증이 있는 리소스 핸들. Tag로 타입을 구분해 서로 다른 리소스 핸들이 섞이지 않게 한다.
//   struct FMeshHandleTag {}; using FMeshHandle = THandle<FMeshHandleTag>;
template <typename TTag>
struct THandle
{
	static constexpr uint32 InvalidIndex = ~0u;

	uint32 Index      = InvalidIndex;
	uint32 Generation = 0;

	constexpr bool IsValid() const { return Index != InvalidIndex; }

	constexpr bool operator==(const THandle& Other) const { return Index == Other.Index && Generation == Other.Generation; }
	constexpr bool operator!=(const THandle& Other) const { return !(*this == Other); }

	constexpr uint64 ToId() const { return (static_cast<uint64>(Generation) << 32) | Index; }
};

template <typename TTag>
struct std::hash<THandle<TTag>>
{
	size_t operator()(const THandle<TTag>& Handle) const noexcept { return std::hash<uint64>{}(Handle.ToId()); }
};
