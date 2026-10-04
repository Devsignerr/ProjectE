# 쿠킹 도구 단위 테스트 (패키지 의존성 매니페스트 스캐너 — Tools/Cook/Source/PackageManifest.cpp를 함께 컴파일)
file(GLOB_RECURSE COOK_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(CookTests ${COOK_TEST_SOURCES})
target_include_directories(CookTests PRIVATE "${CMAKE_SOURCE_DIR}/Tools/Cook/Source")
target_link_libraries(CookTests PRIVATE ProjectE::Engine)

e_set_target_defaults(CookTests "Tests")
# 도구 소스는 이 폴더 밖이라 source_group(TREE) 뒤에 덧붙인다
target_sources(CookTests PRIVATE "${CMAKE_SOURCE_DIR}/Tools/Cook/Source/PackageManifest.cpp")

add_test(NAME CookTests COMMAND CookTests)