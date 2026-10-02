#include "Core/Testing/TestFramework.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"
#include "UI/UIComponent.h"
#include "UI/UIInstance.h"
#include "UI/UISystem.h"

#include <filesystem>
#include <fstream>

namespace
{
	namespace fs = std::filesystem;

	fs::path GetUIContent()
	{
		static const fs::path Directory = [] {
			const fs::path  Path = FTestRegistry::GetTempDirectory() / L"ProjectEUIScriptTests";
			std::error_code ErrorCode;
			fs::remove_all(Path, ErrorCode);
			fs::create_directories(Path / L"Scripts");
			fs::create_directories(Path / L"UI");
			return Path;
		}();
		return Directory;
	}

	FUIFrameInput MakePointer(float X, float Y, bool bDown, bool bPressed, bool bReleased)
	{
		FUIFrameInput Input;
		Input.Viewport          = FUIRect(FVector2::ZeroVector, FVector2(800.0f, 600.0f));
		Input.bHasPointer       = true;
		Input.Pointer.bInside   = true;
		Input.Pointer.Position  = FVector2(X, Y);
		Input.Pointer.bDown     = bDown;
		Input.Pointer.bPressed  = bPressed;
		Input.Pointer.bReleased = bReleased;
		return Input;
	}
} // namespace

E_TEST(UIScript_WidgetValuesAndClickEvent)
{
	const fs::path Content = GetUIContent();
	{
		FUIAsset Asset;
		Asset.DesignSize = FVector2(800.0f, 600.0f);
		Asset.ScaleMode  = EUIScaleMode::None;
		FUIWidget* Button = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Button));
		Button->Name         = "PlayButton";
		Button->Slot.Offsets = FUIMargin(100.0f, 100.0f, 200.0f, 50.0f);
		FUIWidget* Label = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Text));
		Label->Name      = "Label";
		FUIWidget* Bar   = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::ProgressBar));
		Bar->Name        = "Bar";
		E_EXPECT_TRUE(Asset.SaveToFile(Content / L"UI/Menu.eui"));
	}
	std::ofstream(Content / L"Scripts/Menu.lua", std::ios::binary | std::ios::trunc) << R"(
local Menu = { Properties = {} }
function Menu:OnStart()
	self.Label = self.entity:GetWidget("Label")
	self.Label.Text = "시작 전"
	Missing = self.entity:GetWidget("NoSuchWidget")
	LabelType = self.Label.Type
end
function Menu:OnUIClicked_PlayButton()
	Clicks = (Clicks or 0) + 1
	self.Label.Text = "시작!"
	local Bar = self.entity:GetWidget("Bar")
	Bar.Percent = 2.0 -- 0~1로 잘린다
	self.entity:GetWidget("PlayButton").Enabled = false
	self.entity:GetWidget("Label").Visible = false
end
function Menu:OnUIHoverBegin_PlayButton()
	Hovers = (Hovers or 0) + 1
end
return Menu
)";

	FScene        Scene;
	const FEntity Entity = Scene.CreateEntity("Menu");
	Scene.GetRegistry().Emplace<FUIComponent>(Entity).Asset            = "UI/Menu.eui";
	Scene.GetRegistry().Emplace<FScriptComponent>(Entity).ScriptAsset = "Scripts/Menu.lua";

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(Content);
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));

	// 프레임 순서 = UI 갱신 → 스크립트 (앱과 같다)
	FUISystem::Update(Scene, MakePointer(10.0f, 10.0f, false, false, false), Content);
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_TRUE(Scripts.RunString("assert(Missing == nil and LabelType == 'Text')"));
	FUIInstance* Instance = Scene.GetRegistry().Get<FUIComponent>(Entity).Runtime.Instance.get();
	E_EXPECT_TRUE(Instance != nullptr);
	if (Instance == nullptr)
	{
		return;
	}
	E_EXPECT_EQ(Instance->FindWidget("Label")->Text, std::string("시작 전"));

	FUISystem::Update(Scene, MakePointer(150.0f, 120.0f, true, true, false), Content); // 호버 + 누름
	Scripts.Update(0.016f, nullptr);
	FUISystem::Update(Scene, MakePointer(150.0f, 120.0f, false, false, true), Content); // 뗌 → 클릭
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_TRUE(Scripts.RunString("assert(Clicks == 1 and Hovers == 1)"));
	E_EXPECT_EQ(Instance->FindWidget("Label")->Text, std::string("시작!"));
	E_EXPECT_NEAR(Instance->FindWidget("Bar")->Percent, 1.0f, 0.0f);
	E_EXPECT_FALSE(Instance->FindWidget("PlayButton")->bEnabled);
	E_EXPECT_TRUE(Instance->FindWidget("Label")->Visibility == EUIVisibility::Collapsed);

	// 비활성 버튼은 다시 클릭되지 않는다
	FUISystem::Update(Scene, MakePointer(150.0f, 120.0f, true, true, false), Content);
	Scripts.Update(0.016f, nullptr);
	FUISystem::Update(Scene, MakePointer(150.0f, 120.0f, false, false, true), Content);
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_TRUE(Scripts.RunString("assert(Clicks == 1)"));
	Scripts.EndPlay();
}

