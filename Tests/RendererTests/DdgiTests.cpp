#include "Core/Testing/TestFramework.h"
#include "Renderer/DdgiMath.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <utility>

// 동적 GI — DDGI 순수 식 (Renderer/DdgiMath.h, Phase 51) — DdgiCommon.hlsli / DdgiBlend.hlsl / DdgiTrace.hlsl과 같은 식

namespace
{
	float AngleBetween(const FVector3& A, const FVector3& B)
	{
		return std::acos(std::clamp(FVector3::Dot(A.GetNormalized(), B.GetNormalized()), -1.0f, 1.0f));
	}
} // namespace

E_TEST(Ddgi_OctahedralRoundTrip)
{
	using namespace DdgiMath;
	// 여러 방향 왕복 (반구 양쪽, 축, 대각)
	for (uint32 Index = 0; Index < 200; ++Index)
	{
		const FVector3 D       = SphericalFibonacci(Index, 200);
		const FVector2 Encoded = OctEncode(D);
		E_EXPECT_TRUE(FMath::Abs(Encoded.X) <= 1.0f + 1.0e-5f && FMath::Abs(Encoded.Y) <= 1.0f + 1.0e-5f);
		E_EXPECT_NEAR(AngleBetween(OctDecode(Encoded), D), 0.0f, 1.0e-3f);
	}
	E_EXPECT_NEAR(AngleBetween(OctDecode(OctEncode(FVector3(0.0f, 0.0f, -1.0f))), FVector3(0.0f, 0.0f, -1.0f)), 0.0f, 1.0e-4f);
	// 칸 중심 방향 → 좌표 → 같은 칸
	for (uint32 Y = 0; Y < IrradianceTexels; ++Y)
	{
		for (uint32 X = 0; X < IrradianceTexels; ++X)
		{
			const FVector2 Coord = DirectionToTexelCoord(GetTexelDirection(X, Y, IrradianceTexels), IrradianceTexels);
			E_EXPECT_NEAR(Coord.X, static_cast<float>(X) + 0.5f, 1.0e-3f);
			E_EXPECT_NEAR(Coord.Y, static_cast<float>(Y) + 0.5f, 1.0e-3f);
		}
	}
}

E_TEST(Ddgi_BorderTexelsMirrorAcrossOctahedralSeams)
{
	using namespace DdgiMath;
	// 테두리 칸의 연장 좌표를 팔면체 접기로 [-1, 1]²로 되돌린 점 = 매핑된 안쪽 칸 중심 (같은 구 위 방향)
	for (const uint32 Texels : { IrradianceTexels, DistanceTexels })
	{
		const uint32 Last = Texels + 1;
		const auto   Center = [&](uint32 T) { return (static_cast<float>(T) - 1.0f + 0.5f) / static_cast<float>(Texels) * 2.0f - 1.0f; };
		for (uint32 Y = 0; Y <= Last; ++Y)
		{
			for (uint32 X = 0; X <= Last; ++X)
			{
				uint32 MX = 0;
				uint32 MY = 0;
				MapBorderTexel(X, Y, Texels, MX, MY);
				E_EXPECT_TRUE(MX >= 1 && MX <= Texels && MY >= 1 && MY <= Texels);
				// 연장 좌표 접기: |u| > 1이면 (u, v) → (±2 − u, −v), |v| > 1이면 (−u, ±2 − v)
				float U = Center(X);
				float V = Center(Y);
				if (FMath::Abs(V) > 1.0f)
				{
					V = (V > 0.0f ? 2.0f : -2.0f) - V;
					U = -U;
				}
				if (FMath::Abs(U) > 1.0f)
				{
					U = (U > 0.0f ? 2.0f : -2.0f) - U;
					V = -V;
				}
				E_EXPECT_NEAR(U, Center(MX), 1.0e-4f);
				E_EXPECT_NEAR(V, Center(MY), 1.0e-4f);
				// 그 방향들은 실제로 가깝다 (팔면체 이음매 너머 이웃)
				if (X == 0 || Y == 0 || X == Last || Y == Last)
				{
					const FVector3 Mapped = GetTexelDirection(MX - 1, MY - 1, Texels);
					const FVector3 Folded = OctDecode(FVector2(U, V));
					E_EXPECT_NEAR(AngleBetween(Mapped, Folded), 0.0f, 1.0e-3f);
				}
			}
		}
		// 안쪽 칸은 그대로
		uint32 MX = 0;
		uint32 MY = 0;
		MapBorderTexel(3, 4, Texels, MX, MY);
		E_EXPECT_EQ(MX, 3u);
		E_EXPECT_EQ(MY, 4u);
	}
}

