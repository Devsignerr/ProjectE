#pragma once

#include "Core/Log.h"
#include "UI/UIDrawList.h"
#include "UI/UITextEdit.h"

#include <deque>
#include <string>
#include <vector>

class FInput;
class FConsoleManager;

// 런타임 개발자 콘솔 오버레이 (` 키로 열고 닫기, ImGui 없는 앱용 — 게임 UI 그리기 목록에 그린다).
//   입력 줄 = FUITextEdit (IME는 앱이 GetCaretRect로 FWindow::SetTextInput). Enter 실행, Tab 완성, ↑↓ 기록(입력이 비었을 때) /
//   후보 고르기(후보가 보일 때), PageUp/PageDown 출력 스크롤, Esc/` 닫기.
//   출력: FLog 기록(FLog::EnableHistory가 켜져 있어야 함)을 그대로 보여 준다 — 명령 결과도 LogConsole로 남는다.
//   열려 있는 동안 키보드는 콘솔 것 (Update가 true → 앱은 게임에 FInput::WithoutKeyboard, 게임 UI에 키 없음)
class FUIConsoleOverlay
{
public:
	bool bEnabled = true; // false면 ` 키도 무시하고 닫는다 (패키지 게임 기본, 프로젝트 설정 Console.EnableInPackagedGame)

	bool IsOpen() const { return bOpen; }
	void SetOpen(bool bInOpen);

	// 반환: 이번 프레임 콘솔이 키보드를 가져갔음 (열려 있거나 이번에 토글 키를 눌렀음)
	bool Update(const FInput& Input, float DeltaSeconds, FConsoleManager& Console);
	// Viewport: 화면 픽셀 영역. 위쪽 절반에 펼친다
	void Paint(const FUIRect& Viewport, FUIDrawList& Out) const;

	// IME 후보 창 위치 (화면 픽셀, Paint 기준 — 직전 Paint 결과)
	const FUIRect& GetCaretRect() const { return CaretRect; }

	// ---- 테스트/자동 검증
	std::string GetInputText() const;
	void        SetInputText(std::string_view Utf8);
	const std::vector<std::string>& GetCompletionTexts() const { return CompletionTexts; }
	int32       GetSelectedCompletion() const { return SelectedCompletion; }

	static constexpr size_t MaxLines    = 400;
	static constexpr float  FontSize    = 16.0f;

private:
	void RefreshCompletions(FConsoleManager& Console);
	void PullLog();

	bool        bOpen = false;
	FUITextEdit Edit;
	float       CaretBlink = 0.0f;
	int32       HistoryIndex = -1; // -1 = 기록 탐색 안 함 (GetHistory 뒤에서부터 0, 1, ...)
	int32       ScrollLines  = 0;  // 출력 위로 올린 줄 수

	std::vector<std::string> CompletionTexts;   // 후보 전체 줄 (Text)
	std::vector<std::string> CompletionDetails; // 후보 설명
	int32                    SelectedCompletion = -1;
	std::string              CompletionSource;  // 후보를 만든 입력 (바뀌면 다시)

	struct FLine
	{
		ELogVerbosity Verbosity = ELogVerbosity::Log;
		std::string   Text;
	};
	std::deque<FLine> Lines;
	uint64            LastSequence = 0;

	mutable FUIRect CaretRect;
};
