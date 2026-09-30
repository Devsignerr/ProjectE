#include "Core/Testing/TestFramework.h"
#include "Renderer/PixelArtMath.h"

#include <cmath>

namespace
{
	constexpr float Tol = 1.0e-3f;

	// 직교 카메라에서 월드 점이 찍히는 출력 픽셀 X/Y (스냅 없는 이상적 위치)
	FVector2 ProjectIdeal(const FVector3& Point, const FVector3& Camera, const FVector3& Right, const FVector3& Up, float Texel,
	                      uint32 OutputWidth, uint32 OutputHeight, uint32 PixelSize)
	{
		const FVector3 Delta = Point - Camera;
		return FVector2(static_cast<float>(OutputWidth) * 0.5f + FVector3::Dot(Delta, Right) / Texel * static_cast<float>(PixelSize),
		                static_cast<float>(OutputHeight) * 0.5f - FVector3::Dot(Delta, Up) / Texel * static_cast<float>(PixelSize));
	}
} // namespace

E_TEST(PixelArt_SourceDimension)
{
	// 1920 / 4 = 480 + 여백 2
	E_EXPECT_EQ(FPixelArtMath::GetSourceDimension(1920, 4), 482u);
	// 나누어떨어지지 않으면 올림: 1081 / 4 → 271 + 2
	E_EXPECT_EQ(FPixelArtMath::GetSourceDimension(1081, 4), 273u);
	E_EXPECT_EQ(FPixelArtMath::GetSourceDimension(1, 16), 3u);
	E_EXPECT_EQ(FPixelArtMath::ClampPixelSize(0), 1u);
	E_EXPECT_EQ(FPixelArtMath::ClampPixelSize(100), 16u);

	// 직교 높이 1080cm, 출력 1080px, 도트 4px → 도트 하나 4cm
	E_EXPECT_NEAR(FPixelArtMath::GetTexelWorldSize(1080.0f, 1080, 4), 4.0f, Tol);
	// 소스 세로 272텍셀 × 4 = 1088px → 출력 대비 1088/1080
	E_EXPECT_NEAR(FPixelArtMath::GetSourceExtentScale(272, 1080, 4), 1088.0f / 1080.0f, Tol);
}

E_TEST(PixelArt_OutputToSourceStaysInside)
{
	// 출력 모서리 픽셀이 서브픽셀 오프셋 ±0.5에서도 소스 안쪽 텍셀을 읽는다
	const uint32 Sizes[][3] = { { 1920, 1080, 4 }, { 1281, 721, 3 }, { 7, 5, 16 }, { 800, 600, 1 } };
	const float  Offsets[]  = { -0.5f, 0.0f, 0.5f };
	for (const auto& Size : Sizes)
	{
		const uint32 SourceWidth  = FPixelArtMath::GetSourceDimension(Size[0], Size[2]);
		const uint32 SourceHeight = FPixelArtMath::GetSourceDimension(Size[1], Size[2]);
		for (float Offset : Offsets)
		{
			const FVector2 Min = FPixelArtMath::OutputToSource(FVector2(0.5f, 0.5f), Size[0], Size[1], SourceWidth, SourceHeight, Size[2],
			                                                   FVector2(Offset, Offset));
			const FVector2 Max = FPixelArtMath::OutputToSource(FVector2(static_cast<float>(Size[0]) - 0.5f, static_cast<float>(Size[1]) - 0.5f),
			                                                   Size[0], Size[1], SourceWidth, SourceHeight, Size[2], FVector2(Offset, Offset));
			E_EXPECT_TRUE(Min.X >= 0.0f && Min.Y >= 0.0f);
			E_EXPECT_TRUE(std::floor(Max.X) < static_cast<float>(SourceWidth) && std::floor(Max.Y) < static_cast<float>(SourceHeight));
		}
	}

	// 오프셋 0이면 출력 중심 = 소스 중심
	const FVector2 Center = FPixelArtMath::OutputToSource(FVector2(960.0f, 540.0f), 1920, 1080, 482, 272, 4, FVector2(0.0f, 0.0f));
	E_EXPECT_NEAR(Center.X, 241.0f, Tol);
	E_EXPECT_NEAR(Center.Y, 136.0f, Tol);
}

