# ProjectEEditor: 에디터 실행 파일
add_executable(ProjectEEditor "${CMAKE_CURRENT_SOURCE_DIR}/Source/EditorMain.cpp")
target_link_libraries(ProjectEEditor PRIVATE ProjectE::Editor)
set_target_properties(ProjectEEditor PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

e_set_target_defaults(ProjectEEditor "Editor")
e_copy_dxc_runtime(ProjectEEditor)
