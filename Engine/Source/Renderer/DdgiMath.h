#pragma once

#include "Core/Math/Math.h"

#include <algorithm>
#include <cmath>

// 동적 GI — DDGI 프로브 볼륨 (Phase 51)의 CPU 기준 식. GPU는 DdgiCommon.hlsli(적용·공용), DdgiTrace.hlsl(광선), DdgiBlend.hlsl(누적)이
// 같은 식을 쓴다 — 함께 고치고 Ddgi_* 테스트도 고친다.
//
// 프로브 표현 (Majercik 2019, RTXGI 방식): 프로브마다 팔면체 아틀라스 타일 두 장
//   조도 = 8x8 칸 + 테두리 1칸(10x10), RGBA16F, 값 = 조도 E / π (하늘 IBL 확산 맵 IblDiffuse와 같은 규약: × 알베도 = 확산 반사광)
//   거리 = 16x16 칸 + 테두리(18x18), RG16F, 값 = (평균 거리, 제곱 평균) / DistanceClamp (정규화 — 반정밀도 제곱 넘침 방지)
//   프로브 상태 = 1텍셀(RGBA32F): xyz 재배치 오프셋(cm), w 상태 (0 아직 없음, 1 활성, 2 비활성 = 벽 속)
//   테두리 칸은 팔면체를 접어 이웃이 되는 안쪽 칸 값을 복사한다 (MapBorderTexel) → 쌍선형 표본이 접힌 이음매에서도 맞다
// 아틀라스 배치: 전체 프로브 번호 g → 타일 (g % TilesPerRow, g / TilesPerRow). 볼륨은 ProbeOffset부터 연속 번호
// 격자: 상자 [Min, Max]를 축마다 Count칸으로 나눈 칸 가운데에 프로브 (간격 = 2·반 크기 / Count, 첫 프로브 = Min + 간격/2), 볼륨 안 번호 = x + Cx·(y + Cy·z).
//   셰이딩·페이드 상자 = 컴포넌트 상자 그대로 (= 격자 바깥 프로브에서 반 칸씩 더). 상자 끝 반 칸은 삼선형이 경계 프로브로 고정
// 광선: 처음 FixedRayCount개는 회전 없는 구면 피보나치(재배치·분류 전용, 누적 제외), 나머지는 프레임 회전을 곱한 피보나치
// 적용(셰이딩 점 P, 법선 N, 시선 V): 편향 점 P' = P + N·NormalBias + V·ViewBias → 감싸는 8개 프로브 × (삼선형 × 감싸기 × 체비셰프 가시성³)
//   → 작은 가중치는 더 줄이고(0.2 아래 세제곱) 합으로 나눈다. 비활성/아직 없는 프로브는 뺀다
namespace DdgiMath
{
	constexpr uint32 IrradianceTexels   = 8;
	constexpr uint32 IrradianceTileSize = IrradianceTexels + 2;
	constexpr uint32 DistanceTexels     = 16;
	constexpr uint32 DistanceTileSize   = DistanceTexels + 2;
	constexpr uint32 AtlasTilesPerRow   = 64;
	constexpr uint32 MaxVolumes         = 8;     // 프레임에 셰이더가 보는 볼륨 수 (상수 버퍼 배열)
	constexpr uint32 MaxAxisProbes      = 32;
	constexpr uint32 MaxProbesPerVolume = 8192;
	constexpr uint32 MaxTotalProbes     = 16384; // 아틀라스 행 256개 (거리 아틀라스 1152 x 4608)
	constexpr uint32 FixedRayCount      = 32;    // 재배치·분류용 고정 방향 광선 (RTXGI_DDGI_NUM_FIXED_RAYS)
	constexpr uint32 MinRaysPerProbe    = 64;
	constexpr uint32 MaxRaysPerProbe    = 512;
	constexpr float  BackfaceDistanceScale = 0.2f;  // 뒷면 히트 거리는 줄여 음수로 저장 (재배치·누수 방지)
	constexpr float  DistanceExponent      = 50.0f; // 거리 누적 가중치 지수 (cos^50)
	constexpr float  DistanceMinCosine     = 0.85f; // 이보다 비스듬한 광선은 거리 누적에서 뺀다 (0.85^50 ≈ 3e-4)
	constexpr float  DistanceClampScale    = 1.5f;  // 거리 정규화 = 1.5 × |간격|
	constexpr float  MaxRelocation         = 0.45f; // 재배치 오프셋 상한 (간격 비율)
	constexpr float  WeightCrushThreshold  = 0.2f;
	constexpr float  MinVisibility         = 0.05f;

