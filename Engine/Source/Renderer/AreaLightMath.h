#pragma once

#include "Core/Math/Math.h"
#include "Renderer/LightMath.h"

#include <cmath>

// 면광원(사각형·원판) / IES 프로필 / 라이트 쿠키 공용 식 (Phase 52). 셰이더 AreaLight.hlsli / Lighting.hlsli와 **같은 식**을 유지한다
// (테스트 AreaLight_*, Ies_*, LightCookie_*).
//
// 좌표: 라이트 엔티티 로컬 +X(Forward) = 빛이 나가는 면 법선, +Y(Right) = 가로(Width), +Z(Up) = 세로(Height) — UE 사각형 광원과 같은 방향.
//   IES와 쿠키도 같은 축을 쓴다 (점광원/스포트도 트랜스폼 Forward/Right/Up).
//
// 단위: Intensity = 면 법선 방향 1m 거리의 조도 — 점광원 Intensity와 같은 단위(원거리에서 같은 밝기). 면의 휘도(복사 휘도) L은
//   Intensity × (100cm)² / 면적(cm²)이라 면을 키우면 같은 총 빛이 더 넓게 퍼진다(밝기 유지가 필요하면 Intensity를 면적에 비례해 올린다).
//   렌더러는 L을 Color에 미리 곱한다 (IntensityToRadianceScale). 확산 = 알베도 × L × LTC 적분(코사인/π 분포의 다각형 적분 = 형태 계수).
//
// 셰이딩 = LTC(Heitz 2016, 표 = selfshadow/ltc_code GGX 피팅 64x64 두 장 — LtcTables.cpp, BSD 라이선스):
//   - 표 좌표 (거칠기(지각) , sqrt(1 - N·V)) × (63/64) + 0.5/64, 표 1 = 역행렬 (m00, m20, m02, m22 — m11 = 1), 표 2 = (크기, 프레넬, -, 구 근사)
//   - 꼭짓점을 (T1, T2, N) 기저로 옮기고 역행렬을 곱한 뒤 단위 구에 투영, 모서리 적분 벡터(유리 근사)를 더한다 → 형태 계수 벡터 F
//   - 지평선 잘림은 다각형을 자르지 않고 "구 근사" 표(표 2 .w, 좌표 (z·0.5 + 0.5, |F|))로 한다 (Heitz 2017 — 꼭짓점 수와 무관한 비용)
//   - 면 뒤쪽 점(법선 반대편)은 단면 광원이면 0, 양면이면 z를 뒤집어 같은 식
//   - 스펙큘러 = L × 적분(GGX 역행렬) × (F0 × 크기 + (1 - F0) × 프레넬), 확산 = L × 적분(단위 행렬) × 알베도 × (1 - 금속)
//   - 원판은 같은 넓이의 정 8각형으로 근사 (꼭짓점 반지름 × DiscPolygonScale) — 같은 다각형 적분 경로, 타원(가로·세로 반지름) 허용
//   - 감쇠 창 = LightMath::DistanceWindow(면에서 가장 가까운 점까지 거리, Radius) → 경계 구 반지름 = Radius + 면 반 대각선
//   - 문 덮개(barn door): 면 가운데 → 표면 방향의 원뿔 감쇠(스포트와 같은 식, 외부 = BarnDoorAngle, 부드러운 폭 = atan(반 크기 / 길이))
// IES: θ = 라이트 Forward와 이루는 각(0 = 천저 = 빛 축), φ = Right에서 Up 쪽으로 [0, 360). 텍스처(가로 θ 0..180 IesTextureWidth칸,
//   세로 φ 0..360 IesTextureHeight칸 — 끝 칸 = 0°)는 최대 칸델라로 나눈 값. 면광원은 가운데에서 본 방향 하나로 곱한다(근사)
// 쿠키: 스포트/면광원 = 가운데에서의 원근 투영 (반각 = 외부 원뿔 / 문 덮개, 최대 80°) → uv, 점광원 = 위도-경도(IES와 같은 θ, φ).
//   uv = 투영 좌표 × Transform.xy + Transform.zw (ComputeCookieTransform: 크기 배율 + 오프셋 + 시간 패닝). 방향광 = 빛에 수직인 평면 좌표
// RT 히트/볼류메트릭 안개: 면광원은 가운데 대표점 + 면 코사인(원거리 근사, ComputeApproxAreaFactor) — LTC 표 없이
namespace AreaLightMath
{
	constexpr uint32 LtcTableSize = 64;
	// 표 데이터 (LtcTables.cpp, 64 × 64 × RGBA, 번호 = 거칠기 칸 + 64 × 각 칸)
	const float* GetLtcTable1();
	const float* GetLtcTable2();

