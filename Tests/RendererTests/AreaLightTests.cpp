#include "Core/Testing/TestFramework.h"
#include "Renderer/AreaLightMath.h"
#include "Renderer/IesProfile.h"
#include "Renderer/LightMath.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"

#include <cmath>
#include <string>

// 면광원 LTC / IES / 쿠키 순수 식 (Renderer/AreaLightMath.h, Renderer/IesProfile.h ↔ Shaders/AreaLight.hlsli, Lighting.hlsli)

namespace
{
	using LightMath::ELocalLightType;

	// 수치 적분 기준값: 표면 점 P(법선 N)에서 본 사각형/원판의 형태 계수 = ∫ cosθ_r cosθ_e / (π r²) dA (지평선 아래는 0)
	float NumericFormFactor(ELocalLightType Type, const FVector3& P, const FVector3& N, const FVector3& Center, const FVector3& Forward, const FVector3& Right,
	                        const FVector3& Up, float HalfWidth, float HalfHeight, bool bTwoSided)
	{
		constexpr int32 Steps = 240;
		const float     CellW = 2.0f * HalfWidth / Steps;
		const float     CellH = 2.0f * HalfHeight / Steps;
		double          Sum   = 0.0;
		for (int32 Y = 0; Y < Steps; ++Y)
		{
			for (int32 X = 0; X < Steps; ++X)
			{
				const float U = -HalfWidth + (static_cast<float>(X) + 0.5f) * CellW;
				const float V = -HalfHeight + (static_cast<float>(Y) + 0.5f) * CellH;
				if (Type == ELocalLightType::Disc && (U * U) / (HalfWidth * HalfWidth) + (V * V) / (HalfHeight * HalfHeight) > 1.0f)
				{
					continue;
				}
				const FVector3 Q        = Center + Right * U + Up * V;
				const FVector3 D        = Q - P;
				const float    R2       = D.LengthSquared();
				const FVector3 L        = D / std::sqrt(R2);
				const float    CosR     = FVector3::Dot(N, L);
				float          CosE     = FVector3::Dot(Forward, -L);
				CosE                    = bTwoSided ? FMath::Abs(CosE) : CosE;
				if (CosR <= 0.0f || CosE <= 0.0f)
				{
					continue;
				}
				Sum += static_cast<double>(CosR * CosE / (FMath::Pi * R2) * CellW * CellH);
			}
		}
		return static_cast<float>(Sum);
	}

	float LtcDiffuse(ELocalLightType Type, const FVector3& P, const FVector3& N, const FVector3& Center, const FVector3& Forward, const FVector3& Right,
	                 const FVector3& Up, float HalfWidth, float HalfHeight, bool bTwoSided)
	{
		const AreaLightMath::FAreaPolygon Polygon = AreaLightMath::MakePolygon(Type, Center, Right, Up, HalfWidth, HalfHeight);
		// 시선은 확산에 영향 없음 (단위 행렬) — 법선과 조금 다른 임의 방향
		const FVector3 V = (N + FVector3(0.3f, 0.2f, 0.1f)).GetNormalized();
		return AreaLightMath::EvaluatePolygon(N, V, P, AreaLightMath::FLtcInverse{}, Polygon, Center, Forward, bTwoSided, AreaLightMath::GetLtcTable2());
	}

	const char* GSample2002 = "IESNA:LM-63-2002\n"
	                          "[TEST] 단위 테스트\n"
	                          "[MANUFAC] ProjectE\n"
	                          "TILT=NONE\n"
	                          "1 1000 1 5 1 1 2 0.1 0.1 0\n"
	                          "1.0 1.0 50\n"
	                          "0 22.5 45 67.5 90\n"
	                          "0\n"
	                          "1000 900 500 100 0\n";
} // namespace