	enum EProbeState : uint32
	{
		ProbeState_Uninitialized = 0,
		ProbeState_Active        = 1,
		ProbeState_Inactive      = 2,
	};

	// ---- 팔면체 (단위 방향 ↔ [-1, 1]²)
	inline FVector2 OctEncode(const FVector3& D)
	{
		const float L1 = FMath::Abs(D.X) + FMath::Abs(D.Y) + FMath::Abs(D.Z);
		FVector2    P(D.X / L1, D.Y / L1);
		if (D.Z < 0.0f)
		{
			const float X = (1.0f - FMath::Abs(P.Y)) * (P.X >= 0.0f ? 1.0f : -1.0f);
			const float Y = (1.0f - FMath::Abs(P.X)) * (P.Y >= 0.0f ? 1.0f : -1.0f);
			P             = FVector2(X, Y);
		}
		return P;
	}

	inline FVector3 OctDecode(const FVector2& P)
	{
		FVector3    D(P.X, P.Y, 1.0f - FMath::Abs(P.X) - FMath::Abs(P.Y));
		const float T = FMath::Max(-D.Z, 0.0f);
		D.X += D.X >= 0.0f ? -T : T;
		D.Y += D.Y >= 0.0f ? -T : T;
		return D.GetNormalized();
	}

	// 타일 안쪽 칸 (X, Y) ∈ [0, Texels) 중심의 방향
	inline FVector3 GetTexelDirection(uint32 X, uint32 Y, uint32 Texels)
	{
		const float Size = static_cast<float>(Texels);
		return OctDecode(FVector2((static_cast<float>(X) + 0.5f) / Size * 2.0f - 1.0f, (static_cast<float>(Y) + 0.5f) / Size * 2.0f - 1.0f));
	}

	// 방향 → 타일 안쪽 연속 좌표 [0, Texels] (칸 중심 = 정수 + 0.5)
	inline FVector2 DirectionToTexelCoord(const FVector3& D, uint32 Texels)
	{
		const FVector2 P    = OctEncode(D);
		const float    Size = static_cast<float>(Texels);
		return FVector2((P.X * 0.5f + 0.5f) * Size, (P.Y * 0.5f + 0.5f) * Size);
	}

	// 테두리 칸 → 같은 값을 가질 안쪽 칸 (타일 좌표, 테두리 포함 0..Texels+1, 안쪽 1..Texels). 안쪽 칸은 그대로.
	//   변: 반대 방향으로 뒤집은 이웃 줄 (팔면체를 변에서 접으면 (x, -1-e) ~ (-x, -1+e)), 꼭짓점: 대각 반대 꼭짓점 안쪽 칸
	inline void MapBorderTexel(uint32 X, uint32 Y, uint32 Texels, uint32& OutX, uint32& OutY)
	{
		const uint32 Last = Texels + 1;
		const bool   bLeft = X == 0, bRight = X == Last, bTop = Y == 0, bBottom = Y == Last;
		if ((bLeft || bRight) && (bTop || bBottom))
		{
			OutX = bLeft ? Texels : 1u;
			OutY = bTop ? Texels : 1u;
			return;
		}
		if (bTop || bBottom)
		{
			OutX = Last - X;
			OutY = bTop ? 1u : Texels;
			return;
		}
		if (bLeft || bRight)
		{
			OutX = bLeft ? 1u : Texels;
			OutY = Last - Y;
			return;
		}
		OutX = X;
		OutY = Y;
	}

	// ---- 광선 방향
	// 구면 피보나치 (RTXGI SphericalFibonacci): 단위 구에 고르게 N개
	inline FVector3 SphericalFibonacci(uint32 Index, uint32 Count)
	{
		constexpr float Golden   = 0.618033988749895f; // (√5 + 1)/2 − 1
		const float     I        = static_cast<float>(Index);
		const float     Phi      = 2.0f * FMath::Pi * (I * Golden - std::floor(I * Golden));
		const float     CosTheta = 1.0f - (2.0f * I + 1.0f) / static_cast<float>(Count);
		const float     SinTheta = std::sqrt(std::clamp(1.0f - CosTheta * CosTheta, 0.0f, 1.0f));
		return FVector3(std::cos(Phi) * SinTheta, std::sin(Phi) * SinTheta, CosTheta);
	}

