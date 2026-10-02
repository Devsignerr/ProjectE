#pragma once

#include "Core/Math/Math.h"
#include "Core/Math/Units.h"

#include <cmath>

// 물리 기반 대기 (Phase 49, Hillaire 2020 "A Scalable and Production Ready Sky and Atmosphere Rendering Technique") — CPU 참조 식.
// 셰이더 Atmosphere.hlsli(LUT·적분)·AerialPerspective.hlsli(공중 원근)와 같은 식이다 (테스트 AtmosphereTests). 바꾸면 둘 다 고친다.
//
// 단위: 대기 안은 km, 산란/흡수 계수 1/km. 엔진 월드(cm)와는 WorldToAtmosphere/아래 상수로만 바꾼다 (FUnits 경계 규칙).
// 좌표: 행성 중심이 원점, 엔진과 같은 Z-up. 월드 원점(Z = 0)이 행성 표면(바닥 반지름) 바로 위다.
//   월드 (x, y, z) cm → 대기 (x, y, z) × CentimetersToKilometers + (0, 0, 바닥 반지름)
// 태양: SunDirection = 태양을 향하는 방향(빛 진행 방향의 반대). 각도 ↔ 방향·시간대 식은 Scene/SkyAtmosphere.h FSunMath
//
// LUT 구성 (FSkyAtmosphereRenderer):
//   투과율 256x64  — (고도, 천정 코사인) → 대기 끝까지 투과율 (Bruneton 매개화, TransmittanceUvToParams)
//   다중 산란 32x32 — (태양 천정 코사인, 고도) → Ψms (2차 산란 / (1 - f_ms), 등방 위상)
//   하늘 뷰 192x108 — 카메라 고도에서 (시선 천정각(지평선 근처 촘촘), 태양 기준 방위) → 휘도 (태양 조도 1 기준이 아니라 실제 조도 곱)
// 공중 원근: 장면 크기(수 km 이하)에서는 평평한 지면 + 지수 밀도 + 카메라 고도의 태양 투과율 상수로 닫힌 식 적분
//   (ComputeAerialPerspective) — 불투명(안개 적용 패스)·반투명 메시·파티클·물·구름이 모두 Fog.hlsli의 같은 식을 쓴다.
//   투과율은 셰이더 합성 경로(스칼라 알파)에 맞춰 휘도 가중 평균 하나로 줄인다 (ComputeAerialTransmittanceScalar)
namespace FAtmosphereMath
{
	constexpr float CentimetersToKilometers = FUnits::UnitsToMeters * 0.001f; // 1e-5
	constexpr float KilometersToCentimeters = 1.0f / CentimetersToKilometers;
	constexpr float PlanetRadiusOffset      = 0.01f; // km — 지면 그림자 판정에서 표면 바로 위로 띄움 (Hillaire)

	constexpr uint32 TransmittanceLutWidth  = 256;
	constexpr uint32 TransmittanceLutHeight = 64;
	constexpr uint32 MultiScatteringLutSize = 32;
	constexpr uint32 SkyViewLutWidth        = 192;
	constexpr uint32 SkyViewLutHeight       = 108;
	constexpr uint32 MultiScatteringDirections = 64; // 8 x 8 균등 구면 방향
	constexpr uint32 MultiScatteringSteps      = 20;
	constexpr uint32 TransmittanceSteps        = 40;

	// 대기 매질 설정 (km, 1/km). 기본값 = 지구 (Hillaire 논문 표 1)
	struct FParams
	{
		float    BottomRadius        = 6360.0f;
		float    TopRadius           = 6460.0f;
		FVector3 RayleighScattering  = FVector3(0.005802f, 0.013558f, 0.033100f);
		float    RayleighScaleHeight = 8.0f;
		FVector3 MieScattering       = FVector3(0.003996f, 0.003996f, 0.003996f);
		FVector3 MieAbsorption       = FVector3(0.000444f, 0.000444f, 0.000444f);
		float    MieScaleHeight      = 1.2f;
		float    MieAnisotropy       = 0.8f;
		FVector3 OzoneAbsorption     = FVector3(0.000650f, 0.001881f, 0.000085f);
		float    OzoneCenterHeight   = 25.0f; // km — 천막 모양 분포 중심
		float    OzoneHalfWidth      = 15.0f; // km — 중심에서 0이 되는 거리
		FVector3 GroundAlbedo        = FVector3(0.3f, 0.3f, 0.3f);

