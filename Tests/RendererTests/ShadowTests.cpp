#include "Core/Testing/TestFramework.h"
#include "Renderer/ShadowMath.h"

#include <cstring>

E_TEST(Shadow_CascadeSplits)
{
	const auto Uniform = ShadowMath::ComputeCascadeSplits(1.0f, 101.0f, 4, 0.0f);
	const auto Logarithmic = ShadowMath::ComputeCascadeSplits(1.0f, 10000.0f, 4, 1.0f);
	for (uint32 Index = 0; Index < 4; ++Index)
	{
		E_EXPECT_NEAR(Uniform[Index], 1.0f + 25.0f * static_cast<float>(Index + 1), 0.001f);
		E_EXPECT_NEAR(Logarithmic[Index], std::pow(10.0f, static_cast<float>(Index + 1)), 0.01f);
	}
	const auto Single = ShadowMath::ComputeCascadeSplits(0.1f, 60.0f, 0, 0.75f);
	E_EXPECT_NEAR(Single[0], 60.0f, 0.001f);
}

E_TEST(Shadow_CascadeContainsFrustum)
{
	const auto Corners = ShadowMath::ComputeFrustumSliceCorners(FVector3(3.0f, -7.0f, 2.0f),
		FVector3::ForwardVector, FVector3::RightVector, FVector3::UpVector,
		FMath::DegreesToRadians(70.0f), 16.0f / 9.0f, 0.1f, 60.0f);
	const FVector3 Directions[] = { FVector3(0.3f, 0.5f, -1.0f), FVector3::UpVector, -FVector3::UpVector };
	for (const FVector3& Direction : Directions)
	{
		const auto Cascade = ShadowMath::ComputeCascade(Corners, Direction, 256, 50.0f);
		E_EXPECT_TRUE(Cascade.WorldTexelSize > 0.0f);
		for (const FVector3& Corner : Corners)
		{
			const FVector3 Clip = Cascade.ViewProjection.TransformPosition(Corner);
			E_EXPECT_TRUE(FMath::Abs(Clip.X) <= 1.00001f);
			E_EXPECT_TRUE(FMath::Abs(Clip.Y) <= 1.00001f);
			E_EXPECT_TRUE(Clip.Z >= 0.0f && Clip.Z <= 1.0f);
		}
	}
}

E_TEST(Shadow_StableTexelGrid)
{
	const auto Corners = ShadowMath::ComputeFrustumSliceCorners(FVector3::ZeroVector,
		FVector3::ForwardVector, FVector3::RightVector, FVector3::UpVector,
		FMath::DegreesToRadians(60.0f), 1.0f, 1.0f, 20.0f);
	const auto First = ShadowMath::ComputeCascade(Corners, -FVector3::UpVector, 2048, 50.0f);
	const FMatrix4x4 LightView = FMatrix4x4::MakeLookAt(FVector3::ZeroVector, -FVector3::UpVector, FVector3::ForwardVector);
	const FVector3 Center = LightView.TransformPosition(First.SphereCenter);
	E_EXPECT_NEAR(Center.X / First.WorldTexelSize, FMath::Floor(Center.X / First.WorldTexelSize + 0.5f), 0.001f);
	E_EXPECT_NEAR(Center.Y / First.WorldTexelSize, FMath::Floor(Center.Y / First.WorldTexelSize + 0.5f), 0.001f);
}
namespace
{
	std::array<FVector3, 8> MakeSliceAt(const FVector3& Position, float Yaw)
	{
		const FVector3 Forward(FMath::Cos(Yaw), FMath::Sin(Yaw), 0.0f);
		const FVector3 Right(-FMath::Sin(Yaw), FMath::Cos(Yaw), 0.0f);
		return ShadowMath::ComputeFrustumSliceCorners(Position, Forward, Right, FVector3::UpVector, FMath::DegreesToRadians(70.0f), 16.0f / 9.0f, 800.0f,
		                                              2500.0f);
	}
} // namespace