	constexpr uint32 DiscPolygonSides   = 8;
	constexpr uint32 MaxPolygonVertices = 8;
	// 정 8각형 넓이 (n/2 · r² · sin(2π/n) = 2√2 r²)가 원 넓이 π r²가 되도록 꼭짓점 반지름 배율
	constexpr float DiscPolygonScale = 1.05390736f;

	constexpr float MaxCookieAngle = LightMath::MaxSpotConeAngle; // 원근 쿠키·면광원 그림자 반각 상한 (도)

	// IES 텍스처 (θ × φ)
	constexpr uint32 IesTextureWidth  = 64;
	constexpr uint32 IesTextureHeight = 32;
	constexpr float  CandelaToIntensity = 0.01f; // IES 밝기 사용 시: 최대 칸델라 × 이 값 = Intensity (1000cd ≈ 10)

	// ---- 단위

	inline float ComputeArea(LightMath::ELocalLightType Type, float HalfWidth, float HalfHeight)
	{
		const float W = FMath::Max(HalfWidth, 0.0f);
		const float H = FMath::Max(HalfHeight, 0.0f);
		return Type == LightMath::ELocalLightType::Disc ? FMath::Pi * W * H : 4.0f * W * H;
	}

	// Intensity → 휘도 배율 (cm² 기준). Color = 선형 색 × Intensity × 이 값
	inline float IntensityToRadianceScale(float AreaCm2)
	{
		return LightMath::ReferenceDistance * LightMath::ReferenceDistance / FMath::Max(AreaCm2, 1.0f);
	}

	// 경계 구 반지름 (가운데 기준): 영향 반경 + 면 반 대각선
	inline float ComputeBoundingRadius(LightMath::ELocalLightType Type, float Radius, float HalfWidth, float HalfHeight)
	{
		const float Extent = Type == LightMath::ELocalLightType::Disc ? FMath::Max(HalfWidth, HalfHeight)
		                                                               : std::sqrt(HalfWidth * HalfWidth + HalfHeight * HalfHeight);
		return Radius + Extent;
	}

	// 면에서 가장 가까운 점까지의 거리. Local = (Forward, Right, Up) 성분 (가운데 기준). 원판(타원)은 정규화 반지름으로 자른다(원은 정확)
	inline float DistanceToArea(LightMath::ELocalLightType Type, const FVector3& Local, float HalfWidth, float HalfHeight)
	{
		float Y = Local.Y;
		float Z = Local.Z;
		if (Type == LightMath::ELocalLightType::Disc)
		{
			const float NY = Y / FMath::Max(HalfWidth, 1.0e-3f);
			const float NZ = Z / FMath::Max(HalfHeight, 1.0e-3f);
			const float R  = std::sqrt(NY * NY + NZ * NZ);
			if (R > 1.0f)
			{
				Y /= R;
				Z /= R;
			}
		}
		else
		{
			Y = FMath::Clamp(Y, -HalfWidth, HalfWidth);
			Z = FMath::Clamp(Z, -HalfHeight, HalfHeight);
		}
		const float DY = Local.Y - Y;
		const float DZ = Local.Z - Z;
		return std::sqrt(Local.X * Local.X + DY * DY + DZ * DZ);
	}

