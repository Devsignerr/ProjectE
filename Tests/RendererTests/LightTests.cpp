#include "Core/Testing/TestFramework.h"
#include "Renderer/LightMath.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

namespace
{
	// 행벡터 v * M 후 원근 나눗셈 (NDC)
	FVector3 ProjectPoint(const FMatrix4x4& M, const FVector3& P)
	{
		const FVector3 Clip = M.TransformPosition(P);
		const float    W    = P.X * M.M[0][3] + P.Y * M.M[1][3] + P.Z * M.M[2][3] + M.M[3][3];
		return Clip / W;
	}
} // namespace

E_TEST(Light_DistanceAttenuation)
{
	// 1m에서 1, 반경에서 0, 거리에 따라 단조 감소
	E_EXPECT_NEAR(LightMath::InverseSquare(100.0f), 1.0f, 1.0e-5f);
	E_EXPECT_NEAR(LightMath::InverseSquare(200.0f), 0.25f, 1.0e-5f);
	E_EXPECT_NEAR(LightMath::InverseSquare(0.0f), LightMath::InverseSquare(LightMath::MinDistance), 1.0e-5f);
	E_EXPECT_NEAR(LightMath::DistanceAttenuation(1000.0f, 1000.0f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(LightMath::DistanceAttenuation(1500.0f, 1000.0f), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(LightMath::DistanceWindow(0.0f, 1000.0f), 1.0f, 1.0e-6f);
	E_EXPECT_NEAR(LightMath::DistanceWindow(100.0f, 0.0f), 0.0f, 1.0e-6f);

	float Previous = LightMath::DistanceAttenuation(0.0f, 1000.0f);
	for (float Distance = 10.0f; Distance <= 1000.0f; Distance += 10.0f)
	{
		const float Value = LightMath::DistanceAttenuation(Distance, 1000.0f);
		E_EXPECT_TRUE(Value <= Previous + 1.0e-6f);
		E_EXPECT_TRUE(Value >= 0.0f);
		Previous = Value;
	}
	// 반경이 아주 크면 창 함수는 거의 1 → 역제곱과 같다
	E_EXPECT_NEAR(LightMath::DistanceAttenuation(300.0f, 1.0e6f), 1.0f / 9.0f, 1.0e-4f);
}

E_TEST(Light_ConeAttenuation)
{
	const LightMath::FConeParams Cone = LightMath::ComputeConeParams(20.0f, 30.0f);
	auto AtAngle = [&](float Degrees) { return LightMath::ConeAttenuation(FMath::Cos(FMath::DegreesToRadians(Degrees)), Cone); };
	E_EXPECT_NEAR(AtAngle(0.0f), 1.0f, 1.0e-5f);
	E_EXPECT_NEAR(AtAngle(19.9f), 1.0f, 1.0e-5f);
	E_EXPECT_NEAR(AtAngle(30.1f), 0.0f, 1.0e-5f);
	E_EXPECT_NEAR(AtAngle(90.0f), 0.0f, 1.0e-5f);
	const float Middle = AtAngle(25.0f);
	E_EXPECT_TRUE(Middle > 0.05f && Middle < 0.95f);
	E_EXPECT_TRUE(AtAngle(22.0f) > AtAngle(28.0f));

	// 점광원 기본값은 항상 1, 내부 >= 외부여도 0 나눗셈 없음
	E_EXPECT_NEAR(LightMath::ConeAttenuation(-1.0f, LightMath::FConeParams{}), 1.0f, 0.0f);
	const LightMath::FConeParams Hard = LightMath::ComputeConeParams(40.0f, 30.0f);
	E_EXPECT_NEAR(LightMath::ConeAttenuation(FMath::Cos(FMath::DegreesToRadians(10.0f)), Hard), 1.0f, 1.0e-5f);
	E_EXPECT_NEAR(LightMath::ConeAttenuation(FMath::Cos(FMath::DegreesToRadians(31.0f)), Hard), 0.0f, 1.0e-5f);
}

E_TEST(Light_SrgbToLinear)
{
	E_EXPECT_NEAR(LightMath::SrgbToLinear(0.0f), 0.0f, 0.0f);
	E_EXPECT_NEAR(LightMath::SrgbToLinear(1.0f), 1.0f, 1.0e-5f);
	E_EXPECT_NEAR(LightMath::SrgbToLinear(0.5f), 0.2140f, 1.0e-3f);
}

E_TEST(Light_ClusterSlices)
{
	const float NearZ = 10.0f;
	const float FarZ  = 100000.0f;
	const uint32 Slices = LightMath::ClusterGridZ;
	const LightMath::FSliceParams Params = LightMath::ComputeSliceParams(NearZ, FarZ, Slices);

	E_EXPECT_EQ(LightMath::DepthToSlice(1.0f, Params, Slices), 0u);
	E_EXPECT_EQ(LightMath::DepthToSlice(NearZ * 1.01f, Params, Slices), 0u);
	E_EXPECT_EQ(LightMath::DepthToSlice(FarZ * 10.0f, Params, Slices), Slices - 1);
	for (uint32 Slice = 0; Slice < Slices; ++Slice)
	{
		// 각 조각 경계 사이의 깊이는 그 조각으로 간다
		const float Begin = LightMath::SliceToDepth(Slice, NearZ, FarZ, Slices);
		const float End   = LightMath::SliceToDepth(Slice + 1, NearZ, FarZ, Slices);
		E_EXPECT_EQ(LightMath::DepthToSlice(std::sqrt(Begin * End), Params, Slices), Slice);
	}
	E_EXPECT_NEAR(LightMath::SliceToDepth(Slices, NearZ, FarZ, Slices), FarZ, 1.0f);
}

E_TEST(Light_ClusterBoundsContainViewPoints)
{
	// 원근 카메라: 뷰 공간 점을 타일/조각으로 분류하면 그 클러스터 AABB 안에 있어야 한다
	const float NearZ = 10.0f;
	const float FarZ  = 50000.0f;
	const float TanY  = FMath::Tan(FMath::DegreesToRadians(30.0f));
	const float TanX  = TanY * 16.0f / 9.0f;
	const LightMath::FSliceParams Params = LightMath::ComputeSliceParams(NearZ, FarZ, LightMath::ClusterGridZ);

	const FVector3 Points[] = { FVector3(0.0f, 0.0f, 500.0f), FVector3(-300.0f, 120.0f, 700.0f), FVector3(1500.0f, -800.0f, 3000.0f),
	                            FVector3(5.0f, 3.0f, 12.0f), FVector3(-40000.0f, 20000.0f, 45000.0f) };
	for (bool bOrthographic : { false, true })
	{
		const float ScaleX = bOrthographic ? 2000.0f : TanX;
		const float ScaleY = bOrthographic ? 1125.0f : TanY;
		for (const FVector3& P : Points)
		{
			const float NdcX = bOrthographic ? P.X / ScaleX : P.X / (P.Z * ScaleX);
			const float NdcY = bOrthographic ? P.Y / ScaleY : P.Y / (P.Z * ScaleY);
			if (FMath::Abs(NdcX) > 1.0f || FMath::Abs(NdcY) > 1.0f)
			{
				continue;
			}
			const uint32 TileX = FMath::Min(static_cast<uint32>((NdcX * 0.5f + 0.5f) * LightMath::ClusterGridX), LightMath::ClusterGridX - 1);
			const uint32 TileY = FMath::Min(static_cast<uint32>((0.5f - NdcY * 0.5f) * LightMath::ClusterGridY), LightMath::ClusterGridY - 1);
			const uint32 Slice = LightMath::DepthToSlice(P.Z, Params, LightMath::ClusterGridZ);
			const FBox   Box   = LightMath::ComputeClusterViewBounds(TileX, TileY, Slice, LightMath::ClusterGridX, LightMath::ClusterGridY,
			                                                         LightMath::ClusterGridZ, NearZ, FarZ, bOrthographic, ScaleX, ScaleY);
			const FBox Grown(Box.Min - FVector3(0.01f), Box.Max + FVector3(0.01f));
			E_EXPECT_TRUE(Grown.Contains(P));
		}
	}
}

E_TEST(Light_SphereBoxIntersection)
{
	const FBox Box(FVector3(0.0f), FVector3(10.0f));
	E_EXPECT_TRUE(LightMath::SphereIntersectsBox(FVector3(5.0f), 1.0f, Box));
	E_EXPECT_TRUE(LightMath::SphereIntersectsBox(FVector3(12.0f, 5.0f, 5.0f), 2.5f, Box));
	E_EXPECT_FALSE(LightMath::SphereIntersectsBox(FVector3(12.0f, 5.0f, 5.0f), 1.5f, Box));
	// 모서리 대각선: 거리 sqrt(3) * 2 ≈ 3.46
	E_EXPECT_FALSE(LightMath::SphereIntersectsBox(FVector3(12.0f), 3.3f, Box));
	E_EXPECT_TRUE(LightMath::SphereIntersectsBox(FVector3(12.0f), 3.6f, Box));
}

E_TEST(Light_CubeFaceSelectionMatchesProjection)
{
	// 면 선택 결과의 뷰-투영으로 투영하면 NDC [-1, 1], 깊이 [0, 1] 안에 들어와야 한다
	const FVector3 LightPosition(100.0f, -50.0f, 300.0f);
	const float    Radius = 1000.0f;
	const FVector3 Offsets[] = { FVector3(500.0f, 10.0f, -20.0f), FVector3(-300.0f, 200.0f, 100.0f), FVector3(20.0f, 400.0f, -399.0f),
	                             FVector3(10.0f, -700.0f, 5.0f), FVector3(-30.0f, 50.0f, 600.0f), FVector3(100.0f, 99.0f, -101.0f),
	                             FVector3(300.0f, 300.0f, 299.0f) };
	for (const FVector3& Offset : Offsets)
	{
		const uint32 Face = LightMath::SelectCubeFace(Offset);
		E_EXPECT_TRUE(FVector3::Dot(LightMath::GetCubeFaceDirection(Face), Offset) > 0.0f);
		const FMatrix4x4 ViewProjection = LightMath::ComputeCubeFaceViewProjection(LightPosition, Face, Radius, 5.0f, 512);
		const FVector3   Ndc            = ProjectPoint(ViewProjection, LightPosition + Offset);
		E_EXPECT_TRUE(FMath::Abs(Ndc.X) <= 1.0f && FMath::Abs(Ndc.Y) <= 1.0f);
		E_EXPECT_TRUE(Ndc.Z >= 0.0f && Ndc.Z <= 1.0f);
		// 면 경계(45°)에서도 여백 안 (|ndc| < 1 - 2텍셀)
		E_EXPECT_TRUE(FMath::Abs(Ndc.X) <= 1.0f - 2.0f * 2.0f / 512.0f + 1.0e-3f);
	}
	E_EXPECT_EQ(LightMath::SelectCubeFace(FVector3(0.0f, 0.0f, -1.0f)), 5u);
	E_EXPECT_EQ(LightMath::SelectCubeFace(FVector3(0.0f, -1.0f, 0.5f)), 3u);
}

E_TEST(Light_SpotProjectionCoversCone)
{
	const FVector3 Position(0.0f, 0.0f, 500.0f);
	const FVector3 Direction = FVector3(0.3f, 0.2f, -1.0f).GetNormalized();
	const float    Outer     = 40.0f;
	const FMatrix4x4 ViewProjection = LightMath::ComputeSpotViewProjection(Position, Direction, Outer, 2000.0f, 5.0f, 1024);

	// 외부 원뿔 경계 방향의 점은 화면 안
	const FVector3 Side   = FVector3::Cross(Direction, FVector3::UpVector).GetNormalized();
	const FQuat    Tilt   = FQuat::FromAxisAngle(Side, FMath::DegreesToRadians(Outer));
	const FVector3 Edge   = Tilt.RotateVector(Direction);
	const FVector3 Ndc    = ProjectPoint(ViewProjection, Position + Edge * 1000.0f);
	E_EXPECT_TRUE(FMath::Abs(Ndc.X) <= 1.0f && FMath::Abs(Ndc.Y) <= 1.0f);
	E_EXPECT_TRUE(Ndc.Z > 0.0f && Ndc.Z < 1.0f);

	// 정면 방향은 화면 중앙
	const FVector3 Center = ProjectPoint(ViewProjection, Position + Direction * 800.0f);
	E_EXPECT_NEAR(Center.X, 0.0f, 1.0e-4f);
	E_EXPECT_NEAR(Center.Y, 0.0f, 1.0e-4f);
}

E_TEST(Light_ComponentsSerializeRoundTrip)
{
	// 리플렉션 등록만으로 직렬화된다
	FScene Source;
	const FEntity Torch = Source.CreateEntity("Torch");
	FPointLightComponent& Point = Source.GetRegistry().Emplace<FPointLightComponent>(Torch);
	Point.Color        = FVector3(1.0f, 0.5f, 0.25f);
	Point.Radius       = 750.0f;
	Point.bCastShadows = true;
	const FEntity Spot = Source.CreateEntity("Spot");
	FSpotLightComponent& SpotLight = Source.GetRegistry().Emplace<FSpotLightComponent>(Spot);
	SpotLight.InnerConeAngle = 12.0f;
	SpotLight.OuterConeAngle = 24.0f;
	SpotLight.Intensity      = 55.0f;

	FScene Loaded;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Loaded, FSceneSerializer::ToJsonString(Source)));
	int32 PointCount = 0;
	Loaded.GetRegistry().View<FPointLightComponent>().Each([&](FEntity, FPointLightComponent& Light) {
		++PointCount;
		E_EXPECT_NEAR(Light.Color.Y, 0.5f, 1.0e-6f);
		E_EXPECT_NEAR(Light.Radius, 750.0f, 1.0e-4f);
		E_EXPECT_TRUE(Light.bCastShadows);
	});
	int32 SpotCount = 0;
	Loaded.GetRegistry().View<FSpotLightComponent>().Each([&](FEntity, FSpotLightComponent& Light) {
		++SpotCount;
		E_EXPECT_NEAR(Light.InnerConeAngle, 12.0f, 1.0e-4f);
		E_EXPECT_NEAR(Light.OuterConeAngle, 24.0f, 1.0e-4f);
		E_EXPECT_NEAR(Light.Intensity, 55.0f, 1.0e-4f);
		E_EXPECT_FALSE(Light.bCastShadows);
	});
	E_EXPECT_EQ(PointCount, 1);
	E_EXPECT_EQ(SpotCount, 1);
}
