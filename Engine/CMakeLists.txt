add_subdirectory(Source/Core)
add_subdirectory(Source/RHI)
add_subdirectory(Source/Scene)
add_subdirectory(Source/Scripting)
add_subdirectory(Source/UI)
add_subdirectory(Source/Renderer)
add_subdirectory(Source/Audio)
add_subdirectory(Source/Physics)
add_subdirectory(Source/AI)
add_subdirectory(Source/World)
add_subdirectory(Source/Network)
add_subdirectory(Source/Online)
# ---- 엔진 런타임 DLL (ProjectEEngine.dll) 또는 정적 묶음
set(E_ENGINE_RUNTIME_MODULES ECore ERHI EScene ERenderer EAudio EPhysics EAI EScripting EWorld ENetwork EUI EOnline)
if(E_ENGINE_SHARED)
    add_library(ProjectEEngine SHARED "${CMAKE_CURRENT_SOURCE_DIR}/EngineDll.cpp")
    target_link_libraries(ProjectEEngine PUBLIC ${E_ENGINE_RUNTIME_MODULES}) # OBJECT 직접 링크 → 객체가 DLL에 들어간다
    set_target_properties(ProjectEEngine PROPERTIES FOLDER "Engine")
    # 내보내기 = WINDOWS_EXPORT_ALL_SYMBOLS와 같은 자동 목록에서 sol2 내부 템플릿만 뺀 .def (DLL 내보내기 한도 65535 —
    # sol 인스턴스만 ~9800개이고 DLL 밖은 sol 심볼을 가져다 쓰지 않는다, 헤더 템플릿은 쓰는 쪽이 각자 인스턴스화)
    set(_EngineObjList "${CMAKE_CURRENT_BINARY_DIR}/ProjectEEngineObjects_$<CONFIG>.txt")
    set(_EngineDef "${CMAKE_CURRENT_BINARY_DIR}/ProjectEEngine_$<CONFIG>.def")
    set(_EngineObjs "")
    foreach(_Module IN LISTS E_ENGINE_RUNTIME_MODULES)
        list(APPEND _EngineObjs "$<TARGET_OBJECTS:${_Module}>")
    endforeach()
    file(GENERATE OUTPUT "${_EngineObjList}" CONTENT "$<JOIN:${_EngineObjs},\n>\n")
    add_custom_command(OUTPUT "${_EngineDef}"
        COMMAND ${CMAKE_COMMAND} -E __create_def "${_EngineDef}.all" "${_EngineObjList}"
        COMMAND ${CMAKE_COMMAND} -DIN=${_EngineDef}.all -DOUT=${_EngineDef} -P "${PROJECT_SOURCE_DIR}/CMake/FilterEngineExports.cmake"
        DEPENDS ${_EngineObjs} "${PROJECT_SOURCE_DIR}/CMake/FilterEngineExports.cmake"
        COMMENT "엔진 DLL 내보내기 목록 생성 (sol2 내부 제외)"
        VERBATIM)
    target_sources(ProjectEEngine PRIVATE "${_EngineDef}")
    target_compile_options(ProjectEEngine PRIVATE /W4 /WX)
    target_compile_definitions(ProjectEEngine PRIVATE E_ENGINE_EXPORTS)
    # 쿠킹 셰이더만 배포한 패키지는 dxcompiler.dll 없이 실행 (FD3D12ShaderCompiler가 존재 확인 후 사용)
    target_link_libraries(ProjectEEngine PRIVATE delayimp)
    target_link_options(ProjectEEngine PRIVATE "/DELAYLOAD:dxcompiler.dll")
    # Steamworks(선택): steam_api64.dll을 실행 파일 폴더로 (패키징도 여기서 가져간다)
    if(E_STEAMWORKS_DLL)
        add_custom_command(TARGET ProjectEEngine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${E_STEAMWORKS_DLL}" "$<TARGET_FILE_DIR:ProjectEEngine>"
            COMMENT "Steamworks 런타임 복사: steam_api64.dll")
    endif()
else()
    add_library(ProjectEEngine INTERFACE)
    target_link_libraries(ProjectEEngine INTERFACE ${E_ENGINE_RUNTIME_MODULES})
endif()
add_library(ProjectE::Engine ALIAS ProjectEEngine)

add_subdirectory(Source/Editor)