E_TEST(AreaLight_LtcFormFactorMatchesAnalytic)
{
	// 바닥 점 위 2m에 아래를 보는 1m × 0.5m 사각형: 수치 적분과 LTC(단위 행렬 + 구 근사)
	const FVector3 Forward(0.0f, 0.0f, -1.0f);
	const FVector3 Right(0.0f, 1.0f, 0.0f);
	const FVector3 Up = FVector3::Cross(Forward, Right);
	const FVector3 Center(0.0f, 0.0f, 200.0f);
	const FVector3 N(0.0f, 0.0f, 1.0f);
	struct FCase
	{
		FVector3 P;
		FVector3 Normal;
	};
	const FCase Cases[] = { { FVector3(0.0f, 0.0f, 0.0f), N },
		                    { FVector3(80.0f, -40.0f, 0.0f), N },
		                    { FVector3(0.0f, 0.0f, 150.0f), N },                                          // 가까움 (넓은 입체각)
		                    { FVector3(150.0f, 0.0f, 120.0f), FVector3(-1.0f, 0.0f, 0.3f).GetNormalized() } }; // 벽 쪽 기울어진 면
	for (const FCase& Case : Cases)
	{
		const float Reference = NumericFormFactor(ELocalLightType::Rect, Case.P, Case.Normal, Center, Forward, Right, Up, 50.0f, 25.0f, false);
		const float Ltc       = LtcDiffuse(ELocalLightType::Rect, Case.P, Case.Normal, Center, Forward, Right, Up, 50.0f, 25.0f, false);
		E_EXPECT_TRUE(Reference > 0.0f);
		E_EXPECT_NEAR(Ltc / Reference, 1.0f, 0.02f); // 꼭짓점 순서가 뒤집히면 0(잘림) 또는 음수
	}
	// 원판 (같은 넓이 8각형 근사): 수 % 이내
	const float DiscReference = NumericFormFactor(ELocalLightType::Disc, FVector3(30.0f, 0.0f, 0.0f), N, Center, Forward, Right, Up, 40.0f, 40.0f, false);
	const float DiscLtc       = LtcDiffuse(ELocalLightType::Disc, FVector3(30.0f, 0.0f, 0.0f), N, Center, Forward, Right, Up, 40.0f, 40.0f, false);
	E_EXPECT_NEAR(DiscLtc / DiscReference, 1.0f, 0.03f);
}

E_TEST(AreaLight_HorizonClippingApproximation)
{
	// 광원이 표면 지평선에 걸치는 경우 (구 근사 잘림): 수치 적분과 비교 — 근사라 허용 오차를 넓게
	const FVector3 Forward(-1.0f, 0.0f, 0.0f); // 벽에 붙은 창문 (빛이 -X로)
	const FVector3 Right(0.0f, 1.0f, 0.0f);
	const FVector3 Up = FVector3::Cross(Forward, Right);
	const FVector3 Center(0.0f, 0.0f, 50.0f); // 바닥에서 아래 끝이 걸침 (세로 반 100cm)
	const FVector3 P(-120.0f, 0.0f, 0.0f);
	const FVector3 N(0.0f, 0.0f, 1.0f);
	const float    Reference = NumericFormFactor(ELocalLightType::Rect, P, N, Center, Forward, Right, Up, 60.0f, 100.0f, false);
	const float    Ltc       = LtcDiffuse(ELocalLightType::Rect, P, N, Center, Forward, Right, Up, 60.0f, 100.0f, false);
	E_EXPECT_TRUE(Reference > 0.01f);
	E_EXPECT_NEAR(Ltc / Reference, 1.0f, 0.12f);
	// 완전히 지평선 아래면 0
	const float Below = LtcDiffuse(ELocalLightType::Rect, FVector3(-120.0f, 0.0f, 400.0f), N, Center, Forward, Right, Up, 60.0f, 100.0f, false);
	E_EXPECT_NEAR(Below, 0.0f, 1.0e-3f);
}

E_TEST(AreaLight_BackFaceAndTwoSided)
{
	const FVector3 Forward(0.0f, 0.0f, -1.0f);
	const FVector3 Right(0.0f, 1.0f, 0.0f);
	const FVector3 Up = FVector3::Cross(Forward, Right);
	const FVector3 Center(0.0f, 0.0f, 100.0f);
	// 위쪽 점 (광원 뒤): 단면 0, 양면이면 아래쪽 대칭점과 같다
	const FVector3 Above(20.0f, 10.0f, 200.0f);
	const FVector3 Below(20.0f, 10.0f, 0.0f);
	const FVector3 DownN(0.0f, 0.0f, -1.0f);
	const FVector3 UpN(0.0f, 0.0f, 1.0f);
	E_EXPECT_NEAR(LtcDiffuse(ELocalLightType::Rect, Above, DownN, Center, Forward, Right, Up, 30.0f, 30.0f, false), 0.0f, 0.0f);
	const float Back  = LtcDiffuse(ELocalLightType::Rect, Above, DownN, Center, Forward, Right, Up, 30.0f, 30.0f, true);
	const float Front = LtcDiffuse(ELocalLightType::Rect, Below, UpN, Center, Forward, Right, Up, 30.0f, 30.0f, true);
	E_EXPECT_TRUE(Front > 0.01f);
	E_EXPECT_NEAR(Back, Front, Front * 0.01f);
}

