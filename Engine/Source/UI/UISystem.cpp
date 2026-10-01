#include "UI/UISystem.h"

#include "Core/FileSystem.h"
#include "Core/Input.h"
#include "Core/StringConv.h"
#include "Scene/Scene.h"
#include "UI/UIComponent.h"
#include "UI/UIFont.h"
#include "UI/UIInstance.h"
#include "UI/UIPlatform.h"

#include <algorithm>
#include <cwctype>

namespace
{
	struct FOrderedUI
	{
		FUIComponent* Component = nullptr;
		int32         ZOrder    = 0;
		uint32        Order     = 0; // 같은 Z면 씬 순서
	};

	// 보이는 UI 컴포넌트를 Z 순서 오름차순으로
	std::vector<FOrderedUI> CollectVisible(FScene& Scene)
	{
		std::vector<FOrderedUI> Result;
		Scene.GetRegistry().View<FUIComponent>().Each([&](FEntity, FUIComponent& Component) {
			if (Component.bVisible && !Component.Asset.empty())
			{
				Result.push_back({ &Component, Component.ZOrder, static_cast<uint32>(Result.size()) });
			}
		});
		std::sort(Result.begin(), Result.end(), [](const FOrderedUI& A, const FOrderedUI& B) { return A.ZOrder != B.ZOrder ? A.ZOrder < B.ZOrder : A.Order < B.Order; });
		return Result;
	}
} // namespace

// ---------------------------------------------------------------- FUIAssetLibrary

FUIAssetLibrary& FUIAssetLibrary::Get()
{
	static FUIAssetLibrary Library;
	return Library;
}

std::shared_ptr<const FUIAsset> FUIAssetLibrary::Load(const std::filesystem::path& Path)
{
	std::wstring Key = Path.lexically_normal().generic_wstring();
	std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });

	const std::optional<std::filesystem::file_time_type> FoundTime = FFileSystem::GetLastWriteTime(Path); // pak 항목은 pak 시각
	const bool                                           bMissing  = !FoundTime.has_value();
	const std::filesystem::file_time_type                WriteTime = FoundTime.value_or(std::filesystem::file_time_type{});
	const auto                                           It        = Entries.find(Key);
	if (It != Entries.end() && It->second.WriteTime == WriteTime)
	{
		return It->second.Asset;
	}
	auto Asset = std::make_shared<FUIAsset>();
	FEntry Entry;
	Entry.WriteTime = WriteTime;
	if (!bMissing && Asset->LoadFromFile(Path))
	{
		Entry.Asset = std::move(Asset);
	}
	else if (bMissing)
	{
		E_LOG(LogUI, Error, "UI 에셋이 없습니다: {}", FStringConv::ToUtf8(Path.wstring()));
	}
	auto& Stored = Entries[Key];
	Stored       = std::move(Entry);
	return Stored.Asset;
}

// ---------------------------------------------------------------- FUISystem

FUIInstance* FUISystem::EnsureInstance(FUIComponent& Component, const std::filesystem::path& ContentDirectory)
{
	FUIComponentRuntime& Runtime = Component.Runtime;
	if (Runtime.Instance && Runtime.LoadedAsset == Component.Asset)
	{
		return Runtime.Instance.get();
	}
	Runtime.Instance.reset();
	Runtime.LoadedAsset = Component.Asset;
	if (Component.Asset.empty())
	{
		return nullptr;
	}
	const std::filesystem::path     Relative = FStringConv::ToWide(Component.Asset);
	std::shared_ptr<const FUIAsset> Asset    = FUIAssetLibrary::Get().Load(Relative.is_absolute() ? Relative : ContentDirectory / Relative);
	if (!Asset)
	{
		return nullptr;
	}
	Runtime.Instance = std::make_shared<FUIInstance>(*Asset);
	return Runtime.Instance.get();
}

FUIInputResult FUISystem::Update(FScene& Scene, const FUIFrameInput& Input, const std::filesystem::path& ContentDirectory)
{
	FUIFontLibrary& Fonts = FUIFontLibrary::Get();
	Fonts.SetContentDirectory(ContentDirectory);

	// 지난 프레임 이벤트 비우기 (숨긴 UI 포함)
	Scene.GetRegistry().View<FUIComponent>().Each([](FEntity, FUIComponent& Component) {
		Component.Runtime.Events.clear();
		Component.Runtime.bPointerOver = false;
	});

	std::vector<FOrderedUI> Ordered = CollectVisible(Scene);
	bool                    bTaken  = false; // 위 UI가 포인터를 가져갔으면 아래 UI는 포인터 밖으로
	bool                    bKeysTaken = false;
	FUIInputResult          Result;
	for (auto It = Ordered.rbegin(); It != Ordered.rend(); ++It)
	{
		FUIComponent& Component = *It->Component;
		FUIInstance*  Instance  = EnsureInstance(Component, ContentDirectory);
		if (Instance == nullptr)
		{
			continue;
		}
		const bool      bInput  = Component.bReceiveInput && Input.bHasPointer;
		FUIPointerInput Pointer = Input.Pointer;
		Pointer.bInside         = Pointer.bInside && !bTaken;
		if (bTaken)
		{
			Pointer.bPressed = false;
			Pointer.Wheel    = 0.0f;
		}
		FUIKeyInput Keys;
		if (!bKeysTaken)
		{
			Keys = Input.Keys;
			if (!Component.bKeyboardFocus)
			{
				// 버튼 탐색 키는 메뉴형 UI만 (게임 키와 겹침) — 텍스트 상자 편집은 항상
				Keys.bActivate = Keys.bFocusNext = Keys.bFocusPrevious = false;
			}
		}
		if (bInput)
		{
			Component.Runtime.bPointerOver = Instance->Update(Input.Viewport, &Pointer, &Keys, Fonts, Component.Runtime.Events, Input.DeltaSeconds);
			bTaken                         = bTaken || Component.Runtime.bPointerOver;
			// 복사/잘라내기 → OS 클립보드
			if (std::string Copied; Instance->GetInputRouter().TakeClipboardText(Copied))
			{
				FUIPlatform::SetClipboardText(Copied);
			}
			if (!bKeysTaken && Instance->WantsKeyboard())
			{
				bKeysTaken           = true;
				Result.bHasTextCaret = Instance->GetTextCaretPixels(Result.TextCaret, Fonts);
			}
		}
		else
		{
			Instance->Layout(Input.Viewport, Fonts);
		}
	}
	Result.bPointer  = bTaken;
	Result.bKeyboard = bKeysTaken;
	return Result;
}

