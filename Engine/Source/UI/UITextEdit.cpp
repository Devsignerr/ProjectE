#include "UI/UITextEdit.h"

#include "UI/UIFont.h"
#include "UI/UIInput.h"
#include "UI/Widget.h"

#include <algorithm>

namespace
{
	// 단어 경계: 글자/숫자(한글 포함 — 0x80 이상은 모두 글자로)와 그 밖(공백/구두점)
	bool IsWordChar(uint32 Char)
	{
		if (Char >= 0x80)
		{
			return Char != 0x3000; // 전각 공백만 구분자
		}
		return (Char >= '0' && Char <= '9') || (Char >= 'A' && Char <= 'Z') || (Char >= 'a' && Char <= 'z') || Char == '_';
	}
} // namespace

std::string FUITextEdit::GetSelectedText() const
{
	if (!HasSelection())
	{
		return {};
	}
	return EncodeUtf8(std::vector<uint32>(Text.begin() + GetSelectionBegin(), Text.begin() + GetSelectionEnd()));
}

bool FUITextEdit::DeleteSelection()
{
	if (!HasSelection())
	{
		Anchor = -1;
		return false;
	}
	const int32 Begin = GetSelectionBegin();
	Text.erase(Text.begin() + Begin, Text.begin() + GetSelectionEnd());
	Caret  = Begin;
	Anchor = -1;
	return true;
}

bool FUITextEdit::Insert(std::u32string_view Chars, int32 MaxLength)
{
	bool bChanged = DeleteSelection();
	for (const char32_t Char : Chars)
	{
		if (MaxLength > 0 && static_cast<int32>(Text.size()) >= MaxLength)
		{
			break;
		}
		Text.insert(Text.begin() + Caret, static_cast<uint32>(Char));
		++Caret;
		bChanged = true;
	}
	return bChanged;
}

void FUITextEdit::MoveTo(int32 Position, bool bExtend)
{
	Position = std::clamp(Position, 0, static_cast<int32>(Text.size()));
	if (bExtend)
	{
		if (Anchor < 0)
		{
			Anchor = Caret;
		}
	}
	else
	{
		Anchor = -1;
	}
	Caret = Position;
}

int32 FUITextEdit::FindWordLeft(const std::vector<uint32>& Text, int32 From)
{
	int32 Index = std::clamp(From, 0, static_cast<int32>(Text.size()));
	while (Index > 0 && !IsWordChar(Text[static_cast<size_t>(Index - 1)]))
	{
		--Index;
	}
	while (Index > 0 && IsWordChar(Text[static_cast<size_t>(Index - 1)]))
	{
		--Index;
	}
	return Index;
}

int32 FUITextEdit::FindWordRight(const std::vector<uint32>& Text, int32 From)
{
	const int32 Size  = static_cast<int32>(Text.size());
	int32       Index = std::clamp(From, 0, Size);
	while (Index < Size && IsWordChar(Text[static_cast<size_t>(Index)]))
	{
		++Index;
	}
	while (Index < Size && !IsWordChar(Text[static_cast<size_t>(Index)]))
	{
		++Index;
	}
	return Index;
}

std::u32string FUITextEdit::SanitizePaste(std::string_view Utf8)
{
	std::u32string Result;
	for (const uint32 Char : DecodeUtf8(Utf8))
	{
		if (Char == '\r')
		{
			continue;
		}
		if (Char == '\n' || Char == '\t')
		{
			Result.push_back(U' ');
		}
		else if (Char >= 0x20 && Char != 0x7F)
		{
			Result.push_back(static_cast<char32_t>(Char));
		}
	}
	return Result;
}

bool FUITextEdit::Apply(const FUIKeyInput& Keys, int32 MaxLength, std::string* OutClipboard, bool* OutMoved)
{
	Caret       = std::clamp(Caret, 0, static_cast<int32>(Text.size()));
	bool bChanged = false;
	bool bMoved   = false;

	if (Keys.bSelectAll)
	{
		Anchor = 0;
		Caret  = static_cast<int32>(Text.size());
		bMoved = true;
	}
	if ((Keys.bCopy || Keys.bCut) && HasSelection() && OutClipboard != nullptr)
	{
		*OutClipboard = GetSelectedText();
	}
	if (Keys.bCut && HasSelection())
	{
		bChanged |= DeleteSelection();
	}
	if (Keys.bBackspace)
	{
		if (HasSelection())
		{
			bChanged |= DeleteSelection();
		}
		else if (Caret > 0)
		{
			const int32 From = Keys.bWordMove ? FindWordLeft(Text, Caret) : Caret - 1;
			Text.erase(Text.begin() + From, Text.begin() + Caret);
			Caret    = From;
			Anchor   = -1;
			bChanged = true;
		}
	}
	if (Keys.bDelete)
	{
		if (HasSelection())
		{
			bChanged |= DeleteSelection();
		}
		else if (Caret < static_cast<int32>(Text.size()))
		{
			const int32 To = Keys.bWordMove ? FindWordRight(Text, Caret) : Caret + 1;
			Text.erase(Text.begin() + Caret, Text.begin() + To);
			Anchor   = -1;
			bChanged = true;
		}
	}
	if (!Keys.Typed.empty())
	{
		bChanged |= Insert(Keys.Typed, MaxLength);
	}
	if (Keys.bPaste && !Keys.PasteText.empty())
	{
		bChanged |= Insert(SanitizePaste(Keys.PasteText), MaxLength);
	}

	// 이동: 선택이 있는데 Shift 없이 ←/→면 선택 끝으로 접는다
	if (Keys.bLeft)
	{
		if (HasSelection() && !Keys.bShift)
		{
			MoveTo(GetSelectionBegin(), false);
		}
		else
		{
			MoveTo(Keys.bWordMove ? FindWordLeft(Text, Caret) : Caret - 1, Keys.bShift);
		}
		bMoved = true;
	}
	if (Keys.bRight)
	{
		if (HasSelection() && !Keys.bShift)
		{
			MoveTo(GetSelectionEnd(), false);
		}
		else
		{
			MoveTo(Keys.bWordMove ? FindWordRight(Text, Caret) : Caret + 1, Keys.bShift);
		}
		bMoved = true;
	}
	if (Keys.bHome)
	{
		MoveTo(0, Keys.bShift);
		bMoved = true;
	}
	if (Keys.bEnd)
	{
		MoveTo(static_cast<int32>(Text.size()), Keys.bShift);
		bMoved = true;
	}
	if (Anchor == Caret)
	{
		Anchor = -1;
	}
	if (OutMoved != nullptr)
	{
		*OutMoved = bMoved;
	}
	return bChanged;
}

std::string BuildTextBoxDisplay(const FUIWidget& Box, int32& OutCaret, int32& OutCompositionBegin, int32& OutCompositionEnd)
{
	if (Box.State.Composition.empty())
	{
		OutCaret            = Box.State.CaretIndex;
		OutCompositionBegin = OutCompositionEnd = OutCaret;
		return Box.Text;
	}
	std::vector<uint32> Text  = DecodeUtf8(Box.Text);
	const int32         Caret = std::clamp(Box.State.CaretIndex, 0, static_cast<int32>(Text.size()));
	Text.insert(Text.begin() + Caret, Box.State.Composition.begin(), Box.State.Composition.end());
	const int32 Length  = static_cast<int32>(Box.State.Composition.size());
	OutCompositionBegin = Caret;
	OutCompositionEnd   = Caret + Length;
	OutCaret            = Caret + std::clamp(Box.State.CompositionCursor, 0, Length);
	return EncodeUtf8(Text);
}
