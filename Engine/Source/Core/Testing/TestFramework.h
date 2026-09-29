#pragma once

#include "Core/CoreMinimal.h"

#include <cmath>
#include <format>
#include <string>

// 경량 단위 테스트 프레임워크.
// 사용:
//   E_TEST(VectorDot) { E_EXPECT_NEAR(FVector3::Dot(A, B), 1.0f, 1e-5f); }
//   int main() { FLog::Init(); return FTestRegistry::RunAll() == 0 ? 0 : 1; }

using FTestFunction = void (*)();

class FTestRegistry
{
public:
	static void Register(const char* Name, FTestFunction Function);

	// 등록된 모든 테스트 실행. 실패한 테스트 수를 반환.
	static int32 RunAll();

	static void ReportFailure(const char* File, int32 Line, const std::string& Message);
};

struct FTestAutoRegister
{
	FTestAutoRegister(const char* Name, FTestFunction Function) { FTestRegistry::Register(Name, Function); }
};

#define E_TEST(TestName)                                                                              \
	static void TestFunction_##TestName();                                                            \
	static const FTestAutoRegister TestRegister_##TestName(#TestName, &TestFunction_##TestName);      \
	static void TestFunction_##TestName()

#define E_EXPECT_TRUE(Condition)                                                                      \
	do                                                                                                \
	{                                                                                                 \
		if (!(Condition))                                                                             \
		{                                                                                             \
			FTestRegistry::ReportFailure(__FILE__, __LINE__, "기대: " #Condition);                     \
		}                                                                                             \
	} while (0)

#define E_EXPECT_FALSE(Condition) E_EXPECT_TRUE(!(Condition))

#define E_EXPECT_EQ(Actual, Expected)                                                                 \
	do                                                                                                \
	{                                                                                                 \
		const auto ActualValue_   = (Actual);                                                         \
		const auto ExpectedValue_ = (Expected);                                                       \
		if (!(ActualValue_ == ExpectedValue_))                                                        \
		{                                                                                             \
			FTestRegistry::ReportFailure(__FILE__, __LINE__,                                          \
			                             std::format("기대: {} == {} (실제 {}, 기대 {})",              \
			                                         #Actual, #Expected, ActualValue_, ExpectedValue_)); \
		}                                                                                             \
	} while (0)

#define E_EXPECT_NEAR(Actual, Expected, Tolerance)                                                    \
	do                                                                                                \
	{                                                                                                 \
		const float ActualValue_   = static_cast<float>(Actual);                                      \
		const float ExpectedValue_ = static_cast<float>(Expected);                                    \
		if (!(std::abs(ActualValue_ - ExpectedValue_) <= (Tolerance)))                                \
		{                                                                                             \
			FTestRegistry::ReportFailure(__FILE__, __LINE__,                                          \
			                             std::format("기대: {} ≈ {} (실제 {}, 기대 {}, 허용 {})",       \
			                                         #Actual, #Expected, ActualValue_, ExpectedValue_, \
			                                         (Tolerance)));                                   \
		}                                                                                             \
	} while (0)

// Equals(Other, Tolerance) 멤버를 가진 타입(FVector3, FQuat, FMatrix4x4 등)용
#define E_EXPECT_EQUALS(Actual, Expected, Tolerance)                                                  \
	do                                                                                                \
	{                                                                                                 \
		if (!(Actual).Equals((Expected), (Tolerance)))                                                \
		{                                                                                             \
			FTestRegistry::ReportFailure(__FILE__, __LINE__, "기대: " #Actual " ≈ " #Expected);        \
		}                                                                                             \
	} while (0)