		FVector3 GetMieExtinction() const { return MieScattering + MieAbsorption; }
	};

	struct FMedium
	{
		FVector3 Scattering;     // 레일리 + 미
		FVector3 Extinction;     // 레일리 + 미(산란+흡수) + 오존 흡수
		FVector3 RayleighScattering;
		FVector3 MieScattering;
	};

	inline FVector3 Exp(const FVector3& V) { return FVector3(std::exp(V.X), std::exp(V.Y), std::exp(V.Z)); }

	// 고도 h(km, 표면 기준)의 매질 계수
	inline FMedium SampleMedium(const FParams& Params, float Height)
	{
		const float RayleighDensity = std::exp(-FMath::Max(Height, 0.0f) / Params.RayleighScaleHeight);
		const float MieDensity      = std::exp(-FMath::Max(Height, 0.0f) / Params.MieScaleHeight);
		const float OzoneDensity    = FMath::Max(0.0f, 1.0f - FMath::Abs(Height - Params.OzoneCenterHeight) / Params.OzoneHalfWidth);
		FMedium     Medium;
		Medium.RayleighScattering = Params.RayleighScattering * RayleighDensity;
		Medium.MieScattering      = Params.MieScattering * MieDensity;
		Medium.Scattering         = Medium.RayleighScattering + Medium.MieScattering;
		Medium.Extinction         = Medium.RayleighScattering + Params.GetMieExtinction() * MieDensity + Params.OzoneAbsorption * OzoneDensity;
		return Medium;
	}

	// 광선 - 구 (원점 중심) 가장 가까운 양의 교차 거리, 없으면 -1
	inline float RaySphereIntersectNearest(const FVector3& Origin, const FVector3& Direction, float Radius)
	{
		const float B = FVector3::Dot(Origin, Direction);
		const float C = FVector3::Dot(Origin, Origin) - Radius * Radius;
		const float Discriminant = B * B - C;
		if (Discriminant < 0.0f)
		{
			return -1.0f;
		}
		const float Root = std::sqrt(Discriminant);
		const float T0   = -B - Root;
		const float T1   = -B + Root;
		if (T0 < 0.0f && T1 < 0.0f)
		{
			return -1.0f;
		}
		if (T0 < 0.0f)
		{
			return FMath::Max(0.0f, T1);
		}
		return FMath::Max(0.0f, T0);
	}

	// 위상 함수
	inline float RayleighPhase(float CosTheta)
	{
		return 3.0f / (16.0f * FMath::Pi) * (1.0f + CosTheta * CosTheta);
	}
	// 코넷-섕크스 (Hillaire 셰이더와 같은 미 위상)
	inline float MiePhase(float G, float CosTheta)
	{
		const float K       = 3.0f / (8.0f * FMath::Pi) * (1.0f - G * G) / (2.0f + G * G);
		const float Denom   = FMath::Max(1.0f + G * G - 2.0f * G * CosTheta, 1.0e-6f);
		return K * (1.0f + CosTheta * CosTheta) / (Denom * std::sqrt(Denom));
	}

	// ---- 투과율 LUT 매개화 (Bruneton). UV = (x_mu, x_r) — 텍셀 중심은 (i + 0.5) / 크기
	inline void TransmittanceUvToParams(const FParams& Params, float U, float V, float& OutViewHeight, float& OutViewZenithCos)
	{
		const float H        = std::sqrt(FMath::Max(Params.TopRadius * Params.TopRadius - Params.BottomRadius * Params.BottomRadius, 0.0f));
		const float Rho      = H * V;
		OutViewHeight        = std::sqrt(Rho * Rho + Params.BottomRadius * Params.BottomRadius);
		const float DMin     = Params.TopRadius - OutViewHeight;
		const float DMax     = Rho + H;
		const float D        = DMin + U * (DMax - DMin);
		OutViewZenithCos     = D == 0.0f ? 1.0f : (H * H - Rho * Rho - D * D) / (2.0f * OutViewHeight * D);
		OutViewZenithCos     = FMath::Clamp(OutViewZenithCos, -1.0f, 1.0f);
	}
	inline void TransmittanceParamsToUv(const FParams& Params, float ViewHeight, float ViewZenithCos, float& OutU, float& OutV)
	{
		const float H            = std::sqrt(FMath::Max(Params.TopRadius * Params.TopRadius - Params.BottomRadius * Params.BottomRadius, 0.0f));
		const float Rho          = std::sqrt(FMath::Max(ViewHeight * ViewHeight - Params.BottomRadius * Params.BottomRadius, 0.0f));
		const float Discriminant = ViewHeight * ViewHeight * (ViewZenithCos * ViewZenithCos - 1.0f) + Params.TopRadius * Params.TopRadius;
		const float D            = FMath::Max(0.0f, -ViewHeight * ViewZenithCos + std::sqrt(FMath::Max(Discriminant, 0.0f)));
		const float DMin         = Params.TopRadius - ViewHeight;
		const float DMax         = Rho + H;
		OutU                     = (D - DMin) / FMath::Max(DMax - DMin, 1.0e-6f);
		OutV                     = Rho / FMath::Max(H, 1.0e-6f);
	}

