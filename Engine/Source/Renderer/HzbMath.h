#pragma once

#include "Core/Math/Math.h"

#include <algorithm>
#include <functional>
#include <vector>

// 계층 Z(HZB) 오클루전 판정 식 (순수 함수 — RendererTests의 OcclusionTests). OcclusionCulling.hlsl과 같은 식을 쓴다.
//   HZB 밉 0 = 깊이 버퍼를 2x2씩 최댓값(가장 먼 깊이)으로 줄인 것. 크기는 올림(깊이 / 2)을 2의 거듭제곱으로 올린 값
//   (D3D12 밉 체인은 내림 반감이라 2의 거듭제곱이어야 단계마다 정확히 반이 된다 — 깊이 밖 텍셀은 0 = 가리지 않음).
//   밉 k = 밉 k-1을 2x2 최댓값으로. 밉 k의 텍셀 하나는 깊이 픽셀 2^(k+1) x 2^(k+1)을 정확히 덮는다 (한 축이 1이 되면 그 축 전체).
//   판정: 월드 AABB 8꼭짓점을 투영 → 화면 사각형(깊이 픽셀) + 가장 가까운 깊이 MinZ. 사각형이 2x2 텍셀 이하로 들어가는 밉을 골라
//   그 텍셀들의 최댓값보다 MinZ가 멀면 가려짐. 꼭짓점이 근평면 뒤(w ≤ 0)거나 화면 밖이면 판정하지 않고 보이는 것으로 한다.
//   깊이 규약: 0 = 가까움, 1 = 멂 (클리어 1)
namespace HzbMath
{
	struct FScreenRect
	{
		FVector2 PixelMin; // 깊이 픽셀 좌표 (y 아래로)
		FVector2 PixelMax;
		float    MinZ = 1.0f;
		bool     bValid = false; // false = 판정 불가 (근평면을 지나거나 화면 밖) → 보이는 것으로
	};

	// 깊이 크기 → HZB 밉 0 크기 (축마다)
	inline uint32 GetHzbDimension(uint32 DepthDimension)
	{
		const uint32 Half = std::max((DepthDimension + 1) / 2, 1u);
		uint32       Size = 1;
		while (Size < Half)
		{
			Size <<= 1;
		}
		return Size;
	}

	// HZB 밉 0 크기 → 밉 수 (1x1까지)
	inline uint32 GetMipCount(uint32 HzbWidth, uint32 HzbHeight)
	{
		uint32 Count = 1;
		while (HzbWidth > 1 || HzbHeight > 1)
		{
			HzbWidth  = std::max(HzbWidth / 2, 1u);
			HzbHeight = std::max(HzbHeight / 2, 1u);
			++Count;
		}
		return Count;
	}

	inline uint32 GetMipDimension(uint32 HzbDimension, uint32 Mip)
	{
		return std::max(HzbDimension >> Mip, 1u);
	}

	inline FScreenRect ProjectBounds(const FVector3& BoundsMin, const FVector3& BoundsMax, const FMatrix4x4& ViewProjection, float DepthWidth, float DepthHeight)
	{
		FScreenRect Rect;
		FVector2    NdcMin(1.0e30f, 1.0e30f);
		FVector2    NdcMax(-1.0e30f, -1.0e30f);
		float       MinZ = 1.0f;
		for (uint32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector3 P((Corner & 1) ? BoundsMax.X : BoundsMin.X, (Corner & 2) ? BoundsMax.Y : BoundsMin.Y, (Corner & 4) ? BoundsMax.Z : BoundsMin.Z);
			const float    X = P.X * ViewProjection.M[0][0] + P.Y * ViewProjection.M[1][0] + P.Z * ViewProjection.M[2][0] + ViewProjection.M[3][0];
			const float    Y = P.X * ViewProjection.M[0][1] + P.Y * ViewProjection.M[1][1] + P.Z * ViewProjection.M[2][1] + ViewProjection.M[3][1];
			const float    Z = P.X * ViewProjection.M[0][2] + P.Y * ViewProjection.M[1][2] + P.Z * ViewProjection.M[2][2] + ViewProjection.M[3][2];
			const float    W = P.X * ViewProjection.M[0][3] + P.Y * ViewProjection.M[1][3] + P.Z * ViewProjection.M[2][3] + ViewProjection.M[3][3];
			if (W <= 1.0e-4f)
			{
				return Rect; // 근평면을 지난다
			}
			NdcMin = FVector2(std::min(NdcMin.X, X / W), std::min(NdcMin.Y, Y / W));
			NdcMax = FVector2(std::max(NdcMax.X, X / W), std::max(NdcMax.Y, Y / W));
			MinZ   = std::min(MinZ, Z / W);
		}
		if (MinZ <= 0.0f || NdcMax.X < -1.0f || NdcMax.Y < -1.0f || NdcMin.X > 1.0f || NdcMin.Y > 1.0f)
		{
			return Rect; // 근평면 앞쪽에 걸치거나 화면 밖 (이 시점에선 판정 불가)
		}
		NdcMin = FVector2(std::clamp(NdcMin.X, -1.0f, 1.0f), std::clamp(NdcMin.Y, -1.0f, 1.0f));
		NdcMax = FVector2(std::clamp(NdcMax.X, -1.0f, 1.0f), std::clamp(NdcMax.Y, -1.0f, 1.0f));
		Rect.PixelMin = FVector2((NdcMin.X * 0.5f + 0.5f) * DepthWidth, (0.5f - NdcMax.Y * 0.5f) * DepthHeight);
		Rect.PixelMax = FVector2((NdcMax.X * 0.5f + 0.5f) * DepthWidth, (0.5f - NdcMin.Y * 0.5f) * DepthHeight);
		Rect.MinZ     = MinZ;
		Rect.bValid   = true;
		return Rect;
	}

