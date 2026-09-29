#pragma once

// sol2(+Lua) 포함 전용 헤더. Scripting 모듈 .cpp에서만 포함한다 (공개 헤더에 sol 타입을 노출하지 않음).
// Lua는 C++로 컴파일되므로 SOL_USING_CXX_LUA=1 (ThirdParty::sol2가 정의)
#pragma warning(push, 0)
#include <sol/sol.hpp>
#pragma warning(pop)