	// 문 덮개 원뿔 (스포트와 같은 식). BarnDoorAngle >= 90 = 없음 (항상 1)
	inline LightMath::FConeParams ComputeBarnDoorCone(float BarnDoorAngle, float BarnDoorLength, float HalfExtent)
	{
		LightMath::FConeParams Params;
		if (BarnDoorAngle >= 90.0f)
		{
			return Params;
		}
		const float Outer    = FMath::Clamp(BarnDoorAngle, 1.0f, 90.0f);
		const float Soft     = FMath::RadiansToDegrees(std::atan(FMath::Max(HalfExtent, 0.0f) / FMath::Max(BarnDoorLength, 0.1f)));
		const float Inner    = FMath::Max(Outer - Soft, 0.0f);
		const float CosOuter = FMath::Cos(FMath::DegreesToRadians(Outer));
		const float CosInner = FMath::Cos(FMath::DegreesToRadians(Inner));
		Params.Scale         = 1.0f / FMath::Max(CosInner - CosOuter, 1.0e-4f);
		Params.Offset        = -CosOuter * Params.Scale;
		return Params;
	}

	// ---- 다각형

	struct FAreaPolygon
	{
		FVector3 Points[MaxPolygonVertices];
		uint32   Count = 0;
	};

	// 꼭짓점 순서: 앞(빛 나가는 쪽)에서 본 적분이 양수가 되는 방향 (테스트 AreaLight_LtcFormFactorMatchesAnalytic)
	inline FAreaPolygon MakePolygon(LightMath::ELocalLightType Type, const FVector3& Center, const FVector3& Right, const FVector3& Up, float HalfWidth,
	                                float HalfHeight)
	{
		FAreaPolygon Polygon;
		if (Type == LightMath::ELocalLightType::Disc)
		{
			Polygon.Count = DiscPolygonSides;
			for (uint32 Index = 0; Index < DiscPolygonSides; ++Index)
			{
				// Up에서 Right 쪽으로 (사각형과 같은 회전 방향)
				const float Angle = FMath::TwoPi * static_cast<float>(Index) / static_cast<float>(DiscPolygonSides);
				Polygon.Points[Index] =
					Center + Up * (std::cos(Angle) * HalfHeight * DiscPolygonScale) + Right * (std::sin(Angle) * HalfWidth * DiscPolygonScale);
			}
			return Polygon;
		}
		Polygon.Count     = 4;
		Polygon.Points[0] = Center - Right * HalfWidth - Up * HalfHeight;
		Polygon.Points[1] = Center - Right * HalfWidth + Up * HalfHeight;
		Polygon.Points[2] = Center + Right * HalfWidth + Up * HalfHeight;
		Polygon.Points[3] = Center + Right * HalfWidth - Up * HalfHeight;
		return Polygon;
	}

	// ---- LTC

	// 단위 벡터 A → B 모서리의 형태 계수 벡터 (Heitz 2017 유리 근사: θ / sin θ / 2π)
	inline FVector3 IntegrateEdgeVector(const FVector3& A, const FVector3& B)
	{
		const float X              = FVector3::Dot(A, B);
		const float Y              = FMath::Abs(X);
		const float NumA           = 0.8543985f + (0.4965155f + 0.0145206f * Y) * Y;
		const float NumB           = 3.4175940f + (4.1616724f + Y) * Y;
		const float V              = NumA / NumB;
		const float ThetaSinTheta  = X > 0.0f ? V : 0.5f / std::sqrt(FMath::Max(1.0f - X * X, 1.0e-7f)) - V;
		return FVector3::Cross(A, B) * ThetaSinTheta;
	}

	// 표 좌표 (텍셀 가운데 보정 포함)
	inline FVector2 ComputeLtcCoords(float Roughness, float NdotV)
	{
		const float Scale = static_cast<float>(LtcTableSize - 1) / static_cast<float>(LtcTableSize);
		const float Bias  = 0.5f / static_cast<float>(LtcTableSize);
		return FVector2(FMath::Clamp(Roughness, 0.0f, 1.0f) * Scale + Bias, std::sqrt(FMath::Clamp(1.0f - NdotV, 0.0f, 1.0f)) * Scale + Bias);
	}

