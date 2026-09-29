#pragma once

#include "Core/CoreTypes.h"

#include <format>
#include <string>
#include <string_view>
#include <vector>

// 로그 상세 수준 (값이 클수록 더 상세)
enum class ELogVerbosity : uint8
{
	Fatal = 0, // 출력 후 프로세스 중단
	Error,
	Warning,
	Display,   // 사용자에게 보여줄 주요 진행 상황
	Log,       // 일반 로그
	Verbose,   // 상세 디버깅용
};

// 로그 카테고리: 이름과 출력을 허용하는 최대 상세 수준
struct FLogCategory
{
	const char*   Name;
	ELogVerbosity MaxVerbosity;
};

struct FLogMessage
{
	uint64 Sequence = 0;
	ELogVerbosity Verbosity = ELogVerbosity::Log;
	std::string Text;
};

// 헤더에서 카테고리 선언, 하나의 .cpp에서 정의
#define E_DECLARE_LOG_CATEGORY(CategoryName) extern FLogCategory CategoryName;
#define E_DEFINE_LOG_CATEGORY(CategoryName, DefaultVerbosity) \
	FLogCategory CategoryName{ #CategoryName, ELogVerbosity::DefaultVerbosity };

class FLog
{
public:
	// 콘솔 UTF-8 / 색상 출력 설정
	static void Init();
	static void Shutdown();

	// 에디터 진입점에서 활성화하면 초기화 로그부터 제한된 개수만 보관한다.
	static constexpr size_t MaxHistoryMessages = 2000;
	static void EnableHistory();
	// 지정한 번호 이후의 로그만 복사한다. 로그 기록 스레드와 동기화된다.
	static std::vector<FLogMessage> ReadHistory(uint64 AfterSequence);

	static bool ShouldLog(const FLogCategory& Category, ELogVerbosity Verbosity);

	// 콘솔과 디버거 출력 창에 기록. Fatal이면 중단.
	static void Write(const FLogCategory& Category, ELogVerbosity Verbosity, std::string_view Message);
};

// 사용 예: E_LOG(LogCore, Display, "창 생성 완료: {}x{}", Width, Height);
// 포맷 문자열은 std::format 문법을 따른다.
#define E_LOG(CategoryName, Verbosity, Format, ...)                                                            \
	do                                                                                                         \
	{                                                                                                          \
		if (FLog::ShouldLog(CategoryName, ELogVerbosity::Verbosity))                                            \
		{                                                                                                      \
			FLog::Write(CategoryName, ELogVerbosity::Verbosity, std::format(Format __VA_OPT__(,) __VA_ARGS__)); \
		}                                                                                                      \
	} while (0)

E_DECLARE_LOG_CATEGORY(LogCore)
