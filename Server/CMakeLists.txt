# ProjectEServer: 전용 서버 실행 파일 (콘솔 서브시스템, 창/GPU 없음). 게임 월드만 고정 틱으로 돌린다
file(GLOB_RECURSE SERVER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp")

add_executable(ProjectEServer ${SERVER_SOURCES})
target_link_libraries(ProjectEServer PRIVATE ProjectE::Engine)
set_target_properties(ProjectEServer PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

e_set_target_defaults(ProjectEServer "Runtime")
