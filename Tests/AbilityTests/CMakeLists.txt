# 능력 시스템 단위 테스트 (Scene/Ability: 태그·수정자·효과 쌓기/주기/면역·쿨다운/비용·C++/Lua 능력·예측 확정/거절(루프백)·데이터 테이블)
file(GLOB_RECURSE ABILITY_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(AbilityTests ${ABILITY_TEST_SOURCES})
target_link_libraries(AbilityTests PRIVATE ProjectE::Engine)

e_set_target_defaults(AbilityTests "Tests")

add_test(NAME AbilityTests COMMAND AbilityTests)