E_TEST(AreaLight_FarFieldMatchesPointLightUnits)
{
	// 단위 규약: 면 법선 방향 원거리에서 휘도 × 형태 계수 = 점광원 Intensity × 역제곱 (같은 Intensity)
	const FVector3 Forward(0.0f, 0.0f, -1.0f);
	const FVector3 Right(0.0f, 1.0f, 0.0f);
	const FVector3 Up       = FVector3::Cross(Forward, Right);
	const float    Distance = 1000.0f;
	const FVector3 Center(0.0f, 0.0f, Distance);
	for (const ELocalLightType Type : { ELocalLightType::Rect, ELocalLightType::Disc })
	{
		const float HalfW  = 20.0f;
		const float HalfH  = 10.0f;
		const float Area   = AreaLightMath::ComputeArea(Type, HalfW, HalfH);
		const float Ltc    = LtcDiffuse(Type, FVector3::ZeroVector, FVector3::UpVector, Center, Forward, Right, Up, HalfW, HalfH, false);
		const float Shaded = Ltc * AreaLightMath::IntensityToRadianceScale(Area); // Intensity 1
		// 확산 = 알베도 × 휘도 × 형태 계수 ↔ 점광원 확산 = 알베도 / π × Intensity × 역제곱
		E_EXPECT_NEAR(Shaded * FMath::Pi / LightMath::InverseSquare(Distance), 1.0f, 0.02f);
		// 대표점 근사(RT·안개)도 같은 값 (휘도 배율을 되돌림)
		const float Approx = AreaLightMath::IntensityToRadianceScale(Area) * AreaLightMath::ComputeApproxAreaFactor(1.0f, false, Area) *
		                     LightMath::InverseSquare(Distance);
		E_EXPECT_NEAR(Approx / LightMath::InverseSquare(Distance), 1.0f, 1.0e-4f);
	}
	E_EXPECT_NEAR(AreaLightMath::ComputeArea(ELocalLightType::Rect, 50.0f, 25.0f), 5000.0f, 1.0e-2f);
	E_EXPECT_NEAR(AreaLightMath::IntensityToRadianceScale(10000.0f), 1.0f, 1.0e-6f); // 1m² → 휘도 = Intensity
	E_EXPECT_NEAR(AreaLightMath::ComputeApproxAreaFactor(-0.5f, false, 10000.0f), 0.0f, 0.0f);
	E_EXPECT_NEAR(AreaLightMath::ComputeApproxAreaFactor(-0.5f, true, 10000.0f), 0.5f, 1.0e-6f);
}

E_TEST(AreaLight_DiscPolygonArea)
{
	const AreaLightMath::FAreaPolygon Polygon =
		AreaLightMath::MakePolygon(ELocalLightType::Disc, FVector3::ZeroVector, FVector3::RightVector, FVector3::UpVector, 30.0f, 30.0f);
	E_EXPECT_EQ(Polygon.Count, AreaLightMath::DiscPolygonSides);
	float Area = 0.0f; // 신발끈 공식 (Y, Z 평면)
	for (uint32 Index = 0; Index < Polygon.Count; ++Index)
	{
		const FVector3& A = Polygon.Points[Index];
		const FVector3& B = Polygon.Points[(Index + 1) % Polygon.Count];
		Area += A.Y * B.Z - B.Y * A.Z;
	}
	E_EXPECT_NEAR(FMath::Abs(Area) * 0.5f, FMath::Pi * 30.0f * 30.0f, 1.0f);
}

E_TEST(AreaLight_SpecularLtcNormalization)
{
	// 아주 큰 면(반구 거의 전체)을 위에 두면 GGX LTC 적분 ≈ 1 (분포 정규화), 크기 계수 ≤ 1
	const FVector3 Forward(0.0f, 0.0f, -1.0f);
	const FVector3 Right(0.0f, 1.0f, 0.0f);
	const FVector3 Up = FVector3::Cross(Forward, Right);
	const AreaLightMath::FAreaPolygon Polygon =
		AreaLightMath::MakePolygon(ELocalLightType::Rect, FVector3(0.0f, 0.0f, 10.0f), Right, Up, 100000.0f, 100000.0f);
	for (const float Roughness : { 0.2f, 0.5f, 1.0f })
	{
		for (const float ViewAngle : { 0.0f, 45.0f })
		{
			const float    Radians = FMath::DegreesToRadians(ViewAngle);
			const FVector3 V(std::sin(Radians), 0.0f, std::cos(Radians));
			const AreaLightMath::FAreaLightTerms Terms = AreaLightMath::EvaluateAreaLightTerms(FVector3::UpVector, V, FVector3::ZeroVector, Roughness, Polygon,
			                                                                                     FVector3(0.0f, 0.0f, 10.0f), Forward, false);
			E_EXPECT_NEAR(Terms.Diffuse, 1.0f, 0.02f);
			E_EXPECT_NEAR(Terms.Specular, 1.0f, 0.06f);
			E_EXPECT_TRUE(Terms.Magnitude > 0.3f && Terms.Magnitude <= 1.01f);
			E_EXPECT_TRUE(Terms.Fresnel >= 0.0f && Terms.Fresnel < Terms.Magnitude);
		}
	}
	// 표 좌표: 텍셀 가운데 (0 → 0.5/64, 1 → 63.5/64)
	const FVector2 Low  = AreaLightMath::ComputeLtcCoords(0.0f, 1.0f);
	const FVector2 High = AreaLightMath::ComputeLtcCoords(1.0f, 0.0f);
	E_EXPECT_NEAR(Low.X, 0.5f / 64.0f, 1.0e-6f);
	E_EXPECT_NEAR(Low.Y, 0.5f / 64.0f, 1.0e-6f);
	E_EXPECT_NEAR(High.X, 63.5f / 64.0f, 1.0e-6f);
	E_EXPECT_NEAR(High.Y, 63.5f / 64.0f, 1.0e-6f);
	// 모서리 적분 벡터: 직각 모서리(θ = 90°)의 θ/sinθ/2π = 1/4
	const FVector3 Edge = AreaLightMath::IntegrateEdgeVector(FVector3::ForwardVector, FVector3::RightVector);
	E_EXPECT_NEAR(Edge.Z, 0.25f, 2.0e-3f);
}

