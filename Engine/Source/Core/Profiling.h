#pragma once

#include "Core/CoreTypes.h"

// Tracy 프로파일러 래퍼 (CMake 옵션 E_TRACY). 꺼져 있으면 모든 매크로가 빈 문장 → 비용 0.
//   Tracy 헤더는 노출하지 않는다: 존은 Tracy C API와 같은 배치의 정적 위치 구조체 + 엔진 DLL 함수 호출.
//   Tracy 클라이언트는 엔진 DLL 안 하나 (실행 파일/게임 모듈/테스트도 이 함수들을 거쳐 같은 인스턴스를 쓴다).
//   수명: FApplication::Run이 Profiling::Startup/Shutdown (패키지 게임은 --tracy일 때만, 테스트는 시작하지 않음 → 존 무시).
//   켜져 있어도 뷰어가 연결되기 전에는 기록하지 않는다 (TRACY_ON_DEMAND), 로컬호스트만 받는다 (TRACY_ONLY_LOCALHOST).
//
//   E_PROFILE_SCOPE("이름")     이 블록 끝까지 CPU 존 (이름은 문자열 리터럴 — 정적 수명)
//   E_PROFILE_FUNCTION()        함수 이름으로 CPU 존
//   E_PROFILE_FRAME()           프레임 경계 (앱 루프가 한 번)
//   E_PROFILE_PLOT("이름", 값)  값 그래프 (이름은 리터럴)
//   E_PROFILE_THREAD_NAME("이름")

#if E_TRACY

// Tracy C API ___tracy_source_location_data와 같은 배치 (Profiling.cpp에서 static_assert)
struct FProfileSourceLocation
{
	const char* Name;
	const char* Function;
	const char* File;
	uint32      Line;
	uint32      Color;
};

// Tracy C API ___tracy_c_zone_context와 같은 배치
struct FProfileZone
{
	uint32 Id     = 0;
	int32  Active = 0;
};

namespace Profiling
{
	// 프로파일러 시작/종료 (FApplication이 부른다). 시작 전·종료 후의 존/프레임 호출은 무시된다
	void Startup();
	void Shutdown();
	bool IsStarted();
	bool IsConnected(); // 뷰어가 연결되어 기록 중

	FProfileZone BeginZone(const FProfileSourceLocation* Location);
	void         EndZone(FProfileZone Zone);
	void         FrameMark();
	void         Plot(const char* Name, double Value);
	void         SetThreadName(const char* Name);
} // namespace Profiling

class FProfileScope
{
public:
	explicit FProfileScope(const FProfileSourceLocation* Location) : Zone(Profiling::BeginZone(Location)) {}
	~FProfileScope() { Profiling::EndZone(Zone); }
	FProfileScope(const FProfileScope&)            = delete;
	FProfileScope& operator=(const FProfileScope&) = delete;

private:
	FProfileZone Zone;
};

#define E_PROFILE_CONCAT_INNER(A, B) A##B
#define E_PROFILE_CONCAT(A, B)       E_PROFILE_CONCAT_INNER(A, B)
#define E_PROFILE_SCOPE(Name)                                                                                                   \
	static constexpr FProfileSourceLocation E_PROFILE_CONCAT(ProfileLocation_, __LINE__){ Name, __FUNCTION__, __FILE__,          \
	                                                                                      static_cast<uint32>(__LINE__), 0 };   \
	const FProfileScope E_PROFILE_CONCAT(ProfileScope_, __LINE__)(&E_PROFILE_CONCAT(ProfileLocation_, __LINE__))
#define E_PROFILE_FUNCTION()               E_PROFILE_SCOPE(nullptr)
#define E_PROFILE_FRAME()                  Profiling::FrameMark()
#define E_PROFILE_PLOT(Name, Value)        Profiling::Plot(Name, static_cast<double>(Value))
#define E_PROFILE_THREAD_NAME(Name)        Profiling::SetThreadName(Name)

#else

namespace Profiling
{
	inline void Startup() {}
	inline void Shutdown() {}
	inline bool IsStarted() { return false; }
	inline bool IsConnected() { return false; }
} // namespace Profiling

#define E_PROFILE_SCOPE(Name)              do { } while (0)
#define E_PROFILE_FUNCTION()               do { } while (0)
#define E_PROFILE_FRAME()                  do { } while (0)
#define E_PROFILE_PLOT(Name, Value)        do { } while (0)
#define E_PROFILE_THREAD_NAME(Name)        do { } while (0)

#endif