	// 선형 클램프 표본 (GPU 선형 샘플러와 같은 위치 — 하드웨어 보간 정밀도 차이는 테스트 허용 오차)
	inline FVector4 SampleTable(const float* Table, const FVector2& UV)
	{
		const float Size = static_cast<float>(LtcTableSize);
		const float X    = FMath::Clamp(UV.X * Size - 0.5f, 0.0f, Size - 1.0f);
		const float Y    = FMath::Clamp(UV.Y * Size - 0.5f, 0.0f, Size - 1.0f);
		const uint32 X0  = static_cast<uint32>(X);
		const uint32 Y0  = static_cast<uint32>(Y);
		const uint32 X1  = FMath::Min(X0 + 1, LtcTableSize - 1);
		const uint32 Y1  = FMath::Min(Y0 + 1, LtcTableSize - 1);
		const float  FX  = X - static_cast<float>(X0);
		const float  FY  = Y - static_cast<float>(Y0);
		auto Fetch = [Table](uint32 TX, uint32 TY) {
			const float* T = Table + (TX + TY * LtcTableSize) * 4;
			return FVector4(T[0], T[1], T[2], T[3]);
		};
		auto Lerp4 = [](const FVector4& A, const FVector4& B, float T) {
			return FVector4(A.X + (B.X - A.X) * T, A.Y + (B.Y - A.Y) * T, A.Z + (B.Z - A.Z) * T, A.W + (B.W - A.W) * T);
		};
		return Lerp4(Lerp4(Fetch(X0, Y0), Fetch(X1, Y0), FX), Lerp4(Fetch(X0, Y1), Fetch(X1, Y1), FX), FY);
	}

	// 역행렬 성분 (표 1): x' = M00·x + M02·z, y' = y, z' = M20·x + M22·z
	struct FLtcInverse
	{
		float M00 = 1.0f;
		float M20 = 0.0f;
		float M02 = 0.0f;
		float M22 = 1.0f;
	};

	inline FLtcInverse MakeLtcInverse(const FVector4& Table1)
	{
		return FLtcInverse{ Table1.X, Table1.Y, Table1.Z, Table1.W };
	}

	// 구 근사 지평선 잘림: 형태 계수 벡터 F → 잘린 형태 계수 (표 2 .w, 좌표 (z·0.5 + 0.5, |F|))
	inline float ClipFormFactor(const FVector3& FormFactor, bool bFlipZ, const float* Table2)
	{
		const float Length = FormFactor.Length();
		if (Length <= 1.0e-7f)
		{
			return 0.0f;
		}
		float       Z     = FormFactor.Z / Length;
		Z                 = bFlipZ ? -Z : Z;
		const float Scale = static_cast<float>(LtcTableSize - 1) / static_cast<float>(LtcTableSize);
		const float Bias  = 0.5f / static_cast<float>(LtcTableSize);
		const FVector2 UV((Z * 0.5f + 0.5f) * Scale + Bias, FMath::Clamp(Length, 0.0f, 1.0f) * Scale + Bias);
		return Length * SampleTable(Table2, UV).W;
	}

	// 다각형 적분 (LTC). N/V = 표면 법선/시선(표면 → 카메라), P = 표면 위치, LightForward = 빛 나가는 면 법선.
	//   bBehind(점이 면 뒤) && !bTwoSided면 0
	inline float EvaluatePolygon(const FVector3& N, const FVector3& V, const FVector3& P, const FLtcInverse& Inverse, const FAreaPolygon& Polygon,
	                             const FVector3& LightCenter, const FVector3& LightForward, bool bTwoSided, const float* Table2)
	{
		const bool bBehind = FVector3::Dot(P - LightCenter, LightForward) < 0.0f;
		if (bBehind && !bTwoSided)
		{
			return 0.0f;
		}
		// 법선 기준 기저 (T1 = 시선의 접평면 성분). 시선이 법선과 같으면 임의 접선
		FVector3    T1     = V - N * FVector3::Dot(V, N);
		const float T1Len  = T1.Length();
		T1                 = T1Len > 1.0e-5f ? T1 / T1Len : (FMath::Abs(N.Z) < 0.999f ? FVector3::Cross(N, FVector3::UpVector).GetNormalized()
		                                                                                 : FVector3::ForwardVector);
		const FVector3 T2  = FVector3::Cross(N, T1);

		FVector3 L[MaxPolygonVertices];
		for (uint32 Index = 0; Index < Polygon.Count; ++Index)
		{
			const FVector3 W = Polygon.Points[Index] - P;
			const FVector3 Local(FVector3::Dot(W, T1), FVector3::Dot(W, T2), FVector3::Dot(W, N));
			const FVector3 Transformed(Inverse.M00 * Local.X + Inverse.M02 * Local.Z, Local.Y, Inverse.M20 * Local.X + Inverse.M22 * Local.Z);
			const float    Length = Transformed.Length();
			L[Index]              = Length > 1.0e-7f ? Transformed / Length : FVector3::UpVector;
		}
		FVector3 Sum = FVector3::ZeroVector;
		for (uint32 Index = 0; Index < Polygon.Count; ++Index)
		{
			Sum += IntegrateEdgeVector(L[Index], L[(Index + 1) % Polygon.Count]);
		}
		return ClipFormFactor(Sum, bBehind, Table2);
	}