E_TEST(AreaLight_BoundsAndClusters)
{
	using namespace AreaLightMath;
	E_EXPECT_NEAR(ComputeBoundingRadius(ELocalLightType::Rect, 500.0f, 30.0f, 40.0f), 550.0f, 1.0e-3f);
	E_EXPECT_NEAR(ComputeBoundingRadius(ELocalLightType::Disc, 500.0f, 30.0f, 40.0f), 540.0f, 1.0e-3f);
	// 면 거리: 면 위 투영이 안이면 법선 거리, 밖이면 모서리까지
	E_EXPECT_NEAR(DistanceToArea(ELocalLightType::Rect, FVector3(10.0f, 5.0f, -3.0f), 30.0f, 40.0f), 10.0f, 1.0e-4f);
	E_EXPECT_NEAR(DistanceToArea(ELocalLightType::Rect, FVector3(0.0f, 33.0f, 44.0f), 30.0f, 40.0f), 5.0f, 1.0e-4f);
	E_EXPECT_NEAR(DistanceToArea(ELocalLightType::Disc, FVector3(0.0f, 50.0f, 0.0f), 30.0f, 30.0f), 20.0f, 1.0e-4f);
	E_EXPECT_NEAR(DistanceToArea(ELocalLightType::Disc, FVector3(4.0f, 3.0f, 0.0f), 30.0f, 30.0f), 4.0f, 1.0e-4f);

	// 클러스터: 면 모서리에서 영향 반경 안 지점의 클러스터는 경계 구와 반드시 만난다
	const LightMath::FSliceParams Slices = LightMath::ComputeSliceParams(10.0f, 10000.0f, LightMath::ClusterGridZ);
	const float                   Radius = 300.0f;
	const float                   Bounds = ComputeBoundingRadius(ELocalLightType::Rect, Radius, 200.0f, 100.0f);
	const FVector3                Center(0.0f, 0.0f, 1500.0f); // 뷰 공간 (+Z 앞)
	const FVector3                Lit(450.0f, 0.0f, 1500.0f);  // 면 모서리(가로 끝 200)에서 250 — 영향 안
	E_EXPECT_TRUE(DistanceToArea(ELocalLightType::Rect, FVector3(0.0f, Lit.X - Center.X, 0.0f), 200.0f, 100.0f) < Radius);
	const float    Tan  = 0.5f;
	const uint32   Slice = LightMath::DepthToSlice(Lit.Z, Slices, LightMath::ClusterGridZ);
	const uint32   TileX = static_cast<uint32>((Lit.X / (Lit.Z * Tan) * 0.5f + 0.5f) * LightMath::ClusterGridX);
	const FBox     Box   = LightMath::ComputeClusterViewBounds(TileX, LightMath::ClusterGridY / 2, Slice, LightMath::ClusterGridX, LightMath::ClusterGridY,
	                                                           LightMath::ClusterGridZ, 10.0f, 10000.0f, false, Tan, Tan);
	E_EXPECT_TRUE(LightMath::SphereIntersectsBox(Center, Bounds, Box));
}

E_TEST(AreaLight_BarnDoorCone)
{
	const LightMath::FConeParams None = AreaLightMath::ComputeBarnDoorCone(90.0f, 20.0f, 50.0f);
	E_EXPECT_NEAR(LightMath::ConeAttenuation(0.0f, None), 1.0f, 0.0f);
	const LightMath::FConeParams Door = AreaLightMath::ComputeBarnDoorCone(45.0f, 100.0f, 10.0f); // 부드러운 폭 atan(0.1) ≈ 5.7°
	auto AtAngle = [&](float Degrees) { return LightMath::ConeAttenuation(FMath::Cos(FMath::DegreesToRadians(Degrees)), Door); };
	E_EXPECT_NEAR(AtAngle(0.0f), 1.0f, 1.0e-5f);
	E_EXPECT_NEAR(AtAngle(39.0f), 1.0f, 1.0e-5f);
	E_EXPECT_NEAR(AtAngle(45.5f), 0.0f, 1.0e-5f);
	const float Middle = AtAngle(42.0f);
	E_EXPECT_TRUE(Middle > 0.05f && Middle < 0.95f);
	// 길이가 짧으면 경계가 더 부드럽다
	const LightMath::FConeParams Short = AreaLightMath::ComputeBarnDoorCone(45.0f, 5.0f, 10.0f);
	E_EXPECT_TRUE(LightMath::ConeAttenuation(FMath::Cos(FMath::DegreesToRadians(30.0f)), Short) < 1.0f);
}

