#include "Core/Testing/TestFramework.h"
#include "Renderer/Camera.h"
#include "Renderer/HzbMath.h"

namespace
{
	constexpr uint32 DepthWidth  = 64;
	constexpr uint32 DepthHeight = 32;

	// 원점에서 +X를 보는 카메라 (가로세로 2:1, 세로 시야 60도)
	FCamera MakeCamera()
	{
		FCamera Camera;
		Camera.SetPerspective(60.0f, 2.0f, 10.0f, 100000.0f);
		Camera.SetPosition(FVector3::ZeroVector);
		Camera.LookAt(FVector3(1000.0f, 0.0f, 0.0f));
		return Camera;
	}

	float ProjectDepth(const FMatrix4x4& ViewProjection, const FVector3& P)
	{
		const float Z = P.X * ViewProjection.M[0][2] + P.Y * ViewProjection.M[1][2] + P.Z * ViewProjection.M[2][2] + ViewProjection.M[3][2];
		const float W = P.X * ViewProjection.M[0][3] + P.Y * ViewProjection.M[1][3] + P.Z * ViewProjection.M[2][3] + ViewProjection.M[3][3];
		return Z / W;
	}

	// 깊이 → HZB 밉 체인 (CPU 참조)
	struct FCpuHzb
	{
		std::vector<std::vector<float>> Mips;
		std::vector<uint32>             Widths;

		explicit FCpuHzb(const std::vector<float>& Depth)
		{
			uint32             Width = 0, Height = 0;
			std::vector<float> Level = HzbMath::BuildFirstMip(Depth, DepthWidth, DepthHeight, Width, Height);
			Mips.push_back(Level);
			Widths.push_back(Width);
			while (Width > 1 || Height > 1)
			{
				uint32 OutWidth = 0, OutHeight = 0;
				Level = HzbMath::Downsample(Level, Width, Height, OutWidth, OutHeight);
				Mips.push_back(Level);
				Widths.push_back(OutWidth);
				Width  = OutWidth;
				Height = OutHeight;
			}
		}

		float Load(uint32 Mip, int32 X, int32 Y) const { return Mips[Mip][static_cast<size_t>(Y) * Widths[Mip] + X]; }
	};
} // namespace

E_TEST(Occlusion_HzbDimensionsAndDownsample)
{
	E_EXPECT_EQ(HzbMath::GetHzbDimension(64), 32u);
	E_EXPECT_EQ(HzbMath::GetHzbDimension(65), 64u); // 올림(65 / 2) = 33 → 2의 거듭제곱 64
	E_EXPECT_EQ(HzbMath::GetHzbDimension(1280), 1024u);
	E_EXPECT_EQ(HzbMath::GetHzbDimension(1), 1u);
	E_EXPECT_EQ(HzbMath::GetMipCount(32, 16), 6u); // 32x16 → 16x8 → 8x4 → 4x2 → 2x1 → 1x1
	E_EXPECT_EQ(HzbMath::GetMipCount(1024, 512), 11u);
	E_EXPECT_EQ(HzbMath::GetMipCount(1, 1), 1u);
	E_EXPECT_EQ(HzbMath::GetMipDimension(32, 0), 32u);
	E_EXPECT_EQ(HzbMath::GetMipDimension(32, 2), 8u);
	E_EXPECT_EQ(HzbMath::GetMipDimension(4, 5), 1u);

	// 홀수 깊이 크기: 마지막 열/행도 빠지지 않는다 (최댓값 보존, 깊이 밖은 0)
	const std::vector<float> Source = { 0.1f, 0.2f, 0.9f, //
	                                    0.3f, 0.4f, 0.5f, //
	                                    0.6f, 0.7f, 0.8f };
	uint32 Width = 0, Height = 0;
	const std::vector<float> Level = HzbMath::BuildFirstMip(Source, 3, 3, Width, Height);
	E_EXPECT_EQ(Width, 2u);
	E_EXPECT_EQ(Height, 2u);
	E_EXPECT_NEAR(Level[0], 0.4f, 0.0f);
	E_EXPECT_NEAR(Level[1], 0.9f, 0.0f);
	E_EXPECT_NEAR(Level[2], 0.7f, 0.0f);
	E_EXPECT_NEAR(Level[3], 0.8f, 0.0f);

	// 밉 선택: 사각형이 텍셀 2개 이하 (밉 k 텍셀 = 2^(k+1) 픽셀)
	E_EXPECT_EQ(HzbMath::SelectMip(1.0f, 1.0f, 6), 0u);
	E_EXPECT_EQ(HzbMath::SelectMip(2.0f, 1.0f, 6), 0u);
	E_EXPECT_EQ(HzbMath::SelectMip(3.0f, 1.0f, 6), 1u);
	E_EXPECT_EQ(HzbMath::SelectMip(16.0f, 5.0f, 6), 3u);
	E_EXPECT_EQ(HzbMath::SelectMip(1000.0f, 5.0f, 6), 5u); // 마지막 밉에서 멈춤
}

