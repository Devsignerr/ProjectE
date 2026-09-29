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
