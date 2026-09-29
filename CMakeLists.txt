cmake_minimum_required(VERSION 3.25)

project(ProjectE
    VERSION 0.1.0
    DESCRIPTION "자체 3D 게임 엔진"
    LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "ProjectE는 현재 Windows + MSVC(DirectX 12)만 지원합니다.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Debug / Release 두 구성만 사용
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
endif()

# Visual Studio 솔루션 폴더 구조
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")

# 출력 디렉터리
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Lib")

# 공통 컴파일 옵션 (모든 타깃)
add_compile_options(
    /W4             # 경고 레벨 4
    /permissive-    # 표준 준수 모드
    /utf-8          # 소스/실행 문자 집합 UTF-8 (한글 주석·로그)
    /Zc:preprocessor # 표준 준수 전처리기 (__VA_OPT__ 지원)
    /Zc:__cplusplus
    /EHsc
    /MP             # 병렬 컴파일
)
add_compile_definitions(
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    $<IF:$<CONFIG:Debug>,E_DEBUG=1,E_DEBUG=0>
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake")
include(EngineTargets)
include(ThirdParty)

option(E_BUILD_TESTS "단위 테스트 빌드" ON)

add_subdirectory(Engine)
add_subdirectory(Sandbox)
add_subdirectory(Editor)
add_subdirectory(Runtime)
add_subdirectory(Tools)

if(E_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT ProjectEEditor)
