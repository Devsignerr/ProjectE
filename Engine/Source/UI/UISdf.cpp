#include "UI/UISdf.h"

#include "Core/Math/Math.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr float GInfinity = 1.0e20f;
}

void UISdf::DistanceTransform1D(const float* F, float* D, int32 N, int32* V, float* Z)
{
	// 포물선 하한 포락선 (Felzenszwalb & Huttenlocher)
	const auto Intersect = [F, V](int32 Q, int32 K) {
		return ((F[Q] + static_cast<float>(Q * Q)) - (F[V[K]] + static_cast<float>(V[K] * V[K]))) / static_cast<float>(2 * Q - 2 * V[K]);
	};
	int32 K = 0;
	V[0]    = 0;
	Z[0]    = -GInfinity;
	Z[1]    = GInfinity;
	for (int32 Q = 1; Q < N; ++Q)
	{
		float S = Intersect(Q, K);
		while (S <= Z[K])
		{
			--K;
			S = Intersect(Q, K);
		}
		++K;
		V[K]     = Q;
		Z[K]     = S;
		Z[K + 1] = GInfinity;
	}
	K = 0;
	for (int32 Q = 0; Q < N; ++Q)
	{
		while (Z[K + 1] < static_cast<float>(Q))
		{
			++K;
		}
		const float Delta = static_cast<float>(Q - V[K]);
		D[Q]              = Delta * Delta + F[V[K]];
	}
}

void UISdf::DistanceTransform2D(std::vector<float>& Grid, int32 Width, int32 Height)
{
	const size_t       MaxDim = static_cast<size_t>(std::max(Width, Height));
	std::vector<float> F(MaxDim);
	std::vector<float> D(MaxDim);
	std::vector<int32> V(MaxDim);
	std::vector<float> Z(MaxDim + 1);
	// 열
	for (int32 X = 0; X < Width; ++X)
	{
		for (int32 Y = 0; Y < Height; ++Y)
		{
			F[static_cast<size_t>(Y)] = Grid[static_cast<size_t>(Y) * Width + X];
		}
		DistanceTransform1D(F.data(), D.data(), Height, V.data(), Z.data());
		for (int32 Y = 0; Y < Height; ++Y)
		{
			Grid[static_cast<size_t>(Y) * Width + X] = D[static_cast<size_t>(Y)];
		}
	}
	// 행
	for (int32 Y = 0; Y < Height; ++Y)
	{
		float* Row = &Grid[static_cast<size_t>(Y) * Width];
		std::copy(Row, Row + Width, F.begin());
		DistanceTransform1D(F.data(), D.data(), Width, V.data(), Z.data());
		std::copy(D.begin(), D.begin() + Width, Row);
	}
}

void UISdf::MakeSdf(const uint8* Coverage, int32 Width, int32 Height, int32 Oversample, float DistanceRange, std::vector<uint8>& Out)
{
	const size_t       Count = static_cast<size_t>(Width) * Height;
	std::vector<float> ToInside(Count);  // 안쪽 픽셀까지 제곱 거리 (밖 픽셀용)
	std::vector<float> ToOutside(Count); // 바깥 픽셀까지 제곱 거리 (안 픽셀용)
	for (size_t Index = 0; Index < Count; ++Index)
	{
		const bool bInside = Coverage[Index] >= 128;
		ToInside[Index]    = bInside ? 0.0f : GInfinity;
		ToOutside[Index]   = bInside ? GInfinity : 0.0f;
	}
	DistanceTransform2D(ToInside, Width, Height);
	DistanceTransform2D(ToOutside, Width, Height);

	const int32 OutWidth  = Width / Oversample;
	const int32 OutHeight = Height / Oversample;
	const float Scale     = 128.0f / (DistanceRange * static_cast<float>(Oversample)); // 고해상도 픽셀 거리 → 값
	const float Inverse   = 1.0f / static_cast<float>(Oversample * Oversample);
	Out.assign(static_cast<size_t>(OutWidth) * OutHeight, 0);
	for (int32 OutY = 0; OutY < OutHeight; ++OutY)
	{
		for (int32 OutX = 0; OutX < OutWidth; ++OutX)
		{
			float Sum = 0.0f;
			for (int32 SubY = 0; SubY < Oversample; ++SubY)
			{
				const size_t RowStart = static_cast<size_t>(OutY * Oversample + SubY) * Width + static_cast<size_t>(OutX) * Oversample;
				for (int32 SubX = 0; SubX < Oversample; ++SubX)
				{
					const size_t Index = RowStart + static_cast<size_t>(SubX);
					Sum += Coverage[Index] >= 128 ? std::sqrt(ToOutside[Index]) - 0.5f : 0.5f - std::sqrt(ToInside[Index]);
				}
			}
			const float Value                                = 128.0f + Sum * Inverse * Scale;
			Out[static_cast<size_t>(OutY) * OutWidth + OutX] = static_cast<uint8>(FMath::Clamp(Value + 0.5f, 0.0f, 255.0f));
		}
	}
}
