# 엔진 DLL 자동 내보내기 목록(.def)에서 sol2 내부 심볼을 뺀다 (Engine/CMakeLists.txt가 호출).
# 사용: cmake -DIN=<자동 .def> -DOUT=<결과 .def> -P FilterEngineExports.cmake
# 맹글링 이름의 "@sol@@" = 최상위 네임스페이스 sol (이름이 ...sol로 끝나는 다른 클래스는 "@Xsol@@"라 걸리지 않음)
file(STRINGS "${IN}" _Lines)
list(FILTER _Lines EXCLUDE REGEX "@sol@@")
list(LENGTH _Lines _Count)
math(EXPR _Exports "${_Count} - 1") # 첫 줄 EXPORTS
if(_Exports GREATER 65535)
    message(FATAL_ERROR "엔진 DLL 내보내기 ${_Exports}개 > 65535 (FilterEngineExports.cmake에서 더 거를 것)")
endif()
list(JOIN _Lines "\n" _Text)
file(WRITE "${OUT}" "${_Text}\n")
