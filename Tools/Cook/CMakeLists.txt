# ProjectECook: 쿠킹 도구 (콘솔). 셰이더를 DXIL로, 프로젝트 에셋을 엔진 바이너리로 변환한다. GPU 불필요.
file(GLOB_RECURSE COOK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp")

add_executable(ProjectECook ${COOK_SOURCES})
target_link_libraries(ProjectECook PRIVATE ProjectE::Core ProjectE::RHI ProjectE::Renderer)
set_target_properties(ProjectECook PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

e_set_target_defaults(ProjectECook "Tools")
e_copy_dxc_runtime(ProjectECook)
