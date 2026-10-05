#pragma once

#include "Core/Math/Math.h"

#include <cmath>

// 점광원/스포트라이트 공용 식 (CPU 테스트용 순수 함수). 셰이더 Lighting.hlsli / ClusterCulling.hlsl과 **같은 식**을 유지한다.
//   - 감쇠: 역제곱(1m 기준, 10cm 이하는 고정) × 반경 창 함수 saturate(1 - (d/R)^4)^2 (UE/Frostbite 방식) → 반경에서 정확히 0
//   - 원뿔: t = saturate(cos * Scale + Offset), 결과 t^2. Scale = 1 / (cos(내부) - cos(외부)), Offset = -cos(외부) * Scale
//   - 클러스터: 화면 GridX x GridY 타일 x 뷰 깊이 GridZ 조각(로그 분할, Near~Far). 조각 = floor(log(z) * SliceScale + SliceBias)
//   - 점광원 그림자: 큐브 6면(+X,-X,+Y,-Y,+Z,-Z)을 각각 90°보다 조금 넓은 원근으로 그린다(PCF가 면 경계 밖을 읽지 않도록)
namespace LightMath
{
	constexpr float ReferenceDistance = 100.0f; // cm: Intensity가 그대로 나오는 거리
	constexpr float MinDistance       = 10.0f;  // cm: 이보다 가까우면 역제곱을 고정 (특이점 방지)
	constexpr float MaxSpotConeAngle  = 80.0f;  // 도: 그림자 원근(시야각 = 2 × 외부 원뿔)이 유효한 범위

	// 클러스터 격자 (ShaderTypes.h FClusterConstants, ClusterCulling.hlsl과 공유)
	constexpr uint32 ClusterGridX        = 16;
	constexpr uint32 ClusterGridY        = 9;
	constexpr uint32 ClusterGridZ        = 24;
	constexpr uint32 ClusterCount        = ClusterGridX * ClusterGridY * ClusterGridZ;
	constexpr uint32 ClusterStride       = 64;                // 클러스터 하나의 uint 칸 수: [0] = 개수, [1..] = 라이트 인덱스
	constexpr uint32 MaxLightsPerCluster = ClusterStride - 1; // 넘치면 버린다 (가까운 순서가 아니라 목록 순서)
	constexpr uint32 MaxLocalLights      = 1024;              // 프레임당 라이트 목록 상한 (카메라에 가까운 순)

	// 번호는 셰이더와 공유 (Lighting.hlsli E_LOCAL_LIGHT_*) — 끝에만 추가
	enum class ELocalLightType : uint32
	{
		Point = 0,
		Spot  = 1,
		Rect  = 2, // 면광원 사각형 (Phase 52, AreaLightMath.h)
		Disc  = 3, // 면광원 원판
	};

	// FLocalLightGpuData::Flags (Lighting.hlsli E_LOCAL_LIGHT_FLAG_*)
	constexpr uint32 LocalLightFlag_TwoSided = 1u << 0; // 면광원 양면 발광
	// 비트 8~15 = 정반사 감소량 R (0~255): 정반사 배율 = 1 - R / 255 (Lighting.hlsli GetLocalLightSpecularScale).
	// 감소량으로 두어 기본 0 = 배율 정확히 1 (기존 광원 화면 비트 동일)
	constexpr uint32 LocalLightSpecularShift = 8;
	inline uint32 EncodeSpecularScale(float Scale)
	{
		const float Reduction = 1.0f - FMath::Clamp(Scale, 0.0f, 1.0f);
		return static_cast<uint32>(Reduction * 255.0f + 0.5f) << LocalLightSpecularShift;
	}
	inline float DecodeSpecularScale(uint32 Flags)
	{
		return 1.0f - static_cast<float>((Flags >> LocalLightSpecularShift) & 255u) / 255.0f;
	}

	inline bool IsAreaLightType(uint32 Type)
	{
		return Type >= static_cast<uint32>(ELocalLightType::Rect);
	}

	inline float SrgbToLinear(float Value)
	{
		return Value <= 0.04045f ? Value / 12.92f : std::pow((Value + 0.055f) / 1.055f, 2.4f);
	}
	inline FVector3 SrgbToLinear(const FVector3& Color)
	{
		return FVector3(SrgbToLinear(Color.X), SrgbToLinear(Color.Y), SrgbToLinear(Color.Z));
	}

	// 반경 창: 1 (d = 0) → 0 (d >= Radius)
	inline float DistanceWindow(float Distance, float Radius)
	{
		if (Radius <= 0.0f)
		{
			return 0.0f;
		}
		const float Ratio  = Distance / Radius;
		const float Ratio2 = Ratio * Ratio;
		const float Window = FMath::Clamp(1.0f - Ratio2 * Ratio2, 0.0f, 1.0f);
		return Window * Window;
	}