	// 고도 ViewHeight(행성 중심 거리, km)에서 천정 코사인 방향으로 대기 끝까지 광학 깊이 → 투과율 (수치 적분, LUT 한 칸과 같은 식).
	// 지면에 막히는 방향이면 지면까지 (LUT도 같다 — 쓰는 쪽이 지면 그림자를 따로 판정)
	inline FVector3 ComputeTransmittanceToTop(const FParams& Params, float ViewHeight, float ViewZenithCos, uint32 Steps = TransmittanceSteps)
	{
		const FVector3 Origin(0.0f, 0.0f, ViewHeight);
		const FVector3 Direction(std::sqrt(FMath::Max(1.0f - ViewZenithCos * ViewZenithCos, 0.0f)), 0.0f, ViewZenithCos);
		const float    TBottom = RaySphereIntersectNearest(Origin, Direction, Params.BottomRadius);
		const float    TTop    = RaySphereIntersectNearest(Origin, Direction, Params.TopRadius);
		float          TMax    = TTop;
		if (TBottom > 0.0f && (TTop < 0.0f || TBottom < TTop))
		{
			TMax = TBottom;
		}
		if (TMax <= 0.0f)
		{
			return FVector3::OneVector;
		}
		FVector3    OpticalDepth = FVector3::ZeroVector;
		const float Dt           = TMax / static_cast<float>(Steps);
		for (uint32 Step = 0; Step < Steps; ++Step)
		{
			const FVector3 P = Origin + Direction * ((static_cast<float>(Step) + 0.5f) * Dt);
			OpticalDepth += SampleMedium(Params, P.Length() - Params.BottomRadius).Extinction * Dt;
		}
		return Exp(-OpticalDepth);
	}

	// ---- 다중 산란 LUT 매개화: U = 태양 천정 코사인 * 0.5 + 0.5, V = 고도 / 대기 두께 (서브 UV 보정은 셰이더 샘플 쪽)
	inline float FromUnitToSubUv(float U, float Resolution) { return (U + 0.5f / Resolution) * (Resolution / (Resolution + 1.0f)); }
	inline float FromSubUvToUnit(float U, float Resolution) { return (U - 0.5f / Resolution) * (Resolution / (Resolution - 1.0f)); }

	// 8x8 균등 구면 방향 (다중 산란 적분 방향 Index, 0..63) — 셰이더 MultiScatteringLutCS와 같은 순서
	inline FVector3 GetMultiScatteringDirection(uint32 Index)
	{
		const float I         = (static_cast<float>(Index % 8) + 0.5f) / 8.0f;
		const float J         = (static_cast<float>(Index / 8) + 0.5f) / 8.0f;
		const float Theta     = 2.0f * FMath::Pi * I;
		const float CosPhi    = 1.0f - 2.0f * J;
		const float SinPhi    = std::sqrt(FMath::Max(1.0f - CosPhi * CosPhi, 0.0f));
		return FVector3(std::cos(Theta) * SinPhi, std::sin(Theta) * SinPhi, CosPhi);
	}

