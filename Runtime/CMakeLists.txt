# ProjectERuntime: 게임 런타임 실행 파일 (Windows 서브시스템, 콘솔 없음)
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp")

add_executable(ProjectERuntime ${RUNTIME_SOURCES})
target_link_libraries(ProjectERuntime PRIVATE ProjectE::Core ProjectE::RHI ProjectE::Scene ProjectE::Renderer)
set_target_properties(ProjectERuntime PROPERTIES
    WIN32_EXECUTABLE ON
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
# main()을 유지한 채 창 서브시스템으로 링크
target_link_options(ProjectERuntime PRIVATE "/ENTRY:mainCRTStartup")

e_set_target_defaults(ProjectERuntime "Runtime")
e_copy_dxc_runtime(ProjectERuntime)