	// 1m 기준 역제곱 (1m에서 1)
	inline float InverseSquare(float Distance)
	{
		const float Scaled = FMath::Max(Distance, MinDistance) / ReferenceDistance;
		return 1.0f / (Scaled * Scaled);
	}

	inline float DistanceAttenuation(float Distance, float Radius)
	{
		return InverseSquare(Distance) * DistanceWindow(Distance, Radius);
	}

	struct FConeParams
	{
		float Scale  = 0.0f; // 점광원: Scale 0, Offset 1 → 항상 1
		float Offset = 1.0f;
	};

	// 반각(도)으로 원뿔 인자 계산. 외부는 [1, MaxSpotConeAngle], 내부는 [0, 외부)
	inline FConeParams ComputeConeParams(float InnerDegrees, float OuterDegrees)
	{
		const float Outer    = FMath::Clamp(OuterDegrees, 1.0f, MaxSpotConeAngle);
		const float Inner    = FMath::Clamp(InnerDegrees, 0.0f, Outer);
		const float CosOuter = FMath::Cos(FMath::DegreesToRadians(Outer));
		const float CosInner = FMath::Cos(FMath::DegreesToRadians(Inner));
		FConeParams Params;
		Params.Scale  = 1.0f / FMath::Max(CosInner - CosOuter, 1.0e-4f);
		Params.Offset = -CosOuter * Params.Scale;
		return Params;
	}

	// CosAngle = dot(광원 방향, 광원 → 표면 방향)
	inline float ConeAttenuation(float CosAngle, const FConeParams& Params)
	{
		const float T = FMath::Clamp(CosAngle * Params.Scale + Params.Offset, 0.0f, 1.0f);
		return T * T;
	}

	// ---- 클러스터

	struct FSliceParams
	{
		float Scale = 0.0f;
		float Bias  = 0.0f;
	};

	inline FSliceParams ComputeSliceParams(float NearZ, float FarZ, uint32 SliceCount)
	{
		const float LogRatio = std::log(FMath::Max(FarZ, NearZ * 1.001f) / NearZ);
		FSliceParams Params;
		Params.Scale = static_cast<float>(SliceCount) / LogRatio;
		Params.Bias  = -static_cast<float>(SliceCount) * std::log(NearZ) / LogRatio;
		return Params;
	}

	// 뷰 깊이(cm) → 조각 번호 [0, SliceCount)
	inline uint32 DepthToSlice(float ViewDepth, const FSliceParams& Params, uint32 SliceCount)
	{
		const float Slice = std::floor(std::log(FMath::Max(ViewDepth, 1.0e-3f)) * Params.Scale + Params.Bias);
		return static_cast<uint32>(FMath::Clamp(Slice, 0.0f, static_cast<float>(SliceCount - 1)));
	}

	// 조각 경계 깊이: Near * (Far / Near)^(Slice / SliceCount)
	inline float SliceToDepth(uint32 Slice, float NearZ, float FarZ, uint32 SliceCount)
	{
		return NearZ * std::pow(FarZ / NearZ, static_cast<float>(Slice) / static_cast<float>(SliceCount));
	}

	// 클러스터 (TileX, TileY, Slice)의 뷰 공간 AABB. 타일 Y = 0은 화면 위쪽.
	//   원근: 뷰 x = ndc.x * z * ProjScaleX (ProjScale = tan(반 시야각)), 직교: 뷰 x = ndc.x * ProjScaleX (ProjScale = 반 폭/높이)
	//   마지막 조각은 FarZ 너머까지 (조각 번호를 고정한 픽셀이 빠지지 않게)
	inline FBox ComputeClusterViewBounds(uint32 TileX, uint32 TileY, uint32 Slice, uint32 GridX, uint32 GridY, uint32 GridZ, float NearZ,
	                                     float FarZ, bool bOrthographic, float ProjScaleX, float ProjScaleY)
	{
		const float NdcMinX = -1.0f + 2.0f * static_cast<float>(TileX) / static_cast<float>(GridX);
		const float NdcMaxX = -1.0f + 2.0f * static_cast<float>(TileX + 1) / static_cast<float>(GridX);
		const float NdcMaxY = 1.0f - 2.0f * static_cast<float>(TileY) / static_cast<float>(GridY);
		const float NdcMinY = 1.0f - 2.0f * static_cast<float>(TileY + 1) / static_cast<float>(GridY);
		const float ZNear   = Slice == 0 ? 0.0f : SliceToDepth(Slice, NearZ, FarZ, GridZ);
		const float ZFar    = Slice + 1 >= GridZ ? FarZ * 1.0e3f : SliceToDepth(Slice + 1, NearZ, FarZ, GridZ);

		FBox Bounds;
		const float Depths[2] = { ZNear, ZFar };
		for (const float Z : Depths)
		{
			const float ScaleX = bOrthographic ? ProjScaleX : Z * ProjScaleX;
			const float ScaleY = bOrthographic ? ProjScaleY : Z * ProjScaleY;
			Bounds.AddPoint(FVector3(NdcMinX * ScaleX, NdcMinY * ScaleY, Z));
			Bounds.AddPoint(FVector3(NdcMaxX * ScaleX, NdcMaxY * ScaleY, Z));
		}
		return Bounds;
	}

