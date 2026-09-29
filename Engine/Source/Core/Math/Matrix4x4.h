#pragma once

#include "Core/Math/Quat.h"
#include "Core/Math/Vector4.h"

// 4x4 행렬. 행우선(row-major) 저장, 행벡터 규약: v' = v * M.
// 변환 합성은 적용 순서대로 곱한다: World = Scale * Rotation * Translation, MVP = World * View * Projection.
// 이동 성분은 마지막 행(M[3][0..2])에 위치한다.
struct FMatrix4x4
{
	float M[4][4];

	// 단위 행렬로 초기화
	constexpr FMatrix4x4()
		: M{ { 1.0f, 0.0f, 0.0f, 0.0f },
		     { 0.0f, 1.0f, 0.0f, 0.0f },
		     { 0.0f, 0.0f, 1.0f, 0.0f },
		     { 0.0f, 0.0f, 0.0f, 1.0f } }
	{
	}

	constexpr FMatrix4x4(const FVector4& Row0, const FVector4& Row1, const FVector4& Row2, const FVector4& Row3)
		: M{ { Row0.X, Row0.Y, Row0.Z, Row0.W },
		     { Row1.X, Row1.Y, Row1.Z, Row1.W },
		     { Row2.X, Row2.Y, Row2.Z, Row2.W },
		     { Row3.X, Row3.Y, Row3.Z, Row3.W } }
	{
	}

	static const FMatrix4x4 Identity;

	FMatrix4x4  operator*(const FMatrix4x4& B) const;
	FMatrix4x4& operator*=(const FMatrix4x4& B) { *this = *this * B; return *this; }

	float*       operator[](int32 Row) { return M[Row]; }
	const float* operator[](int32 Row) const { return M[Row]; }

	// v * M (동차 좌표)
	FVector4 TransformVector4(const FVector4& V) const;
	// 위치 변환 (W = 1). 원근 나눗셈은 하지 않는다.
	FVector3 TransformPosition(const FVector3& P) const;
	// 방향 변환 (W = 0, 이동 무시)
	FVector3 TransformVector(const FVector3& V) const;

	FMatrix4x4 GetTransposed() const;
	float      Determinant() const;

	// 특이 행렬이면 false를 반환하고 OutInverse는 변경하지 않는다
	bool TryGetInverse(FMatrix4x4& OutInverse) const;
	// 특이 행렬이면 Identity 반환
	FMatrix4x4 GetInverse() const;

	constexpr FVector3 GetOrigin() const { return { M[3][0], M[3][1], M[3][2] }; }
	constexpr FVector3 GetAxisX() const { return { M[0][0], M[0][1], M[0][2] }; }
	constexpr FVector3 GetAxisY() const { return { M[1][0], M[1][1], M[1][2] }; }
	constexpr FVector3 GetAxisZ() const { return { M[2][0], M[2][1], M[2][2] }; }

	constexpr void SetOrigin(const FVector3& Origin) { M[3][0] = Origin.X; M[3][1] = Origin.Y; M[3][2] = Origin.Z; }

	bool Equals(const FMatrix4x4& Other, float Tolerance = FMath::KindaSmallNumber) const;

	static FMatrix4x4 MakeTranslation(const FVector3& Translation);
	static FMatrix4x4 MakeScale(const FVector3& Scale);
	static FMatrix4x4 MakeScale(float UniformScale) { return MakeScale(FVector3(UniformScale)); }
	static FMatrix4x4 MakeRotation(const FQuat& Rotation);
	// Scale * Rotation * Translation
	static FMatrix4x4 MakeTransform(const FVector3& Translation, const FQuat& Rotation, const FVector3& Scale);

	// 뷰 행렬. 월드(Z-up)를 뷰 공간(+X 오른쪽, +Y 위, +Z 앞)으로 변환한다.
	static FMatrix4x4 MakeLookAt(const FVector3& Eye, const FVector3& Target, const FVector3& WorldUp = FVector3::UpVector);

	// 왼손 원근 투영, 깊이 범위 [0, 1]
	static FMatrix4x4 MakePerspectiveFov(float FovYRadians, float AspectRatio, float NearZ, float FarZ);

	// 왼손 직교 투영, 깊이 범위 [0, 1]
	static FMatrix4x4 MakeOrthographic(float Width, float Height, float NearZ, float FarZ);
};

inline const FMatrix4x4 FMatrix4x4::Identity;
