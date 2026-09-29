#pragma once

#include "Core/Math/MathUtils.h"

// 2차원 벡터
struct FVector2
{
	float X;
	float Y;

	constexpr FVector2() : X(0.0f), Y(0.0f) {}
	constexpr FVector2(float InX, float InY) : X(InX), Y(InY) {}
	constexpr explicit FVector2(float Scalar) : X(Scalar), Y(Scalar) {}

	static const FVector2 ZeroVector;
	static const FVector2 OneVector;

	constexpr FVector2 operator+(const FVector2& V) const { return { X + V.X, Y + V.Y }; }
	constexpr FVector2 operator-(const FVector2& V) const { return { X - V.X, Y - V.Y }; }
	constexpr FVector2 operator*(const FVector2& V) const { return { X * V.X, Y * V.Y }; }
	constexpr FVector2 operator*(float Scale) const { return { X * Scale, Y * Scale }; }
	constexpr FVector2 operator/(float Scale) const { return { X / Scale, Y / Scale }; }
	constexpr FVector2 operator-() const { return { -X, -Y }; }

	constexpr FVector2& operator+=(const FVector2& V) { X += V.X; Y += V.Y; return *this; }
	constexpr FVector2& operator-=(const FVector2& V) { X -= V.X; Y -= V.Y; return *this; }
	constexpr FVector2& operator*=(float Scale) { X *= Scale; Y *= Scale; return *this; }
	constexpr FVector2& operator/=(float Scale) { X /= Scale; Y /= Scale; return *this; }

	constexpr bool operator==(const FVector2& V) const { return X == V.X && Y == V.Y; }
	constexpr bool operator!=(const FVector2& V) const { return !(*this == V); }

	float&       operator[](int32 Index) { return (&X)[Index]; }
	const float& operator[](int32 Index) const { return (&X)[Index]; }

	static constexpr float Dot(const FVector2& A, const FVector2& B) { return A.X * B.X + A.Y * B.Y; }
	// 2D 외적 (Z 성분 스칼라)
	static constexpr float Cross(const FVector2& A, const FVector2& B) { return A.X * B.Y - A.Y * B.X; }

	constexpr float LengthSquared() const { return X * X + Y * Y; }
	float           Length() const { return FMath::Sqrt(LengthSquared()); }

	FVector2 GetNormalized(float Tolerance = FMath::SmallNumber) const
	{
		const float LenSq = LengthSquared();
		return LenSq > Tolerance ? *this * FMath::InvSqrt(LenSq) : ZeroVector;
	}

	constexpr bool IsNearlyZero(float Tolerance = FMath::KindaSmallNumber) const
	{
		return FMath::Abs(X) <= Tolerance && FMath::Abs(Y) <= Tolerance;
	}

	constexpr bool Equals(const FVector2& V, float Tolerance = FMath::KindaSmallNumber) const
	{
		return FMath::Abs(X - V.X) <= Tolerance && FMath::Abs(Y - V.Y) <= Tolerance;
	}
};

inline const FVector2 FVector2::ZeroVector(0.0f, 0.0f);
inline const FVector2 FVector2::OneVector(1.0f, 1.0f);

constexpr FVector2 operator*(float Scale, const FVector2& V) { return V * Scale; }
