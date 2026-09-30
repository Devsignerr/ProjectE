# imgui-node-editor 패치 (FetchContent PATCH_COMMAND, 작업 디렉터리 = 소스 루트)
# ImGui 1.92.0(IMGUI_VERSION_NUM 19200)부터 IMGUI_DEFINE_MATH_OPERATORS 블록이 operator*(float, ImVec2)를 제공하므로
# 노드 편집기의 같은 정의를 이전 버전에서만 쓰도록 감싼다 (중복 정의 C2084). 여러 번 실행해도 안전하다
function(e_patch_file File Old New)
    file(READ "${File}" Content)
    string(FIND "${Content}" "${New}" AlreadyPatched)
    if(NOT AlreadyPatched EQUAL -1)
        return()
    endif()
    string(FIND "${Content}" "${Old}" Found)
    if(Found EQUAL -1)
        message(FATAL_ERROR "imgui-node-editor 패치 대상을 찾지 못했습니다: ${File}")
    endif()
    string(REPLACE "${Old}" "${New}" Content "${Content}")
    file(WRITE "${File}" "${Content}")
endfunction()

e_patch_file("imgui_extra_math.h"
    "inline ImVec2 operator*(const float lhs, const ImVec2& rhs);\n"
    "# if IMGUI_VERSION_NUM < 19200 // ProjectE 패치\ninline ImVec2 operator*(const float lhs, const ImVec2& rhs);\n# endif\n")

e_patch_file("imgui_extra_math.inl"
    "inline ImVec2 operator*(const float lhs, const ImVec2& rhs)\n{\n    return ImVec2(lhs * rhs.x, lhs * rhs.y);\n}\n"
    "# if IMGUI_VERSION_NUM < 19200 // ProjectE 패치\ninline ImVec2 operator*(const float lhs, const ImVec2& rhs)\n{\n    return ImVec2(lhs * rhs.x, lhs * rhs.y);\n}\n# endif\n")