// 양자화 캐스케이드(그림자 캐시 이동 중 재사용): 카메라가 칸 안에서 조금 움직이면 뷰-투영이 비트 단위로 같고,
// 칸을 넘으면 바뀌며, 조각은 언제나 캐스케이드 안에 들어 있다
E_TEST(Shadow_QuantizedCascadeStableAndContains)
{
	const FVector3 Light = FVector3(0.4f, 0.3f, -1.0f).GetNormalized();
	const float    Quantize = 0.125f;
	const auto     Base     = ShadowMath::ComputeCascade(MakeSliceAt(FVector3(1234.5f, -876.25f, 180.0f), 0.3f), Light, 2048, 5000.0f, Quantize);
	const auto     Plain    = ShadowMath::ComputeCascade(MakeSliceAt(FVector3(1234.5f, -876.25f, 180.0f), 0.3f), Light, 2048, 5000.0f);
	E_EXPECT_TRUE(Base.WorldTexelSize > Plain.WorldTexelSize);
	E_EXPECT_TRUE(Base.WorldTexelSize <= Plain.WorldTexelSize * (1.0f + Quantize) * 1.05f); // 해상도 손실 ≈ (1 + Quantize)
	uint32 Same    = 0;
	uint32 Changed = 0;
	for (int32 Step = 0; Step < 400; ++Step)
	{
		// 프레임마다 3cm씩 걷는 카메라 (12m)
		const auto     Corners = MakeSliceAt(FVector3(1234.5f + 3.0f * static_cast<float>(Step), -876.25f + 1.0f * static_cast<float>(Step), 180.0f), 0.3f);
		const auto     Cascade = ShadowMath::ComputeCascade(Corners, Light, 2048, 5000.0f, Quantize);
		const bool     bSame   = std::memcmp(&Cascade.ViewProjection, &Base.ViewProjection, sizeof(FMatrix4x4)) == 0;
		(bSame ? Same : Changed) += 1;
		for (const FVector3& Corner : Corners)
		{
			const FVector3 Clip = Cascade.ViewProjection.TransformPosition(Corner);
			E_EXPECT_TRUE(FMath::Abs(Clip.X) <= 1.00001f);
			E_EXPECT_TRUE(FMath::Abs(Clip.Y) <= 1.00001f);
			E_EXPECT_TRUE(Clip.Z >= 0.0f && Clip.Z <= 1.0f);
		}
	}
	E_EXPECT_TRUE(Same >= 1);    // 첫 칸 안에서는 그대로
	E_EXPECT_TRUE(Changed >= 1); // 칸을 넘으면 바뀐다
	// 칸 수 = 바뀐 횟수: 연속 프레임끼리 비교해 바뀐 횟수가 적어야 한다 (12m 이동 / 칸 ≈ 1.5m 이상)
	uint32     Transitions = 0;
	FMatrix4x4 Previous    = Base.ViewProjection;
	for (int32 Step = 0; Step < 400; ++Step)
	{
		const auto Cascade =
			ShadowMath::ComputeCascade(MakeSliceAt(FVector3(1234.5f + 3.0f * static_cast<float>(Step), -876.25f + 1.0f * static_cast<float>(Step), 180.0f), 0.3f),
			                           Light, 2048, 5000.0f, Quantize);
		Transitions += std::memcmp(&Cascade.ViewProjection, &Previous, sizeof(FMatrix4x4)) != 0 ? 1u : 0u;
		Previous = Cascade.ViewProjection;
	}
	E_EXPECT_TRUE(Transitions >= 1 && Transitions <= 40); // 평균 10프레임 이상 유지
	// 양자화 없으면 텍셀 스냅이라 거의 매 프레임 바뀐다 (비교 기준)
	uint32 PlainTransitions = 0;
	Previous                = Plain.ViewProjection;
	for (int32 Step = 0; Step < 400; ++Step)
	{
		const auto Cascade =
			ShadowMath::ComputeCascade(MakeSliceAt(FVector3(1234.5f + 3.0f * static_cast<float>(Step), -876.25f + 1.0f * static_cast<float>(Step), 180.0f), 0.3f),
			                           Light, 2048, 5000.0f);
		PlainTransitions += std::memcmp(&Cascade.ViewProjection, &Previous, sizeof(FMatrix4x4)) != 0 ? 1u : 0u;
		Previous = Cascade.ViewProjection;
	}
	E_EXPECT_TRUE(PlainTransitions > Transitions * 4);
}
