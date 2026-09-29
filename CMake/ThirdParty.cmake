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

# ---------------------------------------------------------------- stb_image_write (v1.16, 같은 stb 커밋) — 스크린샷 PNG 저장
FetchContent_Declare(stb_image_write
    URL      "https://raw.githubusercontent.com/nothings/stb/${E_STB_COMMIT}/stb_image_write.h"
    URL_HASH SHA256=cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(stb_image_write)

add_library(stb_image_write INTERFACE)
add_library(ThirdParty::stb_image_write ALIAS stb_image_write)
target_include_directories(stb_image_write SYSTEM INTERFACE "${stb_image_write_SOURCE_DIR}")

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

# ---------------------------------------------------------------- miniaudio (0.11.25, Public Domain 또는 MIT-0) — 오디오 재생/디코딩(wav/flac/mp3)/3D 공간화
set(E_MINIAUDIO_COMMIT "9634bedb5b5a2ca38c1ee7108a9358a4e233f14d")
FetchContent_Declare(miniaudio
    URL      "https://raw.githubusercontent.com/mackron/miniaudio/${E_MINIAUDIO_COMMIT}/miniaudio.h"
    URL_HASH SHA256=ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(miniaudio)

# 구현부는 한 번만 컴파일 (프로젝트 언어가 CXX뿐이므로 C++로). 백엔드 설정 매크로는 구조체 배치에 영향을 주므로 PUBLIC으로 모든 사용처에 전파
file(WRITE "${CMAKE_BINARY_DIR}/ThirdParty/miniaudio_impl.cpp"
    "#define MINIAUDIO_IMPLEMENTATION\n#include \"miniaudio.h\"\n")
add_library(miniaudio STATIC "${CMAKE_BINARY_DIR}/ThirdParty/miniaudio_impl.cpp")
add_library(ThirdParty::miniaudio ALIAS miniaudio)
target_include_directories(miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
target_compile_definitions(miniaudio PUBLIC
    MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_WASAPI MA_ENABLE_NULL # Windows: WASAPI + 테스트용 null 장치
    MA_NO_ENCODING)
target_compile_options(miniaudio PRIVATE /W0)
set_target_properties(miniaudio PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- bc7enc_rdo (BC7 인코더 + rgbcx BC4/BC5 + BC7 디코더, MIT 또는 Unlicense) — 텍스처 쿠킹
# 저장소에 대용량 바이너리(ispc.exe)가 있어 필요한 파일만 개별로 받는다 (각 SHA256 고정)
set(E_BC7ENC_COMMIT "b9438627eef73a1157e84201b6fa6eb2ffd6d9f0")
set(E_BC7ENC_DIR "${CMAKE_BINARY_DIR}/_deps/bc7enc-src")
set(E_BC7ENC_FILES
    "LICENSE=b3e843763e8d3bdeb0f469ecd4107fc32b5fa27ebee1af10d2066bc339292800"
    "bc7enc.cpp=b90444a091530a13c61fa6375583fcc9bc9ca47e474fd71f818876de68c1b581"
    "bc7enc.h=f40f4a956ed6a8845b7909e83831de09512424408b32ca29551bf49aee2fecf6"
    "bc7decomp.cpp=cfbe156926ebd96443b4407a9da1b5b7a424fdf989fedca3cf2fd0515e0c368a"
    "bc7decomp.h=cbba8778640e0021f55ca686b6bc19e6960032539084c70af698922b05c5b242"
    "rgbcx.cpp=059243ce8e8e3ec634bc1e3da3996115aaf36196a8d75a24d3dbb3c8bfe0ca98"
    "rgbcx.h=111bd4c0211eca172bec9d757e2e597a47c4e4731bb122c9e8761dca85711bba"
    "rgbcx_table4.h=5051cf4ce17cb1f9d011ef788b7c8d174f2270532da0514c9f394830bab72f36")
foreach(_Entry IN LISTS E_BC7ENC_FILES)
    string(REPLACE "=" ";" _Pair "${_Entry}")
    list(GET _Pair 0 _File)
    list(GET _Pair 1 _Hash)
    file(DOWNLOAD "https://raw.githubusercontent.com/richgel999/bc7enc_rdo/${E_BC7ENC_COMMIT}/${_File}" "${E_BC7ENC_DIR}/${_File}"
        EXPECTED_HASH SHA256=${_Hash} TLS_VERIFY ON)
endforeach()

add_library(bc7enc STATIC "${E_BC7ENC_DIR}/bc7enc.cpp" "${E_BC7ENC_DIR}/bc7decomp.cpp" "${E_BC7ENC_DIR}/rgbcx.cpp")
add_library(ThirdParty::bc7enc ALIAS bc7enc)
target_include_directories(bc7enc SYSTEM PUBLIC "${E_BC7ENC_DIR}")
target_compile_options(bc7enc PRIVATE /W0)
set_target_properties(bc7enc PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- Lua 5.4.9 (MIT, lua.org 공식 tarball — 해시는 lua.org/ftp 게시 값)
# C++로 컴파일한다: 오류가 longjmp 대신 C++ 예외로 전파되어 바인딩 쪽 C++ 소멸자가 안전하게 실행된다 (sol2: SOL_USING_CXX_LUA)
FetchContent_Declare(lua
    URL      "https://www.lua.org/ftp/lua-5.4.9.tar.gz"
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6)
FetchContent_MakeAvailable(lua)

file(GLOB E_LUA_SOURCES "${lua_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM E_LUA_SOURCES "${lua_SOURCE_DIR}/src/lua.c" "${lua_SOURCE_DIR}/src/luac.c") # 독립 실행 파일 제외
set_source_files_properties(${E_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua STATIC ${E_LUA_SOURCES})
add_library(ThirdParty::lua ALIAS lua)
target_include_directories(lua SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")
target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(lua PRIVATE /W0 /TP)
set_target_properties(lua PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- sol2 v3.3.0 (MIT, Lua C++ 바인딩) — 헤더 전용
FetchContent_Declare(sol2
    URL      "https://github.com/ThePhD/sol2/archive/refs/tags/v3.3.0.zip"
    URL_HASH SHA256=a7489629c596c8a67108ad3603cb6a90073ba6647e50441c8c55492254190d67
    SOURCE_SUBDIR "none") # 자체 CMakeLists를 쓰지 않고 include만 사용
FetchContent_MakeAvailable(sol2)

add_library(sol2 INTERFACE)
add_library(ThirdParty::sol2 ALIAS sol2)
target_include_directories(sol2 SYSTEM INTERFACE "${sol2_SOURCE_DIR}/include")
target_compile_definitions(sol2 INTERFACE SOL_USING_CXX_LUA=1 SOL_ALL_SAFETIES_ON=1)
target_link_libraries(sol2 INTERFACE lua)

# ---------------------------------------------------------------- Jolt Physics (v5.6.0, MIT) — 강체 물리
# Build/CMakeLists.txt 사용 (하위 디렉터리라 샘플/테스트/뷰어는 만들지 않는다). 옵션은 엔진 설정과 맞춘다:
#   동적 CRT(/MD), 예외·RTTI 켜짐, 디버그 렌더러/프로파일러/컴퓨트/ObjectStream 끔, SSE4.2까지만 (AVX 계열은 /arch를 사용자 코드에 강제하므로 끔)
# JPH_* 정의는 Jolt 타깃이 PUBLIC으로 전파한다 (라이브러리와 사용자 코드가 반드시 같아야 함)
foreach(_Option
        USE_STATIC_MSVC_RUNTIME_LIBRARY OVERRIDE_CXX_FLAGS ENABLE_ALL_WARNINGS INTERPROCEDURAL_OPTIMIZATION ENABLE_INSTALL
        DEBUG_RENDERER_IN_DEBUG_AND_RELEASE PROFILER_IN_DEBUG_AND_RELEASE ENABLE_OBJECT_STREAM FLOATING_POINT_EXCEPTIONS_ENABLED
        JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE
        USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD
        TARGET_UNIT_TESTS TARGET_HELLO_WORLD TARGET_PERFORMANCE_TEST TARGET_SAMPLES TARGET_VIEWER)
    set(${_Option} OFF CACHE INTERNAL "")
endforeach()
foreach(_Option CPP_EXCEPTIONS_ENABLED CPP_RTTI_ENABLED USE_SSE4_1 USE_SSE4_2)
    set(${_Option} ON CACHE INTERNAL "")
endforeach()
FetchContent_Declare(jolt
    URL      "https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.zip"
    URL_HASH SHA256=0af9beea51637ef805e624fe838ea2870f7b68cd48cbe1615c853bd9bcf4f1d7
    SOURCE_SUBDIR "Build")
FetchContent_MakeAvailable(jolt)

add_library(ThirdParty::jolt ALIAS Jolt)
target_compile_options(Jolt PRIVATE /W0)
set_target_properties(Jolt PROPERTIES FOLDER "ThirdParty")
