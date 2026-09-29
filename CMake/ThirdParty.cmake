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
