#pragma once

#include "Core/CoreTypes.h"

#include <string>
#include <string_view>
#include <vector>

struct ImGuiInputTextCallbackData;

// 에디터 콘솔 입력 줄 (출력 로그 패널 아래). 실행 결과는 LogConsole로 출력 로그에 남는다.
//   Enter 실행, Tab 완성(고른 후보 또는 공통 접두사), ↑↓ = 후보가 보이면 후보 고르기 / 아니면 입력 기록, Esc 지우기.
//   후보 팝업은 입력 줄 위에 (클릭으로도 고름). 기록은 FConsoleManager 공용 (런타임 콘솔과 같은 목록)
class FConsoleInput
{
public:
	// 반환: 이번 프레임에 명령을 실행함 (출력 로그를 맨 아래로)
	bool Draw();

	// 입력 줄에 글자를 넣고 포커스 (자동 검증 --console-input)
	void SetText(std::string_view Text, bool bFocus);

private:
	static int  HandleCallback(ImGuiInputTextCallbackData* Data);
	void        RefreshCompletions();
	void        ReplaceText(ImGuiInputTextCallbackData* Data, const std::string& Text);

	char                     Buffer[512] = {};
	int32                    HistoryIndex = -1;
	bool                     bRequestFocus = false;
	bool                     bPopupHovered = false;
	std::string              CompletionSource;
	std::vector<std::string> CompletionTexts;
	std::vector<std::string> CompletionNames;
	std::vector<std::string> CompletionDetails;
	std::vector<bool>        CompletionIsCommand;
	int32                    SelectedCompletion = -1;
	std::string              PendingText; // 팝업 클릭으로 고른 줄 (다음 프레임 입력에 반영)
	bool                     bHasPendingText = false;
};
