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

# 공통 컴파일 옵션 (모든 타깃). 경고 레벨(/W4 /WX)은 자체 타깃에만 e_set_target_defaults가 건다 —
# 전역에 두면 서드파티의 /W0와 겹쳐 파일마다 D9025('/W4'을(를) '/W0'(으)로 재정의) 경고가 난다
add_compile_options(
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

# Release에도 PDB 생성 (배포 크래시 덤프 분석용 — 패키지에는 넣지 않고 따로 보관).
# /Z7(오브젝트에 내장)은 병렬 컴파일에서 PDB 잠금 경합이 없다. /DEBUG를 켜면 링커 최적화가 꺼지므로 명시한다
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:ProgramDatabase>$<$<CONFIG:Release>:Embedded>")
add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)

# PC별 로컬 설정 (gitignore): 예) set(E_STEAMWORKS_SDK_DIR "D:/SDK/steamworks_sdk_153a")
if(EXISTS "${CMAKE_SOURCE_DIR}/CMakeLocal.cmake")
    include("${CMAKE_SOURCE_DIR}/CMakeLocal.cmake")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake")
include(EngineTargets)
include(ThirdParty)

option(E_BUILD_TESTS "단위 테스트 빌드" ON)

add_subdirectory(Engine)
add_subdirectory(Sandbox)
add_subdirectory(Editor)
add_subdirectory(Runtime)
add_subdirectory(Server)
add_subdirectory(Tools)

# 프로젝트 게임 모듈 (Projects/<이름>/Source/CMakeLists.txt → <이름>Game.dll). 엔진 DLL 공유 빌드에서만 의미가 있다
if(E_ENGINE_SHARED)
    file(GLOB E_GAME_MODULE_DIRS LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/Projects/*/Source")
    foreach(_Dir IN LISTS E_GAME_MODULE_DIRS)
        if(EXISTS "${_Dir}/CMakeLists.txt")
            file(RELATIVE_PATH _Binary "${CMAKE_SOURCE_DIR}" "${_Dir}")
            add_subdirectory("${_Dir}" "${CMAKE_BINARY_DIR}/${_Binary}")
        endif()
    endforeach()
    get_property(_GameTargets GLOBAL PROPERTY E_GAME_MODULE_TARGETS)
    if(_GameTargets)
        add_dependencies(ProjectEEditor ${_GameTargets}) # F5/빌드 시 게임 모듈도 함께 빌드
        add_dependencies(ProjectERuntime ${_GameTargets})
    endif()
endif()

if(E_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT ProjectEEditor)