// 문 덮개 원뿔은 면에서 가장 가까운 점 기준 — 긴 면(27m × 20cm)의 끝 쪽 앞 표면도 원뿔 안 (가운데 기준이면 거의 0이었다)
E_TEST(AreaLight_BarnDoorUsesNearestPoint)
{
	const auto  Rect       = LightMath::ELocalLightType::Rect;
	const float HalfWidth  = 10.0f;
	const float HalfHeight = 1350.0f;
	const LightMath::FConeParams Door = AreaLightMath::ComputeBarnDoorCone(88.0f, 10.0f, HalfHeight);
	// 면 끝 근처 바로 앞 110cm (가운데에서 본 각 ≈ 85°)
	const FVector3 NearEnd(110.0f, 0.0f, 1300.0f);
	E_EXPECT_NEAR(AreaLightMath::ComputeBarnDoorCos(Rect, NearEnd, HalfWidth, HalfHeight), 1.0f, 1.0e-6f);
	E_EXPECT_NEAR(LightMath::ConeAttenuation(AreaLightMath::ComputeBarnDoorCos(Rect, NearEnd, HalfWidth, HalfHeight), Door), 1.0f, 1.0e-5f);
	const float CenterCos = NearEnd.X / NearEnd.Length();
	E_EXPECT_TRUE(LightMath::ConeAttenuation(CenterCos, Door) < 0.05f);
	// 면 밖 옆쪽은 가장 가까운 모서리에서 본 각, 뒤쪽(양면)은 절댓값
	const FVector3 Beside(100.0f, 110.0f, 0.0f); // 모서리(Y = 10)에서 (100, 100) → 45°
	E_EXPECT_NEAR(AreaLightMath::ComputeBarnDoorCos(Rect, Beside, HalfWidth, HalfHeight), FMath::Cos(FMath::DegreesToRadians(45.0f)), 1.0e-5f);
	E_EXPECT_NEAR(AreaLightMath::ComputeBarnDoorCos(Rect, FVector3(-100.0f, 110.0f, 0.0f), HalfWidth, HalfHeight),
	              FMath::Cos(FMath::DegreesToRadians(45.0f)), 1.0e-5f);
	// 거리는 같은 가장 가까운 점 기준
	E_EXPECT_NEAR(AreaLightMath::DistanceToArea(Rect, Beside, HalfWidth, HalfHeight), std::sqrt(2.0f) * 100.0f, 1.0e-3f);
	// 원판: 면 안 앞쪽은 1
	E_EXPECT_NEAR(AreaLightMath::ComputeBarnDoorCos(LightMath::ELocalLightType::Disc, FVector3(50.0f, 30.0f, 0.0f), 40.0f, 40.0f), 1.0f, 1.0e-6f);
}

E_TEST(AreaLight_ShadowDepthAndPenumbra)
{
	// 원근 깊이 → 뷰 깊이 선형화는 투영의 역
	const float      NearZ = 5.0f;
	const float      FarZ  = 800.0f;
	const FMatrix4x4 Proj  = FMatrix4x4::MakePerspectiveFov(FMath::DegreesToRadians(90.0f), 1.0f, NearZ, FarZ);
	for (const float Depth : { 6.0f, 50.0f, 400.0f, 790.0f })
	{
		const FVector3 P(0.0f, 0.0f, Depth);
		const float    Clip = P.Z * Proj.M[2][2] + Proj.M[3][2];
		const float    W    = P.Z * Proj.M[2][3] + Proj.M[3][3];
		E_EXPECT_NEAR(AreaLightMath::LinearizeShadowDepth(Clip / W, NearZ, FarZ), Depth, Depth * 1.0e-3f);
	}
	// 반그림자: 가림이 수신에 붙으면 0, 멀어질수록 넓다, 면이 크면 넓다
	E_EXPECT_NEAR(AreaLightMath::ComputePenumbraUV(50.0f, 300.0f, 300.0f, 2.0f), 0.0f, 1.0e-6f);
	const float Near = AreaLightMath::ComputePenumbraUV(50.0f, 300.0f, 250.0f, 2.0f);
	const float Far  = AreaLightMath::ComputePenumbraUV(50.0f, 300.0f, 100.0f, 2.0f);
	E_EXPECT_TRUE(Far > Near && Near > 0.0f);
	E_EXPECT_TRUE(AreaLightMath::ComputePenumbraUV(100.0f, 300.0f, 100.0f, 2.0f) > Far);
	// 50 × (300 - 100) / 100 = 100cm 월드 → 수신 깊이 300에서 투영 폭 600 → 1/6
	E_EXPECT_NEAR(Far, 100.0f / 600.0f, 1.0e-5f);
}

