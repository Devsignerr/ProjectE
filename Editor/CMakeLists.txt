# ProjectEEditor: 에디터 실행 파일
add_executable(ProjectEEditor "${CMAKE_CURRENT_SOURCE_DIR}/Source/EditorMain.cpp")
target_link_libraries(ProjectEEditor PRIVATE ProjectE::Editor)
set_target_properties(ProjectEEditor PROPERTIES
    WIN32_EXECUTABLE ON
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
# 콘솔 없이 main()에서 시작한다. 로그는 에디터 출력 로그 패널로 전달한다.
target_link_options(ProjectEEditor PRIVATE "/ENTRY:mainCRTStartup")

e_set_target_defaults(ProjectEEditor "Editor")
e_copy_dxc_runtime(ProjectEEditor)
