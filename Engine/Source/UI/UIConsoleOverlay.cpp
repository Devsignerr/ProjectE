#include "UI/UIConsoleOverlay.h"

#include "Core/Console/Console.h"
#include "Core/Input.h"
#include "UI/UIDebugDraw.h"
#include "UI/UIFont.h"
#include "UI/UIInput.h"
#include "UI/UIPlatform.h"
#include "UI/UISystem.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace
{
	constexpr int32  MaxInputLength     = 512;
	constexpr size_t MaxCompletionShown = 10;

	// "[hh:mm:ss.mmm] LogX: Display: 내용\n" → "LogX: Display: 내용". 콘솔 명령 출력(LogConsole Display)은 내용만
	std::string ShortenLogLine(std::string_view Text)
	{
		while (!Text.empty() && (Text.back() == '\n' || Text.back() == '\r'))
		{
			Text.remove_suffix(1);
		}
		if (!Text.empty() && Text.front() == '[')
		{
			if (const size_t Close = Text.find("] "); Close != std::string_view::npos)
			{
				Text.remove_prefix(Close + 2);
			}
		}
		constexpr std::string_view ConsolePrefix = "LogConsole: Display: ";
		if (Text.starts_with(ConsolePrefix))
		{
			Text.remove_prefix(ConsolePrefix.size());
		}
		return std::string(Text);
	}

	FVector4 GetLineColor(ELogVerbosity Verbosity, const std::string& Text)
	{
		if (Verbosity <= ELogVerbosity::Error)
		{
			return FVector4(1.0f, 0.45f, 0.45f, 1.0f);
		}
		if (Verbosity == ELogVerbosity::Warning)
		{
			return FVector4(1.0f, 0.82f, 0.35f, 1.0f);
		}
		if (Text.starts_with("] "))
		{
			return FVector4(0.55f, 0.85f, 1.0f, 1.0f); // 입력 메아리
		}
		return FVector4(0.86f, 0.88f, 0.9f, 1.0f);
	}
} // namespace

void FUIConsoleOverlay::SetOpen(bool bInOpen)
{
	if (bOpen == bInOpen)
	{
		return;
	}
	bOpen        = bInOpen;
	HistoryIndex = -1;
	ScrollLines  = 0;
	CaretBlink   = 0.0f;
}

std::string FUIConsoleOverlay::GetInputText() const
{
	return EncodeUtf8(Edit.Text);
}

void FUIConsoleOverlay::SetInputText(std::string_view Utf8)
{
	Edit.Text   = DecodeUtf8(Utf8);
	Edit.Caret  = static_cast<int32>(Edit.Text.size());
	Edit.Anchor = -1;
}

void FUIConsoleOverlay::PullLog()
{
	for (FLogMessage& Message : FLog::ReadHistory(LastSequence))
	{
		LastSequence = Message.Sequence;
		Lines.push_back({ Message.Verbosity, ShortenLogLine(Message.Text) });
	}
	while (Lines.size() > MaxLines)
	{
		Lines.pop_front();
	}
}

void FUIConsoleOverlay::RefreshCompletions(FConsoleManager& Console)
{
	const std::string Text = GetInputText();
	if (Text == CompletionSource)
	{
		return;
	}
	CompletionSource = Text;
	CompletionTexts.clear();
	CompletionDetails.clear();
	SelectedCompletion = -1;
	if (Text.empty() || HistoryIndex >= 0)
	{
		return;
	}
	for (FConsoleCompletion& Completion : Console.GetCompletions(Text, MaxCompletionShown))
	{
		if (Completion.Text == Text)
		{
			continue; // 이미 다 친 것
		}
		CompletionTexts.push_back(std::move(Completion.Text));
		CompletionDetails.push_back(std::move(Completion.Detail));
	}
}