E_TEST(Ddgi_FibonacciRaysAndRotation)
{
	using namespace DdgiMath;
	// 피보나치 방향: 단위 길이, 평균 ≈ 0 (고르게), 서로 다름
	constexpr uint32 Count = 128;
	FVector3         Sum;
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const FVector3 D = SphericalFibonacci(Index, Count);
		E_EXPECT_NEAR(D.Length(), 1.0f, 1.0e-4f);
		Sum += D;
	}
	E_EXPECT_NEAR((Sum / static_cast<float>(Count)).Length(), 0.0f, 0.02f);
	// 위 반구 비율 ≈ 1/2
	uint32 Upper = 0;
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		Upper += SphericalFibonacci(Index, Count).Z > 0.0f ? 1u : 0u;
	}
	E_EXPECT_EQ(Upper, Count / 2);

	// 프레임 회전: 직교 정규, 행렬식 +1, 프레임마다 다르고 같은 프레임이면 같다 (결정적)
	for (uint32 Frame = 0; Frame < 16; ++Frame)
	{
		const FRotation R = MakeRayRotation(Frame);
		for (uint32 A = 0; A < 3; ++A)
		{
			E_EXPECT_NEAR(R.Rows[A].Length(), 1.0f, 1.0e-4f);
			for (uint32 B = A + 1; B < 3; ++B)
			{
				E_EXPECT_NEAR(FVector3::Dot(R.Rows[A], R.Rows[B]), 0.0f, 1.0e-4f);
			}
		}
		E_EXPECT_NEAR(FVector3::Dot(FVector3::Cross(R.Rows[0], R.Rows[1]), R.Rows[2]), 1.0f, 1.0e-4f);
		const FRotation Again = MakeRayRotation(Frame);
		E_EXPECT_NEAR(Again.Rows[1].X, R.Rows[1].X, 0.0f);
	}
	E_EXPECT_TRUE(FMath::Abs(MakeRayRotation(1).Rows[0].X - MakeRayRotation(2).Rows[0].X) > 1.0e-4f);

	// 광선: 고정 광선은 회전과 무관, 나머지는 회전 적용
	const FRotation R0 = MakeRayRotation(3);
	const FRotation R1 = MakeRayRotation(4);
	E_EXPECT_NEAR(AngleBetween(GetRayDirection(5, 160, FixedRayCount, R0), GetRayDirection(5, 160, FixedRayCount, R1)), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(AngleBetween(GetRayDirection(5, 160, FixedRayCount, R0), SphericalFibonacci(5, FixedRayCount)), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(AngleBetween(GetRayDirection(40, 160, FixedRayCount, R0), R0.Rotate(SphericalFibonacci(8, 160 - FixedRayCount))), 0.0f, 1.0e-6f);
	E_EXPECT_TRUE(AngleBetween(GetRayDirection(40, 160, FixedRayCount, R0), GetRayDirection(40, 160, FixedRayCount, R1)) > 1.0e-3f);
}

E_TEST(Ddgi_GridIndexAndTrilinear)
{
	using namespace DdgiMath;
	// 상자 반 크기 (450, 350, 150), 간격 100 → 칸 가운데 격자 9/7/3, 첫 프로브 = 최소 꼭짓점 + 반 칸
	const FProbeGrid Grid = MakeGrid(FVector3(0.0f, 0.0f, 150.0f), FVector3(450.0f, 350.0f, 150.0f), 100.0f);
	E_EXPECT_EQ(Grid.Counts[0], 9u);
	E_EXPECT_EQ(Grid.Counts[1], 7u);
	E_EXPECT_EQ(Grid.Counts[2], 3u);
	E_EXPECT_NEAR(Grid.Origin.X, -400.0f, 1.0e-3f);
	E_EXPECT_NEAR(Grid.Origin.Z, 50.0f, 1.0e-3f);
	E_EXPECT_NEAR(Grid.GetMax().Z, 250.0f, 1.0e-3f);
	E_EXPECT_NEAR(Grid.Spacing.X, 100.0f, 1.0e-3f);
	// 셰이딩 상자 = 컴포넌트 상자
	E_EXPECT_NEAR(Grid.GetBoxMin().X, -450.0f, 1.0e-3f);
	E_EXPECT_NEAR(Grid.GetBoxMax().Y, 350.0f, 1.0e-3f);
	E_EXPECT_NEAR(Grid.GetBoxMax().Z, 300.0f, 1.0e-3f);
	// 나누어떨어지지 않으면 간격을 늘리거나 줄여 맞춘다 (상자 크기 그대로)
	const FProbeGrid Odd = MakeGrid(FVector3(0.0f), FVector3(430.0f, 330.0f, 180.0f), 100.0f);
	E_EXPECT_EQ(Odd.Counts[0], 9u);
	E_EXPECT_NEAR(Odd.GetBoxMax().X, 430.0f, 1.0e-3f);
	E_EXPECT_NEAR(Odd.Spacing.X, 860.0f / 9.0f, 1.0e-3f);
	// 번호 ↔ 좌표 ↔ 위치
	for (uint32 Index = 0; Index < Grid.GetProbeCount(); Index += 7)
	{
		uint32 C[3];
		Grid.GetCoords(Index, C);
		E_EXPECT_EQ(Grid.GetIndex(C[0], C[1], C[2]), Index);
	}
	const FVector3 P = Grid.GetPosition(Grid.GetIndex(3, 2, 1));
	E_EXPECT_NEAR(P.X, -100.0f, 1.0e-3f);
	E_EXPECT_NEAR(P.Y, -100.0f, 1.0e-3f);
	E_EXPECT_NEAR(P.Z, 150.0f, 1.0e-3f);
	// 감싸는 칸 + 삼선형 가중 합 = 1, 프로브 위에서는 그 프로브만
	uint32   Base[3];
	FVector3 Alpha;
	FindBaseProbe(Grid, FVector3(-75.0f, -90.0f, 180.0f), Base, Alpha);
	E_EXPECT_EQ(Base[0], 3u);
	E_EXPECT_EQ(Base[1], 2u);
	E_EXPECT_EQ(Base[2], 1u);
	E_EXPECT_NEAR(Alpha.X, 0.25f, 1.0e-4f);
	E_EXPECT_NEAR(Alpha.Z, 0.3f, 1.0e-4f);
	float WeightSum = 0.0f;
	for (uint32 Corner = 0; Corner < 8; ++Corner)
	{
		WeightSum += TrilinearWeight(Alpha, Corner);
	}
	E_EXPECT_NEAR(WeightSum, 1.0f, 1.0e-5f);
	FindBaseProbe(Grid, P, Base, Alpha);
	E_EXPECT_NEAR(TrilinearWeight(Alpha, 0), 1.0f, 1.0e-5f);
	// 격자 밖(상자 끝 반 칸 포함)은 경계 칸으로 고정 (비율 0/1)
	FindBaseProbe(Grid, FVector3(440.0f, -10000.0f, 150.0f), Base, Alpha);
	E_EXPECT_EQ(Base[0], 7u);
	E_EXPECT_EQ(Base[1], 0u);
	E_EXPECT_NEAR(Alpha.X, 1.0f, 1.0e-6f);
	E_EXPECT_NEAR(Alpha.Y, 0.0f, 1.0e-6f);
	// 축 최소 2, 상한 32, 전체 상한을 넘으면 간격을 늘린다
	const FProbeGrid Thin = MakeGrid(FVector3(0.0f), FVector3(500.0f, 500.0f, 10.0f), 100.0f);
	E_EXPECT_EQ(Thin.Counts[2], 2u);
	const FProbeGrid Big = MakeGrid(FVector3(0.0f), FVector3(10000.0f, 10000.0f, 10000.0f), 10.0f, 4096);
	E_EXPECT_TRUE(Big.Counts[0] <= MaxAxisProbes && Big.GetProbeCount() <= 4096u);
	E_EXPECT_NEAR(Grid.GetDistanceClamp(), 1.5f * std::sqrt(3.0f) * 100.0f, 1.0e-2f);
}

E_TEST(Ddgi_ChebyshevVisibilityAndWeights)
{
	using namespace DdgiMath;
	// 평균보다 가까우면 보임, 멀수록 줄고 분산이 작을수록 급하게 준다
	E_EXPECT_NEAR(ComputeVisibility(0.2f, 0.3f, 0.1f), 1.0f, 0.0f);
	const float Near = ComputeVisibility(0.35f, 0.3f, 0.3f * 0.3f + 0.01f);
	const float Far  = ComputeVisibility(0.8f, 0.3f, 0.3f * 0.3f + 0.01f);
	E_EXPECT_TRUE(Near > Far && Far >= 0.0f && Near < 1.0f);
	const float Sharp = ComputeVisibility(0.35f, 0.3f, 0.3f * 0.3f + 0.0001f);
	E_EXPECT_TRUE(Sharp < Near);
	// 벽 너머 (평균 거리 0.1, 분산 거의 0, 셰이딩 점 거리 0.6) → 거의 0 (누수 차단)
	E_EXPECT_TRUE(ComputeVisibility(0.6f, 0.1f, 0.01f + 1.0e-6f) < 1.0e-6f);

	// 프로브 가중치: 앞쪽 프로브 > 뒤쪽 프로브, 보이는 프로브 > 가려진 프로브, 삼선형 비례, 가려져도 0은 아님(바닥 0.05 → 세제곱 감쇠)
	const FVector3 N(0.0f, 0.0f, 1.0f);
	const float Front   = ComputeProbeWeight(FVector3(0.0f, 0.0f, 1.0f), N, 1.0f, 1.0f);
	const float Back    = ComputeProbeWeight(FVector3(0.0f, 0.0f, -1.0f), N, 1.0f, 1.0f);
	const float Hidden  = ComputeProbeWeight(FVector3(0.0f, 0.0f, 1.0f), N, 1.0f, 0.0f);
	const float HalfTri = ComputeProbeWeight(FVector3(0.0f, 0.0f, 1.0f), N, 0.5f, 1.0f);
	E_EXPECT_NEAR(Front, 1.2f, 1.0e-5f);
	E_EXPECT_NEAR(Back, 0.2f, 1.0e-5f); // 감싸기 바닥 0.2 (세제곱 감쇠 경계)
	E_EXPECT_TRUE(Hidden > 0.0f && Hidden < 0.01f);
	E_EXPECT_NEAR(HalfTri, Front * 0.5f, 1.0e-5f);
	// 0.2 아래는 세제곱 비례로 더 줄인다 (w³ / 0.04)
	const float Low = ComputeProbeWeight(FVector3(0.0f, 0.0f, 1.0f), N, 1.0f, 0.1f); // 1.2 × 0.1 = 0.12
	E_EXPECT_NEAR(Low, 0.12f * 0.12f * 0.12f / 0.04f, 1.0e-6f);
}

E_TEST(Ddgi_HysteresisBlend)
{
	using namespace DdgiMath;
	const FVector3 Previous(1.0f, 1.0f, 1.0f);
	// 처음 = 새 값 그대로
	E_EXPECT_NEAR(ComputeHysteresis(Previous, FVector3(5.0f), 0.97f, 0.3f, true), 0.0f, 0.0f);
	// 작은 변화 = 기본 히스테리시스 (느리게 수렴)
	const float Small = ComputeHysteresis(Previous, FVector3(1.1f, 1.0f, 0.95f), 0.97f, 0.3f, false);
	E_EXPECT_NEAR(Small, 0.97f, 0.0f);
	const FVector3 Blended = Blend(Previous, FVector3(2.0f), Small);
	E_EXPECT_NEAR(Blended.X, 1.0f + 1.0f * 0.03f, 1.0e-5f);
	// 급변(시간대·조명) = 0.75 빼서 빠르게
	const float Fast = ComputeHysteresis(Previous, FVector3(3.0f, 1.0f, 1.0f), 0.97f, 0.3f, false);
	E_EXPECT_NEAR(Fast, 0.22f, 1.0e-5f);
	// 어두워지는 급변도 같은 규칙 (크기 = 이전·새 값 중 큰 쪽 기준)
	E_EXPECT_NEAR(ComputeHysteresis(FVector3(4.0f), FVector3(1.0f), 0.97f, 0.3f, false), 0.22f, 1.0e-5f);
	// 아주 어두운 값의 작은 절대 변화는 급변 아님 (바닥 1e-3)
	E_EXPECT_NEAR(ComputeHysteresis(FVector3(0.0f), FVector3(0.0002f), 0.97f, 0.3f, false), 0.97f, 0.0f);
	// 기본(문턱 1 이상)은 급변 감지 없음: 아무리 바뀌어도 기본 히스테리시스 (광선 잡음에 걸려 깜빡이지 않게)
	E_EXPECT_NEAR(ComputeHysteresis(Previous, FVector3(1000.0f), 0.97f, 1.0f, false), 0.97f, 0.0f);
	E_EXPECT_NEAR(ComputeHysteresis(FVector3(1000.0f), FVector3(0.0f), 0.97f, 1.0f, false), 0.97f, 0.0f);
	// 처음 n번은 누적 평균 (n / (n + 1) 상한) — 1번째 0, 2번째 1/2, 3번째 2/3 … 기본값에서 멈춘다
	E_EXPECT_NEAR(ComputeHysteresis(Previous, Previous, 0.97f, 0.3f, false, 1), 0.5f, 1.0e-6f);
	E_EXPECT_NEAR(ComputeHysteresis(Previous, Previous, 0.97f, 0.3f, false, 2), 2.0f / 3.0f, 1.0e-6f);
	E_EXPECT_NEAR(ComputeHysteresis(Previous, Previous, 0.97f, 0.3f, false, 500), 0.97f, 0.0f);
	// 누적 평균이면 n개 표본의 평균과 같다
	{
		FVector3     Mean(0.0f);
		const float  Samples[4] = { 2.0f, 4.0f, 6.0f, 8.0f };
		for (uint32 Index = 0; Index < 4; ++Index)
		{
			Mean = Blend(Mean, FVector3(Samples[Index]), ComputeHysteresis(Mean, FVector3(Samples[Index]), 0.97f, 100.0f, Index == 0, Index));
		}
		E_EXPECT_NEAR(Mean.X, 5.0f, 1.0e-4f);
	}
	// 상태 텍셀 인코딩 (w = 상태 + 4 × 갱신 횟수)
	E_EXPECT_EQ(GetProbeState(EncodeProbeState(ProbeState_Inactive, 37)), static_cast<uint32>(ProbeState_Inactive));
	E_EXPECT_EQ(GetProbeUpdateCount(EncodeProbeState(ProbeState_Active, 37)), 37u);
	E_EXPECT_EQ(GetProbeUpdateCount(EncodeProbeState(ProbeState_Active, 100000)), MaxUpdateCount);
	// 수렴: 일정한 입력을 반복하면 이전 값이 입력으로 다가간다
	FVector3 Value(0.0f);
	for (uint32 Step = 0; Step < 200; ++Step)
	{
		Value = Blend(Value, FVector3(1.0f), ComputeHysteresis(Value, FVector3(1.0f), 0.97f, 0.3f, Step == 0));
	}
	E_EXPECT_NEAR(Value.X, 1.0f, 1.0e-3f);
}

E_TEST(Ddgi_EdgeFadeAndAtlasLayout)
{
	using namespace DdgiMath;
	const FVector3 Min(-100.0f, -100.0f, 0.0f);
	const FVector3 Max(100.0f, 100.0f, 200.0f);
	E_EXPECT_NEAR(ComputeEdgeFade(FVector3(0.0f, 0.0f, 100.0f), Min, Max, 50.0f), 1.0f, 0.0f);
	E_EXPECT_NEAR(ComputeEdgeFade(FVector3(75.0f, 0.0f, 100.0f), Min, Max, 50.0f), 0.5f, 1.0e-5f);
	E_EXPECT_NEAR(ComputeEdgeFade(FVector3(150.0f, 0.0f, 100.0f), Min, Max, 50.0f), 0.0f, 0.0f);
	E_EXPECT_NEAR(ComputeEdgeFade(FVector3(0.0f, 0.0f, 10.0f), Min, Max, 50.0f), 0.2f, 1.0e-5f);

	// 아틀라스: 프로브 g → 타일 (g % 64, g / 64) × 타일 크기, 행/열 수
	uint32 X = 0;
	uint32 Y = 0;
	GetTileOrigin(130, IrradianceTileSize, X, Y);
	E_EXPECT_EQ(X, 2u * IrradianceTileSize);
	E_EXPECT_EQ(Y, 2u * IrradianceTileSize);
	GetTileOrigin(63, DistanceTileSize, X, Y);
	E_EXPECT_EQ(X, 63u * DistanceTileSize);
	E_EXPECT_EQ(Y, 0u);
	E_EXPECT_EQ(GetAtlasRows(64), 1u);
	E_EXPECT_EQ(GetAtlasRows(65), 2u);
	E_EXPECT_EQ(GetAtlasColumns(10), 10u);
	E_EXPECT_EQ(GetAtlasColumns(1000), AtlasTilesPerRow);
	E_EXPECT_EQ(GetAtlasRows(MaxTotalProbes) * DistanceTileSize, 4608u); // 텍스처 크기 상한 안
}

E_TEST(Ddgi_UpdateSchedule)
{
	using namespace DdgiMath;
	// 순환 범위: 시작 90, 20개, 프로브 100 → 90~99, 0~9
	uint32 Row = 0;
	E_EXPECT_TRUE(IsProbeUpdated(95, 100, 90, 20, 7, Row));
	E_EXPECT_EQ(Row, 7u + 5u);
	E_EXPECT_TRUE(IsProbeUpdated(9, 100, 90, 20, 0, Row));
	E_EXPECT_EQ(Row, 19u);
	E_EXPECT_FALSE(IsProbeUpdated(10, 100, 90, 20, 0, Row));
	E_EXPECT_FALSE(IsProbeUpdated(89, 100, 90, 20, 0, Row));
	// 전부 갱신: 모든 프로브가 서로 다른 행
	std::set<uint32> Rows;
	for (uint32 Local = 0; Local < 100; ++Local)
	{
		E_EXPECT_TRUE(IsProbeUpdated(Local, 100, 37, 100, 0, Row));
		Rows.insert(Row);
	}
	E_EXPECT_EQ(static_cast<uint32>(Rows.size()), 100u);
	// 예산 나누기: 무제한 = 전부, 전체 예산은 프로브 수 비율 (최소 1), 볼륨 상한
	E_EXPECT_EQ(ComputeVolumeUpdateCount(500, 0, 1000, 0), 500u);
	E_EXPECT_EQ(ComputeVolumeUpdateCount(500, 0, 1000, 200), 100u);
	E_EXPECT_EQ(ComputeVolumeUpdateCount(1, 0, 1000, 200), 1u);
	E_EXPECT_EQ(ComputeVolumeUpdateCount(500, 50, 1000, 200), 50u);
	E_EXPECT_EQ(ComputeVolumeUpdateCount(500, 0, 1000, 5000), 500u);
	// 몇 프레임 돌면 모든 프로브가 한 번씩 (커서 순환)
	std::set<uint32> Seen;
	uint32           Cursor = 0;
	for (uint32 Frame = 0; Frame < 5; ++Frame)
	{
		const uint32 Count = ComputeVolumeUpdateCount(100, 0, 100, 20);
		for (uint32 Local = 0; Local < 100; ++Local)
		{
			if (IsProbeUpdated(Local, 100, Cursor, Count, 0, Row))
			{
				Seen.insert(Local);
			}
		}
		Cursor = (Cursor + Count) % 100;
	}
	E_EXPECT_EQ(static_cast<uint32>(Seen.size()), 100u);
}

E_TEST(Ddgi_LightChangeBoost)
{
	using namespace DdgiMath;
	const FVector3 Down(0.0f, 0.0f, -1.0f);
	const FVector3 Sun(3.0f, 3.0f, 3.0f);
	// 같은 조명 = 0, 방향 2도 = 1, 1도 = 0.5, 복사 10% = 1, 하늘 배율 20% = 2
	E_EXPECT_NEAR(ComputeLightChange(Down, Sun, 1.0f, Down, Sun, 1.0f), 0.0f, 1.0e-6f);
	const float Angle = FMath::DegreesToRadians(2.0f);
	const FVector3 Tilted(std::sin(Angle), 0.0f, -std::cos(Angle));
	E_EXPECT_NEAR(ComputeLightChange(Down, Sun, 1.0f, Tilted, Sun, 1.0f), 1.0f, 1.0e-2f);
	const float Half = FMath::DegreesToRadians(1.0f);
	E_EXPECT_NEAR(ComputeLightChange(Down, Sun, 1.0f, FVector3(std::sin(Half), 0.0f, -std::cos(Half)), Sun, 1.0f), 0.5f, 1.0e-2f);
	E_EXPECT_NEAR(ComputeLightChange(Down, Sun, 1.0f, Down, Sun * 0.9f, 1.0f), 1.0f, 1.0e-4f);
	E_EXPECT_NEAR(ComputeLightChange(Down, Sun, 1.0f, Down, Sun, 0.8f), 2.0f, 1.0e-4f);
	// 방향광 꺼짐(복사 0)이면 방향은 무시, 켜지면 복사 변화 100% → 큰 값
	E_EXPECT_NEAR(ComputeLightChange(Down, FVector3(0.0f), 1.0f, FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f), 1.0f), 0.0f, 1.0e-6f);
	E_EXPECT_TRUE(ComputeLightChange(Down, FVector3(0.0f), 1.0f, Down, Sun, 1.0f) >= 10.0f - 1.0e-3f);
}