E_TEST(UIScript_AnimationPlayAndFinishEvent)
{
	const fs::path Content = GetUIContent();
	{
		FUIAsset Asset;
		Asset.DesignSize  = FVector2(800.0f, 600.0f);
		FUIWidget* Banner = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Border));
		Banner->Name      = "Banner";
		FUIAnimation Fade;
		Fade.Name   = "Fade";
		Fade.Length = 0.5f;
		FUIAnimTrack& Track = Fade.GetOrAddTrack("Banner", EUIAnimProperty::Opacity);
		Track.SetKey(0.0f, 1.0f);
		Track.SetKey(0.5f, 0.0f);
		Asset.Animations.push_back(Fade);
		E_EXPECT_TRUE(Asset.SaveToFile(Content / L"UI/Anim.eui"));
	}
	std::ofstream(Content / L"Scripts/Anim.lua", std::ios::binary | std::ios::trunc) << R"(
local Anim = { Properties = {} }
function Anim:OnStart()
	Started = self.entity:PlayUIAnimation("Fade")
	Missing = self.entity:PlayUIAnimation("Nope")
end
function Anim:OnUpdate(dt)
	Playing = self.entity:IsUIAnimationPlaying("Fade")
end
function Anim:OnUIAnimationFinished_Fade()
	Finished = (Finished or 0) + 1
end
return Anim
)";
	FScene        Scene;
	const FEntity Entity = Scene.CreateEntity("Anim");
	Scene.GetRegistry().Emplace<FUIComponent>(Entity).Asset            = "UI/Anim.eui";
	Scene.GetRegistry().Emplace<FScriptComponent>(Entity).ScriptAsset = "Scripts/Anim.lua";
	FScriptSystem Scripts;
	Scripts.SetContentDirectory(Content);
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));

	FUIFrameInput Input = MakePointer(0.0f, 0.0f, false, false, false);
	Input.DeltaSeconds  = 0.2f;
	FUISystem::Update(Scene, Input, Content);
	Scripts.Update(0.2f, nullptr); // OnStart: 재생 시작
	E_EXPECT_TRUE(Scripts.RunString("assert(Started == true and Missing == false)"));
	for (int32 Frame = 0; Frame < 4; ++Frame) // 0.8초 → 끝
	{
		FUISystem::Update(Scene, Input, Content);
		Scripts.Update(0.2f, nullptr);
	}
	E_EXPECT_TRUE(Scripts.RunString("assert(Finished == 1 and Playing == false)"));
	const FUIInstance* Instance = Scene.GetRegistry().Get<FUIComponent>(Entity).Runtime.Instance.get();
	E_EXPECT_TRUE(Instance != nullptr && Instance->GetRoot().FindByName("Banner")->RenderOpacity == 0.0f);
	Scripts.EndPlay();
}

// Phase 45: Camera.WorldToScreen/ScreenToWorldRay(UI 레이아웃 좌표) + 위젯 Position/Size + CloneWidget/RemoveWidget
E_TEST(UIScript_WorldToScreenCloneAndPlaceWidgets)
{
	const fs::path Content = GetUIContent();
	{
		FUIAsset Asset;
		Asset.DesignSize = FVector2(400.0f, 300.0f);
		Asset.ScaleMode  = EUIScaleMode::Fit; // 800x600 뷰포트 → 배율 2 (레이아웃 400x300)
		FUIWidget* Tag    = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Border));
		Tag->Name         = "Tag";
		Tag->Visibility   = EUIVisibility::Collapsed; // 숨긴 템플릿
		Tag->Slot.Offsets = FUIMargin(0.0f, 0.0f, 40.0f, 10.0f);
		FUIWidget* Icon   = Tag->AddChild(FUIWidget::Create(EUIWidgetType::Image));
		Icon->Name        = "Icon";
		FUIWidget* Stretch      = Asset.Root->AddChild(FUIWidget::Create(EUIWidgetType::Border));
		Stretch->Name           = "Stretch";
		Stretch->Slot.AnchorMax = FVector2(1.0f, 0.0f); // 가로 늘이기
		E_EXPECT_TRUE(Asset.SaveToFile(Content / L"UI/Tags.eui"));
	}
	std::ofstream(Content / L"Scripts/Tags.lua", std::ios::binary | std::ios::trunc) << R"(