bool FUIConsoleOverlay::Update(const FInput& Input, float DeltaSeconds, FConsoleManager& Console)
{
	PullLog();
	if (!bEnabled)
	{
		SetOpen(false);
		return false;
	}
	if (Input.IsKeyPressed(EKey::Grave))
	{
		SetOpen(!bOpen); // 같은 프레임의 ` 문자는 입력하지 않는다
		return true;
	}
	if (!bOpen)
	{
		return false;
	}
	CaretBlink += DeltaSeconds;

	FUIKeyInput Keys = FUISystem::MakeKeys(Input);
	std::erase(Keys.Typed, U'`');
	if (Keys.bCancel)
	{
		SetOpen(false);
		return true;
	}

	// 실행
	if (Keys.bCommit)
	{
		const std::string Line = GetInputText();
		SetInputText({});
		HistoryIndex = -1;
		ScrollLines  = 0;
		if (!Line.empty())
		{
			Console.AddHistory(Line);
			Console.Execute(Line);
			PullLog(); // 결과를 이번 프레임에 보이게
		}
		RefreshCompletions(Console);
		return true;
	}

	// Tab 완성: 고른 후보가 있으면 그것, 아니면 공통 접두사
	if (Input.IsKeyPressed(EKey::Tab))
	{
		const std::string Text = GetInputText();
		if (SelectedCompletion >= 0 && SelectedCompletion < static_cast<int32>(CompletionTexts.size()))
		{
			SetInputText(CompletionTexts[static_cast<size_t>(SelectedCompletion)] + " ");
		}
		else if (!Text.empty())
		{
			SetInputText(Console.CompleteInput(Text));
		}
		HistoryIndex = -1;
		RefreshCompletions(Console);
		return true;
	}

	// ↑↓: 후보가 보이면 후보 고르기, 아니면 입력 기록
	const bool bUp   = Input.IsKeyRepeated(EKey::Up);
	const bool bDown = Input.IsKeyRepeated(EKey::Down);
	if (bUp || bDown)
	{
		if (!CompletionTexts.empty() && HistoryIndex < 0)
		{
			const int32 Count  = static_cast<int32>(CompletionTexts.size());
			SelectedCompletion = bDown ? (SelectedCompletion + 1) % Count : (SelectedCompletion <= 0 ? Count - 1 : SelectedCompletion - 1);
		}
		else
		{
			const std::vector<std::string>& History = Console.GetHistory();
			const int32                     Count   = static_cast<int32>(History.size());
			HistoryIndex                            = bUp ? std::min(HistoryIndex + 1, Count - 1) : std::max(HistoryIndex - 1, -1);
			SetInputText(HistoryIndex >= 0 ? History[static_cast<size_t>(Count - 1 - HistoryIndex)] : std::string());
			CompletionSource = GetInputText(); // 기록을 꺼낸 것은 후보를 띄우지 않는다
			CompletionTexts.clear();
			CompletionDetails.clear();
			SelectedCompletion = -1;
		}
		return true;
	}

	// 출력 스크롤
	if (Input.IsKeyRepeated(EKey::PageUp))
	{
		ScrollLines = std::min(ScrollLines + 10, static_cast<int32>(Lines.size()));
	}
	if (Input.IsKeyRepeated(EKey::PageDown))
	{
		ScrollLines = std::max(ScrollLines - 10, 0);
	}

	// 글자 편집
	Keys.bCommit = false;
	if (Keys.bPaste)
	{
		Keys.PasteText = FUIPlatform::GetClipboardText();
	}
	std::string                Clipboard;
	const std::vector<uint32>  Before = Edit.Text;
	Edit.Apply(Keys, MaxInputLength, &Clipboard);
	if (!Clipboard.empty())
	{
		FUIPlatform::SetClipboardText(Clipboard);
	}
	if (Edit.Text != Before)
	{
		HistoryIndex = -1;
		CaretBlink   = 0.0f;
	}
	RefreshCompletions(Console);
	return true;
}