namespace
{
	// Tests/GI 방 안쪽 면 (x ±400, y ±300, z 0~300) + +X 벽 창 구멍 (y ±100, z 80~220): 방 안 점에서 가장 가까운 면 거리 (창 = 빗나감)
	float TraceDemoRoom(const FVector3& Origin, const FVector3& Direction)
	{
		const float Planes[3][2] = { { -400.0f, 400.0f }, { -300.0f, 300.0f }, { 0.0f, 300.0f } };
		float       Best         = 1.0e27f;
		for (uint32 Axis = 0; Axis < 3; ++Axis)
		{
			const float D = Axis == 0 ? Direction.X : (Axis == 1 ? Direction.Y : Direction.Z);
			const float O = Axis == 0 ? Origin.X : (Axis == 1 ? Origin.Y : Origin.Z);
			if (FMath::Abs(D) < 1.0e-6f)
			{
				continue;
			}
			for (uint32 Side = 0; Side < 2; ++Side)
			{
				const float T = (Planes[Axis][Side] - O) / D;
				if (T <= 0.0f || T >= Best)
				{
					continue;
				}
				const FVector3 P = Origin + Direction * T;
				if (Axis == 0 && Side == 1 && P.Y >= -100.0f && P.Y <= 100.0f && P.Z >= 80.0f && P.Z <= 220.0f)
				{
					continue;
				}
				Best = T;
			}
		}
		return Best;
	}

