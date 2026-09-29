# 게임 모듈 DLL 경계 테스트: SampleGame.dll을 실제로 로드해 타입 등록/ECS 타입 ID 공유/시스템 실행/언로드를 검증
file(GLOB_RECURSE GAME_MODULE_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(GameModuleTests ${GAME_MODULE_TEST_SOURCES})
target_link_libraries(GameModuleTests PRIVATE ProjectE::Engine)
target_include_directories(GameModuleTests PRIVATE "${CMAKE_SOURCE_DIR}/Projects/Sample/Source") # 게임 컴포넌트 헤더 (같은 타입을 테스트에서도 사용)
add_dependencies(GameModuleTests SampleGame)

e_set_target_defaults(GameModuleTests "Tests")

add_test(NAME GameModuleTests COMMAND GameModuleTests)
