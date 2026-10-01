#include "Core/Testing/TestFramework.h"
#include "Renderer/ReflectionCaptures.h"
#include "Renderer/ReflectionMath.h"

#include <filesystem>

namespace
{
	constexpr float Tol = 1.0e-4f;
} // namespace

E_TEST(Reflection_CubeFaceCameraBasis)
{
	// 면마다: 앞 = 면 중심 방향, 오른쪽/위 = UV 축, 왼손 Z-up 회전(Cross(앞, 오른쪽) = 위), 쿼터니언이 같은 축을 만든다
	for (uint32 Face = 0; Face < 6; ++Face)
	{
		FVector3 Forward;
		FVector3 Right;
		FVector3 Up;
		FReflectionMath::GetCubeFaceBasis(Face, Forward, Right, Up);
		const FVector3 Center = IblMath::CubeFaceUVToDirection(Face, 0.0f, 0.0f);
		E_EXPECT_NEAR(FVector3::Dot(Forward, Center), 1.0f, Tol);
		E_EXPECT_NEAR(FVector3::Dot(Forward, Right), 0.0f, Tol);
		E_EXPECT_NEAR(FVector3::Dot(Forward, Up), 0.0f, Tol);
		const FVector3 Cross = FVector3::Cross(Forward, Right);
		E_EXPECT_NEAR(Cross.X, Up.X, Tol);
		E_EXPECT_NEAR(Cross.Y, Up.Y, Tol);
		E_EXPECT_NEAR(Cross.Z, Up.Z, Tol);

		// 화면 오른쪽 위 모서리 방향 = 큐브 UV (1, -1) 방향 (V는 아래가 +)
		const FVector3 Corner = (Forward + Right + Up).GetNormalized();
		const FVector3 Cube   = IblMath::CubeFaceUVToDirection(Face, 1.0f, -1.0f);
		E_EXPECT_NEAR(FVector3::Dot(Corner, Cube), 1.0f, 1.0e-3f);

		const FQuat Rotation = FReflectionMath::MakeBasisRotation(Forward, Right, Up);
		const FVector3 QF    = Rotation.GetForwardVector();
		const FVector3 QR    = Rotation.GetRightVector();
		const FVector3 QU    = Rotation.GetUpVector();
		E_EXPECT_NEAR(FVector3::Dot(QF, Forward), 1.0f, 1.0e-3f);
		E_EXPECT_NEAR(FVector3::Dot(QR, Right), 1.0f, 1.0e-3f);
		E_EXPECT_NEAR(FVector3::Dot(QU, Up), 1.0f, 1.0e-3f);
	}
}

E_TEST(Reflection_CaptureInfluenceAndParallax)
{
	const FVector3 Center(100.0f, 0.0f, 150.0f);
	const FVector3 Extent(600.0f, 500.0f, 220.0f);
	// 상자: 안쪽 깊이 = 경계까지 최소 거리, 페이드 100
	E_EXPECT_NEAR(FReflectionMath::ComputeInfluence(1, Center, Center, 0.0f, Extent, 100.0f), 1.0f, Tol);
	E_EXPECT_NEAR(FReflectionMath::ComputeInfluence(1, FVector3(100.0f, 0.0f, 150.0f + 170.0f), Center, 0.0f, Extent, 100.0f), 0.5f, Tol);
	E_EXPECT_NEAR(FReflectionMath::ComputeInfluence(1, FVector3(800.0f, 0.0f, 150.0f), Center, 0.0f, Extent, 100.0f), 0.0f, Tol);
	// 구
	E_EXPECT_NEAR(FReflectionMath::ComputeInfluence(0, FVector3(0.0f), FVector3(0.0f), 500.0f, Extent, 100.0f), 1.0f, Tol);
	E_EXPECT_NEAR(FReflectionMath::ComputeInfluence(0, FVector3(450.0f, 0.0f, 0.0f), FVector3(0.0f), 500.0f, Extent, 100.0f), 0.5f, Tol);

	// 시차 보정: 중심에서는 반사 방향 그대로, 벽 가까이에서는 벽 교점 방향
	const FVector3 Same = FReflectionMath::ParallaxCorrect(Center, FVector3(1.0f, 0.0f, 0.0f), Center, Extent);
	E_EXPECT_NEAR(Same.X, 1.0f, Tol);
	// 중심에서 +Y로 400 옮긴 점이 +X로 보면 교점 (700, 400, 150) → 방향 (600, 400, 0)
	const FVector3 Shifted = FReflectionMath::ParallaxCorrect(Center + FVector3(0.0f, 400.0f, 0.0f), FVector3(1.0f, 0.0f, 0.0f), Center, Extent);
	const FVector3 Expected = FVector3(600.0f, 400.0f, 0.0f).GetNormalized();
	E_EXPECT_NEAR(Shifted.X, Expected.X, 1.0e-3f);
	E_EXPECT_NEAR(Shifted.Y, Expected.Y, 1.0e-3f);
}

