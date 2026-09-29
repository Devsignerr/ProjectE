#pragma once

#include "Core/CoreTypes.h"

#include <cmath>

// 스칼라 수학 유틸리티와 상수
struct FMath
{
	static constexpr float Pi      = 3.14159265358979323846f;
	static constexpr float TwoPi   = Pi * 2.0f;
	static constexpr float HalfPi  = Pi * 0.5f;
	static constexpr float InvPi   = 1.0f / Pi;
	static constexpr float DegToRad = Pi / 180.0f;
	static constexpr float RadToDeg = 180.0f / Pi;

	static constexpr float SmallNumber      = 1.0e-8f; // 길이 제곱 등 0 판정용
	static constexpr float KindaSmallNumber = 1.0e-4f; // 일반적인 근사 비교 허용 오차

	static float Sqrt(float Value) { return std::sqrt(Value); }
	static float InvSqrt(float Value) { return 1.0f / std::sqrt(Value); }
	static float Sin(float Radians) { return std::sin(Radians); }
	static float Cos(float Radians) { return std::cos(Radians); }
	static float Tan(float Radians) { return std::tan(Radians); }
	static float Asin(float Value) { return std::asin(Clamp(Value, -1.0f, 1.0f)); }
	static float Acos(float Value) { return std::acos(Clamp(Value, -1.0f, 1.0f)); }
	static float Atan2(float Y, float X) { return std::atan2(Y, X); }
	static float Floor(float Value) { return std::floor(Value); }
	static float Ceil(float Value) { return std::ceil(Value); }
	static float Fmod(float X, float Y) { return std::fmod(X, Y); }

	static void SinCos(float* OutSin, float* OutCos, float Radians)
	{
		*OutSin = std::sin(Radians);
		*OutCos = std::cos(Radians);
	}

	template <typename T>
	static constexpr T Abs(T Value) { return Value < T(0) ? -Value : Value; }

	template <typename T>
	static constexpr T Min(T A, T B) { return A < B ? A : B; }

	template <typename T>
	static constexpr T Max(T A, T B) { return A > B ? A : B; }

	template <typename T>
	static constexpr T Clamp(T Value, T MinValue, T MaxValue) { return Value < MinValue ? MinValue : (Value > MaxValue ? MaxValue : Value); }

	template <typename T>
	static constexpr T Square(T Value) { return Value * Value; }

	// A에서 B로 Alpha(0~1)만큼 선형 보간
	template <typename T>
	static constexpr T Lerp(const T& A, const T& B, float Alpha) { return A + (B - A) * Alpha; }

	static constexpr float DegreesToRadians(float Degrees) { return Degrees * DegToRad; }
	static constexpr float RadiansToDegrees(float Radians) { return Radians * RadToDeg; }

	static constexpr bool IsNearlyEqual(float A, float B, float Tolerance = KindaSmallNumber) { return Abs(A - B) <= Tolerance; }
	static constexpr bool IsNearlyZero(float Value, float Tolerance = KindaSmallNumber) { return Abs(Value) <= Tolerance; }
};
