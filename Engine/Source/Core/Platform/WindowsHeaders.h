#pragma once

// Windows.h를 최소 구성으로 포함한다.
// UNICODE / NOMINMAX / WIN32_LEAN_AND_MEAN 은 CMake에서 전역 정의.
// 플랫폼 코드(.cpp)에서만 포함하고, 공개 헤더에는 포함하지 않는다.
#include <Windows.h>
