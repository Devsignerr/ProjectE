#include "Core/Testing/TestFramework.h"
#include "Renderer/IblMath.h"

#include <cmath>

E_TEST(Ibl_CubeFaceAxes)
{
	const FVector3 Expected[] = {
		FVector3(1.0f, 0.0f, 0.0f),
		FVector3(-1.0f, 0.0f, 0.0f),
		FVector3(0.0f, 1.0f, 0.0f),
		FVector3(0.0f, -1.0f, 0.0f),
		FVector3(0.0f, 0.0f, 1.0f),
		FVector3(0.0f, 0.0f, -1.0f),
	};

	for (uint32 Face = 0; Face < 6; ++Face)
	{
		const FVector3 Actual = IblMath::CubeFaceUVToDirection(Face, 0.0f, 0.0f);
		E_EXPECT_NEAR(FVector3::Dot(Actual, Expected[Face]), 1.0f, 1.0e-6f);
	}
}

E_TEST(Ibl_CubeDirectionRoundTrip)
{
	const float Coordinates[] = { -0.9f, -0.25f, 0.0f, 0.4f, 0.9f };

	for (uint32 Face = 0; Face < 6; ++Face)
	{
		for (float U : Coordinates)
		{
			for (float V : Coordinates)
			{
				const FVector3 Direction = IblMath::CubeFaceUVToDirection(Face, U, V);
				uint32 RecoveredFace = 0;
				float RecoveredU = 0.0f;
				float RecoveredV = 0.0f;
				IblMath::DirectionToCubeFaceUV(
					Direction, RecoveredFace, RecoveredU, RecoveredV);

				E_EXPECT_EQ(RecoveredFace, Face);
				E_EXPECT_NEAR(RecoveredU, U, 1.0e-5f);
				E_EXPECT_NEAR(RecoveredV, V, 1.0e-5f);
				E_EXPECT_NEAR(FVector3::Dot(Direction, Direction), 1.0f, 1.0e-5f);
			}
		}
	}

	// +X 면의 왼쪽 가장자리와 +Z 면의 오른쪽 가장자리는 같은 방향이다.
	const FVector3 A = IblMath::CubeFaceUVToDirection(0, -1.0f, 0.3f);
	const FVector3 B = IblMath::CubeFaceUVToDirection(4, 1.0f, 0.3f);
	E_EXPECT_NEAR(FVector3::Dot(A, B), 1.0f, 1.0e-5f);
}

E_TEST(Ibl_HammersleySequence)
{
	E_EXPECT_NEAR(IblMath::RadicalInverseVdC(0), 0.0f, 1.0e-7f);
	E_EXPECT_NEAR(IblMath::RadicalInverseVdC(1), 0.5f, 1.0e-7f);
	E_EXPECT_NEAR(IblMath::RadicalInverseVdC(2), 0.25f, 1.0e-7f);
	E_EXPECT_NEAR(IblMath::RadicalInverseVdC(3), 0.75f, 1.0e-7f);

	for (uint32 Index = 0; Index < IblMath::IntegrationSampleCount; ++Index)
	{
		const FVector2 Xi = IblMath::Hammersley(Index, IblMath::IntegrationSampleCount);
		E_EXPECT_TRUE(Xi.X >= 0.0f && Xi.X < 1.0f);
		E_EXPECT_TRUE(Xi.Y >= 0.0f && Xi.Y < 1.0f);
	}
}