	// 고정 광선(회전 없는 피보나치 32개)으로 재배치를 Frames번 반복 → 마지막 두 오프셋
	void SimulateRelocation(const FVector3& Base, const FVector3& Spacing, uint32 Frames, FVector3& OutPrevious, FVector3& OutLast)
	{
		using namespace DdgiMath;
		const float MinFrontface = 0.3f * FMath::Min(Spacing.X, FMath::Min(Spacing.Y, Spacing.Z));
		FVector3    Offset(0.0f);
		OutPrevious = Offset;
		for (uint32 Frame = 0; Frame < Frames; ++Frame)
		{
			FRelocationRays Rays;
			for (uint32 Ray = 0; Ray < FixedRayCount; ++Ray)
			{
				const FVector3 Direction = SphericalFibonacci(Ray, FixedRayCount);
				const float    Distance  = TraceDemoRoom(Base + Offset, Direction);
				if (Distance < Rays.ClosestFrontface)
				{
					Rays.ClosestFrontface    = Distance;
					Rays.ClosestFrontfaceDir = Direction;
				}
				if (Distance > Rays.FarthestFrontface)
				{
					Rays.FarthestFrontface    = Distance;
					Rays.FarthestFrontfaceDir = Direction;
				}
			}
			OutPrevious = Offset;
			Offset      = ComputeRelocationOffset(Offset, Rays, Spacing, MinFrontface, 0.25f);
		}
		OutLast = Offset;
	}
} // namespace