	struct FAreaLightTerms
	{
		float Diffuse  = 0.0f; // 형태 계수 (코사인/π 분포)
		float Specular = 0.0f; // GGX LTC 적분 (프레넬 전)
		float Magnitude = 0.0f; // 표 2 .x
		float Fresnel   = 0.0f; // 표 2 .y — 스펙큘러 색 = F0 × Magnitude + (1 - F0) × Fresnel
	};

	inline FAreaLightTerms EvaluateAreaLightTerms(const FVector3& N, const FVector3& V, const FVector3& P, float Roughness, const FAreaPolygon& Polygon,
	                                              const FVector3& LightCenter, const FVector3& LightForward, bool bTwoSided)
	{
		const float     NdotV  = FMath::Clamp(FVector3::Dot(N, V), 1.0e-4f, 1.0f);
		const FVector2  UV     = ComputeLtcCoords(Roughness, NdotV);
		const FVector4  Table1 = SampleTable(GetLtcTable1(), UV);
		const FVector4  Table2 = SampleTable(GetLtcTable2(), UV);
		FAreaLightTerms Terms;
		Terms.Diffuse   = EvaluatePolygon(N, V, P, FLtcInverse{}, Polygon, LightCenter, LightForward, bTwoSided, GetLtcTable2());
		Terms.Specular  = EvaluatePolygon(N, V, P, MakeLtcInverse(Table1), Polygon, LightCenter, LightForward, bTwoSided, GetLtcTable2());
		Terms.Magnitude = Table2.X;
		Terms.Fresnel   = Table2.Y;
		return Terms;
	}

	// 원거리 근사 (RT 히트·볼류메트릭 안개): 가운데 대표점 기준 감쇠에 곱하는 값 = 면 코사인 × 면적 환산 (색에 곱해 둔 휘도 배율을 되돌림)
	//   CosEmit = dot(LightForward, 가운데 → 표면 방향). 양면이면 절댓값, 단면이면 뒤쪽 0
	inline float ComputeApproxAreaFactor(float CosEmit, bool bTwoSided, float AreaCm2)
	{
		const float Cos = bTwoSided ? FMath::Abs(CosEmit) : FMath::Max(CosEmit, 0.0f);
		return Cos * FMath::Max(AreaCm2, 1.0f) / (LightMath::ReferenceDistance * LightMath::ReferenceDistance);
	}

	// ---- IES 좌표 (Local = 라이트 → 표면 방향의 (Forward, Right, Up) 성분, 정규화)

	inline FVector2 ComputeIesAngles(const FVector3& Local)
	{
		const float Theta = std::acos(FMath::Clamp(Local.X, -1.0f, 1.0f));
		float       Phi   = std::atan2(Local.Z, Local.Y);
		if (Phi < 0.0f)
		{
			Phi += FMath::TwoPi;
		}
		return FVector2(Theta, Phi);
	}

	// θ ∈ [0, π] → 가로 칸 가운데 0..W-1, φ ∈ [0, 2π] → 세로 칸 가운데 0..H-1 (끝 칸 = 360° = 0°)
	inline FVector2 ComputeIesUV(const FVector3& Local)
	{
		const FVector2 Angles = ComputeIesAngles(Local);
		const float    W      = static_cast<float>(IesTextureWidth);
		const float    H      = static_cast<float>(IesTextureHeight);
		return FVector2((Angles.X / FMath::Pi * (W - 1.0f) + 0.5f) / W, (Angles.Y / FMath::TwoPi * (H - 1.0f) + 0.5f) / H);
	}

