#include "Core/Input.h"
#include "Core/Testing/TestFramework.h"
#include "UI/UIFont.h"
#include "UI/UIInput.h"
#include "UI/UILayout.h"
#include "UI/UIPainter.h"
#include "UI/UISystem.h"
#include "UI/UITextEdit.h"
#include "UI/Widget.h"

namespace
{
	FUITextEdit MakeEdit(const char* Utf8, int32 Caret, int32 Anchor = -1)
	{
		FUITextEdit Edit;
		Edit.Text   = DecodeUtf8(Utf8);
		Edit.Caret  = Caret;
		Edit.Anchor = Anchor;
		return Edit;
	}

	std::string TextOf(const FUITextEdit& Edit) { return EncodeUtf8(Edit.Text); }
} // namespace

E_TEST(UITextEdit_WordBoundaries)
{
	const std::vector<uint32> Text = DecodeUtf8("hello, 세상 world");
	// h e l l o , _ 세 상 _ w o r l d  (0..15)
	E_EXPECT_EQ(FUITextEdit::FindWordLeft(Text, 15), 10);
	E_EXPECT_EQ(FUITextEdit::FindWordLeft(Text, 10), 7);  // 공백 건너뛰고 "세상" 앞
	E_EXPECT_EQ(FUITextEdit::FindWordLeft(Text, 7), 0);   // ", " 건너뛰고 "hello" 앞
	E_EXPECT_EQ(FUITextEdit::FindWordLeft(Text, 0), 0);
	E_EXPECT_EQ(FUITextEdit::FindWordRight(Text, 0), 7);  // "hello" + ", " 다음 단어 앞
	E_EXPECT_EQ(FUITextEdit::FindWordRight(Text, 7), 10);
	E_EXPECT_EQ(FUITextEdit::FindWordRight(Text, 15), 15);
}

E_TEST(UITextEdit_ShiftSelectionAndCollapse)
{
	FUITextEdit Edit = MakeEdit("abcdef", 2);
	FUIKeyInput Keys;
	Keys.bShift = true;
	Keys.bRight = true;
	Edit.Apply(Keys, 0, nullptr);
	Edit.Apply(Keys, 0, nullptr);
	E_EXPECT_TRUE(Edit.HasSelection());
	E_EXPECT_EQ(Edit.GetSelectionBegin(), 2);
	E_EXPECT_EQ(Edit.GetSelectionEnd(), 4);
	E_EXPECT_EQ(Edit.GetSelectedText(), std::string("cd"));

	// Shift+Home: 앵커는 그대로, 캐럿만 맨 앞 → 선택 [0, 2)
	Keys        = {};
	Keys.bShift = true;
	Keys.bHome  = true;
	Edit.Apply(Keys, 0, nullptr);
	E_EXPECT_EQ(Edit.GetSelectedText(), std::string("ab"));

	// Shift 없이 → 선택 끝(오른쪽)으로 접힌다
	Keys        = {};
	Keys.bRight = true;
	Edit.Apply(Keys, 0, nullptr);
	E_EXPECT_FALSE(Edit.HasSelection());
	E_EXPECT_EQ(Edit.Caret, 2);

	// Ctrl+Shift+End / Ctrl+←
	Edit        = MakeEdit("one two three", 13);
	Keys        = {};
	Keys.bWordMove = true;
	Keys.bLeft     = true;
	Edit.Apply(Keys, 0, nullptr);
	E_EXPECT_EQ(Edit.Caret, 8);
	Keys.bShift = true;
	Edit.Apply(Keys, 0, nullptr);
	E_EXPECT_EQ(Edit.GetSelectedText(), std::string("two "));
}

E_TEST(UITextEdit_SelectAllCopyCutPaste)
{
	FUITextEdit Edit = MakeEdit("가나다", 1);
	std::string Clipboard;
	FUIKeyInput Keys;
	Keys.bSelectAll = true;
	Keys.bCopy      = true;
	E_EXPECT_FALSE(Edit.Apply(Keys, 0, &Clipboard)); // 복사는 내용을 바꾸지 않는다
	E_EXPECT_EQ(Clipboard, std::string("가나다"));
	E_EXPECT_EQ(Edit.GetSelectionBegin(), 0);
	E_EXPECT_EQ(Edit.GetSelectionEnd(), 3);

	// 선택 [1, 3) 잘라내기
	Edit      = MakeEdit("가나다", 3, 1);
	Keys      = {};
	Keys.bCut = true;
	Clipboard.clear();
	E_EXPECT_TRUE(Edit.Apply(Keys, 0, &Clipboard));
	E_EXPECT_EQ(Clipboard, std::string("나다"));
	E_EXPECT_EQ(TextOf(Edit), std::string("가"));
	E_EXPECT_EQ(Edit.Caret, 1);

	// 선택 없이 복사 → 클립보드 그대로
	Keys       = {};
	Keys.bCopy = true;
	Clipboard  = "그대로";
	Edit.Apply(Keys, 0, &Clipboard);
	E_EXPECT_EQ(Clipboard, std::string("그대로"));

	// 붙여넣기: 줄바꿈/탭 → 공백, \r 제거, 선택을 바꾸고 최대 길이에서 자른다
	Edit           = MakeEdit("abXYef", 4, 2);
	Keys           = {};
	Keys.bPaste    = true;
	Keys.PasteText = "1\r\n2\t3";
	E_EXPECT_TRUE(Edit.Apply(Keys, 0, nullptr));
	E_EXPECT_EQ(TextOf(Edit), std::string("ab1 2 3ef"));
	E_EXPECT_EQ(Edit.Caret, 7);
	Edit = MakeEdit("abc", 3);
	E_EXPECT_TRUE(Edit.Apply(Keys, 5, nullptr));
	E_EXPECT_EQ(TextOf(Edit), std::string("abc1 ")); // 5자 제한
	E_EXPECT_TRUE(FUITextEdit::SanitizePaste("a\x01" "b") == std::u32string(U"ab"));
}

