#pragma once

#include "Core/Log.h"

// 조건 검증. 실패 시 Fatal 로그 후 중단.
// 모든 빌드 구성에서 활성화 (Shipping 구성 도입 시 재검토)
#define E_CHECK(Expression)                                                                 \
	do                                                                                      \
	{                                                                                       \
		if (!(Expression))                                                                  \
		{                                                                                   \
			E_LOG(LogCore, Fatal, "검증 실패: {} ({}:{})", #Expression, __FILE__, __LINE__); \
		}                                                                                   \
	} while (0)

// 추가 메시지를 포함하는 검증
#define E_CHECKF(Expression, Format, ...)                                                                      \
	do                                                                                                         \
	{                                                                                                          \
		if (!(Expression))                                                                                     \
		{                                                                                                      \
			E_LOG(LogCore, Fatal, "검증 실패: {} - " Format " ({}:{})", #Expression __VA_OPT__(,) __VA_ARGS__, \
			      __FILE__, __LINE__);                                                                         \
		}                                                                                                      \
	} while (0)
