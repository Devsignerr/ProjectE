#pragma once

// 콘솔 변수를 직접 고치는 ImGui 위젯 (통계 창/포스트 프로세스 패널 — 콘솔 r.* 와 같은 값). 변수가 없으면 비활성 표시
namespace ConsoleVariableWidgets
{
	bool Checkbox(const char* Label, const char* VariableName);
	bool SliderFloat(const char* Label, const char* VariableName, float Min, float Max, const char* Format = "%.2f");
	// 값 이름이 있는 정수 변수 (예: r.DebugView)
	bool Combo(const char* Label, const char* VariableName);
	// 변수 값 읽기 (없으면 Fallback)
	bool GetBool(const char* VariableName, bool Fallback = false);
} // namespace ConsoleVariableWidgets
