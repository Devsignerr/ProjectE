# 외부 라이브러리 (CMake FetchContent). 버전은 커밋/해시로 고정한다.
# 방침: 핵심(창/입력/수학/RHI/렌더러/ECS)은 직접 구현, 이미지·모델 로딩·UI 등은 라이브러리 사용.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- stb_image (v2.30)
set(E_STB_COMMIT "2c980bb59875b0d32144a71867fbdebb2f77cd20")
FetchContent_Declare(stb_image
    URL      "https://raw.githubusercontent.com/nothings/stb/${E_STB_COMMIT}/stb_image.h"
    URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(stb_image)

add_library(stb_image INTERFACE)
add_library(ThirdParty::stb_image ALIAS stb_image)
target_include_directories(stb_image SYSTEM INTERFACE "${stb_image_SOURCE_DIR}")

# ---------------------------------------------------------------- cgltf (glTF 2.0 파서, MIT)
set(E_CGLTF_COMMIT "85cd62382dfea638278962690cf515023f33ed00")
FetchContent_Declare(cgltf
    URL      "https://raw.githubusercontent.com/jkuhlmann/cgltf/${E_CGLTF_COMMIT}/cgltf.h"
    URL_HASH SHA256=efb169dee911696b5d35fc8e3f7ea0c56d679debc529eba9ca6aa6443ba9d5e9
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(cgltf)

add_library(cgltf INTERFACE)
add_library(ThirdParty::cgltf ALIAS cgltf)
target_include_directories(cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")

# ---------------------------------------------------------------- Dear ImGui (docking 브랜치, 1.93.0 WIP, MIT)
set(E_IMGUI_COMMIT "64944b4520b30772de8dbf0b37d0311746477a32")
FetchContent_Declare(imgui
    URL      "https://github.com/ocornut/imgui/archive/${E_IMGUI_COMMIT}.zip"
    URL_HASH SHA256=18a0c1b583c4e89675e8da8a733a607713a431db5e4a0044c65b980466a8a641)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp")
add_library(ThirdParty::imgui ALIAS imgui)
target_include_directories(imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(imgui PUBLIC d3d12 dxgi)
target_compile_options(imgui PRIVATE /W0) # 서드파티 경고 무시 (전역 /W4를 마지막 지정이 덮어씀)
set_target_properties(imgui PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- ImGuizmo (트랜스폼 기즈모, MIT)
set(E_IMGUIZMO_COMMIT "18cef5e031d8c6973d80284c67f60549fafd78c1")
FetchContent_Declare(imguizmo
    URL      "https://github.com/CedricGuillemet/ImGuizmo/archive/${E_IMGUIZMO_COMMIT}.zip"
    URL_HASH SHA256=f9c71db12f0c726157dac94eef7d8bd8f5551da3494496f660e6e3c39e31ac4c
    SOURCE_SUBDIR "src") # 자체 CMakeLists(전체 위젯 빌드, imgui 미링크)를 쓰지 않고 아래에서 필요한 소스만 직접 빌드
FetchContent_MakeAvailable(imguizmo)

add_library(imguizmo STATIC "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
add_library(ThirdParty::imguizmo ALIAS imguizmo)
target_include_directories(imguizmo SYSTEM PUBLIC "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(imguizmo PUBLIC imgui)
target_compile_options(imguizmo PRIVATE /W0)
set_target_properties(imguizmo PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- nlohmann/json (v3.11.3, MIT) — 단일 헤더
FetchContent_Declare(nlohmann_json
    URL      "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
    URL_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(nlohmann_json INTERFACE)
add_library(ThirdParty::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json SYSTEM INTERFACE "${nlohmann_json_SOURCE_DIR}")