E_TEST(UITextEdit_DeleteSelectionTypingAndWordDelete)
{
	// 선택 상태에서 입력 = 바꾸기, Backspace/Delete = 선택 지우기
	FUITextEdit Edit = MakeEdit("hello", 1, 4);
	FUIKeyInput Keys;
	Keys.Typed = U"EY";
	E_EXPECT_TRUE(Edit.Apply(Keys, 0, nullptr));
	E_EXPECT_EQ(TextOf(Edit), std::string("hEYo"));
	Edit            = MakeEdit("hello", 0, 5);
	Keys            = {};
	Keys.bBackspace = true;
	Edit.Apply(Keys, 0, nullptr);
	E_EXPECT_TRUE(Edit.Text.empty() && Edit.Caret == 0 && !Edit.HasSelection());

	// Ctrl+Backspace / Ctrl+Delete: 단어 단위
	Edit            = MakeEdit("one two three", 13);
	Keys            = {};
	Keys.bWordMove  = true;
	Keys.bBackspace = true;
	Edit.Apply(Keys, 0, nullptr);
	E_EXPECT_EQ(TextOf(Edit), std::string("one two "));
	Edit         = MakeEdit("one two", 0);
	Keys         = {};
	Keys.bWordMove = true;
	Keys.bDelete   = true;
	Edit.Apply(Keys, 0, nullptr);
	E_EXPECT_EQ(TextOf(Edit), std::string("two"));

	// 최대 길이 + 선택 바꾸기: 지운 만큼 들어간다
	Edit       = MakeEdit("abcde", 5, 3);
	Keys       = {};
	Keys.Typed = U"123";
	Edit.Apply(Keys, 5, nullptr);
	E_EXPECT_EQ(TextOf(Edit), std::string("abc12"));
}

