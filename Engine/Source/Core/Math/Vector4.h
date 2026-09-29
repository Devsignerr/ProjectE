#pragma once

#include "Core/Math/Vector3.h"

// 4차원 벡터 (동차 좌표, 색상 등)
struct FVector4
{
	float X;
	float Y;
	float Z;
	float W;

	constexpr FVector4() : X(0.0f), Y(0.0f), Z(0.0f), W(0.0f) {}
	constexpr FVector4(float InX, float InY, float InZ, float InW) : X(InX), Y(InY), Z(InZ), W(InW) {}
	constexpr FVector4(const FVector3& V, float InW) : X(V.X), Y(V.Y), Z(V.Z), W(InW) {}

	static const FVector4 ZeroVector;
	static const FVector4 OneVector;

	constexpr FVector4 operator+(const FVector4& V) const { return { X + V.X, Y + V.Y, Z + V.Z, W + V.W }; }
	constexpr FVector4 operator-(const FVector4& V) const { return { X - V.X, Y - V.Y, Z - V.Z, W - V.W }; }
	constexpr FVector4 operator*(const FVector4& V) const { return { X * V.X, Y * V.Y, Z * V.Z, W * V.W }; }
	constexpr FVector4 operator*(float Scale) const { return { X * Scale, Y * Scale, Z * Scale, W * Scale }; }
	constexpr FVector4 operator/(float Scale) const { return { X / Scale, Y / Scale, Z / Scale, W / Scale }; }
	constexpr FVector4 operator-() const { return { -X, -Y, -Z, -W }; }

	constexpr FVector4& operator+=(const FVector4& V) { X += V.X; Y += V.Y; Z += V.Z; W += V.W; return *this; }
	constexpr FVector4& operator-=(const FVector4& V) { X -= V.X; Y -= V.Y; Z -= V.Z; W -= V.W; return *this; }
	constexpr FVector4& operator*=(float Scale) { X *= Scale; Y *= Scale; Z *= Scale; W *= Scale; return *this; }

	constexpr bool operator==(const FVector4& V) const { return X == V.X && Y == V.Y && Z == V.Z && W == V.W; }
	constexpr bool operator!=(const FVector4& V) const { return !(*this == V); }

	float&       operator[](int32 Index) { return (&X)[Index]; }
	const float& operator[](int32 Index) const { return (&X)[Index]; }

	constexpr FVector3 XYZ() const { return { X, Y, Z }; }

	static constexpr float Dot(const FVector4& A, const FVector4& B) { return A.X * B.X + A.Y * B.Y + A.Z * B.Z + A.W * B.W; }

	constexpr bool Equals(const FVector4& V, float Tolerance = FMath::KindaSmallNumber) const
	{
		return FMath::Abs(X - V.X) <= Tolerance && FMath::Abs(Y - V.Y) <= Tolerance &&
		       FMath::Abs(Z - V.Z) <= Tolerance && FMath::Abs(W - V.W) <= Tolerance;
	}
};

inline const FVector4 FVector4::ZeroVector(0.0f, 0.0f, 0.0f, 0.0f);
inline const FVector4 FVector4::OneVector(1.0f, 1.0f, 1.0f, 1.0f);

constexpr FVector4 operator*(float Scale, const FVector4& V) { return V * Scale; }