E_TEST(Ddgi_RelocationConvergesWithoutOscillation)
{
	using namespace DdgiMath;
	// Tests/GI 격자 (상자 반 크기 430x330x180, 가운데 Z 150 → 9x7x4): 벽에서 7~15cm인 바깥 프로브가 밀기 ↔ 되돌아가기로
	// 매 프레임 왕복했다 (예전 되돌아가는 문턱 = MinFrontface). 모든 프로브가 멈추고(마지막 두 프레임 같음) 벽에서 MinFrontface 이상 떨어져야 한다
	const FVector3 Spacing(860.0f / 9.0f, 660.0f / 7.0f, 360.0f / 4.0f);
	const FVector3 Origin   = FVector3(-430.0f, -330.0f, -30.0f) + Spacing * 0.5f;
	const float    MinFront = 0.3f * Spacing.Z;
	uint32         Moved    = 0;
	for (uint32 Z = 0; Z < 4; ++Z)
	{
		for (uint32 Y = 0; Y < 7; ++Y)
		{
			for (uint32 X = 0; X < 9; ++X)
			{
				const FVector3 Base = Origin + FVector3(Spacing.X * static_cast<float>(X), Spacing.Y * static_cast<float>(Y), Spacing.Z * static_cast<float>(Z));
				FVector3       Previous;
				FVector3       Last;
				SimulateRelocation(Base, Spacing, 200, Previous, Last);
				E_EXPECT_NEAR((Last - Previous).Length(), 0.0f, 1.0e-3f);
				Moved += Last.Length() > 1.0f ? 1u : 0u;
				// 밀려난 프로브는 여유를 얻었다 (오프셋 상한에 막히지 않은 경우): 축마다 가장 가까운 벽까지
				const FVector3 P     = Base + Last;
				const float    Clear = FMath::Min(FMath::Min(FMath::Min(P.X + 400.0f, 400.0f - P.X), FMath::Min(P.Y + 300.0f, 300.0f - P.Y)), FMath::Min(P.Z, 300.0f - P.Z));
				if (Last.Length() > 1.0f)
				{
					E_EXPECT_TRUE(Clear >= MinFront * 0.5f);
				}
			}
		}
	}
	E_EXPECT_TRUE(Moved > 0u); // 바깥 층은 실제로 밀린다
	// 되돌아가기: 여유가 ReturnClearance보다 크면 격자 쪽으로 (여유 - ReturnClearance)만큼, 그 안이면 그대로
	FRelocationRays Open;
	Open.ClosestFrontface    = MinFront * RelocationReturnScale + 11.0f;
	Open.ClosestFrontfaceDir = FVector3(1.0f, 0.0f, 0.0f);
	Open.FarthestFrontface   = 500.0f;
	Open.FarthestFrontfaceDir = FVector3(-1.0f, 0.0f, 0.0f);
	const FVector3 Back = ComputeRelocationOffset(FVector3(0.0f, 0.0f, 20.0f), Open, Spacing, MinFront, 0.25f);
	E_EXPECT_NEAR(Back.Z, 9.0f, 1.0e-3f);
	Open.ClosestFrontface = MinFront * RelocationReturnScale - 1.0f;
	E_EXPECT_NEAR(ComputeRelocationOffset(FVector3(0.0f, 0.0f, 20.0f), Open, Spacing, MinFront, 0.25f).Z, 20.0f, 0.0f);
}