E_TEST(Occlusion_WallHidesObjectsBehindIt)
{
	const FCamera    Camera         = MakeCamera();
	const FMatrix4x4 ViewProjection = Camera.GetViewProjectionMatrix();

	// 화면 왼쪽 절반(픽셀 x < 32)에 거리 1000cm 벽, 나머지는 하늘(1)
	const float        WallDepth = ProjectDepth(ViewProjection, FVector3(1000.0f, 0.0f, 0.0f));
	std::vector<float> Depth(DepthWidth * DepthHeight, 1.0f);
	for (uint32 Y = 0; Y < DepthHeight; ++Y)
	{
		for (uint32 X = 0; X < DepthWidth / 2; ++X)
		{
			Depth[Y * DepthWidth + X] = WallDepth;
		}
	}
	const FCpuHzb Hzb(Depth);
	const auto    Load     = [&](uint32 Mip, int32 X, int32 Y) { return Hzb.Load(Mip, X, Y); };
	const uint32  MipCount = static_cast<uint32>(Hzb.Mips.size());
	const auto    Occluded = [&](const FVector3& Center, float HalfSize) {
		return HzbMath::IsOccluded(Center - FVector3(HalfSize), Center + FVector3(HalfSize), ViewProjection, DepthWidth, DepthHeight, MipCount, Load);
	};

	// 화면 왼쪽(-Y), 벽 뒤 → 가려짐 / 벽 앞 → 보임 / 오른쪽(+Y, 하늘) → 보임
	E_EXPECT_TRUE(Occluded(FVector3(3000.0f, -800.0f, 0.0f), 50.0f));
	E_EXPECT_FALSE(Occluded(FVector3(500.0f, -300.0f, 0.0f), 20.0f));
	E_EXPECT_FALSE(Occluded(FVector3(3000.0f, 800.0f, 0.0f), 50.0f));
	// 벽 경계에 걸친 물체 → 보임 (일부가 하늘 쪽)
	E_EXPECT_FALSE(Occluded(FVector3(3000.0f, 0.0f, 0.0f), 200.0f));
	// 카메라를 감싸는(근평면을 지나는) 물체 → 판정 불가 = 보임
	E_EXPECT_FALSE(Occluded(FVector3::ZeroVector, 100.0f));
	// 카메라 뒤 → 판정 불가 = 보임 (프러스텀 컬링이 따로 처리)
	E_EXPECT_FALSE(Occluded(FVector3(-3000.0f, -800.0f, 0.0f), 50.0f));
	// 큰 물체도 벽 뒤에 완전히 있으면 가려짐 (높은 밉 사용)
	E_EXPECT_TRUE(Occluded(FVector3(6000.0f, -2500.0f, 0.0f), 600.0f));

	// HZB가 비어 있으면(전부 1 = 하늘) 아무것도 가리지 않는다
	const FCpuHzb Empty(std::vector<float>(DepthWidth * DepthHeight, 1.0f));
	E_EXPECT_FALSE(HzbMath::IsOccluded(FVector3(2950.0f, -850.0f, -50.0f), FVector3(3050.0f, -750.0f, 50.0f), ViewProjection, DepthWidth, DepthHeight,
	                                   MipCount, [&](uint32 Mip, int32 X, int32 Y) { return Empty.Load(Mip, X, Y); }));
}

E_TEST(Occlusion_ScreenRectMapsToDepthPixels)
{
	const FCamera    Camera         = MakeCamera();
	const FMatrix4x4 ViewProjection = Camera.GetViewProjectionMatrix();
	// 화면 중앙 작은 상자 → 사각형이 중앙 픽셀 근처, 위(+Z)에 있으면 픽셀 y가 작다
	const HzbMath::FScreenRect Center =
		HzbMath::ProjectBounds(FVector3(990.0f, -10.0f, -10.0f), FVector3(1010.0f, 10.0f, 10.0f), ViewProjection, 64.0f, 32.0f);
	E_EXPECT_TRUE(Center.bValid);
	E_EXPECT_TRUE(Center.PixelMin.X < 32.0f && Center.PixelMax.X > 32.0f);
	E_EXPECT_TRUE(Center.PixelMin.Y < 16.0f && Center.PixelMax.Y > 16.0f);
	const HzbMath::FScreenRect Up = HzbMath::ProjectBounds(FVector3(990.0f, -10.0f, 300.0f), FVector3(1010.0f, 10.0f, 320.0f), ViewProjection, 64.0f, 32.0f);
	E_EXPECT_TRUE(Up.bValid && Up.PixelMax.Y < 16.0f);
	E_EXPECT_TRUE(Up.MinZ > 0.0f && Up.MinZ < 1.0f);
	// 화면 밖 → 판정 불가
	E_EXPECT_FALSE(HzbMath::ProjectBounds(FVector3(990.0f, 5000.0f, 0.0f), FVector3(1010.0f, 5100.0f, 10.0f), ViewProjection, 64.0f, 32.0f).bValid);
}
