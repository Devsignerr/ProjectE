#include "Core/Testing/TestFramework.h"
#include "Renderer/AtmosphereMath.h"
#include "Scene/SkyAtmosphere.h"

#include <cmath>

// 물리 기반 대기 (Phase 49) — Renderer/AtmosphereMath.h ↔ Atmosphere.hlsli / AerialPerspective.hlsli 같은 식
namespace
{
	FAtmosphereMath::FParams EarthParams()
	{
		return FAtmosphereMath::FParams{};
	}
} // namespace

E_TEST(Atmosphere_TransmittanceLutParameterizationRoundTrip)
{
	const FAtmosphereMath::FParams Params = EarthParams();
	const float Heights[] = { 0.001f, 0.5f, 3.0f, 20.0f, 80.0f };
	for (const float Height : Heights)
	{
		const float Radius     = Params.BottomRadius + Height;
		const float HorizonCos = -std::sqrt(1.0f - (Params.BottomRadius / Radius) * (Params.BottomRadius / Radius));
		for (int32 Index = 0; Index <= 8; ++Index)
		{
			// 지평선 위 방향만 (Bruneton 매개화 범위)
			const float Mu = HorizonCos + (1.0f - HorizonCos) * static_cast<float>(Index) / 8.0f;
			float       U  = 0.0f;
			float       V  = 0.0f;
			FAtmosphereMath::TransmittanceParamsToUv(Params, Radius, Mu, U, V);
			E_EXPECT_TRUE(U >= -2.0e-3f && U <= 1.002f && V >= 0.0f && V <= 1.0001f);
			float BackHeight = 0.0f;
			float BackMu     = 0.0f;
			FAtmosphereMath::TransmittanceUvToParams(Params, U, V, BackHeight, BackMu);
			E_EXPECT_NEAR(BackHeight, Radius, 0.01f);
			E_EXPECT_NEAR(BackMu, Mu, 2.0e-3f);
		}
	}
}

E_TEST(Atmosphere_ZenithTransmittanceMatchesClosedForm)
{
	// 지면에서 천정: 광학 깊이 = 레일리·미 지수 적분 + 오존 천막 넓이 (닫힌 식)
	const FAtmosphereMath::FParams Params = EarthParams();
	const float    Thickness   = Params.TopRadius - Params.BottomRadius;
	const float    RayleighLen = Params.RayleighScaleHeight * (1.0f - std::exp(-Thickness / Params.RayleighScaleHeight));
	const float    MieLen      = Params.MieScaleHeight * (1.0f - std::exp(-Thickness / Params.MieScaleHeight));
	const float    OzoneLen    = Params.OzoneHalfWidth; // 밑변 2 × 반폭, 높이 1
	const FVector3 Depth       = Params.RayleighScattering * RayleighLen + Params.GetMieExtinction() * MieLen + Params.OzoneAbsorption * OzoneLen;
	const FVector3 Expected    = FAtmosphereMath::Exp(-Depth);
	const FVector3 Numeric     = FAtmosphereMath::ComputeTransmittanceToTop(Params, Params.BottomRadius, 1.0f, 2000);
	const FVector3 Lut         = FAtmosphereMath::ComputeTransmittanceToTop(Params, Params.BottomRadius, 1.0f); // LUT 한 칸과 같은 40단계
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		E_EXPECT_NEAR(Numeric[Channel], Expected[Channel], 1.0e-3f);
		E_EXPECT_NEAR(Lut[Channel], Expected[Channel], 5.0e-3f);
	}
	// 파랑이 가장 많이 사라진다 (레일리)
	E_EXPECT_TRUE(Expected.Z < Expected.Y && Expected.Y < Expected.X + 0.05f);
	// 지평선 쪽은 훨씬 어둡고 붉다
	const FVector3 Horizon = FAtmosphereMath::ComputeTransmittanceToTop(Params, Params.BottomRadius + 0.001f, 0.01f);
	E_EXPECT_TRUE(Horizon.Z < Expected.Z * 0.5f);
	E_EXPECT_TRUE(Horizon.X > Horizon.Z);
}

E_TEST(Atmosphere_SkyViewParameterizationRoundTrip)
{
	const FAtmosphereMath::FParams Params     = EarthParams();
	const float                    ViewHeight = Params.BottomRadius + 0.2f;
	for (int32 VIndex = 0; VIndex < 10; ++VIndex)
	{
		for (int32 UIndex = 0; UIndex < 5; ++UIndex)
		{
			const float V = (static_cast<float>(VIndex) + 0.5f) / 10.0f;
			const float U = (static_cast<float>(UIndex) + 0.5f) / 5.0f;
			float       ViewZenithCos = 0.0f;
			float       LightViewCos  = 0.0f;
			FAtmosphereMath::SkyViewUvToParams(Params, ViewHeight, U, V, ViewZenithCos, LightViewCos);
			float BackU = 0.0f;
			float BackV = 0.0f;
			FAtmosphereMath::SkyViewParamsToUv(Params, ViewHeight, ViewZenithCos, LightViewCos, V >= 0.5f, BackU, BackV);
			E_EXPECT_NEAR(BackU, U, 1.0e-3f);
			E_EXPECT_NEAR(BackV, V, 2.0e-3f);
		}
	}
}