E_TEST(Ddgi_FrameHysteresisBoostAndSettle)
{
	using namespace DdgiMath;
	// 평소 = 볼륨 값, 정착 = min(볼륨, 0.97), 가속 = min(…, 0.7) — 가속이 정착보다 우선
	E_EXPECT_NEAR(ComputeFrameHysteresis(0.99f, false, false, 0.7f, 0.97f), 0.99f, 0.0f);
	E_EXPECT_NEAR(ComputeFrameHysteresis(0.99f, false, true, 0.7f, 0.97f), 0.97f, 0.0f);
	E_EXPECT_NEAR(ComputeFrameHysteresis(0.99f, true, true, 0.7f, 0.97f), 0.7f, 0.0f);
	// 볼륨 값이 정착 값보다 낮으면 그대로 (예전 0.97 씬은 정착 구간이 바꾸지 않는다)
	E_EXPECT_NEAR(ComputeFrameHysteresis(0.97f, false, true, 0.7f, 0.97f), 0.97f, 0.0f);
	E_EXPECT_NEAR(ComputeFrameHysteresis(0.9f, false, true, 0.7f, 0.97f), 0.9f, 0.0f);
	// 상한 0.995
	E_EXPECT_NEAR(ComputeFrameHysteresis(1.0f, false, false, 0.7f, 0.97f), 0.995f, 0.0f);
	// 잡음 비: 정상 상태 std ∝ √((1-h)/(1+h)) — 0.99는 0.97의 약 0.58배
	const float Ratio = std::sqrt((1.0f - 0.99f) / 1.99f) / std::sqrt((1.0f - 0.97f) / 1.97f);
	E_EXPECT_NEAR(Ratio, 0.576f, 0.01f);
}