	// 3x3 회전 (행 3개, 방향 d → (Row0·d, Row1·d, Row2·d))
	struct FRotation
	{
		FVector3 Rows[3] = { FVector3(1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f), FVector3(0.0f, 0.0f, 1.0f) };
		FVector3 Rotate(const FVector3& D) const { return FVector3(FVector3::Dot(Rows[0], D), FVector3::Dot(Rows[1], D), FVector3::Dot(Rows[2], D)); }
	};

	// 정수 해시 → [0, 1) (PCG)
	inline float HashToUnit(uint32 Value)
	{
		uint32 State = Value * 747796405u + 2891336453u;
		uint32 Word  = ((State >> ((State >> 28u) + 4u)) ^ State) * 277803737u;
		Word         = (Word >> 22u) ^ Word;
		return static_cast<float>(Word >> 8) * (1.0f / 16777216.0f);
	}

	// 프레임 번호 → 균등 무작위 회전 (Shoemake 단위 쿼터니언, 결정적 — 같은 프레임 번호면 같은 회전)
	inline FRotation MakeRayRotation(uint32 Frame)
	{
		const float U1 = HashToUnit(Frame * 3u + 0u);
		const float U2 = HashToUnit(Frame * 3u + 1u);
		const float U3 = HashToUnit(Frame * 3u + 2u);
		const float A  = std::sqrt(1.0f - U1);
		const float B  = std::sqrt(U1);
		const float X  = A * std::sin(2.0f * FMath::Pi * U2);
		const float Y  = A * std::cos(2.0f * FMath::Pi * U2);
		const float Z  = B * std::sin(2.0f * FMath::Pi * U3);
		const float W  = B * std::cos(2.0f * FMath::Pi * U3);
		FRotation   R;
		R.Rows[0] = FVector3(1.0f - 2.0f * (Y * Y + Z * Z), 2.0f * (X * Y - Z * W), 2.0f * (X * Z + Y * W));
		R.Rows[1] = FVector3(2.0f * (X * Y + Z * W), 1.0f - 2.0f * (X * X + Z * Z), 2.0f * (Y * Z - X * W));
		R.Rows[2] = FVector3(2.0f * (X * Z - Y * W), 2.0f * (Y * Z + X * W), 1.0f - 2.0f * (X * X + Y * Y));
		return R;
	}

	// 광선 Index의 방향: 고정 광선(Index < FixedRays)은 회전 없는 FixedRays개 피보나치, 나머지는 (RayCount - FixedRays)개 피보나치 × 회전
	inline FVector3 GetRayDirection(uint32 Index, uint32 RayCount, uint32 FixedRays, const FRotation& Rotation)
	{
		if (Index < FixedRays)
		{
			return SphericalFibonacci(Index, FixedRays);
		}
		return Rotation.Rotate(SphericalFibonacci(Index - FixedRays, RayCount - FixedRays));
	}

	// ---- 격자
	struct FProbeGrid
	{
		FVector3 Origin;                    // 프로브 (0,0,0) 위치 = 상자 최소 꼭짓점 + 간격/2
		FVector3 Spacing   = FVector3(100.0f);
		uint32   Counts[3] = { 2, 2, 2 };

