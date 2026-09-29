# Scripting 모듈 단위 테스트 (Lua 스크립트 컴포넌트, 리플렉션 바인딩, 핫 리로드, 씬 스냅샷)
file(GLOB_RECURSE SCRIPTING_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(ScriptingTests ${SCRIPTING_TEST_SOURCES})
target_link_libraries(ScriptingTests PRIVATE ProjectE::Scripting ProjectE::Scene)

e_set_target_defaults(ScriptingTests "Tests")

add_test(NAME ScriptingTests COMMAND ScriptingTests)