	// ---- 쿠키

	// Scale = 반복 배율(1 = 원뿔에 한 장), Offset = uv 이동(시간 패닝 포함 — 호출자가 frac). 원근 종류는 반각 ProjectionDegrees
	inline FVector4 ComputeCookieTransform(LightMath::ELocalLightType Type, float ProjectionDegrees, const FVector2& Scale, const FVector2& Offset)
	{
		if (Type == LightMath::ELocalLightType::Point)
		{
			return FVector4(Scale.X, Scale.Y, Offset.X, Offset.Y);
		}
		const float Tan = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(ProjectionDegrees, 1.0f, MaxCookieAngle)));
		// u = (Right/Forward) / Tan × 0.5 × Sx + 0.5 + Ox, v = -(Up/Forward) / Tan × 0.5 × Sy + 0.5 + Oy (v는 아래로)
		return FVector4(0.5f * Scale.X / Tan, -0.5f * Scale.Y / Tan, 0.5f + Offset.X, 0.5f + Offset.Y);
	}

	// Local = 라이트 → 표면 방향 (Forward, Right, Up 성분, 정규화). 원근 종류에서 뒤쪽이면 bValid = false
	inline FVector2 ComputeCookieUV(LightMath::ELocalLightType Type, const FVector3& Local, const FVector4& Transform, bool& bValid)
	{
		bValid = true;
		if (Type == LightMath::ELocalLightType::Point)
		{
			const FVector2 Angles = ComputeIesAngles(Local);
			return FVector2(Angles.Y / FMath::TwoPi * Transform.X + Transform.Z, Angles.X / FMath::Pi * Transform.Y + Transform.W);
		}
		if (Local.X <= 1.0e-4f)
		{
			bValid = false;
			return FVector2(0.0f, 0.0f);
		}
		return FVector2(Local.Y / Local.X * Transform.X + Transform.Z, Local.Z / Local.X * Transform.Y + Transform.W);
	}

	// 방향광 쿠키: 빛 진행 방향에 수직인 두 축 / 타일 크기(cm) + 오프셋 → u = dot(P, U.xyz) + U.w
	inline void ComputeDirectionalCookieAxes(const FVector3& LightDirection, float TileSize, const FVector2& Offset, FVector4& OutU, FVector4& OutV)
	{
		const FVector3 Dir    = LightDirection.GetNormalized();
		const FVector3 Helper = FMath::Abs(Dir.Z) > 0.99f ? FVector3::ForwardVector : FVector3::UpVector;
		const FVector3 AxisU  = FVector3::Cross(Helper, Dir).GetNormalized();
		const FVector3 AxisV  = FVector3::Cross(Dir, AxisU);
		const float    Inv    = 1.0f / FMath::Max(TileSize, 1.0f);
		OutU                  = FVector4(AxisU * Inv, Offset.X);
		OutV                  = FVector4(AxisV * Inv, Offset.Y);
	}

	// ---- 면광원 그림자 (PCSS 반그림자, 결정적)

	// 원근 그림자 깊이 [0, 1] → 뷰 깊이 (FMatrix4x4::MakePerspectiveFov: z = Range - Near·Range / d)
	inline float LinearizeShadowDepth(float DeviceDepth, float NearZ, float FarZ)
	{
		return NearZ * FarZ / FMath::Max(FarZ - DeviceDepth * (FarZ - NearZ), 1.0e-4f);
	}

	// 반그림자 폭 (그림자 맵 uv): 면 반 크기 × (수신 - 가림) / 가림, 수신 깊이에서의 투영 폭(2 tan × 깊이)으로 나눔
	inline float ComputePenumbraUV(float SourceRadius, float ReceiverDepth, float BlockerDepth, float TwoTanHalfFov)
	{
		const float Blocker = FMath::Max(BlockerDepth, 1.0f);
		const float World   = SourceRadius * FMath::Max(ReceiverDepth - Blocker, 0.0f) / Blocker;
		return World / FMath::Max(TwoTanHalfFov * ReceiverDepth, 1.0e-3f);
	}
} // namespace AreaLightMath