E_TEST(LightCookie_ProjectionUV)
{
	using namespace AreaLightMath;
	// 스포트 반각 45°, 배율 1: 축 = (0.5, 0.5), 원뿔 가장자리(오른쪽) = u 1, 위쪽 = v 0
	const FVector4 Transform = ComputeCookieTransform(ELocalLightType::Spot, 45.0f, FVector2(1.0f, 1.0f), FVector2(0.0f, 0.0f));
	bool           bValid    = false;
	FVector2       UV        = ComputeCookieUV(ELocalLightType::Spot, FVector3(1.0f, 0.0f, 0.0f), Transform, bValid);
	E_EXPECT_TRUE(bValid);
	E_EXPECT_NEAR(UV.X, 0.5f, 1.0e-5f);
	E_EXPECT_NEAR(UV.Y, 0.5f, 1.0e-5f);
	UV = ComputeCookieUV(ELocalLightType::Spot, FVector3(1.0f, 1.0f, 0.0f).GetNormalized(), Transform, bValid);
	E_EXPECT_NEAR(UV.X, 1.0f, 1.0e-5f);
	UV = ComputeCookieUV(ELocalLightType::Rect, FVector3(1.0f, 0.0f, 1.0f).GetNormalized(), ComputeCookieTransform(ELocalLightType::Rect, 45.0f, FVector2(1.0f, 1.0f), FVector2(0.0f, 0.0f)), bValid);
	E_EXPECT_NEAR(UV.Y, 0.0f, 1.0e-5f);
	ComputeCookieUV(ELocalLightType::Spot, FVector3(-1.0f, 0.0f, 0.0f), Transform, bValid);
	E_EXPECT_FALSE(bValid);
	// 오프셋(패닝)·배율
	const FVector4 Panned = ComputeCookieTransform(ELocalLightType::Spot, 45.0f, FVector2(2.0f, 1.0f), FVector2(0.25f, 0.0f));
	UV                    = ComputeCookieUV(ELocalLightType::Spot, FVector3(1.0f, 1.0f, 0.0f).GetNormalized(), Panned, bValid);
	E_EXPECT_NEAR(UV.X, 1.75f, 1.0e-5f); // 0.5 + 0.25 + 1 × 0.5 × 2
	// 점광원 = 위도-경도 (θ / π, φ / 2π)
	const FVector4 Point = ComputeCookieTransform(ELocalLightType::Point, 0.0f, FVector2(1.0f, 1.0f), FVector2(0.0f, 0.0f));
	UV                   = ComputeCookieUV(ELocalLightType::Point, FVector3(0.0f, 0.0f, 1.0f), Point, bValid);
	E_EXPECT_TRUE(bValid);
	E_EXPECT_NEAR(UV.X, 0.25f, 1.0e-5f); // φ = 90°
	E_EXPECT_NEAR(UV.Y, 0.5f, 1.0e-5f);  // θ = 90°
	// 방향광 쿠키 축: 빛에 수직, 길이 = 1 / 타일
	FVector4 U;
	FVector4 V;
	ComputeDirectionalCookieAxes(FVector3(0.0f, 0.0f, -1.0f), 500.0f, FVector2(0.1f, 0.2f), U, V);
	E_EXPECT_NEAR(FVector3::Dot(FVector3(U.X, U.Y, U.Z), FVector3(0.0f, 0.0f, -1.0f)), 0.0f, 1.0e-6f);
	E_EXPECT_NEAR(FVector3(V.X, V.Y, V.Z).Length(), 1.0f / 500.0f, 1.0e-7f);
	E_EXPECT_NEAR(U.W, 0.1f, 0.0f);
}

