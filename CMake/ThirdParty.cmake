# 외부 라이브러리 (CMake FetchContent). 버전은 커밋/해시로 고정한다.
# 방침: 핵심(창/입력/수학/RHI/렌더러/ECS)은 직접 구현, 이미지·모델 로딩·UI 등은 라이브러리 사용.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- 소스 캐시 (모든 빌드 폴더 공유)
# 소스는 Build/_deps/<이름>-src 한 곳에 받고 ninja-debug/ninja-release/vs2022가 같이 쓴다 (빌드 결과물 <이름>-build는 폴더마다 따로).
# 한 번 받은 뒤에는 FETCHCONTENT_SOURCE_DIR_<이름>으로 그 폴더를 바로 써서 다운로드와 하위 빌드(VS 제너레이터에서 느림)를 건너뛴다.
# 선언(URL/해시/옵션)이나 인자로 넘긴 파일(패치 스크립트 등)의 내용이 바뀌면 표식(<이름>.populated)이 달라져 다시 받는다.
# 강제로 다시 받으려면 Build/_deps/<이름>.populated를 지운다
set(E_THIRDPARTY_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/Build/_deps" CACHE PATH "서드파티 소스 공유 폴더")

function(e_fetchcontent_declare Name)
    set(Source "${E_THIRDPARTY_SOURCE_CACHE}/${Name}-src")
    set(KeyText "${ARGN}")
    foreach(Argument IN LISTS ARGN)
        if(IS_ABSOLUTE "${Argument}" AND EXISTS "${Argument}" AND NOT IS_DIRECTORY "${Argument}")
            file(SHA256 "${Argument}" FileHash)
            string(APPEND KeyText ";${FileHash}")
        endif()
    endforeach()
    string(SHA256 Key "${KeyText}")
    set_property(GLOBAL PROPERTY E_FETCH_KEY_${Name} "${Key}")

    string(TOUPPER "${Name}" UpperName)
    set(Marker "${E_THIRDPARTY_SOURCE_CACHE}/${Name}.populated")
    if(EXISTS "${Marker}" AND IS_DIRECTORY "${Source}")
        file(READ "${Marker}" StoredKey)
        if(StoredKey STREQUAL Key)
            set(FETCHCONTENT_SOURCE_DIR_${UpperName} "${Source}" PARENT_SCOPE) # 받아 둔 소스를 그대로 (다운로드/하위 빌드 없음)
        endif()
    endif()
    FetchContent_Declare(${Name} ${ARGN} SOURCE_DIR "${Source}")
endfunction()

macro(e_fetchcontent_make_available)
    FetchContent_MakeAvailable(${ARGN})
    foreach(_EFetchName ${ARGN})
        get_property(_EFetchKey GLOBAL PROPERTY E_FETCH_KEY_${_EFetchName})
        file(WRITE "${E_THIRDPARTY_SOURCE_CACHE}/${_EFetchName}.populated" "${_EFetchKey}")
    endforeach()
endmacro()

# ---------------------------------------------------------------- stb_image (v2.30)
set(E_STB_COMMIT "2c980bb59875b0d32144a71867fbdebb2f77cd20")
e_fetchcontent_declare(stb_image
    URL      "https://raw.githubusercontent.com/nothings/stb/${E_STB_COMMIT}/stb_image.h"
    URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
    DOWNLOAD_NO_EXTRACT TRUE)
e_fetchcontent_make_available(stb_image)

add_library(stb_image INTERFACE)
add_library(ThirdParty::stb_image ALIAS stb_image)
target_include_directories(stb_image SYSTEM INTERFACE "${stb_image_SOURCE_DIR}")

# ---------------------------------------------------------------- stb_image_write (v1.16, 같은 stb 커밋) — 스크린샷 PNG 저장
e_fetchcontent_declare(stb_image_write
    URL      "https://raw.githubusercontent.com/nothings/stb/${E_STB_COMMIT}/stb_image_write.h"
    URL_HASH SHA256=cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
    DOWNLOAD_NO_EXTRACT TRUE)
e_fetchcontent_make_available(stb_image_write)

add_library(stb_image_write INTERFACE)
add_library(ThirdParty::stb_image_write ALIAS stb_image_write)
target_include_directories(stb_image_write SYSTEM INTERFACE "${stb_image_write_SOURCE_DIR}")

# ---------------------------------------------------------------- cgltf (glTF 2.0 파서, MIT)
set(E_CGLTF_COMMIT "85cd62382dfea638278962690cf515023f33ed00")
e_fetchcontent_declare(cgltf
    URL      "https://raw.githubusercontent.com/jkuhlmann/cgltf/${E_CGLTF_COMMIT}/cgltf.h"
    URL_HASH SHA256=efb169dee911696b5d35fc8e3f7ea0c56d679debc529eba9ca6aa6443ba9d5e9
    DOWNLOAD_NO_EXTRACT TRUE)
e_fetchcontent_make_available(cgltf)

add_library(cgltf INTERFACE)
add_library(ThirdParty::cgltf ALIAS cgltf)
target_include_directories(cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")

# ---------------------------------------------------------------- Dear ImGui (docking 브랜치, 1.93.0 WIP, MIT)
set(E_IMGUI_COMMIT "64944b4520b30772de8dbf0b37d0311746477a32")
e_fetchcontent_declare(imgui
    URL      "https://github.com/ocornut/imgui/archive/${E_IMGUI_COMMIT}.zip"
    URL_HASH SHA256=18a0c1b583c4e89675e8da8a733a607713a431db5e4a0044c65b980466a8a641)
e_fetchcontent_make_available(imgui)

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
target_compile_options(imgui PRIVATE /W0) # 서드파티 경고 무시 (/W4는 자체 타깃에만 — e_set_target_defaults)
set_target_properties(imgui PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- stb_truetype (imgui 동봉 imstb_truetype.h 재사용 — 게임 UI SDF 글꼴, 추가 다운로드 없음)
add_library(stb_truetype INTERFACE)
add_library(ThirdParty::stb_truetype ALIAS stb_truetype)
target_include_directories(stb_truetype SYSTEM INTERFACE "${imgui_SOURCE_DIR}")

# ---------------------------------------------------------------- ImGuizmo (트랜스폼 기즈모, MIT)
set(E_IMGUIZMO_COMMIT "18cef5e031d8c6973d80284c67f60549fafd78c1")
e_fetchcontent_declare(imguizmo
    URL      "https://github.com/CedricGuillemet/ImGuizmo/archive/${E_IMGUIZMO_COMMIT}.zip"
    URL_HASH SHA256=f9c71db12f0c726157dac94eef7d8bd8f5551da3494496f660e6e3c39e31ac4c
    SOURCE_SUBDIR "src") # 자체 CMakeLists(전체 위젯 빌드, imgui 미링크)를 쓰지 않고 아래에서 필요한 소스만 직접 빌드
e_fetchcontent_make_available(imguizmo)

add_library(imguizmo STATIC "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp")
add_library(ThirdParty::imguizmo ALIAS imguizmo)
target_include_directories(imguizmo SYSTEM PUBLIC "${imguizmo_SOURCE_DIR}/src")
target_link_libraries(imguizmo PUBLIC imgui)
target_compile_options(imguizmo PRIVATE /W0)
set_target_properties(imguizmo PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- imgui-node-editor (노드 그래프 편집기, 에디터 전용, MIT)
# master 2026-02-20 ("fixing for modern imgui" 이후). 예제/외부 의존은 쓰지 않고 라이브러리 소스만 빌드한다
set(E_IMGUI_NODE_EDITOR_COMMIT "021aa0ea4da13fed864bafb2a92d4c5205076866")
e_fetchcontent_declare(imgui_node_editor
    URL      "https://github.com/thedmd/imgui-node-editor/archive/${E_IMGUI_NODE_EDITOR_COMMIT}.zip"
    URL_HASH SHA256=ffc7a1d6868e7d00a52bb0c54a3baa32c19085e15992372dcb9d9a1ac9a461b6
    PATCH_COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/Patches/ImGuiNodeEditor.cmake" # ImGui 1.92+ 연산자 중복 정의
    SOURCE_SUBDIR "_none") # 루트 CMakeLists(예제)는 쓰지 않는다
e_fetchcontent_make_available(imgui_node_editor)

add_library(imgui_node_editor STATIC
    "${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp"
    "${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp"
    "${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp"
    "${imgui_node_editor_SOURCE_DIR}/crude_json.cpp")
add_library(ThirdParty::imgui_node_editor ALIAS imgui_node_editor)
target_include_directories(imgui_node_editor SYSTEM PUBLIC "${imgui_node_editor_SOURCE_DIR}")
target_link_libraries(imgui_node_editor PUBLIC imgui)
target_compile_options(imgui_node_editor PRIVATE /W0)
set_target_properties(imgui_node_editor PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- nlohmann/json (v3.11.3, MIT) — 단일 헤더
e_fetchcontent_declare(nlohmann_json
    URL      "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
    URL_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
    DOWNLOAD_NO_EXTRACT TRUE)
e_fetchcontent_make_available(nlohmann_json)

add_library(nlohmann_json INTERFACE)
add_library(ThirdParty::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json SYSTEM INTERFACE "${nlohmann_json_SOURCE_DIR}")

# ---------------------------------------------------------------- miniaudio (0.11.25, Public Domain 또는 MIT-0) — 오디오 재생/디코딩(wav/flac/mp3)/3D 공간화
set(E_MINIAUDIO_COMMIT "9634bedb5b5a2ca38c1ee7108a9358a4e233f14d")
e_fetchcontent_declare(miniaudio
    URL      "https://raw.githubusercontent.com/mackron/miniaudio/${E_MINIAUDIO_COMMIT}/miniaudio.h"
    URL_HASH SHA256=ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
    DOWNLOAD_NO_EXTRACT TRUE)
e_fetchcontent_make_available(miniaudio)

# 구현부는 한 번만 컴파일 (프로젝트 언어가 CXX뿐이므로 C++로). 백엔드 설정 매크로는 구조체 배치에 영향을 주므로 PUBLIC으로 모든 사용처에 전파
# file(CONFIGURE)는 내용이 같으면 파일을 다시 쓰지 않는다 — file(WRITE)는 구성할 때마다 시각을 바꿔 miniaudio → 엔진 DLL을
# 다시 링크시켰다 (에디터가 엔진 DLL을 잡고 있는 동안 게임 모듈만 다시 빌드하려 해도 LNK1168로 실패)
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/ThirdParty/miniaudio_impl.cpp"
    CONTENT "#define MINIAUDIO_IMPLEMENTATION\n#include \"miniaudio.h\"\n")
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
set(E_BC7ENC_DIR "${E_THIRDPARTY_SOURCE_CACHE}/bc7enc-src") # 소스 캐시 공유 (file(DOWNLOAD)는 해시가 맞는 파일이 있으면 받지 않는다)
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
e_fetchcontent_declare(lua
    URL      "https://www.lua.org/ftp/lua-5.4.9.tar.gz"
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6)
e_fetchcontent_make_available(lua)

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
e_fetchcontent_declare(sol2
    URL      "https://github.com/ThePhD/sol2/archive/refs/tags/v3.3.0.zip"
    URL_HASH SHA256=a7489629c596c8a67108ad3603cb6a90073ba6647e50441c8c55492254190d67
    SOURCE_SUBDIR "none") # 자체 CMakeLists를 쓰지 않고 include만 사용
e_fetchcontent_make_available(sol2)

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
        TARGET_UNIT_TESTS TARGET_HELLO_WORLD TARGET_PERFORMANCE_TEST TARGET_SAMPLES TARGET_VIEWER
        GENERATE_DEBUG_SYMBOLS) # 디버그 정보는 전역 CMAKE_MSVC_DEBUG_INFORMATION_FORMAT(Release /Z7)를 따른다 — 켜면 /Zi가 덧붙어 D9025 경고
    set(${_Option} OFF CACHE INTERNAL "")
endforeach()
foreach(_Option CPP_EXCEPTIONS_ENABLED CPP_RTTI_ENABLED USE_SSE4_1 USE_SSE4_2)
    set(${_Option} ON CACHE INTERNAL "")
endforeach()
e_fetchcontent_declare(jolt
    URL      "https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.zip"
    URL_HASH SHA256=0af9beea51637ef805e624fe838ea2870f7b68cd48cbe1615c853bd9bcf4f1d7
    SOURCE_SUBDIR "Build")
e_fetchcontent_make_available(jolt)

add_library(ThirdParty::jolt ALIAS Jolt)
target_compile_options(Jolt PRIVATE /W0)
set_target_properties(Jolt PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- Recast/Detour (v1.6.0, zlib) — 내비메시 굽기 + 경로 탐색
# 자체 CMakeLists(데모/테스트/설치)를 쓰지 않고 Recast, Detour, DetourCrowd 소스만 정적 라이브러리로 빌드한다
e_fetchcontent_declare(recastnavigation
    URL      "https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.zip"
    URL_HASH SHA256=8b50c62177249554b226514b307e6c529934a62b3926822777bf6abb8906a155
    SOURCE_SUBDIR "_none")
e_fetchcontent_make_available(recastnavigation)

file(GLOB E_RECAST_SOURCES
    "${recastnavigation_SOURCE_DIR}/Recast/Source/*.cpp"
    "${recastnavigation_SOURCE_DIR}/Detour/Source/*.cpp"
    "${recastnavigation_SOURCE_DIR}/DetourCrowd/Source/*.cpp")
add_library(recast STATIC ${E_RECAST_SOURCES})
add_library(ThirdParty::recast ALIAS recast)
target_include_directories(recast SYSTEM PUBLIC
    "${recastnavigation_SOURCE_DIR}/Recast/Include"
    "${recastnavigation_SOURCE_DIR}/Detour/Include"
    "${recastnavigation_SOURCE_DIR}/DetourCrowd/Include")
target_compile_options(recast PRIVATE /W0)
set_target_properties(recast PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- Font Awesome 6 Free Solid (아이콘 글꼴, SIL OFL 1.1) + IconFontCppHeaders (zlib)
# 에디터 UI 아이콘. 글꼴은 실행 파일에 바이트 배열로 넣어 경로 의존 없이 쓴다 (ImGui AddFontFromMemoryTTF)
e_fetchcontent_declare(fontawesome_font
    URL      "https://raw.githubusercontent.com/FortAwesome/Font-Awesome/6.7.2/webfonts/fa-solid-900.ttf"
    URL_HASH SHA256=af19d135d3a935b3ebfbd80320716ffe1202052c5f68dc2c5f1abc57005ac605
    DOWNLOAD_NO_EXTRACT TRUE)
e_fetchcontent_make_available(fontawesome_font)
set(E_ICON_HEADERS_COMMIT "210b5a399a64270674560d633638952d1e8d804d")
e_fetchcontent_declare(icon_font_headers
    URL      "https://raw.githubusercontent.com/juliettef/IconFontCppHeaders/${E_ICON_HEADERS_COMMIT}/IconsFontAwesome6.h"
    URL_HASH SHA256=1986b023825b269fb8c60ccc2419c4594280249596e828e573d5c8ef309e265d
    DOWNLOAD_NO_EXTRACT TRUE)
e_fetchcontent_make_available(icon_font_headers)

set(E_FONTAWESOME_SOURCE "${CMAKE_BINARY_DIR}/Generated/FontAwesomeSolid.cpp")
if(NOT EXISTS "${E_FONTAWESOME_SOURCE}")
    file(READ "${fontawesome_font_SOURCE_DIR}/fa-solid-900.ttf" _FontHex HEX)
    string(LENGTH "${_FontHex}" _FontHexLength)
    math(EXPR _FontSize "${_FontHexLength} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _FontBytes "${_FontHex}")
    file(WRITE "${E_FONTAWESOME_SOURCE}"
        "// 자동 생성 (CMake/ThirdParty.cmake): Font Awesome 6.7.2 Free Solid\n"
        "extern const unsigned char GFontAwesomeSolidData[] = {${_FontBytes}};\n"
        "extern const unsigned int GFontAwesomeSolidSize = ${_FontSize};\n")
endif()
add_library(fontawesome STATIC "${E_FONTAWESOME_SOURCE}")
add_library(ThirdParty::fontawesome ALIAS fontawesome)
target_include_directories(fontawesome SYSTEM PUBLIC "${icon_font_headers_SOURCE_DIR}")
target_compile_options(fontawesome PRIVATE /W0)
set_target_properties(fontawesome PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- ufbx v0.23.1 (FBX 로더, MIT/퍼블릭 도메인 중 택일 — MIT)
set(E_UFBX_COMMIT "26a482ae66871d7de36eb722aa060bce95bce274")
e_fetchcontent_declare(ufbx_source
    URL      "https://raw.githubusercontent.com/ufbx/ufbx/${E_UFBX_COMMIT}/ufbx.c"
    URL_HASH SHA256=4a956c26a708e40d82ecb0aeaee54da709d5ba7c144c9642174970059bda7e1d
    DOWNLOAD_NO_EXTRACT TRUE)
e_fetchcontent_declare(ufbx_header
    URL      "https://raw.githubusercontent.com/ufbx/ufbx/${E_UFBX_COMMIT}/ufbx.h"
    URL_HASH SHA256=f787be529af577efc04f3e3e735844a08556718dbb613d1211c2b313d72895dc
    DOWNLOAD_NO_EXTRACT TRUE)
e_fetchcontent_make_available(ufbx_source ufbx_header)

# 프로젝트 언어가 CXX뿐이므로 C++로 컴파일 (ufbx는 C/C++ 양쪽 컴파일을 지원)
add_library(ufbx STATIC "${ufbx_source_SOURCE_DIR}/ufbx.c")
set_source_files_properties("${ufbx_source_SOURCE_DIR}/ufbx.c" PROPERTIES LANGUAGE CXX)
add_library(ThirdParty::ufbx ALIAS ufbx)
target_include_directories(ufbx SYSTEM PUBLIC "${ufbx_header_SOURCE_DIR}")
target_compile_options(ufbx PRIVATE /W0)
set_target_properties(ufbx PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- protobuf v21.12 (BSD-3) — GameNetworkingSockets 메시지 직렬화 전용
# 22 이상은 abseil이 필수라 abseil 없이 빌드되는 마지막 버전으로 고정. 정적 라이브러리 + 동적 CRT(/MD).
# protoc는 빌드 중 GNS .proto 코드 생성에만 쓴다. 엔진 코드는 protobuf를 직접 쓰지 않는다
set(protobuf_BUILD_TESTS OFF CACHE INTERNAL "")
set(protobuf_BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(protobuf_MSVC_STATIC_RUNTIME OFF CACHE INTERNAL "")
set(protobuf_WITH_ZLIB OFF CACHE INTERNAL "")
set(protobuf_INSTALL ON CACHE INTERNAL "") # 설치는 안 하지만 GNS install(EXPORT)가 libprotobuf의 export 세트를 요구한다
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE INTERNAL "")
e_fetchcontent_declare(protobuf
    URL      "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.tar.gz"
    URL_HASH SHA256=22fdaf641b31655d4b2297f9981fa5203b2866f8332d3c6333f6b0107bb320de
    OVERRIDE_FIND_PACKAGE) # GNS의 find_package(Protobuf)가 이 소스 빌드를 찾게 한다
e_fetchcontent_make_available(protobuf)
foreach(_Target libprotobuf libprotobuf-lite libprotoc protoc)
    target_compile_options(${_Target} PRIVATE /W0)
    set_target_properties(${_Target} PROPERTIES FOLDER "ThirdParty/protobuf")
endforeach()

# find_package 리디렉트 스텁이 포함하는 추가 파일: 설치형 protobuf 설정 파일이 주던 protobuf_generate_cpp를 소스 빌드 protoc로 제공
file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/protobuf-extra.cmake" [=[
function(protobuf_generate_cpp OutSources OutHeaders)
    set(_Sources)
    set(_Headers)
    foreach(_Proto ${ARGN})
        get_filename_component(_Absolute "${_Proto}" ABSOLUTE)
        get_filename_component(_Directory "${_Absolute}" DIRECTORY)
        get_filename_component(_Name "${_Absolute}" NAME_WE)
        set(_Source "${CMAKE_CURRENT_BINARY_DIR}/${_Name}.pb.cc")
        set(_Header "${CMAKE_CURRENT_BINARY_DIR}/${_Name}.pb.h")
        add_custom_command(
            OUTPUT "${_Source}" "${_Header}"
            COMMAND protoc --cpp_out "${CMAKE_CURRENT_BINARY_DIR}" -I "${_Directory}" "${_Absolute}"
            DEPENDS "${_Absolute}" protoc
            COMMENT "protoc ${_Name}.proto"
            VERBATIM)
        list(APPEND _Sources "${_Source}")
        list(APPEND _Headers "${_Header}")
    endforeach()
    set(${OutSources} ${_Sources} PARENT_SCOPE)
    set(${OutHeaders} ${_Headers} PARENT_SCOPE)
endfunction()
]=])

# ---------------------------------------------------------------- GameNetworkingSockets v1.6.0 (BSD-3) — 멀티플레이 전송 계층 (신뢰/비신뢰 메시지, 암호화)
# 정적 라이브러리, 암호화는 Windows BCrypt(OpenSSL 불필요), ICE(NAT 통과 P2P)는 끔 — LAN/전용 서버만 쓴다.
# 사용자 코드는 STEAMNETWORKINGSOCKETS_STATIC_LINK 정의가 필요하며 GNS 타깃이 INTERFACE로 전파한다.
# GNS는 C 소스(ed25519-donna)가 있고 c_std_99를 PUBLIC 기능으로 전파하므로 프로젝트 전체에 C 언어를 켠다
enable_language(C)
foreach(_Option BUILD_SHARED_LIB BUILD_EXAMPLES BUILD_TESTS BUILD_TOOLS LTO ENABLE_ICE USE_STEAMWEBRTC)
    set(${_Option} OFF CACHE INTERNAL "")
endforeach()
set(BUILD_STATIC_LIB ON CACHE INTERNAL "")
set(Protobuf_USE_STATIC_LIBS ON CACHE INTERNAL "")
set(USE_CRYPTO "BCrypt" CACHE INTERNAL "")
e_fetchcontent_declare(gamenetworkingsockets
    URL      "https://github.com/ValveSoftware/GameNetworkingSockets/archive/refs/tags/v1.6.0.tar.gz"
    URL_HASH SHA256=bddfe735d29ff2bbf186013a945ed57caaf4b79893eb7918c06c0f64955016f3)
e_fetchcontent_make_available(gamenetworkingsockets)

# 전역 WIN32_LEAN_AND_MEAN은 GNS가 쓰는 timeBeginPeriod(mmsystem)를 빼 버리므로 GNS 디렉터리에서만 제거
get_property(_GnsDefinitions DIRECTORY "${gamenetworkingsockets_SOURCE_DIR}/src" PROPERTY COMPILE_DEFINITIONS)
list(REMOVE_ITEM _GnsDefinitions WIN32_LEAN_AND_MEAN)
set_property(DIRECTORY "${gamenetworkingsockets_SOURCE_DIR}/src" PROPERTY COMPILE_DEFINITIONS ${_GnsDefinitions})

add_library(ThirdParty::gns ALIAS GameNetworkingSockets_s)
target_compile_options(GameNetworkingSockets_s PRIVATE /W0)
# GNS는 예외를 끈다(/EHs-c-). 전역 /EHsc가 함께 붙으면 파일마다 D9025('/EHs'을(를) '/EHs-'(으)로 재정의) 경고가 나므로 이 타깃에서만 뺀다
get_target_property(_GnsOptions GameNetworkingSockets_s COMPILE_OPTIONS)
list(REMOVE_ITEM _GnsOptions /EHsc)
set_target_properties(GameNetworkingSockets_s PROPERTIES COMPILE_OPTIONS "${_GnsOptions}")
set_target_properties(GameNetworkingSockets_s PROPERTIES FOLDER "ThirdParty")

# ---------------------------------------------------------------- Steamworks SDK (선택, Valve 파트너 계정 전용 — 저장소/FetchContent에 넣지 않는다)
# 경로: CMake 변수 E_STEAMWORKS_SDK_DIR (루트 CMakeLocal.cmake에서 set 권장 — gitignore) → 환경 변수 STEAMWORKS_SDK_DIR.
# SDK 루트(steamworks_sdk_XXX) 또는 그 안의 sdk 폴더 모두 받는다. 없으면 Online 모듈이 빈 구현으로 빌드된다 (FSteamSubsystem::IsCompiledIn)
if(NOT DEFINED E_STEAMWORKS_SDK_DIR OR "${E_STEAMWORKS_SDK_DIR}" STREQUAL "")
    set(E_STEAMWORKS_SDK_DIR "$ENV{STEAMWORKS_SDK_DIR}")
endif()
set(E_STEAMWORKS_DLL "")
if(NOT "${E_STEAMWORKS_SDK_DIR}" STREQUAL "")
    file(TO_CMAKE_PATH "${E_STEAMWORKS_SDK_DIR}" _SteamSdk)
    if(EXISTS "${_SteamSdk}/sdk/public/steam/steam_api.h")
        set(_SteamSdk "${_SteamSdk}/sdk")
    endif()
    if(EXISTS "${_SteamSdk}/public/steam/steam_api.h" AND EXISTS "${_SteamSdk}/redistributable_bin/win64/steam_api64.lib")
        add_library(steamworks SHARED IMPORTED GLOBAL)
        set_target_properties(steamworks PROPERTIES
            IMPORTED_IMPLIB   "${_SteamSdk}/redistributable_bin/win64/steam_api64.lib"
            IMPORTED_LOCATION "${_SteamSdk}/redistributable_bin/win64/steam_api64.dll")
        target_include_directories(steamworks SYSTEM INTERFACE "${_SteamSdk}/public")
        add_library(ThirdParty::steamworks ALIAS steamworks)
        set(E_STEAMWORKS_DLL "${_SteamSdk}/redistributable_bin/win64/steam_api64.dll")
        message(STATUS "Steamworks SDK: ${_SteamSdk}")
    else()
        message(WARNING "E_STEAMWORKS_SDK_DIR에 Steamworks SDK가 없습니다 (public/steam/steam_api.h, redistributable_bin/win64): ${E_STEAMWORKS_SDK_DIR} — Steam 기능 없이 빌드합니다")
    endif()
endif()

# ---------------------------------------------------------------- Tracy v0.11.1 (BSD-3) — 프레임 프로파일러 클라이언트 (뷰어는 저장소에 넣지 않는다 — 같은 버전 tracy-profiler.exe)
# 클라이언트는 엔진 DLL 안 하나 (정적 라이브러리를 Core/Renderer가 PRIVATE 링크 — 실행 파일/게임 모듈은 Core/Profiling.h 함수로 같은 인스턴스를 쓴다).
#   ON_DEMAND: 뷰어가 붙기 전에는 기록하지 않음, ONLY_LOCALHOST: 127.0.0.1만 받음, DELAYED_INIT + MANUAL_LIFETIME: FApplication이 시작/종료
#   (테스트·패키지 게임은 시작하지 않으면 스레드/포트 없음), 크래시 처리기·시스템 추적·콜스택 샘플링은 끔 (FCrashHandler/dbghelp와 겹치지 않게)
# E_TRACY=OFF면 받지도 않고 E_PROFILE_* 매크로가 비어 비용 0
option(E_TRACY "Tracy 프로파일러 존 (끄면 E_PROFILE_* 매크로가 비어 비용 0)" ON)
if(E_TRACY)
    e_fetchcontent_declare(tracy
        URL      "https://github.com/wolfpld/tracy/archive/refs/tags/v0.11.1.zip"
        URL_HASH SHA256=2213f8c39ccbda555ec646a6bfa77daabce5837733770937578c06ad20e747cd
        SOURCE_SUBDIR "_none") # 자체 CMakeLists(설치/옵션)를 쓰지 않고 TracyClient.cpp만 빌드
    e_fetchcontent_make_available(tracy)

    add_library(tracy STATIC "${tracy_SOURCE_DIR}/public/TracyClient.cpp")
    add_library(ThirdParty::tracy ALIAS tracy)
    target_include_directories(tracy SYSTEM PUBLIC "${tracy_SOURCE_DIR}/public")
    target_compile_definitions(tracy PUBLIC
        TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST TRACY_DELAYED_INIT TRACY_MANUAL_LIFETIME
        TRACY_NO_CRASH_HANDLER TRACY_NO_SYSTEM_TRACING TRACY_NO_CALLSTACK TRACY_NO_SAMPLING)
    target_compile_options(tracy PRIVATE /W0)
    set_target_properties(tracy PROPERTIES FOLDER "ThirdParty")
endif()