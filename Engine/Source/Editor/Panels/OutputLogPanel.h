#pragma once

#include "Core/Log.h"
#include "Editor/Panels/ConsoleInput.h"

#include <deque>

struct FEditorContext;

class FOutputLogPanel
{
public:
	void Draw(FEditorContext& Context);
	bool bOpen = true;

	// 콘솔 입력 줄에 글자를 넣고 창/입력에 포커스 (자동 검증 --console-input)
	void FocusConsole(std::string_view Text);

private:
	std::deque<FLogMessage> Messages;
	uint64 LastSequence = 0;
	char Search[256] = {};
	bool bAutoScroll = true;
	bool bScrollToBottom = false;
	bool bFocusWindow    = false;
	bool bFocusInput     = false;
	std::string PendingConsoleText;
	FConsoleInput ConsoleInput; // 아래 콘솔 명령 줄 (결과는 LogConsole로 이 패널에)
};