E_TEST(Ies_ParseFormats)
{
	FIesProfile Profile;
	std::string Error;
	E_EXPECT_TRUE(FIesProfile::Parse(GSample2002, Profile, &Error));
	E_EXPECT_TRUE(Profile.FormatName == "LM-63-2002");
	E_EXPECT_EQ(Profile.VerticalAngles.size(), size_t(5));
	E_EXPECT_EQ(Profile.HorizontalAngles.size(), size_t(1));
	E_EXPECT_NEAR(Profile.MaxCandela, 1000.0f, 1.0e-3f);
	E_EXPECT_NEAR(Profile.Sample(0.0f, 123.0f), 1000.0f, 1.0e-3f); // 회전 대칭
	E_EXPECT_NEAR(Profile.Sample(33.75f, 0.0f), 700.0f, 1.0e-3f);  // 22.5~45 가운데
	E_EXPECT_NEAR(Profile.Sample(120.0f, 0.0f), 0.0f, 0.0f);       // 범위 밖 (위쪽)

	// 1986 (IESNA 줄 없음), 쉼표·줄바꿈 섞임, 배율 × 안정기 계수, TILT=INCLUDE
	const char* Old = "제조사 머리 글\n"
	                  "TILT=INCLUDE\n"
	                  "1\n3\n0 45 90\n1 0.9 0.8\n"
	                  "1, -1, 2, 3, 1, 1, 2, 0, 0, 0,\n0.5 1 10\n"
	                  "0 45 90\n0\n"
	                  "100,\n50\n10\n";
	E_EXPECT_TRUE(FIesProfile::Parse(Old, Profile, &Error));
	E_EXPECT_TRUE(Profile.FormatName == "LM-63-1986");
	E_EXPECT_NEAR(Profile.MaxCandela, 100.0f, 1.0e-3f); // 100 × 2 × 0.5
	E_EXPECT_NEAR(Profile.LumensPerLamp, -1.0f, 0.0f);   // 절대 측광

	// 1995 + 4분면 대칭 (수평 0, 90): φ = 180은 0과 같다
	const char* Quadrant = "IESNA:LM-63-1995\r\nTILT=NONE\r\n1 1000 1 3 2 1 2 0 0 0\r\n1 1 10\r\n0 45 90\r\n0 90\r\n100 50 0\r\n200 100 0\r\n";
	E_EXPECT_TRUE(FIesProfile::Parse(Quadrant, Profile, &Error));
	E_EXPECT_NEAR(Profile.FoldHorizontalAngle(180.0f), 0.0f, 1.0e-4f);
	E_EXPECT_NEAR(Profile.FoldHorizontalAngle(270.0f), 90.0f, 1.0e-4f);
	E_EXPECT_NEAR(Profile.FoldHorizontalAngle(315.0f), 45.0f, 1.0e-4f);
	E_EXPECT_NEAR(Profile.Sample(0.0f, 180.0f), 100.0f, 1.0e-3f);
	E_EXPECT_NEAR(Profile.Sample(0.0f, 45.0f), 150.0f, 1.0e-3f);
	E_EXPECT_NEAR(Profile.Sample(0.0f, 270.0f), 200.0f, 1.0e-3f);

	// 앞뒤 대칭 (90..270): φ = 0 → 180, 300 → 240
	const char* FrontBack = "IESNA91\nTILT=NONE\n1 1000 1 1 3 1 2 0 0 0\n1 1 10\n0\n90 180 270\n10 20 30\n";
	E_EXPECT_TRUE(FIesProfile::Parse(FrontBack, Profile, &Error));
	E_EXPECT_TRUE(Profile.FormatName == "LM-63-1991");
	E_EXPECT_NEAR(Profile.FoldHorizontalAngle(0.0f), 180.0f, 1.0e-4f);
	E_EXPECT_NEAR(Profile.FoldHorizontalAngle(300.0f), 240.0f, 1.0e-4f);
	E_EXPECT_NEAR(Profile.Sample(0.0f, 0.0f), 20.0f, 1.0e-3f);
}

E_TEST(Ies_InvalidInput)
{
	FIesProfile Profile;
	std::string Error;
	E_EXPECT_FALSE(FIesProfile::Parse("", Profile, &Error));
	E_EXPECT_FALSE(Error.empty());
	E_EXPECT_FALSE(FIesProfile::Parse("IESNA:LM-63-2002\n1 1000 1 2 1 1 2 0 0 0\n", Profile, &Error)); // TILT 없음
	E_EXPECT_FALSE(FIesProfile::Parse("TILT=NONE\n1 1000 1 3 1 1 2 0 0 0\n1 1 10\n0 45 90\n0\n100 50\n", Profile, &Error)); // 칸델라 모자람
	E_EXPECT_FALSE(FIesProfile::Parse("TILT=NONE\n1 1000 1 2 1 1 2 0 0 0\n1 1 10\n45 0\n0\n100 50\n", Profile, &Error));       // 오름차순 아님
	E_EXPECT_FALSE(FIesProfile::Parse("TILT=NONE\n1 1000 1 2 1 1 2 0 0 0\n1 1 10\n0 90\n0\n0 0\n", Profile, &Error));          // 모두 0
	E_EXPECT_FALSE(FIesProfile::Parse("TILT=NONE\n1 1000 1 0 1 1 2 0 0 0\n1 1 10\n0\n", Profile, &Error));                     // 각 수 0
	E_EXPECT_FALSE(FIesProfile::Parse("TILT=NONE\n1 1000 1 2 1 1 2 0 0 0\n1 1 10\n0 abc\n0\n1 1\n", Profile, &Error));         // 숫자 아님
	E_EXPECT_TRUE(Profile.Candela.empty()); // 실패하면 비운다
}