	// 다중 산란 Ψms (고도 h km 표면 기준, 태양 천정 코사인). 태양 조도 1, 등방 위상. 셰이더는 투과율을 LUT에서 읽고 CPU는 수치 적분
	inline FVector3 ComputeMultipleScattering(const FParams& Params, float Height, float SunZenithCos)
	{
		const FVector3 Origin(0.0f, 0.0f, Params.BottomRadius + FMath::Clamp(Height, 0.0f, Params.TopRadius - Params.BottomRadius));
		const FVector3 SunDirection(std::sqrt(FMath::Max(1.0f - SunZenithCos * SunZenithCos, 0.0f)), 0.0f, SunZenithCos);
		const float    IsotropicPhase = 1.0f / (4.0f * FMath::Pi);
		FVector3       SecondOrder    = FVector3::ZeroVector;
		FVector3       MultiAsOne     = FVector3::ZeroVector;
		for (uint32 DirIndex = 0; DirIndex < MultiScatteringDirections; ++DirIndex)
		{
			const FVector3 Direction = GetMultiScatteringDirection(DirIndex);
			const float    TBottom   = RaySphereIntersectNearest(Origin, Direction, Params.BottomRadius);
			const float    TTop      = RaySphereIntersectNearest(Origin, Direction, Params.TopRadius);
			float          TMax      = TTop;
			if (TBottom > 0.0f && (TTop < 0.0f || TBottom < TTop))
			{
				TMax = TBottom;
			}
			if (TMax <= 0.0f)
			{
				continue;
			}
			const float Dt          = TMax / static_cast<float>(MultiScatteringSteps);
			FVector3    Throughput  = FVector3::OneVector;
			FVector3    Luminance   = FVector3::ZeroVector;
			FVector3    ScatterOnce = FVector3::ZeroVector;
			for (uint32 Step = 0; Step < MultiScatteringSteps; ++Step)
			{
				const FVector3 P      = Origin + Direction * ((static_cast<float>(Step) + 0.3f) * Dt);
				const float    Radius = P.Length();
				const FMedium  Medium = SampleMedium(Params, Radius - Params.BottomRadius);
				const FVector3 Up     = P / Radius;
				const float    SunCos = FVector3::Dot(SunDirection, Up);
				const FVector3 SunTransmittance = ComputeTransmittanceToTop(Params, Radius, SunCos);
				const bool     bShadow = RaySphereIntersectNearest(P + Up * PlanetRadiusOffset, SunDirection, Params.BottomRadius) >= 0.0f;
				const FVector3 SampleTransmittance = Exp(-(Medium.Extinction * Dt));
				const FVector3 SafeExtinction(FMath::Max(Medium.Extinction.X, 1.0e-9f), FMath::Max(Medium.Extinction.Y, 1.0e-9f),
				                              FMath::Max(Medium.Extinction.Z, 1.0e-9f));
				// 구간 해석 적분: ∫ T dt 계수 = (1 - 구간 투과율) / 소멸
				const FVector3 Integral = (FVector3::OneVector - SampleTransmittance) / SafeExtinction;
				const FVector3 S        = bShadow ? FVector3::ZeroVector : Medium.Scattering * SunTransmittance * IsotropicPhase;
				Luminance += Throughput * S * Integral;
				ScatterOnce += Throughput * Medium.Scattering * Integral;
				Throughput = Throughput * SampleTransmittance;
			}
			// 지면 반사 (램버트)
			if (TBottom > 0.0f && TMax == TBottom)
			{
				const FVector3 P      = Origin + Direction * TBottom;
				const FVector3 Up     = P.GetNormalized();
				const float    SunCos = FVector3::Dot(SunDirection, Up);
				const FVector3 SunTransmittance = ComputeTransmittanceToTop(Params, P.Length(), SunCos);
				Luminance += Throughput * SunTransmittance * Params.GroundAlbedo * (FMath::Max(SunCos, 0.0f) / FMath::Pi);
			}
			SecondOrder += Luminance;
			MultiAsOne += ScatterOnce;
		}
		const float Inverse = 1.0f / static_cast<float>(MultiScatteringDirections);
		SecondOrder *= Inverse;
		MultiAsOne *= Inverse * IsotropicPhase * 4.0f * FMath::Pi; // 등방 위상 × 4π = 1 (구면 평균)
		const FVector3 Sum(1.0f / FMath::Max(1.0f - MultiAsOne.X, 1.0e-3f), 1.0f / FMath::Max(1.0f - MultiAsOne.Y, 1.0e-3f),
		                   1.0f / FMath::Max(1.0f - MultiAsOne.Z, 1.0e-3f));
		return SecondOrder * Sum;
	}