	// 사각형 크기(깊이 픽셀)가 텍셀 2개 이하로 들어가는 밉: 밉 k 텍셀 = 2^(k+1) 픽셀
	inline uint32 SelectMip(float PixelWidth, float PixelHeight, uint32 MipCount)
	{
		const float Size = std::max(PixelWidth, PixelHeight);
		uint32      Mip  = 0;
		while (Mip + 1 < MipCount && static_cast<float>(2u << Mip) < Size)
		{
			++Mip;
		}
		return Mip;
	}

	// 밉 Mip에서 사각형이 덮는 텍셀 범위 (포함, 밉 크기로 고정)
	inline void GetTexelRange(const FScreenRect& Rect, uint32 Mip, uint32 MipWidth, uint32 MipHeight, int32 OutMin[2], int32 OutMax[2])
	{
		const float Scale = 1.0f / static_cast<float>(2u << Mip);
		OutMin[0]         = std::clamp(static_cast<int32>(FMath::Floor(Rect.PixelMin.X * Scale)), 0, static_cast<int32>(MipWidth) - 1);
		OutMin[1]         = std::clamp(static_cast<int32>(FMath::Floor(Rect.PixelMin.Y * Scale)), 0, static_cast<int32>(MipHeight) - 1);
		OutMax[0]         = std::clamp(static_cast<int32>(FMath::Floor(Rect.PixelMax.X * Scale)), 0, static_cast<int32>(MipWidth) - 1);
		OutMax[1]         = std::clamp(static_cast<int32>(FMath::Floor(Rect.PixelMax.Y * Scale)), 0, static_cast<int32>(MipHeight) - 1);
	}

	// CPU 참조 구현 (테스트용, HzbFromDepthCS와 같은 식): 깊이 → 밉 0 (깊이 밖 텍셀은 0)
	inline std::vector<float> BuildFirstMip(const std::vector<float>& Depth, uint32 DepthWidth, uint32 DepthHeight, uint32& OutWidth, uint32& OutHeight)
	{
		OutWidth  = GetHzbDimension(DepthWidth);
		OutHeight = GetHzbDimension(DepthHeight);
		std::vector<float> Result(static_cast<size_t>(OutWidth) * OutHeight, 0.0f);
		for (uint32 Y = 0; Y < OutHeight; ++Y)
		{
			for (uint32 X = 0; X < OutWidth; ++X)
			{
				float Farthest = 0.0f;
				for (uint32 DY = 0; DY < 2; ++DY)
				{
					for (uint32 DX = 0; DX < 2; ++DX)
					{
						const uint32 SX = X * 2 + DX, SY = Y * 2 + DY;
						if (SX < DepthWidth && SY < DepthHeight)
						{
							Farthest = std::max(Farthest, Depth[static_cast<size_t>(SY) * DepthWidth + SX]);
						}
					}
				}
				Result[static_cast<size_t>(Y) * OutWidth + X] = Farthest;
			}
		}
		return Result;
	}

	// CPU 참조 구현 (HzbDownsampleCS와 같은 식): 밉 k-1 → 밉 k (2x2 최댓값, 내림 반감 — 크기는 2의 거듭제곱)
	inline std::vector<float> Downsample(const std::vector<float>& Source, uint32 SourceWidth, uint32 SourceHeight, uint32& OutWidth, uint32& OutHeight)
	{
		OutWidth  = std::max(SourceWidth / 2, 1u);
		OutHeight = std::max(SourceHeight / 2, 1u);
		std::vector<float> Result(static_cast<size_t>(OutWidth) * OutHeight);
		for (uint32 Y = 0; Y < OutHeight; ++Y)
		{
			for (uint32 X = 0; X < OutWidth; ++X)
			{
				const uint32 X0 = X * 2, Y0 = Y * 2;
				const uint32 X1 = std::min(X0 + 1, SourceWidth - 1), Y1 = std::min(Y0 + 1, SourceHeight - 1);
				Result[Y * OutWidth + X] = std::max(std::max(Source[Y0 * SourceWidth + X0], Source[Y0 * SourceWidth + X1]),
				                                    std::max(Source[Y1 * SourceWidth + X0], Source[Y1 * SourceWidth + X1]));
			}
		}
		return Result;
	}

	// 가려졌으면 true. Load(밉, x, y) = HZB 값. MipCount = GetMipCount(HZB 밉 0 크기)
	inline bool IsOccluded(const FVector3& BoundsMin, const FVector3& BoundsMax, const FMatrix4x4& ViewProjection, uint32 DepthWidth, uint32 DepthHeight,
	                       uint32 MipCount, const std::function<float(uint32, int32, int32)>& Load)
	{
		const FScreenRect Rect = ProjectBounds(BoundsMin, BoundsMax, ViewProjection, static_cast<float>(DepthWidth), static_cast<float>(DepthHeight));
		if (!Rect.bValid)
		{
			return false;
		}
		const uint32 Mip = SelectMip(Rect.PixelMax.X - Rect.PixelMin.X, Rect.PixelMax.Y - Rect.PixelMin.Y, MipCount);
		int32        Min[2];
		int32        Max[2];
		GetTexelRange(Rect, Mip, GetMipDimension(GetHzbDimension(DepthWidth), Mip), GetMipDimension(GetHzbDimension(DepthHeight), Mip), Min, Max);
		float Farthest = 0.0f;
		for (int32 Y = Min[1]; Y <= Max[1]; ++Y)
		{
			for (int32 X = Min[0]; X <= Max[0]; ++X)
			{
				Farthest = std::max(Farthest, Load(Mip, X, Y));
			}
		}
		return Rect.MinZ > Farthest;
	}
} // namespace HzbMath
