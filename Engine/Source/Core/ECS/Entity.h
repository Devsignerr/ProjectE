#pragma once

#include "Core/CoreTypes.h"

#include <functional>

// 엔티티 식별자: 슬롯 인덱스 + 세대(재사용된 슬롯의 오래된 핸들을 구별)
struct FEntity
{
	static constexpr uint32 InvalidIndex = ~0u;

	uint32 Index      = InvalidIndex;
	uint32 Generation = 0;

	constexpr bool IsValid() const { return Index != InvalidIndex; }

	constexpr bool operator==(const FEntity& Other) const { return Index == Other.Index && Generation == Other.Generation; }
	constexpr bool operator!=(const FEntity& Other) const { return !(*this == Other); }

	constexpr uint64 ToId() const { return (static_cast<uint64>(Generation) << 32) | Index; }
	static constexpr FEntity FromId(uint64 Id) { return { static_cast<uint32>(Id & 0xFFFFFFFFu), static_cast<uint32>(Id >> 32) }; }
};

inline constexpr FEntity NullEntity{};

template <>
struct std::hash<FEntity>
{
	size_t operator()(const FEntity& Entity) const noexcept { return std::hash<uint64>{}(Entity.ToId()); }
};