		uint32   GetProbeCount() const { return Counts[0] * Counts[1] * Counts[2]; }
		FVector3 GetMax() const
		{
			return Origin + FVector3(Spacing.X * static_cast<float>(Counts[0] - 1), Spacing.Y * static_cast<float>(Counts[1] - 1),
			                         Spacing.Z * static_cast<float>(Counts[2] - 1));
		}
		uint32 GetIndex(uint32 X, uint32 Y, uint32 Z) const { return X + Counts[0] * (Y + Counts[1] * Z); }
		void   GetCoords(uint32 Index, uint32 Out[3]) const
		{
			Out[0] = Index % Counts[0];
			Out[1] = (Index / Counts[0]) % Counts[1];
			Out[2] = Index / (Counts[0] * Counts[1]);
		}
		FVector3 GetPosition(uint32 Index) const
		{
			uint32 C[3];
			GetCoords(Index, C);
			return Origin + FVector3(Spacing.X * static_cast<float>(C[0]), Spacing.Y * static_cast<float>(C[1]), Spacing.Z * static_cast<float>(C[2]));
		}
		// 거리 정규화 (가시성 모멘트): 1.5 × 칸 대각선
		float GetDistanceClamp() const { return DistanceClampScale * Spacing.Length(); }
		float GetMinSpacing() const { return FMath::Min(Spacing.X, FMath::Min(Spacing.Y, Spacing.Z)); }
		// 셰이딩·페이드 상자 (= 컴포넌트 상자)
		FVector3 GetBoxMin() const { return Origin - Spacing * 0.5f; }
		FVector3 GetBoxMax() const { return GetMax() + Spacing * 0.5f; }
	};