void FUIConsoleOverlay::Paint(const FUIRect& Viewport, FUIDrawList& Out) const
{
	if (!bOpen || Viewport.IsEmpty())
	{
		return;
	}
	const float    LineHeight = FUIDebugDraw::GetLineHeight(FontSize);
	constexpr float Padding   = 8.0f;
	const FUIRect  Panel(Viewport.Min, FVector2(Viewport.Max.X, Viewport.Min.Y + std::max(Viewport.GetHeight() * 0.45f, LineHeight * 4.0f)));
	FUIDebugDraw::AddRect(Out, Panel, FVector4(0.04f, 0.05f, 0.07f, 0.9f), Viewport);

	// 입력 줄
	const float   InputTop = Panel.Max.Y - LineHeight - Padding;
	const FUIRect InputBox(FVector2(Panel.Min.X, InputTop - Padding * 0.5f), FVector2(Panel.Max.X, Panel.Max.Y));
	FUIDebugDraw::AddRect(Out, InputBox, FVector4(0.1f, 0.12f, 0.16f, 0.95f), Viewport);
	FUIDebugDraw::AddRect(Out, FUIRect(InputBox.Min, FVector2(InputBox.Max.X, InputBox.Min.Y + 1.0f)), FVector4(0.3f, 0.55f, 0.9f, 1.0f), Viewport);
	const FVector2 PromptPosition(Panel.Min.X + Padding, InputTop);
	const float    PromptWidth = FUIDebugDraw::AddText(Out, ">", PromptPosition, FontSize, FVector4(0.55f, 0.85f, 1.0f, 1.0f), Viewport) + FontSize * 0.5f;
	const FVector2 TextPosition(PromptPosition.X + PromptWidth, InputTop);
	const std::string Text = GetInputText();
	FUIDebugDraw::AddText(Out, Text, TextPosition, FontSize, FVector4(1.0f, 1.0f, 1.0f, 1.0f), Viewport);

	// 선택 영역 + 캐럿 (글자 경계 X)
	std::vector<float> CaretPositions;
	if (FUIFont* Font = FUIDebugDraw::GetFont())
	{
		Font->GetCaretPositions(Text, FontSize, CaretPositions);
	}
	const auto CaretX = [&](int32 Index) {
		return CaretPositions.empty() ? 0.0f : CaretPositions[static_cast<size_t>(std::clamp(Index, 0, static_cast<int32>(CaretPositions.size()) - 1))];
	};
	if (Edit.HasSelection())
	{
		const float X0 = TextPosition.X + CaretX(Edit.GetSelectionBegin());
		const float X1 = TextPosition.X + CaretX(Edit.GetSelectionEnd());
		FUIDebugDraw::AddRect(Out, FUIRect(FVector2(X0, InputTop), FVector2(X1, InputTop + LineHeight)), FVector4(0.3f, 0.5f, 0.9f, 0.45f), Viewport);
	}
	const float CaretLeft = TextPosition.X + CaretX(Edit.Caret);
	CaretRect             = FUIRect(FVector2(CaretLeft, InputTop), FVector2(CaretLeft + 2.0f, InputTop + LineHeight));
	if (std::fmod(CaretBlink, 1.0f) < 0.6f)
	{
		FUIDebugDraw::AddRect(Out, CaretRect, FVector4(1.0f, 1.0f, 1.0f, 0.9f), Viewport);
	}

	// 출력 (아래에서 위로, 스크롤만큼 건너뜀)
	const FUIRect OutputClip(FVector2(Panel.Min.X, Panel.Min.Y), FVector2(Panel.Max.X, InputBox.Min.Y));
	float         Y     = InputBox.Min.Y - LineHeight - 2.0f;
	int32         Skip  = ScrollLines;
	for (auto It = Lines.rbegin(); It != Lines.rend() && Y + LineHeight > Panel.Min.Y; ++It)
	{
		if (Skip-- > 0)
		{
			continue;
		}
		FUIDebugDraw::AddText(Out, It->Text, FVector2(Panel.Min.X + Padding, Y), FontSize, GetLineColor(It->Verbosity, It->Text), OutputClip);
		Y -= LineHeight;
	}
	if (ScrollLines > 0)
	{
		FUIDebugDraw::AddText(Out, std::format("위로 {}줄 (PageDown)", ScrollLines), FVector2(Panel.Max.X - 200.0f, Panel.Min.Y + 4.0f), FontSize * 0.85f,
		                      FVector4(1.0f, 0.82f, 0.35f, 1.0f), Viewport);
	}

	// 자동 완성 후보 (입력 줄 아래)
	if (!CompletionTexts.empty())
	{
		float NameWidth = 0.0f;
		for (const std::string& Name : CompletionTexts)
		{
			NameWidth = std::max(NameWidth, FUIDebugDraw::MeasureText(Name, FontSize));
		}
		const float   ListWidth = std::min(Viewport.GetWidth() - Padding * 2.0f, NameWidth + 520.0f);
		const FUIRect List(FVector2(TextPosition.X - 4.0f, Panel.Max.Y + 2.0f),
		                   FVector2(TextPosition.X - 4.0f + ListWidth, Panel.Max.Y + 2.0f + LineHeight * static_cast<float>(CompletionTexts.size()) + 4.0f));
		FUIDebugDraw::AddRect(Out, List, FVector4(0.08f, 0.09f, 0.12f, 0.96f), Viewport);
		float RowY = List.Min.Y + 2.0f;
		for (size_t Index = 0; Index < CompletionTexts.size(); ++Index)
		{
			if (static_cast<int32>(Index) == SelectedCompletion)
			{
				FUIDebugDraw::AddRect(Out, FUIRect(FVector2(List.Min.X, RowY), FVector2(List.Max.X, RowY + LineHeight)), FVector4(0.2f, 0.4f, 0.75f, 0.9f), Viewport);
			}
			FUIDebugDraw::AddText(Out, CompletionTexts[Index], FVector2(List.Min.X + 4.0f, RowY), FontSize, FVector4(1.0f, 1.0f, 1.0f, 1.0f), List);
			FUIDebugDraw::AddText(Out, CompletionDetails[Index], FVector2(List.Min.X + NameWidth + 24.0f, RowY), FontSize, FVector4(0.6f, 0.65f, 0.7f, 1.0f),
			                      List);
			RowY += LineHeight;
		}
	}
}