E_TEST(Atmosphere_MultipleScatteringBounded)
{
	const FAtmosphereMath::FParams Params = EarthParams();
	const FVector3 Noon     = FAtmosphereMath::ComputeMultipleScattering(Params, 0.0f, 1.0f);
	const FVector3 Sunset   = FAtmosphereMath::ComputeMultipleScattering(Params, 0.0f, 0.05f);
	const FVector3 Night    = FAtmosphereMath::ComputeMultipleScattering(Params, 0.0f, -0.5f);
	const FVector3 HighNoon = FAtmosphereMath::ComputeMultipleScattering(Params, 30.0f, 1.0f);
	for (int32 Channel = 0; Channel < 3; ++Channel)
	{
		E_EXPECT_TRUE(Noon[Channel] > 0.0f && Noon[Channel] < 1.0f);
		E_EXPECT_TRUE(Sunset[Channel] < Noon[Channel]);
		E_EXPECT_TRUE(Night[Channel] < Sunset[Channel]);
		E_EXPECT_TRUE(HighNoon[Channel] >= 0.0f);
	}
	// 다중 산란도 파랑 쪽 (레일리)
	E_EXPECT_TRUE(Noon.Z > Noon.X);
}

E_TEST(Atmosphere_AerialPerspectiveMatchesNumericIntegration)
{
	const FAtmosphereMath::FParams Atmosphere = EarthParams();
	FAtmosphereMath::FAerialParams Params;
	Params.RayleighScattering  = Atmosphere.RayleighScattering * FAtmosphereMath::CentimetersToKilometers;
	Params.RayleighScaleHeight = Atmosphere.RayleighScaleHeight * FAtmosphereMath::KilometersToCentimeters;
	Params.MieScattering       = Atmosphere.MieScattering * FAtmosphereMath::CentimetersToKilometers;
	Params.MieScaleHeight      = Atmosphere.MieScaleHeight * FAtmosphereMath::KilometersToCentimeters;
	Params.MieExtinction       = Atmosphere.GetMieExtinction() * FAtmosphereMath::CentimetersToKilometers;
	Params.MieAnisotropy       = Atmosphere.MieAnisotropy;
	Params.SunIlluminance      = FVector3(3.0f, 2.8f, 2.6f);
	Params.SunDirection        = FVector3(0.3f, 0.2f, 0.9f).GetNormalized();
	Params.MultiScattering     = FVector3(0.05f, 0.08f, 0.12f);

	const FVector3 Camera(0.0f, 0.0f, 200.0f);
	const FVector3 Targets[] = { FVector3(500000.0f, 0.0f, 200.0f), FVector3(300000.0f, 100000.0f, 80000.0f), FVector3(-200000.0f, 50000.0f, 0.0f) };
	for (const FVector3& Target : Targets)
	{
		const FAtmosphereMath::FAerialResult Closed = FAtmosphereMath::ComputeAerialPerspective(Params, Camera, Target);
		// 수치 적분: 단일 성분이 아니어도 각 점에서 σs(위상 E + Ψ)를 앞쪽 투과율로 쌓는다
		const int32    Steps    = 20000;
		const FVector3 Delta    = Target - Camera;
		const float    Distance = Delta.Length();
		const FVector3 Dir      = Delta / Distance;
		const float    Dt       = Distance / static_cast<float>(Steps);
		const float    CosTheta = FVector3::Dot(Dir, Params.SunDirection);
		FVector3       Depth    = FVector3::ZeroVector;
		FVector3       Inscatter = FVector3::ZeroVector;
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			const FVector3 P        = Camera + Dir * ((static_cast<float>(Step) + 0.5f) * Dt);
			const float    Rayleigh = std::exp(-FMath::Max(P.Z, 0.0f) / Params.RayleighScaleHeight);
			const float    Mie      = std::exp(-FMath::Max(P.Z, 0.0f) / Params.MieScaleHeight);
			const FVector3 Extinction = Params.RayleighScattering * Rayleigh + Params.MieExtinction * Mie;
			const FVector3 Source = (Params.RayleighScattering * (Rayleigh * FAtmosphereMath::RayleighPhase(CosTheta)) +
			                         Params.MieScattering * (Mie * FAtmosphereMath::MiePhase(Params.MieAnisotropy, CosTheta))) * Params.SunIlluminance +
			                        (Params.RayleighScattering * Rayleigh + Params.MieScattering * Mie) * Params.MultiScattering;
			const FVector3 Half = Depth + Extinction * (0.5f * Dt);
			Inscatter += Source * FAtmosphereMath::Exp(-Half) * Dt;
			Depth += Extinction * Dt;
		}
		const FVector3 NumericT = FAtmosphereMath::Exp(-Depth);
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			E_EXPECT_NEAR(Closed.Transmittance[Channel], NumericT[Channel], 2.0e-3f);
			// 산란/소멸 비율이 고도에 따라 조금 바뀌는 근사라 상대 6% 이내
			E_EXPECT_NEAR(Closed.Inscatter[Channel], Inscatter[Channel], Inscatter[Channel] * 0.06f + 1.0e-5f);
		}
	}
	// 거리 0이면 아무것도 없다
	const FAtmosphereMath::FAerialResult Zero = FAtmosphereMath::ComputeAerialPerspective(Params, Camera, Camera);
	E_EXPECT_NEAR(Zero.Transmittance.Y, 1.0f, 1.0e-6f);
	E_EXPECT_NEAR(Zero.Inscatter.Z, 0.0f, 1.0e-9f);
	E_EXPECT_NEAR(FAtmosphereMath::ComputeAerialTransmittanceScalar(FVector3::OneVector), 1.0f, 1.0e-6f);
}

