#include "Core/Testing/TestFramework.h"
#include "Renderer/DebugDraw.h"

#include <cmath>

// 3D 디버그 그리기 저장소 (Phase 41-3, Renderer/DebugDraw.h — GPU 없이 검증)
namespace
{
	// 테스트마다 전역 저장소를 깨끗하게 (다른 테스트가 남긴 상태 제거)
	FDebugDraw& ResetDebugDraw()
	{
		FDebugDraw& Draw = FDebugDraw::Get();
		Draw.SetEnabled(true);
		Draw.Clear();
		return Draw;
	}
} // namespace

E_TEST(DebugDraw_ShapesExpandToLines)
{
	FDebugDraw& Draw = ResetDebugDraw();
	Draw.DrawLine(FVector3::ZeroVector, FVector3(100.0f, 0.0f, 0.0f));
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(1));
	Draw.DrawBox(FVector3::ZeroVector, FVector3(10.0f, 20.0f, 30.0f), FQuat::Identity);
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(1 + 12));
	Draw.DrawSphere(FVector3::ZeroVector, 50.0f, FVector4(1.0f, 0.0f, 0.0f, 1.0f), 0.0f, true, 8);
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(13 + 3 * 8));
	Draw.DrawArrow(FVector3::ZeroVector, FVector3(0.0f, 0.0f, 100.0f));
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(37 + 5));
	Draw.DrawCapsule(FVector3::ZeroVector, 10.0f, 50.0f, FQuat::Identity, FVector4(1.0f, 1.0f, 1.0f, 1.0f), 0.0f, true, 8);
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(42 + 2 * 8 + 4 + 4 * 4));

	// 상자 모서리: 회전·반 크기가 적용된 꼭짓점 (모든 끝점이 ±반 크기 위)
	Draw.Clear();
	const FQuat Yaw90 = FQuat::FromAxisAngle(FVector3::UpVector, FMath::Pi * 0.5f);
	Draw.DrawBox(FVector3(0.0f, 0.0f, 100.0f), FVector3(10.0f, 20.0f, 30.0f), Yaw90);
	bool bAllOnCorners = true;
	for (const FDebugLine& Line : Draw.GetLines())
	{
		for (const FVector3& Point : { Line.Start, Line.End })
		{
			// 90도 돌리면 X 반 크기 10은 Y로, Y 반 크기 20은 X로
			bAllOnCorners &= std::abs(std::abs(Point.X) - 20.0f) < 1.0e-3f && std::abs(std::abs(Point.Y) - 10.0f) < 1.0e-3f &&
			                 std::abs(std::abs(Point.Z - 100.0f) - 30.0f) < 1.0e-3f;
		}
	}
	E_EXPECT_TRUE(bAllOnCorners);

	// 구: 모든 점이 반지름 위
	Draw.Clear();
	Draw.DrawSphere(FVector3(5.0f, 6.0f, 7.0f), 40.0f);
	bool bOnSphere = true;
	for (const FDebugLine& Line : Draw.GetLines())
	{
		bOnSphere &= std::abs((Line.Start - FVector3(5.0f, 6.0f, 7.0f)).Length() - 40.0f) < 1.0e-2f;
	}
	E_EXPECT_TRUE(bOnSphere);
	Draw.Clear();
}

E_TEST(DebugDraw_DurationExpiresOnTick)
{
	FDebugDraw& Draw = ResetDebugDraw();
	Draw.DrawLine(FVector3::ZeroVector, FVector3::UpVector);                                         // 한 프레임
	Draw.DrawLine(FVector3::ZeroVector, FVector3::UpVector, FVector4(1.0f, 1.0f, 1.0f, 1.0f), 0.25f); // 0.25초
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(2));
	Draw.Tick(0.1f); // 한 프레임짜리는 한 번 그려진 뒤 사라진다
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(1));
	Draw.Tick(0.1f);
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(1));
	Draw.Tick(0.1f); // 0.3초 > 0.25초
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(0));

	// 꺼져 있으면 무시 (GPU 없는 서버), 끄면 비운다
	Draw.DrawLine(FVector3::ZeroVector, FVector3::UpVector, FVector4(1.0f, 1.0f, 1.0f, 1.0f), 10.0f);
	Draw.SetEnabled(false);
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(0));
	Draw.DrawSphere(FVector3::ZeroVector, 10.0f);
	E_EXPECT_EQ(Draw.GetLines().size(), static_cast<size_t>(0));
	Draw.SetEnabled(true);
}

E_TEST(DebugDraw_CapAndColor)
{
	FDebugDraw& Draw = ResetDebugDraw();
	for (size_t Index = 0; Index < FDebugDraw::MaxLines + 100; ++Index)
	{
		Draw.DrawLine(FVector3::ZeroVector, FVector3::UpVector, FVector4(1.0f, 1.0f, 1.0f, 1.0f), 5.0f);
	}
	E_EXPECT_EQ(Draw.GetLines().size(), FDebugDraw::MaxLines); // 넘친 것은 버린다 (경고 한 번)
	Draw.Clear();

	// 색: sRGB → 선형 RGBA8 (R, G, B, A 순서)
	E_EXPECT_EQ(FDebugDraw::PackColor(FVector4(1.0f, 1.0f, 1.0f, 1.0f)), 0xFFFFFFFFu);
	E_EXPECT_EQ(FDebugDraw::PackColor(FVector4(1.0f, 0.0f, 0.0f, 0.5f)), 0x800000FFu);
	const uint32 Gray = FDebugDraw::PackColor(FVector4(0.5f, 0.5f, 0.5f, 1.0f)) & 0xFFu;
	E_EXPECT_TRUE(Gray >= 53u && Gray <= 56u); // sRGB 0.5 ≈ 선형 0.214
}