	// 상자(중심, 반 크기) + 목표 간격 → 칸 가운데 격자. 축 수는 [2, MaxAxisProbes], 전체가 MaxProbes를 넘으면 간격을 늘린다
	inline FProbeGrid MakeGrid(const FVector3& Center, const FVector3& HalfExtents, float TargetSpacing, uint32 MaxProbes = MaxProbesPerVolume)
	{
		const FVector3 Half(FMath::Max(HalfExtents.X, 1.0f), FMath::Max(HalfExtents.Y, 1.0f), FMath::Max(HalfExtents.Z, 1.0f));
		float          Spacing = FMath::Max(TargetSpacing, 1.0f);
		FProbeGrid     Grid;
		for (;;)
		{
			const float Sizes[3] = { Half.X * 2.0f, Half.Y * 2.0f, Half.Z * 2.0f };
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				const float Count = std::round(Sizes[Axis] / Spacing);
				Grid.Counts[Axis] = static_cast<uint32>(std::clamp(Count, 2.0f, static_cast<float>(MaxAxisProbes)));
			}
			if (Grid.GetProbeCount() <= std::max(MaxProbes, 8u))
			{
				break;
			}
			Spacing *= 1.1f;
		}
		Grid.Spacing = FVector3(Half.X * 2.0f / static_cast<float>(Grid.Counts[0]), Half.Y * 2.0f / static_cast<float>(Grid.Counts[1]),
		                        Half.Z * 2.0f / static_cast<float>(Grid.Counts[2]));
		Grid.Origin  = Center - Half + Grid.Spacing * 0.5f;
		return Grid;
	}

	// 점 P를 감싸는 칸: 시작 프로브(축마다 [0, Count - 2])와 칸 안 비율 [0, 1]³ (격자 밖은 경계 칸으로 고정)
	inline void FindBaseProbe(const FProbeGrid& Grid, const FVector3& P, uint32 OutBase[3], FVector3& OutAlpha)
	{
		const float Coord[3] = { (P.X - Grid.Origin.X) / Grid.Spacing.X, (P.Y - Grid.Origin.Y) / Grid.Spacing.Y, (P.Z - Grid.Origin.Z) / Grid.Spacing.Z };
		float       Alpha[3] = {};
		for (uint32 Axis = 0; Axis < 3; ++Axis)
		{
			const float Base = std::clamp(std::floor(Coord[Axis]), 0.0f, static_cast<float>(Grid.Counts[Axis] - 2));
			OutBase[Axis]    = static_cast<uint32>(Base);
			Alpha[Axis]      = std::clamp(Coord[Axis] - Base, 0.0f, 1.0f);
		}
		OutAlpha = FVector3(Alpha[0], Alpha[1], Alpha[2]);
	}

	// 삼선형 모서리 c (비트 0 = x, 1 = y, 2 = z) 가중치
	inline float TrilinearWeight(const FVector3& Alpha, uint32 Corner)
	{
		const float X = (Corner & 1) ? Alpha.X : 1.0f - Alpha.X;
		const float Y = (Corner & 2) ? Alpha.Y : 1.0f - Alpha.Y;
		const float Z = (Corner & 4) ? Alpha.Z : 1.0f - Alpha.Z;
		return X * Y * Z;
	}

	// ---- 가시성/가중치
	// 체비셰프 상한 (정규화 거리): 평균보다 가까우면 1, 멀면 (σ² / (σ² + Δ²))³ — 세제곱은 누수를 더 세게 자른다 (RTXGI)
	inline float ComputeVisibility(float Distance, float Mean, float MeanSquared)
	{
		if (Distance <= Mean)
		{
			return 1.0f;
		}
		const float Variance = FMath::Abs(MeanSquared - Mean * Mean);
		const float Delta    = Distance - Mean;
		const float Bound    = Variance / FMath::Max(Variance + Delta * Delta, 1.0e-8f);
		return FMath::Max(Bound * Bound * Bound, 0.0f);
	}

	// 프로브 하나의 셰이딩 가중치 (삼선형 포함): 감싸기 셰이딩(뒤쪽 프로브 부드럽게) × max(가시성, 0.05) → 0.2 아래는 세제곱으로 더 줄인다 → × 삼선형
	//   DirectionToProbe = 편향 점 → 프로브 (정규화)
	inline float ComputeProbeWeight(const FVector3& DirectionToProbe, const FVector3& N, float Trilinear, float Visibility)
	{
		const float Wrap   = (FVector3::Dot(DirectionToProbe, N) + 1.0f) * 0.5f;
		float       Weight = Wrap * Wrap + 0.2f;
		Weight *= FMath::Max(Visibility, MinVisibility);
		Weight = FMath::Max(Weight, 1.0e-6f);
		if (Weight < WeightCrushThreshold)
		{
			Weight *= Weight * Weight / (WeightCrushThreshold * WeightCrushThreshold);
		}
		return Weight * Trilinear;
	}

	// 볼륨 경계 페이드: 격자 상자 안쪽으로 FadeDistance만큼 들어가면 1, 경계/밖은 0 (밖 = 하늘 IBL 조도)
	inline float ComputeEdgeFade(const FVector3& P, const FVector3& Min, const FVector3& Max, float FadeDistance)
	{
		const float Inside = FMath::Min(FMath::Min(FMath::Min(P.X - Min.X, Max.X - P.X), FMath::Min(P.Y - Min.Y, Max.Y - P.Y)),
		                                FMath::Min(P.Z - Min.Z, Max.Z - P.Z));
		return FMath::Clamp(Inside / FMath::Max(FadeDistance, 1.0e-3f), 0.0f, 1.0f);
	}

	// 프로브 상태 텍셀 w = 상태 + 4 × 갱신 횟수 (상한 MaxUpdateCount — float32에 정확)
	constexpr uint32 MaxUpdateCount = 1023;
	inline float  EncodeProbeState(uint32 State, uint32 UpdateCount) { return static_cast<float>(State + 4u * std::min(UpdateCount, MaxUpdateCount)); }
	inline uint32 GetProbeState(float W) { return static_cast<uint32>(W) & 3u; }
	inline uint32 GetProbeUpdateCount(float W) { return static_cast<uint32>(W) >> 2; }

	// ---- 누적 (히스테리시스)
	// 이번 누적의 이전 값 비중: 처음(아직 없음)이면 0, 처음 n번(UpdateCount = 지난 갱신 횟수)은 누적 평균 상한 n/(n+1) (수렴 가속),
	// 성분 최대 변화가 이전·새 값 크기의 ChangeThreshold배를 넘으면(급변 — 시간대·조명 변화) 0.75를 뺀 값(빠르게 따라감)
	// (ChangeThreshold ≥ 1이면 발동하지 않는다 — 기본. 광선 잡음에 걸려 프로브가 깜빡였다, 조명 변화는 ComputeLightChange 가속이 맡음)
	inline float ComputeHysteresis(const FVector3& Previous, const FVector3& Current, float Base, float ChangeThreshold, bool bFirst,
	                               uint32 UpdateCount = MaxUpdateCount)
	{
		if (bFirst)
		{
			return 0.0f;
		}
		Base = FMath::Min(Base, static_cast<float>(UpdateCount) / static_cast<float>(UpdateCount + 1u));
		const FVector3 Delta(FMath::Abs(Current.X - Previous.X), FMath::Abs(Current.Y - Previous.Y), FMath::Abs(Current.Z - Previous.Z));
		const float    MaxDelta = FMath::Max(Delta.X, FMath::Max(Delta.Y, Delta.Z));
		const float    Scale    = FMath::Max(FMath::Max(FMath::Max(Previous.X, Previous.Y), Previous.Z), FMath::Max(FMath::Max(Current.X, Current.Y), Current.Z));
		if (MaxDelta > ChangeThreshold * FMath::Max(Scale, 1.0e-3f))
		{
			return FMath::Max(Base - 0.75f, 0.0f);
		}
		return Base;
	}

	inline FVector3 Blend(const FVector3& Previous, const FVector3& Current, float Hysteresis)
	{
		return Current + (Previous - Current) * Hysteresis;
	}

	// 조명 변화 정도 (1 = 가속 기준): 방향광 방향 2도, 방향광 복사 휘도·하늘 배율 상대 10%. 방향광이 꺼져 있으면(복사 0) 방향은 보지 않는다
	inline float ComputeLightChange(const FVector3& DirectionA, const FVector3& RadianceA, float AmbientA, const FVector3& DirectionB,
	                                const FVector3& RadianceB, float AmbientB)
	{
		const float LumA = RadianceA.X * 0.2126f + RadianceA.Y * 0.7152f + RadianceA.Z * 0.0722f;
		const float LumB = RadianceB.X * 0.2126f + RadianceB.Y * 0.7152f + RadianceB.Z * 0.0722f;
		float       Change = 0.0f;
		if (LumA > 1.0e-4f && LumB > 1.0e-4f)
		{
			const float Cosine = std::clamp(FVector3::Dot(DirectionA.GetNormalized(), DirectionB.GetNormalized()), -1.0f, 1.0f);
			Change             = FMath::Max(Change, std::acos(Cosine) / FMath::DegreesToRadians(2.0f));
		}
		const float MaxLum = FMath::Max(LumA, LumB);
		if (MaxLum > 1.0e-4f)
		{
			Change = FMath::Max(Change, FMath::Abs(LumA - LumB) / MaxLum / 0.1f);
		}
		const float MaxAmbient = FMath::Max(FMath::Abs(AmbientA), FMath::Abs(AmbientB));
		if (MaxAmbient > 1.0e-4f)
		{
			Change = FMath::Max(Change, FMath::Abs(AmbientA - AmbientB) / MaxAmbient / 0.1f);
		}
		return Change;
	}

	// ---- 아틀라스
	inline void GetTileOrigin(uint32 Probe, uint32 TileSize, uint32& OutX, uint32& OutY)
	{
		OutX = (Probe % AtlasTilesPerRow) * TileSize;
		OutY = (Probe / AtlasTilesPerRow) * TileSize;
	}
	inline uint32 GetAtlasRows(uint32 TotalProbes) { return std::max((TotalProbes + AtlasTilesPerRow - 1) / AtlasTilesPerRow, 1u); }
	inline uint32 GetAtlasColumns(uint32 TotalProbes) { return std::clamp(TotalProbes, 1u, AtlasTilesPerRow); }

	// ---- 갱신 일정 (프레임마다 볼륨별로 프로브 일부를 돌아가며)
	// 볼륨 안 번호 Local이 이번 프레임 갱신 범위 [Start, Start + Count) (순환)에 들면 true와 추적 행(RowOffset + k)
	inline bool IsProbeUpdated(uint32 Local, uint32 ProbeCount, uint32 Start, uint32 Count, uint32 RowOffset, uint32& OutRow)
	{
		const uint32 K = (Local + ProbeCount - Start % ProbeCount) % ProbeCount;
		OutRow         = RowOffset + K;
		return K < Count;
	}

	// 전체 예산을 볼륨 프로브 수 비율로 나눈다 (볼륨마다 최소 1, 자기 상한 VolumeBudget(0 = 전부) 이하). 0이면 전부
	inline uint32 ComputeVolumeUpdateCount(uint32 ProbeCount, uint32 VolumeBudget, uint32 TotalProbes, uint32 GlobalBudget)
	{
		uint32 Count = ProbeCount;
		if (VolumeBudget > 0)
		{
			Count = std::min(Count, VolumeBudget);
		}
		if (GlobalBudget > 0 && GlobalBudget < TotalProbes)
		{
			const uint64 Share = static_cast<uint64>(GlobalBudget) * ProbeCount / std::max(TotalProbes, 1u);
			Count              = std::min(Count, std::max(static_cast<uint32>(Share), 1u));
		}
		return Count;
	}
} // namespace DdgiMath
