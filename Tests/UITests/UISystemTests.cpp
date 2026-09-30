#include "Core/Input.h"
#include "Core/Testing/TestFramework.h"
#include "Scene/Scene.h"
#include "UI/UIComponent.h"
#include "UI/UIInstance.h"
#include "UI/UISystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
	namespace fs = std::filesystem;

	fs::path GetContent()
	{
		static const fs::path Directory = [] {
			const fs::path  Path = fs::temp_directory_path() / L"ProjectEUISystemTests";
			std::error_code ErrorCode;
			fs::remove_all(Path, ErrorCode);
			fs::create_directories(Path / L"UI");
			return Path;
		}();
		return Directory;
	}

	// 캔버스 루트 + (X, Y, W, H) 버튼 하나. 설계 해상도 = 화면(1000x500), 배율 없음
	void WriteButtonUI(const char* File, const char* ButtonName, float X, float Y, float Width, float Height)
	{
		FUIAsset Asset;
		Asset.DesignSize     = FVector2(1000.0f, 500.0f);
		Asset.ScaleMode      = EUIScaleMode::None;
		FUIWidget* Button    = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Button));
		Button->Name         = ButtonName;
		Button->Slot.Offsets = FUIMargin(X, Y, Width, Height);
		E_EXPECT_TRUE(Asset.SaveToFile(GetContent() / File));
	}

	FUIFrameInput MakeInput(float X, float Y, bool bPressed = false)
	{
		FUIFrameInput Input;
		Input.Viewport         = FUIRect(FVector2::ZeroVector, FVector2(1000.0f, 500.0f));
		Input.bHasPointer      = true;
		Input.Pointer.bInside  = true;
		Input.Pointer.Position = FVector2(X, Y);
		Input.Pointer.bPressed = bPressed;
		Input.Pointer.bDown    = bPressed;
		return Input;
	}

	bool HasEvent(const FUIComponent& Component, EUIEventType Type, const char* Name)
	{
		for (const FUIEvent& Event : Component.Runtime.Events)
		{
			if (Event.Type == Type && Event.WidgetName == Name)
			{
				return true;
			}
		}
		return false;
	}
} // namespace

E_TEST(UISystem_TopUIGetsPointerFirst)
{
	WriteButtonUI("UI/Bottom.eui", "Big", 0.0f, 0.0f, 1000.0f, 500.0f);
	WriteButtonUI("UI/Top.eui", "Small", 0.0f, 0.0f, 100.0f, 100.0f);

	FScene        Scene;
	const FEntity BottomEntity = Scene.CreateEntity("Bottom");
	const FEntity TopEntity    = Scene.CreateEntity("Top");
	Scene.GetRegistry().Emplace<FUIComponent>(BottomEntity).Asset = "UI/Bottom.eui";
	FUIComponent& TopComponent  = Scene.GetRegistry().Emplace<FUIComponent>(TopEntity);
	TopComponent.Asset          = "UI/Top.eui";
	TopComponent.ZOrder         = 5;

	// 두 UI가 겹친 곳: 위(ZOrder 5)만 호버
	E_EXPECT_TRUE(FUISystem::Update(Scene, MakeInput(50.0f, 50.0f), GetContent()));
	const FUIComponent& Top    = Scene.GetRegistry().Get<FUIComponent>(TopEntity);
	const FUIComponent& Bottom = Scene.GetRegistry().Get<FUIComponent>(BottomEntity);
	E_EXPECT_TRUE(HasEvent(Top, EUIEventType::HoverBegin, "Small"));
	E_EXPECT_TRUE(Bottom.Runtime.Events.empty());
	E_EXPECT_TRUE(Top.Runtime.bPointerOver && !Bottom.Runtime.bPointerOver);

	// 위 UI 밖: 아래 UI가 받는다 (이전 프레임 이벤트는 비워진다)
	E_EXPECT_TRUE(FUISystem::Update(Scene, MakeInput(500.0f, 300.0f), GetContent()));
	E_EXPECT_TRUE(HasEvent(Scene.GetRegistry().Get<FUIComponent>(BottomEntity), EUIEventType::HoverBegin, "Big"));
	E_EXPECT_TRUE(HasEvent(Scene.GetRegistry().Get<FUIComponent>(TopEntity), EUIEventType::HoverEnd, "Small"));

	// 입력 끔 / 숨김: 포인터를 가져가지 않는다, 숨기면 그리지도 않는다
	Scene.GetRegistry().Get<FUIComponent>(BottomEntity).bReceiveInput = false;
	E_EXPECT_FALSE(FUISystem::Update(Scene, MakeInput(500.0f, 300.0f), GetContent()));
	FUIDrawList List;
	FUISystem::Paint(Scene, List);
	E_EXPECT_EQ(List.Quads.size(), size_t(2));
	Scene.GetRegistry().Get<FUIComponent>(TopEntity).bVisible = false;
	List.Clear();
	FUISystem::Update(Scene, MakeInput(50.0f, 50.0f), GetContent());
	FUISystem::Paint(Scene, List);
	E_EXPECT_EQ(List.Quads.size(), size_t(1));
}