void FUISystem::Paint(FScene& Scene, FUIDrawList& Out)
{
	FUIFontLibrary& Fonts = FUIFontLibrary::Get();
	for (const FOrderedUI& Entry : CollectVisible(Scene))
	{
		if (const FUIInstance* Instance = Entry.Component->Runtime.Instance.get(); Instance != nullptr && Entry.Component->Runtime.LoadedAsset == Entry.Component->Asset)
		{
			Instance->Paint(Out, Fonts);
		}
	}
}

FUIPointerInput FUISystem::MakePointer(const FInput& Input, const FVector2& PixelOffset, bool bInside)
{
	FUIPointerInput Pointer;
	Pointer.Position  = FVector2(static_cast<float>(Input.GetMouseX()), static_cast<float>(Input.GetMouseY())) + PixelOffset;
	Pointer.bInside   = bInside;
	Pointer.bDown     = Input.IsMouseButtonDown(EMouseButton::Left);
	Pointer.bPressed  = Input.IsMouseButtonPressed(EMouseButton::Left);
	Pointer.bReleased = Input.IsMouseButtonReleased(EMouseButton::Left);
	Pointer.Wheel     = Input.GetMouseWheelDelta();
	return Pointer;
}

FUIKeyInput FUISystem::MakeKeys(const FInput& Input)
{
	FUIKeyInput Keys;
	const bool  bShift  = Input.IsKeyDown(EKey::LeftShift) || Input.IsKeyDown(EKey::RightShift);
	Keys.bActivate      = Input.IsKeyPressed(EKey::Enter) || Input.IsKeyPressed(EKey::Space);
	Keys.bFocusNext     = Input.IsKeyPressed(EKey::Tab) && !bShift;
	Keys.bFocusPrevious = Input.IsKeyPressed(EKey::Tab) && bShift;
	for (const char32_t Char : Input.GetTypedText())
	{
		if (Char >= 0x20 && Char != 0x7F)
		{
			Keys.Typed.push_back(Char); // 제어 문자(Backspace 0x08, Enter 0x0D, Tab 0x09 등)는 아래 키로
		}
	}
	const bool bControl = Input.IsKeyDown(EKey::LeftControl) || Input.IsKeyDown(EKey::RightControl);
	const bool bAlt     = Input.IsKeyDown(EKey::LeftAlt) || Input.IsKeyDown(EKey::RightAlt);
	const bool bShortcut = bControl && !bAlt; // AltGr(= Ctrl+Alt) 글자 입력과 구분
	Keys.bShift         = bShift;
	Keys.bWordMove      = bShortcut;
	Keys.bSelectAll     = bShortcut && Input.IsKeyPressed(EKey::A);
	Keys.bCopy          = bShortcut && (Input.IsKeyPressed(EKey::C) || Input.IsKeyPressed(EKey::Insert));
	Keys.bCut           = bShortcut && Input.IsKeyRepeated(EKey::X);
	Keys.bPaste         = (bShortcut && Input.IsKeyRepeated(EKey::V)) || (bShift && !bControl && Input.IsKeyPressed(EKey::Insert));
	if (Keys.bPaste)
	{
		Keys.PasteText = FUIPlatform::GetClipboardText(); // Ctrl+V를 누른 프레임에만 OS 클립보드를 읽는다
	}
	Keys.Composition       = Input.GetCompositionText();
	Keys.CompositionCursor = Input.GetCompositionCursor();
	Keys.bBackspace = Input.IsKeyRepeated(EKey::Backspace);
	Keys.bDelete    = Input.IsKeyRepeated(EKey::Delete);
	Keys.bLeft      = Input.IsKeyRepeated(EKey::Left);
	Keys.bRight     = Input.IsKeyRepeated(EKey::Right);
	Keys.bHome      = Input.IsKeyPressed(EKey::Home);
	Keys.bEnd       = Input.IsKeyPressed(EKey::End);
	Keys.bCommit    = Input.IsKeyPressed(EKey::Enter) || Input.IsKeyPressed(EKey::NumpadEnter);
	Keys.bCancel    = Input.IsKeyPressed(EKey::Escape);
	return Keys;
}
