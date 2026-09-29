#include "Core/Testing/TestFramework.h"
#include "Renderer/ShadowMath.h"

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