E_TEST(UISystem_CopyDoesNotShareRuntimeAndAssetReloads)
{
	WriteButtonUI("UI/Reload.eui", "First", 0.0f, 0.0f, 10.0f, 10.0f);
	FUIComponent Component;
	Component.Asset       = "UI/Reload.eui";
	FUIInstance* Instance = FUISystem::EnsureInstance(Component, GetContent());
	E_EXPECT_TRUE(Instance != nullptr && Instance->FindWidget("First") != nullptr);

	// 플레이 모드 복제처럼 값 복사 → 런타임은 비어 있다 (인스턴스를 공유하지 않음)
	const FUIComponent Copy = Component;
	E_EXPECT_TRUE(Copy.Runtime.Instance == nullptr && Copy.Asset == Component.Asset);

	// 파일이 바뀌면(수정 시각) 새 인스턴스부터 반영
	WriteButtonUI("UI/Reload.eui", "Second", 0.0f, 0.0f, 10.0f, 10.0f);
	std::filesystem::last_write_time(GetContent() / L"UI/Reload.eui", std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
	FUIComponent Fresh;
	Fresh.Asset = "UI/Reload.eui";
	FUIInstance* Reloaded = FUISystem::EnsureInstance(Fresh, GetContent());
	E_EXPECT_TRUE(Reloaded != nullptr && Reloaded->FindWidget("Second") != nullptr);

	// 없는 에셋은 nullptr
	FUIComponent Missing;
	Missing.Asset = "UI/Nope.eui";
	E_EXPECT_TRUE(FUISystem::EnsureInstance(Missing, GetContent()) == nullptr);
}

E_TEST(UISystem_InputWithoutMouseButtons)
{
	FInput       Input;
	FWindowEvent Event;
	Event.Type   = EWindowEventType::MouseButtonDown;
	Event.Button = EMouseButton::Left;
	Event.MouseX = 30;
	Event.MouseY = 40;
	Input.ProcessEvent(Event);
	FWindowEvent Key;
	Key.Type = EWindowEventType::KeyDown;
	Key.Key  = EKey::W;
	Input.ProcessEvent(Key);
	E_EXPECT_TRUE(Input.IsMouseButtonPressed(EMouseButton::Left));

	const FInput Blocked = Input.WithoutMouseButtons();
	E_EXPECT_FALSE(Blocked.IsMouseButtonDown(EMouseButton::Left));
	E_EXPECT_FALSE(Blocked.IsMouseButtonPressed(EMouseButton::Left));
	E_EXPECT_TRUE(Blocked.IsKeyDown(EKey::W)); // 키보드는 그대로
	E_EXPECT_EQ(Blocked.GetMouseX(), 30);

	const FUIPointerInput Pointer = FUISystem::MakePointer(Input, FVector2(-10.0f, -20.0f), true);
	E_EXPECT_NEAR(Pointer.Position.X, 20.0f, 0.0f);
	E_EXPECT_NEAR(Pointer.Position.Y, 20.0f, 0.0f);
	E_EXPECT_TRUE(Pointer.bPressed && Pointer.bDown);
}