E_TEST(Atmosphere_WorldToAtmosphereUnits)
{
	const FAtmosphereMath::FParams Params = EarthParams();
	const FVector3 P = FAtmosphereMath::WorldToAtmosphere(Params, FVector3(100000.0f, 0.0f, 50000.0f)); // 1km, 0.5km
	E_EXPECT_NEAR(P.X, 1.0f, 1.0e-5f);
	E_EXPECT_NEAR(P.Z, Params.BottomRadius + 0.5f, 1.0e-3f);
	// 지면 아래 카메라는 표면 바로 위로 올린다
	const FVector3 Below = FAtmosphereMath::GetCameraAtmospherePosition(Params, FVector3(0.0f, 0.0f, -5000.0f));
	E_EXPECT_TRUE(Below.Length() > Params.BottomRadius);
}

E_TEST(Sun_AnglesDirectionAndLightRotation)
{
	const float Cases[][2] = { { 30.0f, 0.0f }, { 5.0f, 90.0f }, { 60.0f, -135.0f }, { -10.0f, 200.0f } };
	for (const auto& Case : Cases)
	{
		const FVector3 Sun = FSunMath::SunDirectionFromAngles(Case[0], Case[1]);
		E_EXPECT_NEAR(Sun.Length(), 1.0f, 1.0e-5f);
		E_EXPECT_NEAR(Sun.Z, std::sin(FMath::DegreesToRadians(Case[0])), 1.0e-5f);
		float Elevation = 0.0f;
		float Azimuth   = 0.0f;
		FSunMath::SunAnglesFromDirection(Sun, Elevation, Azimuth);
		E_EXPECT_NEAR(Elevation, Case[0], 1.0e-3f);
		const FVector3 Again = FSunMath::SunDirectionFromAngles(Elevation, Azimuth);
		E_EXPECT_NEAR(FVector3::Dot(Again, Sun), 1.0f, 1.0e-5f);
		// 방향광 전방(빛 진행) = -태양 방향
		const FVector3 Forward = FSunMath::SunAnglesToLightRotation(Case[0], Case[1]).GetForwardVector();
		E_EXPECT_NEAR(FVector3::Dot(Forward, -Sun), 1.0f, 1.0e-4f);
	}
	// 방위 90 = +Y (오른쪽)
	const FVector3 East = FSunMath::SunDirectionFromAngles(0.0f, 90.0f);
	E_EXPECT_NEAR(East.Y, 1.0f, 1.0e-5f);
}

E_TEST(Sun_TimeOfDayAngles)
{
	float Elevation = 0.0f;
	float Azimuth   = 0.0f;
	FSunMath::TimeOfDayToSunAngles(6.0f, 60.0f, 0.0f, Elevation, Azimuth);
	E_EXPECT_NEAR(Elevation, 0.0f, 1.0e-4f);
	E_EXPECT_NEAR(Azimuth, 90.0f, 1.0e-3f); // 동 = 북 + 90
	FSunMath::TimeOfDayToSunAngles(12.0f, 60.0f, 0.0f, Elevation, Azimuth);
	E_EXPECT_NEAR(Elevation, 60.0f, 1.0e-3f);
	E_EXPECT_NEAR(Azimuth, 180.0f, 1.0e-3f);
	FSunMath::TimeOfDayToSunAngles(18.0f, 60.0f, 0.0f, Elevation, Azimuth);
	E_EXPECT_NEAR(Elevation, 0.0f, 1.0e-3f);
	E_EXPECT_NEAR(Azimuth, 270.0f, 1.0e-3f);
	FSunMath::TimeOfDayToSunAngles(0.0f, 60.0f, 0.0f, Elevation, Azimuth);
	E_EXPECT_NEAR(Elevation, -60.0f, 1.0e-3f);
	FSunMath::TimeOfDayToSunAngles(-6.0f, 60.0f, 0.0f, Elevation, Azimuth); // 18시와 같음 (감기)
	E_EXPECT_NEAR(Elevation, 0.0f, 1.0e-3f);
	E_EXPECT_NEAR(Azimuth, 270.0f, 1.0e-3f);
}
