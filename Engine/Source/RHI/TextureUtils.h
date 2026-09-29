#pragma once

#include "Core/CoreTypes.h"

// 플랫폼 독립 텍스처 유틸리티 (순수 함수, 테스트 대상)

// 전체 밉 체인 레벨 수: floor(log2(max(Width, Height))) + 1. 0 크기는 1을 반환.
constexpr uint32 CalculateMipCount(uint32 Width, uint32 Height)
{
	uint32 Size  = Width > Height ? Width : Height;
	uint32 Count = 1;
	while (Size > 1)
	{
		Size >>= 1;
		++Count;
	}
	return Count;
}

// 해당 밉 레벨의 한 변 크기 (최소 1)
constexpr uint32 GetMipDimension(uint32 BaseDimension, uint32 MipLevel)
{
	const uint32 Dimension = BaseDimension >> MipLevel;
	return Dimension > 0 ? Dimension : 1;
}