	// ---- 하늘 뷰 LUT 매개화 (Hillaire): V = 시선 천정각 (지평선 근처 제곱 분포), U = 태양 기준 방위 코사인 (제곱)
	inline void SkyViewUvToParams(const FParams& Params, float ViewHeight, float U, float V, float& OutViewZenithCos, float& OutLightViewCos)
	{
		const float VHorizon           = std::sqrt(FMath::Max(ViewHeight * ViewHeight - Params.BottomRadius * Params.BottomRadius, 0.0f));
		const float CosBeta            = VHorizon / ViewHeight;
		const float Beta               = std::acos(FMath::Clamp(CosBeta, -1.0f, 1.0f));
		const float ZenithHorizonAngle = FMath::Pi - Beta;
		float       ViewZenithAngle;
		if (V < 0.5f)
		{
			float Coord = 2.0f * V;
			Coord       = 1.0f - Coord;
			Coord *= Coord;
			Coord           = 1.0f - Coord;
			ViewZenithAngle = ZenithHorizonAngle * Coord;
		}
		else
		{
			float Coord = V * 2.0f - 1.0f;
			Coord *= Coord;
			ViewZenithAngle = ZenithHorizonAngle + Beta * Coord;
		}
		OutViewZenithCos = std::cos(ViewZenithAngle);
		const float Coord = U * U;
		OutLightViewCos   = -(Coord * 2.0f - 1.0f);
	}
	inline void SkyViewParamsToUv(const FParams& Params, float ViewHeight, float ViewZenithCos, float LightViewCos, bool bIntersectGround, float& OutU,
	                              float& OutV)
	{
		const float VHorizon           = std::sqrt(FMath::Max(ViewHeight * ViewHeight - Params.BottomRadius * Params.BottomRadius, 0.0f));
		const float CosBeta            = VHorizon / ViewHeight;
		const float Beta               = std::acos(FMath::Clamp(CosBeta, -1.0f, 1.0f));
		const float ZenithHorizonAngle = FMath::Pi - Beta;
		if (!bIntersectGround)
		{
			float Coord = std::acos(FMath::Clamp(ViewZenithCos, -1.0f, 1.0f)) / ZenithHorizonAngle;
			Coord       = 1.0f - Coord;
			Coord       = std::sqrt(FMath::Max(Coord, 0.0f));
			Coord       = 1.0f - Coord;
			OutV        = Coord * 0.5f;
		}
		else
		{
			float Coord = (std::acos(FMath::Clamp(ViewZenithCos, -1.0f, 1.0f)) - ZenithHorizonAngle) / FMath::Max(Beta, 1.0e-6f);
			Coord       = std::sqrt(FMath::Max(Coord, 0.0f));
			OutV        = Coord * 0.5f + 0.5f;
		}
		OutU = std::sqrt(FMath::Max(-LightViewCos * 0.5f + 0.5f, 0.0f));
	}

	// ---- 월드 ↔ 대기 좌표
	inline FVector3 WorldToAtmosphere(const FParams& Params, const FVector3& WorldCentimeters)
	{
		FVector3 P = WorldCentimeters * CentimetersToKilometers;
		P.Z += Params.BottomRadius;
		return P;
	}
	// 카메라 위치 (지면 아래·표면에 붙는 것을 막아 최소 고도로 올린다)
	inline FVector3 GetCameraAtmospherePosition(const FParams& Params, const FVector3& WorldCentimeters)
	{
		FVector3    P      = WorldToAtmosphere(Params, WorldCentimeters);
		const float Height = P.Length();
		const float MinRadius = Params.BottomRadius + 0.001f;
		if (Height < MinRadius)
		{
			P = Height > 1.0e-3f ? P * (MinRadius / Height) : FVector3(0.0f, 0.0f, MinRadius);
		}
		return P;
	}

