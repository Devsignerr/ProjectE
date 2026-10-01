#pragma once

#include "Core/CoreTypes.h"

#include <string>
#include <string_view>
#include <vector>

struct FUIKeyInput;
struct FUIWidget;

// 한 줄 텍스트 편집 (텍스트 상자의 순수 로직 — 글꼴/창과 무관). 위치는 코드 포인트 번호 (0 = 맨 앞).
//   선택 = [min(Anchor, Caret), max(Anchor, Caret)), Anchor < 0이면 선택 없음
//   한 프레임 순서: 전체 선택 → 복사/잘라내기 → 지우기(Backspace/Delete, 선택이 있으면 선택 영역) → 입력 문자 → 붙여넣기 → 이동(Shift = 선택 넓히기)
//   단어 이동(Ctrl+←/→): 공백/구두점 경계. 붙여넣기는 줄바꿈/탭을 공백으로 바꾸고 MaxLength에 맞춰 자른다
struct FUITextEdit
{
	std::vector<uint32> Text;
	int32               Caret  = 0;
	int32               Anchor = -1;

	bool  HasSelection() const { return Anchor >= 0 && Anchor != Caret; }
	int32 GetSelectionBegin() const { return HasSelection() ? (Anchor < Caret ? Anchor : Caret) : Caret; }
	int32 GetSelectionEnd() const { return HasSelection() ? (Anchor < Caret ? Caret : Anchor) : Caret; }
	std::string GetSelectedText() const;

	// 선택 영역을 지운다. 반환: 지운 것이 있음
	bool DeleteSelection();
	// 선택을 바꿔 넣는다 (MaxLength > 0이면 넘는 글자는 버린다). 반환: 내용이 바뀜
	bool Insert(std::u32string_view Chars, int32 MaxLength);
	// 키 입력 하나를 적용. OutClipboard: 복사/잘라내기 시 클립보드에 쓸 글자 (없으면 그대로). 반환: 내용이 바뀜
	bool Apply(const FUIKeyInput& Keys, int32 MaxLength, std::string* OutClipboard, bool* OutMoved = nullptr);
	// 이동 (bExtend = Shift: 선택 넓히기)
	void MoveTo(int32 Position, bool bExtend);

	// Ctrl+화살표 단어 경계 (From에서 왼쪽/오른쪽으로)
	static int32 FindWordLeft(const std::vector<uint32>& Text, int32 From);
	static int32 FindWordRight(const std::vector<uint32>& Text, int32 From);
	// 붙여넣기 글자 정리: \r 제거, \n/\t → 공백, 다른 제어 문자 제거
	static std::u32string SanitizePaste(std::string_view Utf8);
};

// 텍스트 상자 표시용 글자: Text의 캐럿 자리에 IME 조합 중 글자를 끼운 것.
// OutCaret = 표시 글자 안 캐럿(조합 안 커서 포함), OutCompositionBegin/End = 조합 구간 (없으면 같은 값)
std::string BuildTextBoxDisplay(const FUIWidget& Box, int32& OutCaret, int32& OutCompositionBegin, int32& OutCompositionEnd);
