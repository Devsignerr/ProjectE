# 실내 절차적 생성 단위 테스트 (Scene/Building: 생성 코어 결정성·연결성·모듈/소품 규칙, 씬 적용·재생성 보존, JSON)
file(GLOB_RECURSE BUILDING_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(BuildingTests ${BUILDING_TEST_SOURCES})
target_link_libraries(BuildingTests PRIVATE ProjectE::Engine)

e_set_target_defaults(BuildingTests "Tests")

add_test(NAME BuildingTests COMMAND BuildingTests)