E_TEST(Ibl_GgxSamplesAndMipRoughness)
{
	E_EXPECT_NEAR(IblMath::MipToRoughness(0, 6), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(IblMath::MipToRoughness(5, 6), 1.0f, 1.0e-6f);
	E_EXPECT_NEAR(IblMath::MipToRoughness(0, 1), 0.0f, 1.0e-6f);

	for (uint32 Mip = 0; Mip < IblMath::PrefilterMipCount; ++Mip)
	{
		const float Roughness = IblMath::MipToRoughness(Mip, IblMath::PrefilterMipCount);

		for (uint32 Index = 0; Index < 256; ++Index)
		{
			const FVector3 H = IblMath::ImportanceSampleGGXTangent(
				IblMath::Hammersley(Index, 256), Roughness);

			E_EXPECT_TRUE(std::isfinite(H.X) && std::isfinite(H.Y) && std::isfinite(H.Z));
			E_EXPECT_TRUE(H.Z >= 0.0f);
			E_EXPECT_NEAR(FVector3::Dot(H, H), 1.0f, 1.0e-4f);

			if (Mip == 0)
			{
				E_EXPECT_NEAR(H.Z, 1.0f, 1.0e-5f);
			}
		}
	}
}

E_TEST(Ibl_FilteredImportanceSampling)
{
	E_EXPECT_EQ(IblMath::GetFullMipCount(512), 10u);
	E_EXPECT_EQ(IblMath::GetFullMipCount(128), 8u);
	E_EXPECT_EQ(IblMath::GetFullMipCount(1), 1u);

	// GGX D: 정점(NdotH = 1)은 1 / (pi alpha^2), 반구 적분(D · cos)은 1
	const float Roughness = 0.4f;
	const float Alpha     = Roughness * Roughness;
	E_EXPECT_NEAR(IblMath::GgxDistribution(1.0f, Roughness), 1.0f / (FMath::Pi * Alpha * Alpha), 1.0e-3f);
	float Integral = 0.0f;
	constexpr int Steps = 4096;
	for (int Index = 0; Index < Steps; ++Index)
	{
		const float Theta = (static_cast<float>(Index) + 0.5f) / Steps * FMath::Pi * 0.5f;
		Integral += IblMath::GgxDistribution(FMath::Cos(Theta), Roughness) * FMath::Cos(Theta) * FMath::Sin(Theta) * 2.0f * FMath::Pi * (FMath::Pi * 0.5f / Steps);
	}
	E_EXPECT_NEAR(Integral, 1.0f, 0.01f);

	// 표본 입체각 = 텍셀 입체각이면 밉 1(+1 치우침), 표본 수 4배 → 밉 1 감소, 원본 크기 2배 → 밉 1 증가, 음수는 0
	const uint32 Size        = 512;
	const float  TexelAngle  = 4.0f * FMath::Pi / (6.0f * Size * Size);
	const float  MatchedPdf  = 1.0f / (256.0f * TexelAngle);
	E_EXPECT_NEAR(IblMath::ComputeFilteredSampleLod(MatchedPdf, 256, Size), 1.0f, 1.0e-3f);
	E_EXPECT_NEAR(IblMath::ComputeFilteredSampleLod(MatchedPdf / 64.0f, 256, Size), 4.0f, 1.0e-3f);
	E_EXPECT_NEAR(IblMath::ComputeFilteredSampleLod(MatchedPdf / 64.0f, 1024, Size), 3.0f, 1.0e-3f);
	E_EXPECT_NEAR(IblMath::ComputeFilteredSampleLod(MatchedPdf / 64.0f, 256, Size * 2), 5.0f, 1.0e-3f);
	E_EXPECT_NEAR(IblMath::ComputeFilteredSampleLod(MatchedPdf * 1.0e6f, 256, Size), 0.0f, 1.0e-6f);
}

E_TEST(Ibl_BrdfSmoothSurface)
{
	// 완전 매끈한 표면에서는 A=1-Fc, B=Fc인 Schlick 항으로 수렴한다.
	for (float NdotV : { 0.1f, 0.5f, 1.0f })
	{
		const FVector2 Result = IblMath::IntegrateBrdf(NdotV, 0.0f, 256);
		const float Fc = std::pow(1.0f - NdotV, 5.0f);

		E_EXPECT_NEAR(Result.X, 1.0f - Fc, 1.0e-4f);
		E_EXPECT_NEAR(Result.Y, Fc, 1.0e-4f);
	}
}

E_TEST(Ibl_BrdfFiniteAndBounded)
{
	for (float Roughness : { 0.0f, 0.1f, 0.5f, 1.0f })
	{
		for (float NdotV : { 0.0f, 0.1f, 0.5f, 1.0f })
		{
			const FVector2 Result = IblMath::IntegrateBrdf(NdotV, Roughness, 1024);

			E_EXPECT_TRUE(std::isfinite(Result.X) && std::isfinite(Result.Y));
			E_EXPECT_TRUE(Result.X >= 0.0f && Result.Y >= 0.0f);
			// 유한 샘플 오차를 허용하되 큰 에너지 증가를 검출한다.
			E_EXPECT_TRUE(Result.X + Result.Y <= 1.05f);
		}
	}

	const FVector2 Empty = IblMath::IntegrateBrdf(0.5f, 0.5f, 0);
	E_EXPECT_NEAR(Empty.X, 0.0f, 0.0f);
	E_EXPECT_NEAR(Empty.Y, 0.0f, 0.0f);
}
