# 엔진/샌드박스 타깃 공통 설정
#   - 솔루션 폴더 지정
#   - 경고를 에러로 처리 (/WX)
#   - 소스 트리 구조를 IDE 필터로 반영
function(e_set_target_defaults TargetName FolderName)
    set_target_properties(${TargetName} PROPERTIES FOLDER "${FolderName}")
    target_compile_options(${TargetName} PRIVATE /WX)
    get_target_property(_Sources ${TargetName} SOURCES)
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${_Sources})
endfunction()

# ---- 엔진 런타임 모듈 빌드 형태
# E_ENGINE_SHARED=ON(기본): Core/RHI/Scene/Renderer/Audio/Physics/Scripting을 OBJECT 라이브러리로 만들어 ProjectEEngine.dll 하나로 링크한다.
#   소비자(에디터 모듈/실행 파일/게임 모듈/테스트)는 ProjectE::Engine만 링크한다.
#   공유 빌드에서 모듈 타깃(ProjectE::Core 등)을 직접 링크하면 객체가 DLL과 실행 파일에 중복 링크되므로 금지.
# OFF: 모듈은 정적 라이브러리, ProjectE::Engine은 모든 모듈을 묶는 INTERFACE.
option(E_ENGINE_SHARED "엔진 런타임 모듈을 ProjectEEngine.dll로 빌드" ON)
if(E_ENGINE_SHARED)
    set(E_ENGINE_MODULE_TYPE OBJECT)
else()
    set(E_ENGINE_MODULE_TYPE STATIC)
endif()

# 엔진 런타임 모듈 공통 설정 (e_set_target_defaults + DLL 경계 정의)
function(e_set_engine_module_defaults TargetName)
    e_set_target_defaults(${TargetName} "Engine")
    if(E_ENGINE_SHARED)
        target_compile_definitions(${TargetName} PUBLIC E_ENGINE_SHARED PRIVATE E_ENGINE_EXPORTS)
    endif()
endfunction()

# Windows 10 SDK의 x64 bin 디렉터리 탐색 (VsDevCmd 환경 변수 우선, 없으면 레지스트리 + 최신 버전)
function(e_find_windows_sdk_bin OutVar)
    if(DEFINED ENV{WindowsSdkDir} AND NOT "$ENV{WindowsSdkDir}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" _KitsRoot)
    else()
        get_filename_component(_KitsRoot
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
    endif()
    string(REGEX REPLACE "/+$" "" _KitsRoot "${_KitsRoot}")

    set(_Version "$ENV{WindowsSDKVersion}")
    string(REGEX REPLACE "[\\/]+$" "" _Version "${_Version}")
    if("${_Version}" STREQUAL "")
        file(GLOB _Versions RELATIVE "${_KitsRoot}/bin" "${_KitsRoot}/bin/10.*")
        list(SORT _Versions COMPARE NATURAL ORDER DESCENDING)
        list(GET _Versions 0 _Version)
    endif()

    set(${OutVar} "${_KitsRoot}/bin/${_Version}/x64" PARENT_SCOPE)
endfunction()

# DXC 런타임(dxcompiler.dll, dxil.dll)을 실행 파일 옆으로 복사
function(e_copy_dxc_runtime TargetName)
    e_find_windows_sdk_bin(_SdkBin)
    foreach(_Dll dxcompiler.dll dxil.dll)
        if(NOT EXISTS "${_SdkBin}/${_Dll}")
            message(FATAL_ERROR "DXC 런타임을 찾을 수 없습니다: ${_SdkBin}/${_Dll}")
        endif()
        add_custom_command(TARGET ${TargetName} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_SdkBin}/${_Dll}" "$<TARGET_FILE_DIR:${TargetName}>"
            COMMENT "DXC 런타임 복사: ${_Dll}")
    endforeach()
endfunction()