E_TEST(Ies_TextureCoords)
{
	using namespace AreaLightMath;
	// 빛 축(Forward) = θ 0 → 첫 칸 가운데, 반대 = 마지막 칸 가운데
	FVector2 UV = ComputeIesUV(FVector3(1.0f, 0.0f, 0.0f));
	E_EXPECT_NEAR(UV.X, 0.5f / IesTextureWidth, 1.0e-6f);
	UV = ComputeIesUV(FVector3(-1.0f, 0.0f, 0.0f));
	E_EXPECT_NEAR(UV.X, (IesTextureWidth - 0.5f) / IesTextureWidth, 1.0e-5f);
	// φ: Right = 0°, Up = 90° (세로 칸 (H-1) / 4 위치)
	UV = ComputeIesUV(FVector3(0.0f, 1.0f, 0.0f));
	E_EXPECT_NEAR(UV.Y, 0.5f / IesTextureHeight, 1.0e-6f);
	UV = ComputeIesUV(FVector3(0.0f, 0.0f, 1.0f));
	E_EXPECT_NEAR(UV.Y, ((IesTextureHeight - 1) * 0.25f + 0.5f) / IesTextureHeight, 1.0e-5f);
	E_EXPECT_NEAR(UV.X, (0.5f * (IesTextureWidth - 1) + 0.5f) / IesTextureWidth, 1.0e-5f); // θ = 90°

	// 굽기: 칸 가운데 = 같은 각, 최대 1로 정규화
	FIesProfile Profile;
	E_EXPECT_TRUE(FIesProfile::Parse(GSample2002, Profile));
	const std::vector<float> Pixels = Profile.BakeTexture(IesTextureWidth, IesTextureHeight);
	E_EXPECT_EQ(Pixels.size(), size_t(IesTextureWidth * IesTextureHeight));
	E_EXPECT_NEAR(Pixels[0], 1.0f, 1.0e-6f);
	const uint32 Column = static_cast<uint32>(std::lround(45.0f / 180.0f * (IesTextureWidth - 1))); // ≈ 45°
	const float  Theta  = 180.0f * Column / (IesTextureWidth - 1);
	E_EXPECT_NEAR(Pixels[Column], Profile.Sample(Theta, 0.0f) / 1000.0f, 1.0e-5f);
	E_EXPECT_NEAR(Pixels[IesTextureWidth - 1], 0.0f, 0.0f); // 180°
	E_EXPECT_NEAR(Pixels[(IesTextureHeight - 1) * IesTextureWidth], Pixels[0], 0.0f); // φ 끝 칸 = 0°
}

E_TEST(AreaLight_SerializeComponent)
{
	FScene  Source;
	FEntity Entity                     = Source.CreateEntity("Window");
	FAreaLightComponent& Area          = Source.GetRegistry().Emplace<FAreaLightComponent>(Entity);
	Area.Shape                         = static_cast<int32>(EAreaLightShape::Disc);
	Area.Width                         = 80.0f;
	Area.bTwoSided                     = true;
	Area.BarnDoorAngle                 = 40.0f;
	Area.IesProfile                    = "Lights/Downlight.ies";
	Area.CookieTexture                 = "Lights/Cookie.png";
	Area.CookiePanSpeed                = FVector2(0.5f, 0.0f);
	FSpotLightComponent& Spot          = Source.GetRegistry().Emplace<FSpotLightComponent>(Entity);
	Spot.bUseIesIntensity              = true;
	FDirectionalLightComponent& Sun    = Source.GetRegistry().Emplace<FDirectionalLightComponent>(Entity);
	Sun.CookieTileSize                 = 2500.0f;

	const std::string Json = FSceneSerializer::ToJsonString(Source);
	FScene            Loaded;
	E_EXPECT_TRUE(FSceneSerializer::FromJsonString(Loaded, Json));
	bool bFound = false;
	Loaded.GetRegistry().View<FAreaLightComponent>().Each([&](FEntity, FAreaLightComponent& Light) {
		bFound = true;
		E_EXPECT_EQ(Light.Shape, static_cast<int32>(EAreaLightShape::Disc));
		E_EXPECT_NEAR(Light.Width, 80.0f, 0.0f);
		E_EXPECT_TRUE(Light.bTwoSided);
		E_EXPECT_NEAR(Light.BarnDoorAngle, 40.0f, 0.0f);
		E_EXPECT_TRUE(Light.IesProfile == "Lights/Downlight.ies");
		E_EXPECT_TRUE(Light.CookieTexture == "Lights/Cookie.png");
		E_EXPECT_NEAR(Light.CookiePanSpeed.X, 0.5f, 0.0f);
	});
	E_EXPECT_TRUE(bFound);
	Loaded.GetRegistry().View<FSpotLightComponent>().Each([&](FEntity, FSpotLightComponent& Light) { E_EXPECT_TRUE(Light.bUseIesIntensity); });
	Loaded.GetRegistry().View<FDirectionalLightComponent>().Each(
		[&](FEntity, FDirectionalLightComponent& Light) { E_EXPECT_NEAR(Light.CookieTileSize, 2500.0f, 0.0f); });
}
