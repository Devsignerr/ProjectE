#pragma once

#include "Core/Math/MathUtils.h"

// 3차원 벡터.
// 엔진 좌표계: 왼손 Z-up — +X 앞(Forward), +Y 오른쪽(Right), +Z 위(Up). Cross(Forward, Right) == Up.
struct FVector3
{
	float X;
	float Y;
	float Z;

	constexpr FVector3() : X(0.0f), Y(0.0f), Z(0.0f) {}
	constexpr FVector3(float InX, float InY, float InZ) : X(InX), Y(InY), Z(InZ) {}
	constexpr explicit FVector3(float Scalar) : X(Scalar), Y(Scalar), Z(Scalar) {}

	static const FVector3 ZeroVector;
	static const FVector3 OneVector;
	static const FVector3 ForwardVector; // (1, 0, 0)
	static const FVector3 RightVector;   // (0, 1, 0)
	static const FVector3 UpVector;      // (0, 0, 1)

	constexpr FVector3 operator+(const FVector3& V) const { return { X + V.X, Y + V.Y, Z + V.Z }; }
	constexpr FVector3 operator-(const FVector3& V) const { return { X - V.X, Y - V.Y, Z - V.Z }; }
	constexpr FVector3 operator*(const FVector3& V) const { return { X * V.X, Y * V.Y, Z * V.Z }; }
	constexpr FVector3 operator/(const FVector3& V) const { return { X / V.X, Y / V.Y, Z / V.Z }; }
	constexpr FVector3 operator*(float Scale) const { return { X * Scale, Y * Scale, Z * Scale }; }
	constexpr FVector3 operator/(float Scale) const { return { X / Scale, Y / Scale, Z / Scale }; }
	constexpr FVector3 operator-() const { return { -X, -Y, -Z }; }

	constexpr FVector3& operator+=(const FVector3& V) { X += V.X; Y += V.Y; Z += V.Z; return *this; }
	constexpr FVector3& operator-=(const FVector3& V) { X -= V.X; Y -= V.Y; Z -= V.Z; return *this; }
	constexpr FVector3& operator*=(float Scale) { X *= Scale; Y *= Scale; Z *= Scale; return *this; }
	constexpr FVector3& operator/=(float Scale) { X /= Scale; Y /= Scale; Z /= Scale; return *this; }

	// 정확 비교. 부동소수점 근사 비교는 Equals 사용
	constexpr bool operator==(const FVector3& V) const { return X == V.X && Y == V.Y && Z == V.Z; }
	constexpr bool operator!=(const FVector3& V) const { return !(*this == V); }

	float&       operator[](int32 Index) { return (&X)[Index]; }
	const float& operator[](int32 Index) const { return (&X)[Index]; }

	static constexpr float Dot(const FVector3& A, const FVector3& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z; }

	static constexpr FVector3 Cross(const FVector3& A, const FVector3& B)
	{
		return { A.Y * B.Z - A.Z * B.Y,
		         A.Z * B.X - A.X * B.Z,
		         A.X * B.Y - A.Y * B.X };
	}

	constexpr float LengthSquared() const { return X * X + Y * Y + Z * Z; }
	float           Length() const { return FMath::Sqrt(LengthSquared()); }

	// 길이가 Tolerance 이하이면 ZeroVector 반환
	FVector3 GetNormalized(float Tolerance = FMath::SmallNumber) const
	{
		const float LenSq = LengthSquared();
		return LenSq > Tolerance ? *this * FMath::InvSqrt(LenSq) : ZeroVector;
	}

	// 제자리 정규화. 길이가 Tolerance 이하이면 변경하지 않고 false 반환
	bool Normalize(float Tolerance = FMath::SmallNumber)
	{
		const float LenSq = LengthSquared();
		if (LenSq > Tolerance)
		{
			*this *= FMath::InvSqrt(LenSq);
			return true;
		}
		return false;
	}

	bool IsNormalized(float Tolerance = FMath::KindaSmallNumber) const
	{
		return FMath::Abs(1.0f - LengthSquared()) <= Tolerance;
	}

	constexpr bool IsNearlyZero(float Tolerance = FMath::KindaSmallNumber) const
	{
		return FMath::Abs(X) <= Tolerance && FMath::Abs(Y) <= Tolerance && FMath::Abs(Z) <= Tolerance;
	}

	constexpr bool Equals(const FVector3& V, float Tolerance = FMath::KindaSmallNumber) const
	{
		return FMath::Abs(X - V.X) <= Tolerance && FMath::Abs(Y - V.Y) <= Tolerance && FMath::Abs(Z - V.Z) <= Tolerance;
	}

	static constexpr FVector3 Lerp(const FVector3& A, const FVector3& B, float Alpha) { return A + (B - A) * Alpha; }
	static float              Distance(const FVector3& A, const FVector3& B) { return (B - A).Length(); }
	static constexpr float    DistanceSquared(const FVector3& A, const FVector3& B) { return (B - A).LengthSquared(); }
};

inline const FVector3 FVector3::ZeroVector(0.0f, 0.0f, 0.0f);
inline const FVector3 FVector3::OneVector(1.0f, 1.0f, 1.0f);
inline const FVector3 FVector3::ForwardVector(1.0f, 0.0f, 0.0f);
inline const FVector3 FVector3::RightVector(0.0f, 1.0f, 0.0f);
inline const FVector3 FVector3::UpVector(0.0f, 0.0f, 1.0f);

constexpr FVector3 operator*(float Scale, const FVector3& V) { return V * Scale; }
