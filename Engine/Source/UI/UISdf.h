#pragma once

#include "Core/CoreTypes.h"

#include <vector>

// 커버리지 비트맵 → SDF (글꼴 아틀라스용). 곡선 종류와 무관하게 래스터 결과만 쓰므로 TTF/OTF(CFF 3차 곡선) 모두 같은 품질.
//   1) 커버리지 >= 128을 안쪽으로 이진화  2) 안/밖 각각 정확한 유클리드 거리 변환(Felzenszwalb, 선형 시간)
//   3) 부호 거리 = 안쪽이면 +(밖까지 거리 - 0.5), 밖이면 -(안까지 거리 - 0.5)  4) Oversample x Oversample 블록 평균으로 축소
//   5) 값 = 128 + 거리(출력 픽셀) × 128 / DistanceRange, 0~255로 자름 (가장자리 128, 안쪽이 큼 — stb_truetype SDF와 같은 규약)
namespace UISdf
{
	// 1차원 제곱 거리 변환: F(입력 비용) → D(출력). 작업 버퍼 V/Z는 크기 N / N+1 이상
	void DistanceTransform1D(const float* F, float* D, int32 N, int32* V, float* Z);
	// 2차원 제곱 거리 변환 (제자리): Grid[y * Width + x], 특징 픽셀 = 0, 나머지 = 큰 값
	void DistanceTransform2D(std::vector<float>& Grid, int32 Width, int32 Height);

	// Coverage: Width x Height (Oversample의 배수), Out: (Width/Oversample) x (Height/Oversample)
	void MakeSdf(const uint8* Coverage, int32 Width, int32 Height, int32 Oversample, float DistanceRange, std::vector<uint8>& Out);
} // namespace UISdf