E_TEST(UITextBox_MouseDragSelectionClipboardAndComposition)
{
	auto       Root = FUIWidget::Create(EUIWidgetType::Canvas);
	FUIWidget* Box  = Root->AddChild(FUIWidget::Create(EUIWidgetType::TextBox));
	Box->Name         = "Chat";
	Box->Text         = "abcdef";
	Box->Slot.Offsets = FUIMargin(0.0f, 0.0f, 400.0f, 40.0f);
	Box->ContentPadding = FUIMargin(0.0f);
	Root->AssignIds();
	FUIFontLibrary& Fonts = FUIFontLibrary::Get();
	FUIFont*        Font  = Fonts.GetFont(Box->Font);
	if (Font == nullptr)
	{
		return; // 글꼴 없는 환경
	}
	FUILayout::Compute(*Root, FVector2(800.0f, 600.0f), Fonts);
	std::vector<float> Positions;
	Font->GetCaretPositions(Box->Text, Box->FontSize, Positions);

	// 'b' 앞을 누르고 'e' 앞까지 끌기 → 선택 [1, 4). 끄는 중에는 상자 밖이어도 포인터를 가져간다
	FUIInputRouter        Router;
	std::vector<FUIEvent> Events;
	FUIPointerInput       Pointer;
	Pointer.bInside  = true;
	Pointer.Position = FVector2(Positions[1], 20.0f);
	Pointer.bPressed = Pointer.bDown = true;
	Router.Process(*Root, Pointer, {}, Events);
	Pointer.bPressed = false;
	Pointer.Position = FVector2(Positions[4], 300.0f); // 아래로 벗어나도 X로 캐럿
	E_EXPECT_TRUE(Router.Process(*Root, Pointer, {}, Events));
	Pointer.bDown     = false;
	Pointer.bReleased = true;
	Router.Process(*Root, Pointer, {}, Events);
	E_EXPECT_EQ(Box->State.SelectionAnchor, 1);
	E_EXPECT_EQ(Box->State.CaretIndex, 4);

	// Ctrl+C → 라우터가 클립보드 글자를 내놓는다 (한 번만)
	Pointer = FUIPointerInput{};
	FUIKeyInput Keys;
	Keys.bCopy = true;
	Router.Process(*Root, Pointer, Keys, Events);
	std::string Copied;
	E_EXPECT_TRUE(Router.TakeClipboardText(Copied));
	E_EXPECT_EQ(Copied, std::string("bcd"));
	E_EXPECT_FALSE(Router.TakeClipboardText(Copied));

	// 그리기: 선택 영역 사각형이 하나 더 (포커스, 캐럿 보임)
	FUIDrawList WithSelection;
	FUIPainter::Paint(*Root, FUITransform{}, FUIRect::Infinite(), Fonts, WithSelection);
	Box->State.SelectionAnchor = -1;
	FUIDrawList Plain;
	FUIPainter::Paint(*Root, FUITransform{}, FUIRect::Infinite(), Fonts, Plain);
	E_EXPECT_EQ(WithSelection.Quads.size(), Plain.Quads.size() + 1);

	// Shift+클릭: 앵커 유지하며 넓히기
	Box->State.CaretIndex = 2;
	Pointer.bInside       = true;
	Pointer.Position      = FVector2(Positions[5], 20.0f);
	Pointer.bPressed = Pointer.bDown = true;
	Keys        = {};
	Keys.bShift = true;
	Router.Process(*Root, Pointer, Keys, Events);
	E_EXPECT_EQ(Box->State.SelectionAnchor, 2);
	E_EXPECT_EQ(Box->State.CaretIndex, 5);
	Pointer = FUIPointerInput{};
	Router.Process(*Root, Pointer, {}, Events);

	// IME 조합: 선택을 지우고 캐럿 자리에 표시만 (Text는 그대로) → 확정 글자가 오면 들어간다
	Keys                   = {};
	Keys.Composition       = U"한";
	Keys.CompositionCursor = 1;
	Events.clear();
	Router.Process(*Root, Pointer, Keys, Events);
	E_EXPECT_EQ(Box->Text, std::string("abf"));
	E_EXPECT_TRUE(Box->State.Composition == std::u32string(U"한"));
	int32             Caret = 0;
	int32             Begin = 0;
	int32             End   = 0;
	const std::string Display = BuildTextBoxDisplay(*Box, Caret, Begin, End);
	E_EXPECT_EQ(Display, std::string("ab한f"));
	E_EXPECT_TRUE(Caret == 3 && Begin == 2 && End == 3);

	// 그리기: 조합 밑줄 사각형
	FUIDrawList Composing;
	FUIPainter::Paint(*Root, FUITransform{}, FUIRect::Infinite(), Fonts, Composing);
	Box->State.Composition.clear();
	FUIDrawList NotComposing;
	FUIPainter::Paint(*Root, FUITransform{}, FUIRect::Infinite(), Fonts, NotComposing);
	E_EXPECT_EQ(Composing.Quads.size(), NotComposing.Quads.size() + 2); // 밑줄 + 글자 '한'
	Box->State.Composition = U"한";

	Keys       = {};
	Keys.Typed = U"한"; // 확정 (조합은 비워짐)
	Router.Process(*Root, Pointer, Keys, Events);
	E_EXPECT_EQ(Box->Text, std::string("ab한f"));
	E_EXPECT_TRUE(Box->State.Composition.empty());
	E_EXPECT_EQ(Box->State.CaretIndex, 3);
}

E_TEST(UITextBox_InputCompositionAndShortcutKeys)
{
	// 창 IME 조합 이벤트 → FInput 상태(프레임을 넘어 유지) → UI 키 입력
	FInput         Input;
	std::u32string Buffer = U"가";
	FWindowEvent   Event;
	Event.Type              = EWindowEventType::ImeComposition;
	Event.Composition       = Buffer.data();
	Event.CompositionLength = static_cast<uint32>(Buffer.size());
	Event.CompositionCursor = 1;
	Input.ProcessEvent(Event);
	Input.EndFrame();
	E_EXPECT_TRUE(Input.GetCompositionText() == std::u32string(U"가"));
	FWindowEvent Control;
	Control.Type = EWindowEventType::KeyDown;
	Control.Key  = EKey::LeftControl;
	Input.ProcessEvent(Control);
	FWindowEvent KeyA;
	KeyA.Type = EWindowEventType::KeyDown;
	KeyA.Key  = EKey::A;
	Input.ProcessEvent(KeyA);
	const FUIKeyInput Keys = FUISystem::MakeKeys(Input);
	E_EXPECT_TRUE(Keys.Composition == std::u32string(U"가") && Keys.CompositionCursor == 1);
	E_EXPECT_TRUE(Keys.bSelectAll && Keys.bWordMove && !Keys.bCopy && !Keys.bPaste);
	E_EXPECT_TRUE(Input.WithoutKeyboard().GetCompositionText().empty());

	// 조합 끝 (빈 문자열)
	Event.Composition       = nullptr;
	Event.CompositionLength = 0;
	Input.ProcessEvent(Event);
	E_EXPECT_TRUE(Input.GetCompositionText().empty());
}