local Tags = { Properties = {} }
function Tags:OnStart()
	CX, CY, CV = Camera.WorldToScreen(Vector3(1000, 0, 0))          -- 화면 가운데
	RX, RY, RV = Camera.WorldToScreen(Vector3(1000, 1000, 500))     -- 오른쪽 위 (FOV 90, 4:3)
	_, _, BV   = Camera.WorldToScreen(Vector3(-1000, 0, 0))         -- 카메라 뒤
	_, _, OV   = Camera.WorldToScreen(Vector3(1000, 5000, 0))       -- 화면 밖
	_, _, EV   = Camera.WorldToScreen(Vector3(1000, 0, 0), self.entity) -- UI 엔티티 명시
	RayO, RayD = Camera.ScreenToWorldRay(200, 150)

	local W = self.entity:CloneWidget("Tag", "Tag_1")
	W.Visible  = true
	W.Position = Vector2(RX, RY)
	W.Size     = Vector2(50, 20)
	PosX, SizeY = W.Position.X, W.Size.Y
	ChildFound = self.entity:GetWidget("Tag_1.Icon") ~= nil
	DupOk = pcall(self.entity.CloneWidget, self.entity, "Tag", "Tag_1")
	StretchOk = pcall(function() self.entity:GetWidget("Stretch").Position = Vector2(1, 1) end)
	self.entity:CloneWidget("Tag", "Tag_2")
	Removed = self.entity:RemoveWidget("Tag_2")
	RemovedAgain = self.entity:RemoveWidget("Tag_2")
end
return Tags
)";

	FScene            Scene;
	const FEntity     CameraEntity = Scene.CreateEntity("Camera");
	FCameraComponent& Camera       = Scene.GetRegistry().Emplace<FCameraComponent>(CameraEntity);
	Camera.FovYDegrees             = 90.0f;
	const FEntity Low              = Scene.CreateEntity("LowPriority");
	Scene.GetRegistry().Emplace<FCameraComponent>(Low).Priority = -1; // 우선순위가 낮은 다른 주 카메라는 무시
	Scene.GetTransform(Low).Position = FVector3(0.0f, 0.0f, 9999.0f);
	Scene.UpdateTransforms();
	const FEntity Entity = Scene.CreateEntity("Tags");
	Scene.GetRegistry().Emplace<FUIComponent>(Entity).Asset            = "UI/Tags.eui";
	Scene.GetRegistry().Emplace<FScriptComponent>(Entity).ScriptAsset = "Scripts/Tags.lua";

	FScriptSystem Scripts;
	Scripts.SetContentDirectory(Content);
	E_EXPECT_TRUE(Scripts.BeginPlay(Scene));
	FUISystem::Update(Scene, MakePointer(0.0f, 0.0f, false, false, false), Content);
	Scripts.Update(0.016f, nullptr);
	E_EXPECT_EQ(Scripts.GetErrorCount(), 0u);
	E_EXPECT_TRUE(Scripts.RunString("assert(math.abs(CX - 200) < 1e-3 and math.abs(CY - 150) < 1e-3 and CV == true, CX .. ',' .. CY)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(math.abs(RX - 350) < 1e-2 and math.abs(RY - 75) < 1e-2 and RV == true, RX .. ',' .. RY)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(BV == false and OV == false and EV == true)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(math.abs(RayO.X - 10) < 1e-2 and math.abs(RayD.X - 1) < 1e-4 and math.abs(RayD.Y) < 1e-4)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(math.abs(PosX - RX) < 1e-4 and SizeY == 20 and ChildFound)"));
	E_EXPECT_TRUE(Scripts.RunString("assert(DupOk == false and StretchOk == false and Removed == true and RemovedAgain == false)"));

	// 쓰기 → 그리기 전에 다시 레이아웃 (같은 프레임)
	FUIInstance* Instance = Scene.GetRegistry().Get<FUIComponent>(Entity).Runtime.Instance.get();
	E_EXPECT_TRUE(Instance != nullptr && Instance->IsLayoutDirty());
	if (Instance == nullptr)
	{
		return;
	}
	FUIDrawList DrawList;
	FUISystem::Paint(Scene, DrawList);
	E_EXPECT_FALSE(Instance->IsLayoutDirty());
	const FUIWidget* Clone = Instance->FindWidget("Tag_1");
	E_EXPECT_TRUE(Clone != nullptr && Instance->FindWidget("Tag_2") == nullptr && Instance->FindWidget("Tag")->Visibility == EUIVisibility::Collapsed);
	if (Clone != nullptr)
	{
		E_EXPECT_NEAR(Clone->State.Geometry.Min.X, 350.0f, 1.0e-2f);
		E_EXPECT_NEAR(Clone->State.Geometry.Min.Y, 75.0f, 1.0e-2f);
		E_EXPECT_NEAR(Clone->State.Geometry.GetWidth(), 50.0f, 1.0e-3f);
		// 화면 픽셀 = 레이아웃 × 배율 2 → 월드 점이 투영된 픽셀 (700, 150)
		const FVector2 Pixel = Instance->GetTransform().ToPixels(Clone->State.Geometry.Min);
		E_EXPECT_NEAR(Pixel.X, 700.0f, 2.0e-2f);
		E_EXPECT_NEAR(Pixel.Y, 150.0f, 2.0e-2f);
	}
	Scripts.EndPlay();
}