E_TEST(PixelArt_SnapRemainderAndGrid)
{
	const FVector3 Right = FVector3(0.0f, 1.0f, 0.0f);
	const FVector3 Up    = FVector3(0.0f, 0.0f, 1.0f);
	const float    Texel = 4.0f;

	const auto Snap = FPixelArtMath::SnapToTexelGrid(FVector3(123.0f, 10.0f, -7.0f), Right, Up, Texel);
	// Y 10 → 격자 2.5 → 반올림 3(round half away) / Z -7 → -1.75 → -2
	E_EXPECT_EQ(Snap.IndexRight, 3);
	E_EXPECT_EQ(Snap.IndexUp, -2);
	E_EXPECT_NEAR(Snap.Remainder.X, -0.5f, Tol);
	E_EXPECT_NEAR(Snap.Remainder.Y, 0.25f, Tol);
	// 스냅 위치는 격자 위, 앞 방향 성분(X)은 그대로
	E_EXPECT_NEAR(Snap.SnappedPosition.X, 123.0f, Tol);
	E_EXPECT_NEAR(Snap.SnappedPosition.Y, 12.0f, Tol);
	E_EXPECT_NEAR(Snap.SnappedPosition.Z, -8.0f, Tol);

	// 한 텍셀 이동하면 격자 번호만 1 늘고 나머지는 같다
	const auto Moved = FPixelArtMath::SnapToTexelGrid(FVector3(123.0f, 10.0f + Texel * 0.999f, -7.0f + Texel), Right, Up, Texel);
	E_EXPECT_EQ(Moved.IndexUp, Snap.IndexUp + 1);
	E_EXPECT_NEAR(Moved.Remainder.Y, Snap.Remainder.Y, Tol);

	E_EXPECT_EQ(FPixelArtMath::PositiveMod4(-1), 3u);
	E_EXPECT_EQ(FPixelArtMath::PositiveMod4(-8), 0u);
	E_EXPECT_EQ(FPixelArtMath::PositiveMod4(6), 2u);
}

E_TEST(PixelArt_SnapPlusOffsetMatchesIdealProjection)
{
	// 스냅된 카메라로 렌더한 소스를 서브픽셀 오프셋으로 확대하면, 월드 점이 스냅 없는 이상적 위치에 찍혀야 한다
	const uint32   OutputWidth  = 1280;
	const uint32   OutputHeight = 720;
	const uint32   PixelSize    = 4;
	const uint32   SourceWidth  = FPixelArtMath::GetSourceDimension(OutputWidth, PixelSize);
	const uint32   SourceHeight = FPixelArtMath::GetSourceDimension(OutputHeight, PixelSize);
	const float    Texel        = FPixelArtMath::GetTexelWorldSize(900.0f, OutputHeight, PixelSize);
	const FVector3 Forward      = FVector3(1.0f, 1.0f, -1.0f).GetNormalized();
	const FVector3 Right        = FVector3::Cross(FVector3::UpVector, Forward).GetNormalized();
	const FVector3 Up           = FVector3::Cross(Forward, Right);
	const FVector3 Point        = FVector3(200.0f, -50.0f, 30.0f);

	const FVector3 Cameras[] = { FVector3(-500.0f, -480.0f, 600.0f), FVector3(-501.3f, -477.9f, 601.7f), FVector3(1234.5f, -987.6f, 543.2f) };
	for (const FVector3& Camera : Cameras)
	{
		const auto     Snap   = FPixelArtMath::SnapToTexelGrid(Camera, Right, Up, Texel);
		const FVector2 Offset = FPixelArtMath::GetSubPixelOffset(Snap.Remainder);

		// 스냅 카메라 기준 소스 텍셀 좌표
		const FVector3 Delta = Point - Snap.SnappedPosition;
		const FVector2 Source(static_cast<float>(SourceWidth) * 0.5f + FVector3::Dot(Delta, Right) / Texel,
		                      static_cast<float>(SourceHeight) * 0.5f - FVector3::Dot(Delta, Up) / Texel);

		// 이상적 출력 위치에서 확대 식으로 소스를 역산하면 같은 소스 좌표
		const FVector2 Ideal = ProjectIdeal(Point, Camera, Right, Up, Texel, OutputWidth, OutputHeight, PixelSize);
		const FVector2 Mapped =
			FPixelArtMath::OutputToSource(Ideal, OutputWidth, OutputHeight, SourceWidth, SourceHeight, PixelSize, Offset);
		E_EXPECT_NEAR(Mapped.X, Source.X, 1.0e-2f);
		E_EXPECT_NEAR(Mapped.Y, Source.Y, 1.0e-2f);
	}
}
