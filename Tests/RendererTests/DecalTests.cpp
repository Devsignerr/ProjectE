#include "Core/Testing/TestFramework.h"
#include "Renderer/DecalMath.h"

namespace
{
	constexpr float Tol = 1.0e-4f;
} // namespace

E_TEST(Decal_BoxMappingAndUv)
{
	// 크기 (200, 100, 50) 상자를 (1000, 0, 0)에: 월드 점 → 단위 상자 로컬 → UV
	const FMatrix4x4 World        = FMatrix4x4::MakeTranslation(FVector3(1000.0f, 0.0f, 0.0f));
	const FMatrix4x4 DecalToWorld = FDecalMath::MakeDecalToWorld(World, FVector3(200.0f, 100.0f, 50.0f));
	const FMatrix4x4 WorldToDecal = DecalToWorld.GetInverse();

	const FVector3 Center = WorldToDecal.TransformPosition(FVector3(1000.0f, 0.0f, 0.0f));
	E_EXPECT_NEAR(Center.X, 0.0f, Tol);
	const FVector2 CenterUv = FDecalMath::LocalToUv(Center);
	E_EXPECT_NEAR(CenterUv.X, 0.5f, Tol);
	E_EXPECT_NEAR(CenterUv.Y, 0.5f, Tol);

	// 앞(+X) 끝 = 텍스처 위(V = 0), 오른쪽(+Y) 끝 = U 1
	const FVector3 Front = WorldToDecal.TransformPosition(FVector3(1100.0f, 0.0f, 0.0f));
	E_EXPECT_NEAR(FDecalMath::LocalToUv(Front).Y, 0.0f, Tol);
	const FVector3 Right = WorldToDecal.TransformPosition(FVector3(1000.0f, 50.0f, 0.0f));
	E_EXPECT_NEAR(FDecalMath::LocalToUv(Right).X, 1.0f, Tol);

	E_EXPECT_TRUE(FDecalMath::IsInsideBox(WorldToDecal.TransformPosition(FVector3(1050.0f, 20.0f, 20.0f))));
	E_EXPECT_FALSE(FDecalMath::IsInsideBox(WorldToDecal.TransformPosition(FVector3(1000.0f, 0.0f, 30.0f)))); // Z 범위 밖 (±25)
	E_EXPECT_FALSE(FDecalMath::IsInsideBox(WorldToDecal.TransformPosition(FVector3(1101.0f, 0.0f, 0.0f))));

	// 회전: Yaw 90도면 +X 앞이 월드 +Y로
	const FMatrix4x4 Rotated = FDecalMath::MakeDecalToWorld(FMatrix4x4::MakeRotation(FQuat::FromEuler(0.0f, 90.0f, 0.0f)), FVector3(100.0f));
	const FVector3   Local   = Rotated.GetInverse().TransformPosition(FVector3(0.0f, 40.0f, 0.0f));
	E_EXPECT_NEAR(Local.X, 0.4f, 1.0e-3f);
	E_EXPECT_NEAR(Local.Y, 0.0f, 1.0e-3f);
}

E_TEST(Decal_Fades)
{
	E_EXPECT_NEAR(FDecalMath::ComputeAngleFade(1.0f), 1.0f, Tol);
	E_EXPECT_NEAR(FDecalMath::ComputeAngleFade(0.5f), 1.0f, Tol);
	E_EXPECT_NEAR(FDecalMath::ComputeAngleFade(0.25f), 0.0f, Tol);
	E_EXPECT_NEAR(FDecalMath::ComputeAngleFade(-1.0f), 0.0f, Tol); // 뒷면
	E_EXPECT_NEAR(FDecalMath::ComputeDepthFade(0.0f), 1.0f, Tol);
	E_EXPECT_NEAR(FDecalMath::ComputeDepthFade(0.45f), 0.5f, Tol);
	E_EXPECT_NEAR(FDecalMath::ComputeDepthFade(-0.5f), 0.0f, Tol);
	E_EXPECT_NEAR(FDecalMath::ComputeDistanceFade(500.0f, 1000.0f, 2000.0f), 1.0f, Tol);
	E_EXPECT_NEAR(FDecalMath::ComputeDistanceFade(1500.0f, 1000.0f, 2000.0f), 0.5f, Tol);
	E_EXPECT_NEAR(FDecalMath::ComputeDistanceFade(5000.0f, 1000.0f, 2000.0f), 0.0f, Tol);
	E_EXPECT_NEAR(FDecalMath::ComputeDistanceFade(5000.0f, 0.0f, 0.0f), 1.0f, Tol); // 페이드 없음
}

E_TEST(Decal_DBufferComposition)
{
	// 데칼 없음: 표면 그대로
	const FDecalMath::FDBufferValue Empty;
	const FVector3                  Surface(0.2f, 0.4f, 0.6f);
	const FVector3                  Same = FDecalMath::Apply(Surface, Empty);
	E_EXPECT_NEAR(Same.X, 0.2f, Tol);

	// 두 데칼을 순서대로 섞은 결과 = 표면 위에 차례로 알파 블렌드한 결과
	const FVector3                  First(1.0f, 0.0f, 0.0f);
	const FVector3                  Second(0.0f, 0.0f, 1.0f);
	const FDecalMath::FDBufferValue After1 = FDecalMath::Blend(Empty, First, 0.5f);
	const FDecalMath::FDBufferValue After2 = FDecalMath::Blend(After1, Second, 0.25f);
	const FVector3                  Result = FDecalMath::Apply(Surface, After2);
	const FVector3                  Step1  = Surface * 0.5f + First * 0.5f;
	const FVector3                  Direct = Step1 * 0.75f + Second * 0.25f;
	E_EXPECT_NEAR(Result.X, Direct.X, Tol);
	E_EXPECT_NEAR(Result.Y, Direct.Y, Tol);
	E_EXPECT_NEAR(Result.Z, Direct.Z, Tol);
	E_EXPECT_NEAR(After2.Remaining, 0.375f, Tol);

	// 법선: (n*0.5+0.5)을 누적 → 디코드 = normalize(N·남은 비중 + Σ n·w)
	const FVector3                  SurfaceNormal(0.0f, 0.0f, 1.0f);
	const FVector3                  DecalNormal = FVector3(1.0f, 0.0f, 1.0f).GetNormalized();
	const FDecalMath::FDBufferValue Stored      = FDecalMath::Blend(Empty, DecalNormal * 0.5f + FVector3(0.5f), 1.0f);
	const FVector3                  Full        = FDecalMath::ApplyNormal(SurfaceNormal, Stored);
	E_EXPECT_NEAR(Full.X, DecalNormal.X, 1.0e-3f);
	E_EXPECT_NEAR(Full.Z, DecalNormal.Z, 1.0e-3f);
	const FVector3 None = FDecalMath::ApplyNormal(SurfaceNormal, Empty);
	E_EXPECT_NEAR(None.Z, 1.0f, Tol);
	const FDecalMath::FDBufferValue Half     = FDecalMath::Blend(Empty, DecalNormal * 0.5f + FVector3(0.5f), 0.5f);
	const FVector3                  Mixed    = FDecalMath::ApplyNormal(SurfaceNormal, Half);
	const FVector3                  Expected = (SurfaceNormal * 0.5f + DecalNormal * 0.5f).GetNormalized();
	E_EXPECT_NEAR(Mixed.X, Expected.X, 1.0e-3f);
	E_EXPECT_NEAR(Mixed.Z, Expected.Z, 1.0e-3f);
}