	// ---- 공중 원근 (평평한 지면 지수 대기, 단일 산란 + 다중 산란 근사): 셰이더 AerialPerspective.hlsli EvaluateAerialPerspective와 같은 식
	struct FAerialParams
	{
		FVector3 RayleighScattering; // 1/cm (지면 밀도)
		float    RayleighScaleHeight = 800000.0f; // cm
		FVector3 MieScattering;      // 1/cm
		float    MieScaleHeight = 120000.0f;      // cm
		FVector3 MieExtinction;      // 1/cm
		float    MieAnisotropy = 0.8f;
		FVector3 SunIlluminance;     // 카메라 고도 투과율까지 곱한 태양 조도 (× 하늘 밝기 배율)
		FVector3 SunDirection = FVector3(0.0f, 0.0f, 1.0f); // 태양 쪽
		FVector3 MultiScattering;    // Ψms × 태양 조도 (등방, 산란 계수에 곱해짐)
		float    GroundHeight  = 0.0f; // cm (행성 표면 월드 Z)
		float    DistanceScale = 1.0f;
	};

	// 높이 지수 밀도 exp(-(z - 지면) / H)의 광선 [0, Distance] 광학 길이 (Fog.hlsli FogHeightOpticalDepth와 같은 닫힌 식)
	inline float ExponentialOpticalLength(float OriginZ, float DirZ, float Distance, float GroundHeight, float ScaleHeight)
	{
		if (Distance <= 0.0f)
		{
			return 0.0f;
		}
		const float Falloff  = 1.0f / ScaleHeight;
		const float AtStart  = std::exp(-Falloff * FMath::Max(OriginZ - GroundHeight, 0.0f));
		const float Exponent = Falloff * DirZ * Distance;
		if (FMath::Abs(Exponent) < 1.0e-4f)
		{
			return AtStart * Distance * (1.0f - 0.5f * Exponent);
		}
		return AtStart * (1.0f - std::exp(-Exponent)) / (Falloff * DirZ);
	}

	struct FAerialResult
	{
		FVector3 Inscatter;     // 더할 빛
		FVector3 Transmittance; // 채널별 투과율
	};
	inline FAerialResult ComputeAerialPerspective(const FAerialParams& Params, const FVector3& CameraPosition, const FVector3& WorldPosition)
	{
		const FVector3 ToPoint  = WorldPosition - CameraPosition;
		const float    Distance = ToPoint.Length();
		FAerialResult  Result{ FVector3::ZeroVector, FVector3::OneVector };
		if (Distance <= 1.0e-3f)
		{
			return Result;
		}
		const FVector3 Dir      = ToPoint / Distance;
		const float    Scaled   = Distance * FMath::Max(Params.DistanceScale, 0.0f);
		const float    RayleighLength = ExponentialOpticalLength(CameraPosition.Z, Dir.Z, Scaled, Params.GroundHeight, Params.RayleighScaleHeight);
		const float    MieLength      = ExponentialOpticalLength(CameraPosition.Z, Dir.Z, Scaled, Params.GroundHeight, Params.MieScaleHeight);
		const FVector3 RayleighDepth  = Params.RayleighScattering * RayleighLength;
		const FVector3 MieScatterDepth = Params.MieScattering * MieLength;
		const FVector3 OpticalDepth   = RayleighDepth + Params.MieExtinction * MieLength;
		Result.Transmittance          = Exp(-OpticalDepth);
		const float    CosTheta       = FVector3::Dot(Dir, Params.SunDirection);
		const FVector3 Single         = (RayleighDepth * RayleighPhase(CosTheta) + MieScatterDepth * MiePhase(Params.MieAnisotropy, CosTheta)) * Params.SunIlluminance;
		const FVector3 Multi          = (RayleighDepth + MieScatterDepth) * Params.MultiScattering;
		// 산란 깊이 / 소멸 깊이 비율로 (1 - T)를 나눠 준다 (단일 성분이면 정확한 적분)
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			const float Tau = OpticalDepth[Channel];
			const float Factor = Tau > 1.0e-6f ? (1.0f - Result.Transmittance[Channel]) / Tau : 1.0f;
			Result.Inscatter[Channel] = (Single[Channel] + Multi[Channel]) * Factor;
		}
		return Result;
	}
	// 합성 경로(스칼라 알파)용 투과율: 휘도 가중 평균
	inline float ComputeAerialTransmittanceScalar(const FVector3& Transmittance)
	{
		return Transmittance.X * 0.2126f + Transmittance.Y * 0.7152f + Transmittance.Z * 0.0722f;
	}
} // namespace FAtmosphereMath