E_TEST(Reflection_SsrFadeAndHiz)
{
	E_EXPECT_NEAR(FReflectionMath::ComputeSsrRoughnessFade(0.1f, 0.6f), 1.0f, Tol);
	E_EXPECT_NEAR(FReflectionMath::ComputeSsrRoughnessFade(0.3f, 0.6f), 1.0f, Tol);
	E_EXPECT_NEAR(FReflectionMath::ComputeSsrRoughnessFade(0.45f, 0.6f), 0.5f, Tol);
	E_EXPECT_NEAR(FReflectionMath::ComputeSsrRoughnessFade(0.8f, 0.6f), 0.0f, Tol);
	E_EXPECT_EQ(FReflectionMath::GetHizMipCount(1280, 720), 10u); // 1280 → 1 은 11단계지만 최대 10
	E_EXPECT_EQ(FReflectionMath::GetHizMipCount(16, 8), 5u);
	E_EXPECT_EQ(FReflectionMath::GetHizMipCount(1, 1), 1u);
}

E_TEST(Reflection_CaptureFileRoundTrip)
{
	FReflectionCaptureFile File;
	File.Size     = 4;
	File.MipCount = 3;
	File.Data.resize(File.GetExpectedBytes());
	E_EXPECT_EQ(File.Data.size(), static_cast<size_t>((16 + 4 + 1) * 8 * 6));
	for (size_t Index = 0; Index < File.Data.size(); ++Index)
	{
		File.Data[Index] = static_cast<uint8>(Index * 7);
	}
	const std::filesystem::path Path = std::filesystem::temp_directory_path() / "ProjectE_ReflectionCaptureTest.ecapture";
	E_EXPECT_TRUE(File.Save(Path));
	FReflectionCaptureFile Loaded;
	E_EXPECT_TRUE(Loaded.Load(Path));
	E_EXPECT_EQ(Loaded.Size, 4u);
	E_EXPECT_EQ(Loaded.MipCount, 3u);
	E_EXPECT_TRUE(Loaded.Data == File.Data);
	std::error_code Error;
	std::filesystem::remove(Path, Error);
	E_EXPECT_FALSE(Loaded.Load(Path)); // 없는 파일
}

E_TEST(Reflection_SsrBlurRadius)
{
	// 거울은 흐리지 않고, 거칠수록 원뿔이 넓어진다 (거칠기 0.3 ≈ 6도)
	E_EXPECT_NEAR(FReflectionMath::ComputeSpecularConeTangent(0.0f), 0.0f, 1.0e-6f);
	const float Cone03 = FReflectionMath::ComputeSpecularConeTangent(0.3f);
	E_EXPECT_NEAR(Cone03, 0.107f, 0.01f);
	E_EXPECT_TRUE(FReflectionMath::ComputeSpecularConeTangent(0.1f) < Cone03 && Cone03 < FReflectionMath::ComputeSpecularConeTangent(0.5f));
	// 교차 거리에 비례, 상한
	const float Near = FReflectionMath::ComputeSsrBlurRadiusPixels(0.3f, 50.0f, 1.0f, 16.0f);
	const float Far  = FReflectionMath::ComputeSsrBlurRadiusPixels(0.3f, 100.0f, 1.0f, 16.0f);
	E_EXPECT_NEAR(Far, Near * 2.0f, 1.0e-3f);
	E_EXPECT_NEAR(FReflectionMath::ComputeSsrBlurRadiusPixels(0.6f, 1000.0f, 2.0f, 16.0f), 16.0f, 1.0e-4f);
	// 반사된 상의 깊이: 화면 가운데(뷰 깊이 = 거리) 표면 800cm, 교차 400cm → 1200cm. 비스듬하면 뷰 깊이 비율 유지
	E_EXPECT_NEAR(FReflectionMath::ComputeSsrReflectionViewDepth(800.0f, 800.0f, 400.0f), 1200.0f, 1.0e-3f);
	E_EXPECT_NEAR(FReflectionMath::ComputeSsrReflectionViewDepth(400.0f, 800.0f, 400.0f), 600.0f, 1.0e-3f);
	E_EXPECT_NEAR(FReflectionMath::ComputeSsrReflectionViewDepth(800.0f, 800.0f, 0.0f), 800.0f, 1.0e-3f);
}