	inline bool SphereIntersectsBox(const FVector3& Center, float Radius, const FBox& Box)
	{
		const FVector3 Closest(FMath::Clamp(Center.X, Box.Min.X, Box.Max.X), FMath::Clamp(Center.Y, Box.Min.Y, Box.Max.Y),
		                       FMath::Clamp(Center.Z, Box.Min.Z, Box.Max.Z));
		return FVector3::DistanceSquared(Closest, Center) <= Radius * Radius;
	}

	// ---- 점광원 큐브 그림자

	constexpr uint32 CubeFaceCount = 6;

	// 광원 → 표면 방향의 주축 면 (0..5 = +X,-X,+Y,-Y,+Z,-Z)
	inline uint32 SelectCubeFace(const FVector3& Direction)
	{
		const float AX = FMath::Abs(Direction.X);
		const float AY = FMath::Abs(Direction.Y);
		const float AZ = FMath::Abs(Direction.Z);
		if (AX >= AY && AX >= AZ)
		{
			return Direction.X >= 0.0f ? 0u : 1u;
		}
		if (AY >= AZ)
		{
			return Direction.Y >= 0.0f ? 2u : 3u;
		}
		return Direction.Z >= 0.0f ? 4u : 5u;
	}

	inline FVector3 GetCubeFaceDirection(uint32 Face)
	{
		const FVector3 Directions[CubeFaceCount] = { FVector3(1.0f, 0.0f, 0.0f), FVector3(-1.0f, 0.0f, 0.0f), FVector3(0.0f, 1.0f, 0.0f),
		                                             FVector3(0.0f, -1.0f, 0.0f), FVector3(0.0f, 0.0f, 1.0f), FVector3(0.0f, 0.0f, -1.0f) };
		return Directions[Face % CubeFaceCount];
	}

	inline FVector3 GetCubeFaceUp(uint32 Face)
	{
		return Face >= 4 ? FVector3::ForwardVector : FVector3::UpVector;
	}

	// 면 하나의 tan(반 시야각): 가장자리에 2텍셀 여백 (3x3 PCF + 쌍선형 비교가 면 밖을 읽지 않도록)
	inline float CubeFaceTanHalfFov(uint32 Resolution)
	{
		const float Res = static_cast<float>(FMath::Max<uint32>(Resolution, 8));
		return Res / (Res - 4.0f);
	}

	// 광원 위치의 큐브 면 뷰-투영 (근평면 NearZ, 원평면 = 반경)
	inline FMatrix4x4 ComputeCubeFaceViewProjection(const FVector3& LightPosition, uint32 Face, float Radius, float NearZ, uint32 Resolution)
	{
		const FMatrix4x4 View = FMatrix4x4::MakeLookAt(LightPosition, LightPosition + GetCubeFaceDirection(Face), GetCubeFaceUp(Face));
		const float      Fov  = 2.0f * std::atan(CubeFaceTanHalfFov(Resolution));
		return View * FMatrix4x4::MakePerspectiveFov(Fov, 1.0f, NearZ, FMath::Max(Radius, NearZ * 2.0f));
	}

	// 스포트 그림자 뷰-투영 (시야각 = 2 × 외부 원뿔 + 여백)
	inline FMatrix4x4 ComputeSpotViewProjection(const FVector3& LightPosition, const FVector3& Direction, float OuterDegrees, float Radius,
	                                            float NearZ, uint32 Resolution)
	{
		const FVector3   Up   = FMath::Abs(Direction.Z) > 0.99f ? FVector3::ForwardVector : FVector3::UpVector;
		const FMatrix4x4 View = FMatrix4x4::MakeLookAt(LightPosition, LightPosition + Direction, Up);
		const float      Outer = FMath::Clamp(OuterDegrees, 1.0f, MaxSpotConeAngle);
		const float      Tan   = FMath::Tan(FMath::DegreesToRadians(Outer)) * CubeFaceTanHalfFov(Resolution);
		return View * FMatrix4x4::MakePerspectiveFov(2.0f * std::atan(Tan), 1.0f, NearZ, FMath::Max(Radius, NearZ * 2.0f));
	}

	// 그림자 맵 텍셀의 월드 크기 = 깊이 × 이 값 (법선 오프셋용)
	inline float ShadowTexelWorldFactor(float TanHalfFov, uint32 Resolution)
	{
		return 2.0f * TanHalfFov / static_cast<float>(FMath::Max<uint32>(Resolution, 1));
	}
} // namespace LightMath
