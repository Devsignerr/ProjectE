#include "Editor/AssetEditors/WidgetEditor.h"

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "Renderer/Image.h"
#include "Renderer/UIRenderer.h"
#include "UI/UIFont.h"
#include "UI/UILayout.h"
#include "UI/UIPainter.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <format>
#include <unordered_set>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	constexpr const char* GPalettePayload = "E_UI_PALETTE"; // 페이로드 = EUIWidgetType (int32)
	constexpr const char* GWidgetPayload  = "E_UI_WIDGET";  // 페이로드 없음, 경로는 GDraggedPath

	std::vector<int32> GDraggedPath; // 계층에서 끄는 위젯 (한 번에 하나)

	// 캔버스 배경 (선형 클리어 값 — 타깃 최적 클리어 값과 같아야 디버그 레이어 경고가 없다)
	constexpr float GCanvasClearColor[4] = { 0.018f, 0.02f, 0.025f, 1.0f };

	struct FPaletteEntry
	{
		EUIWidgetType Type;
		const char*   Icon;
		const char*   Label;
		const char*   Tooltip;
	};
	constexpr FPaletteEntry GPalette[] = {
		{ EUIWidgetType::Canvas, ICON_FA_OBJECT_GROUP, "캔버스", "자식을 앵커/위치로 자유 배치" },
		{ EUIWidgetType::HorizontalBox, ICON_FA_TABLE_COLUMNS, "가로 박스", "자식을 가로로 나열 (자동/채우기)" },
		{ EUIWidgetType::VerticalBox, ICON_FA_GRIP_LINES, "세로 박스", "자식을 세로로 나열 (자동/채우기)" },
		{ EUIWidgetType::Overlay, ICON_FA_LAYER_GROUP, "오버레이", "자식을 같은 영역에 겹침" },
		{ EUIWidgetType::UniformGrid, ICON_FA_TABLE_CELLS, "균일 그리드", "같은 크기 칸에 행/열 배치" },
		{ EUIWidgetType::ScrollBox, ICON_FA_SCROLL, "스크롤 박스", "넘치는 내용을 휠로 스크롤" },
		{ EUIWidgetType::Border, ICON_FA_SQUARE, "보더 (패널)", "배경 + 자식 1개" },
		{ EUIWidgetType::Image, ICON_FA_IMAGE, "이미지", "텍스처 또는 단색 사각형" },
		{ EUIWidgetType::Text, ICON_FA_FONT, "텍스트", "SDF 글꼴 텍스트" },
		{ EUIWidgetType::Button, ICON_FA_HAND_POINTER, "버튼", "상태별 모양 + 클릭 이벤트 (자식 1개)" },
		{ EUIWidgetType::ProgressBar, ICON_FA_BARS_PROGRESS, "진행 막대", "0~1 비율 채우기" },
	};

	const FPaletteEntry& GetPaletteEntry(EUIWidgetType Type)
	{
		for (const FPaletteEntry& Entry : GPalette)
		{
			if (Entry.Type == Type)
			{
				return Entry;
			}
		}
		return GPalette[0];
	}

	struct FResolutionPreset
	{
		const char* Label;
		float       Width;
		float       Height;
	};
	constexpr FResolutionPreset GResolutions[] = {
		{ "설계 해상도", 0.0f, 0.0f },       { "1280 x 720", 1280.0f, 720.0f },   { "1920 x 1080", 1920.0f, 1080.0f },
		{ "2560 x 1440", 2560.0f, 1440.0f }, { "2560 x 1080 (21:9)", 2560.0f, 1080.0f }, { "1024 x 768 (4:3)", 1024.0f, 768.0f },
		{ "1080 x 1920 (세로)", 1080.0f, 1920.0f },
	};

	constexpr const char* GVisibilityLabels[] = { "보임", "접힘 (공간 없음)", "숨김 (공간 유지)", "입력 안 받음 (자식 포함)", "자신만 입력 안 받음" };
	constexpr const char* GHAlignLabels[]     = { "채우기", "왼쪽", "가운데", "오른쪽" };
	constexpr const char* GVAlignLabels[]     = { "채우기", "위", "가운데", "아래" };
	constexpr const char* GSizeRuleLabels[]   = { "자동", "채우기" };
	constexpr const char* GOrientationLabels[] = { "세로", "가로" };
	constexpr const char* GJustifyLabels[]    = { "왼쪽", "가운데", "오른쪽" };
	constexpr const char* GFillLabels[]       = { "왼쪽 → 오른쪽", "오른쪽 → 왼쪽", "아래 → 위", "위 → 아래" };
	constexpr const char* GScaleModeLabels[]  = { "없음 (1배)", "높이 맞춤", "너비 맞춤", "전체 보이기 (작은 쪽)", "채우기 (큰 쪽)" };
	constexpr const char* GEventLabels[]      = { "클릭", "누름", "뗌", "호버 시작", "호버 끝" };
	constexpr const char* GDrawAsLabels[]     = { "늘이기", "9-slice" };

	template <typename TEnum, size_t N>
	bool EnumCombo(const char* Label, TEnum& Value, const char* const (&Labels)[N])
	{
		int32 Index = static_cast<int32>(Value);
		if (ImGui::Combo(Label, &Index, Labels, static_cast<int32>(N)))
		{
			Value = static_cast<TEnum>(Index);
			return true;
		}
		return false;
	}

	bool IsCanvasChild(const FUIWidget* Widget) { return Widget != nullptr && Widget->Parent != nullptr && Widget->Parent->Type == EUIWidgetType::Canvas; }

	ImVec2 ToImVec2(const FVector2& Value) { return ImVec2(Value.X, Value.Y); }
	ImU32  ToColor(const ImVec4& Color, float Alpha) { return ImGui::GetColorU32(ImVec4(Color.x, Color.y, Color.z, Alpha)); }

	std::string ToLowerExtension(const std::filesystem::path& Path)
	{
		std::string Extension = FStringConv::ToUtf8(Path.extension().wstring());
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](char Char) { return static_cast<char>(std::tolower(static_cast<unsigned char>(Char))); });
		return Extension;
	}

	bool IsImageExtension(const std::string& Extension)
	{
		return Extension == ".png" || Extension == ".jpg" || Extension == ".jpeg" || Extension == ".tga" || Extension == ".bmp";
	}

	// 캔버스 자식으로 새로 만들 때의 기본 슬롯 (UMG 기본 크기와 비슷하게)
	void ApplyCanvasDefaults(FUIWidget& Widget)
	{
		FUISlot& Slot = Widget.Slot;
		switch (Widget.Type)
		{
		case EUIWidgetType::Text:
		case EUIWidgetType::Image:        Slot.bAutoSize = true; break;
		case EUIWidgetType::Button:       Slot.Offsets.Right = 220.0f; Slot.Offsets.Bottom = 64.0f; break;
		case EUIWidgetType::Border:       Slot.Offsets.Right = 320.0f; Slot.Offsets.Bottom = 200.0f; break;
		case EUIWidgetType::ProgressBar:  Slot.Offsets.Right = 320.0f; Slot.Offsets.Bottom = 24.0f; break;
		case EUIWidgetType::ScrollBox:    Slot.Offsets.Right = 320.0f; Slot.Offsets.Bottom = 400.0f; break;
		default:                          Slot.Offsets.Right = 400.0f; Slot.Offsets.Bottom = 300.0f; break;
		}
	}

	// 선택 핸들 (좌상, 상, 우상, 우, 우하, 하, 좌하, 좌) — 사각형 가장자리 위의 비율 위치
	constexpr float GHandleX[8] = { 0.0f, 0.5f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.0f };
	constexpr float GHandleY[8] = { 0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 1.0f, 0.5f };
} // namespace

FWidgetEditor::FWidgetEditor(std::filesystem::path InPath)
	: FAssetEditor(std::move(InPath))
{
	// 자동 검증: --ui-select <위젯 이름> [--ui-zoom <배율>] → 선택하고 그 위젯을 가운데에 확대해 보여 준다
	const FCommandLine CommandLine = FCommandLine::FromProcess();
	AutoSelectName                 = FStringConv::ToUtf8(CommandLine.GetValue(L"--ui-select"));
	const std::wstring ZoomArg     = CommandLine.GetValue(L"--ui-zoom");
	AutoZoom                       = ZoomArg.empty() ? 0.0f : std::stof(ZoomArg);
}

FWidgetEditor::~FWidgetEditor() = default;

// ---------------------------------------------------------------- 에셋

bool FWidgetEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	FUIAsset Loaded;
	if (!Loaded.LoadFromFile(Path))
	{
		return false;
	}
	Asset            = std::move(Loaded);
	ContentDirectory = Env.Editor != nullptr ? Env.Editor->ContentDirectory : Path.parent_path();
	FUIFontLibrary::Get().SetContentDirectory(ContentDirectory);
	ScanContentFiles(Env);
	if (PreviewPreset == 0)
	{
		PreviewSize = Asset.DesignSize;
	}
	if (bHasSelection && ResolvePath(SelectedPath) == nullptr)
	{
		Select(nullptr);
	}
	PreviewRouter.Reset(*Asset.Root);
	return true;
}

bool FWidgetEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	return Asset.SaveToFile(Path);
}

std::string FWidgetEditor::CaptureState() const
{
	return Asset.ToJsonString();
}

void FWidgetEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	(void)Env;
	if (!Asset.FromJsonString(State))
	{
		return;
	}
	if (PreviewPreset == 0)
	{
		PreviewSize = Asset.DesignSize;
	}
	if (bHasSelection && ResolvePath(SelectedPath) == nullptr)
	{
		Select(nullptr);
	}
	NameBufferPath.clear(); // 이름 칸 다시 채움
	PreviewRouter = FUIInputRouter{};
}

void FWidgetEditor::ScanContentFiles(FAssetEditorEnvironment& Env)
{
	(void)Env;
	ImageFiles = FAssetEditorWidgets::ScanImageFiles(ContentDirectory, ContentDirectory);
	FontFiles.clear();
	std::error_code ErrorCode;
	for (auto It = std::filesystem::recursive_directory_iterator(ContentDirectory, ErrorCode); !ErrorCode && It != std::filesystem::recursive_directory_iterator();
	     It.increment(ErrorCode))
	{
		const std::string Extension = ToLowerExtension(It->path());
		if (It->is_regular_file(ErrorCode) && (Extension == ".ttf" || Extension == ".otf"))
		{
			const std::filesystem::path Relative = std::filesystem::relative(It->path(), ContentDirectory, ErrorCode);
			if (!ErrorCode)
			{
				FontFiles.push_back(FStringConv::ToUtf8(Relative.generic_wstring()));
			}
		}
	}
	std::sort(FontFiles.begin(), FontFiles.end());
}

void FWidgetEditor::FramePreview(FAssetEditorEnvironment& Env)
{
	(void)Env;
	bFitRequested = true;
}

void FWidgetEditor::OnClose(FAssetEditorEnvironment& Env)
{
	if (Target)
	{
		Target->ShutdownDeferred(*Env.Rhi);
		Target.reset();
	}
}

// ---------------------------------------------------------------- 선택 / 트리

std::vector<int32> FWidgetEditor::GetPath(const FUIWidget* Widget) const
{
	std::vector<int32> Result;
	for (const FUIWidget* Current = Widget; Current != nullptr && Current->Parent != nullptr; Current = Current->Parent)
	{
		Result.push_back(Current->Parent->GetChildIndex(Current));
	}
	std::reverse(Result.begin(), Result.end());
	return Result;
}

FUIWidget* FWidgetEditor::ResolvePath(const std::vector<int32>& WidgetPath)
{
	FUIWidget* Current = Asset.Root.get();
	for (const int32 Index : WidgetPath)
	{
		if (Current == nullptr || Index < 0 || Index >= static_cast<int32>(Current->Children.size()))
		{
			return nullptr;
		}
		Current = Current->Children[static_cast<size_t>(Index)].get();
	}
	return Current;
}

FUIWidget* FWidgetEditor::GetSelected()
{
	return bHasSelection ? ResolvePath(SelectedPath) : nullptr;
}

void FWidgetEditor::Select(const FUIWidget* Widget)
{
	bHasSelection = Widget != nullptr;
	SelectedPath  = Widget != nullptr ? GetPath(Widget) : std::vector<int32>{};
}

std::string FWidgetEditor::MakeUniqueName(std::string_view Base, const FUIWidget* Except) const
{
	std::unordered_set<std::string> Used;
	static_cast<const FUIWidget&>(*Asset.Root).ForEach([&](const FUIWidget& Widget) {
		if (&Widget != Except)
		{
			Used.insert(Widget.Name);
		}
	});
	std::string Name(Base.empty() ? std::string_view("Widget") : Base);
	if (!Used.contains(Name))
	{
		return Name;
	}
	// 끝의 _숫자는 떼고 다시 붙인다
	const size_t Underscore = Name.find_last_of('_');
	if (Underscore != std::string::npos && Underscore + 1 < Name.size() &&
	    std::all_of(Name.begin() + static_cast<std::ptrdiff_t>(Underscore) + 1, Name.end(), [](char Char) { return Char >= '0' && Char <= '9'; }))
	{
		Name.resize(Underscore);
	}
	for (int32 Number = 1;; ++Number)
	{
		std::string Candidate = std::format("{}_{}", Name, Number);
		if (!Used.contains(Candidate))
		{
			return Candidate;
		}
	}
}

void FWidgetEditor::MakeNamesUnique(FUIWidget& Subtree)
{
	Subtree.ForEach([this](FUIWidget& Widget) { Widget.Name = MakeUniqueName(Widget.Name, &Widget); });
}

FUIWidget* FWidgetEditor::AddWidget(EUIWidgetType Type, FUIWidget* Parent, int32 Index, const FVector2* DropPointUi)
{
	if (Parent == nullptr)
	{
		Parent = Asset.Root.get();
	}
	// 자식을 더 못 받으면(보더/버튼이 차 있거나 잎 위젯) 그 부모의 다음 자리에
	while (Parent != nullptr && !Parent->CanAddChild())
	{
		FUIWidget* Child = Parent;
		Parent           = Parent->Parent;
		Index            = Parent != nullptr ? Parent->GetChildIndex(Child) + 1 : -1;
	}
	if (Parent == nullptr)
	{
		return nullptr;
	}
	std::unique_ptr<FUIWidget> Widget = FUIWidget::Create(Type);
	Widget->Name                      = MakeUniqueName(Widget->Name, nullptr);
	if (Parent->Type == EUIWidgetType::Canvas)
	{
		ApplyCanvasDefaults(*Widget);
		const FVector2 Position = DropPointUi != nullptr ? *DropPointUi - Parent->State.Geometry.Min : FVector2(40.0f, 40.0f);
		Widget->Slot.Offsets.Left = std::round(Position.X);
		Widget->Slot.Offsets.Top  = std::round(Position.Y);
	}
	FUIWidget* Added = Parent->AddChild(std::move(Widget), Index);
	if (Type == EUIWidgetType::Button)
	{
		// 빈 버튼보다 글자가 있는 편이 바로 쓸 수 있다
		std::unique_ptr<FUIWidget> Label = FUIWidget::Create(EUIWidgetType::Text);
		Label->Name                      = MakeUniqueName(Added->Name + "Label", nullptr);
		Label->Text                      = "버튼";
		Label->Slot.HAlign               = EUIHAlign::Center;
		Label->Slot.VAlign               = EUIVAlign::Center;
		Added->AddChild(std::move(Label));
	}
	Select(Added);
	MarkEdited("위젯 추가");
	return Added;
}

void FWidgetEditor::DeleteWidget(FUIWidget* Widget)
{
	if (Widget == nullptr || Widget->Parent == nullptr)
	{
		return; // 루트는 지우지 않는다
	}
	FUIWidget* Parent = Widget->Parent;
	Parent->RemoveChild(Widget);
	Select(Parent);
	MarkEdited("위젯 삭제");
}

void FWidgetEditor::DuplicateWidget(FUIWidget* Widget)
{
	if (Widget == nullptr || Widget->Parent == nullptr || !Widget->Parent->CanAddChild())
	{
		return;
	}
	FUIWidget* Parent = Widget->Parent;
	FUIWidget* Copy   = Parent->AddChild(Widget->Clone(), Parent->GetChildIndex(Widget) + 1);
	MakeNamesUnique(*Copy);
	if (Parent->Type == EUIWidgetType::Canvas)
	{
		FUILayout::MoveCanvasSlot(Copy->Slot, FVector2(20.0f, 20.0f));
	}
	Select(Copy);
	MarkEdited("위젯 복제");
}

void FWidgetEditor::PasteInto(FUIWidget* Destination)
{
	const char* Text = ImGui::GetClipboardText();
	if (Text == nullptr || std::strstr(Text, "\"Type\"") == nullptr)
	{
		return;
	}
	std::unique_ptr<FUIWidget> Widget = FUIAsset::WidgetFromJsonString(Text);
	if (!Widget)
	{
		return;
	}
	FUIWidget* Parent = Destination != nullptr ? Destination : Asset.Root.get();
	int32      Index  = -1;
	while (Parent != nullptr && !Parent->CanAddChild())
	{
		FUIWidget* Child = Parent;
		Parent           = Parent->Parent;
		Index            = Parent != nullptr ? Parent->GetChildIndex(Child) + 1 : -1;
	}
	if (Parent == nullptr)
	{
		return;
	}
	FUIWidget* Added = Parent->AddChild(std::move(Widget), Index);
	MakeNamesUnique(*Added);
	Select(Added);
	MarkEdited("위젯 붙여넣기");
}

void FWidgetEditor::MoveWidget(FUIWidget* Widget, FUIWidget* NewParent, int32 Index)
{
	if (Widget == nullptr || Widget->Parent == nullptr || NewParent == nullptr || NewParent == Widget || Widget->IsAncestorOf(NewParent))
	{
		return;
	}
	FUIWidget* OldParent = Widget->Parent;
	if (OldParent != NewParent && !NewParent->CanAddChild())
	{
		return;
	}
	const int32 OldIndex = OldParent->GetChildIndex(Widget);
	if (OldParent == NewParent && Index >= 0 && OldIndex < Index)
	{
		--Index; // 떼어 낸 만큼 당겨진다
	}
	if (OldParent == NewParent && Index == OldIndex)
	{
		return;
	}
	std::unique_ptr<FUIWidget> Owned = OldParent->RemoveChild(Widget);
	if (NewParent->Type == EUIWidgetType::Canvas && OldParent->Type != EUIWidgetType::Canvas)
	{
		// 배치 패널에서 캔버스로: 보던 위치/크기를 유지
		const FUIRect Rect = Owned->State.Geometry;
		FUILayout::SetCanvasSlotRect(Owned->Slot, NewParent->State.Geometry, Rect);
	}
	FUIWidget* Moved = NewParent->AddChild(std::move(Owned), Index);
	Select(Moved);
	MarkEdited("위젯 옮기기");
}

// ---------------------------------------------------------------- 레이아웃 / 좌표

float FWidgetEditor::GetUiScale() const
{
	return FMath::Max(FUILayout::ComputeScale(Asset.ScaleMode, Asset.DesignSize, PreviewSize), 0.01f);
}

void FWidgetEditor::UpdateLayout()
{
	Asset.Root->AssignIds();
	FUILayout::Compute(*Asset.Root, PreviewSize / GetUiScale(), FUIFontLibrary::Get());
}

FVector2 FWidgetEditor::UiToScreen(const FVector2& Ui) const
{
	return CanvasMin + Pan + Ui * (Zoom * GetUiScale());
}

FVector2 FWidgetEditor::ScreenToUi(const FVector2& Screen) const
{
	return (Screen - CanvasMin - Pan) / (Zoom * GetUiScale());
}

FUIWidget* FWidgetEditor::PickWidget(FUIWidget& Widget, const FVector2& UiPoint)
{
	// 편집용 맞히기: 입력 설정과 관계없이 보이는 위젯 중 가장 깊고 위에 있는 것
	if (Widget.Visibility == EUIVisibility::Collapsed || Widget.Visibility == EUIVisibility::Hidden || !Widget.State.Clip.Contains(UiPoint))
	{
		return nullptr;
	}
	std::vector<FUIWidget*> Children;
	for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
	{
		Children.push_back(Child.get());
	}
	if (Widget.Type == EUIWidgetType::Canvas)
	{
		std::stable_sort(Children.begin(), Children.end(), [](const FUIWidget* A, const FUIWidget* B) { return A->Slot.ZOrder < B->Slot.ZOrder; });
	}
	for (auto It = Children.rbegin(); It != Children.rend(); ++It)
	{
		if (FUIWidget* Hit = PickWidget(**It, UiPoint))
		{
			return Hit;
		}
	}
	return Widget.State.Geometry.Contains(UiPoint) ? &Widget : nullptr;
}

// ---------------------------------------------------------------- 미리보기 영역

void FWidgetEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	if (Env.Editor != nullptr)
	{
		ContentDirectory = Env.Editor->ContentDirectory;
	}
	DrawCanvasToolbar();

	if (ImGui::BeginChild("##UIOutliner", ImVec2(240.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX))
	{
		DrawOutliner(Env);
	}
	ImGui::EndChild();
	ImGui::SameLine();
	if (ImGui::BeginChild("##UICanvasArea", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		DrawCanvas(Env);
	}
	ImGui::EndChild();

	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput)
	{
		HandleShortcuts();
	}
	// 계층/캔버스 순회가 끝난 뒤 구조 변경
	if (PendingAction)
	{
		std::function<void()> Action = std::move(PendingAction);
		PendingAction                = nullptr;
		Action();
		UpdateLayout();
	}
}

void FWidgetEditor::DrawCanvasToolbar()
{
	ImGui::SetNextItemWidth(170.0f);
	if (ImGui::BeginCombo("##Resolution", PreviewPreset == 0 ? std::format("설계 {}x{}", static_cast<int32>(Asset.DesignSize.X), static_cast<int32>(Asset.DesignSize.Y)).c_str()
	                                                          : GResolutions[PreviewPreset].Label))
	{
		for (int32 Index = 0; Index < static_cast<int32>(std::size(GResolutions)); ++Index)
		{
			if (ImGui::Selectable(GResolutions[Index].Label, Index == PreviewPreset))
			{
				PreviewPreset = Index;
				PreviewSize   = Index == 0 ? Asset.DesignSize : FVector2(GResolutions[Index].Width, GResolutions[Index].Height);
				bFitRequested = true;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("미리보기 해상도 — 앵커/배율이 다른 화면에서 어떻게 보이는지 확인");
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_EXPAND, "화면 맞춤 (F)", false))
	{
		bFitRequested = true;
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_BORDER_ALL, "위젯 경계 표시", bShowBounds))
	{
		bShowBounds = !bShowBounds;
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_PLAY, "미리보기 입력 — 호버/클릭/휠을 실제로 처리 (편집 조작 대신)", bPreviewInput))
	{
		bPreviewInput = !bPreviewInput;
		PreviewRouter.Reset(*Asset.Root);
		EventLog.clear();
		Drag = EDragMode::None;
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(60.0f);
	ImGui::DragFloat("스냅", &SnapStep, 0.25f, 1.0f, 64.0f, "%.0f");
	ImGui::SetItemTooltip("이동/크기 조절 격자 (UI 단위)");
	ImGui::SameLine();
	ImGui::TextDisabled("%.0f%%", Zoom * GetUiScale() * 100.0f);
}

void FWidgetEditor::DrawOutliner(FAssetEditorEnvironment& Env)
{
	(void)Env;
	const float PaletteHeight = FMath::Min(ImGui::GetContentRegionAvail().y * 0.45f, ImGui::GetFrameHeightWithSpacing() * 7.5f);
	ImGui::TextDisabled(ICON_FA_LAYER_GROUP " 계층");
	if (ImGui::BeginChild("##UIHierarchy", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - PaletteHeight)))
	{
		DrawHierarchyNode(*Asset.Root);
		// 빈 곳 클릭 = 선택 해제
		if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
		{
			Select(nullptr);
		}
	}
	ImGui::EndChild();
	ImGui::Separator();
	ImGui::TextDisabled(ICON_FA_OBJECT_GROUP " 팔레트 (끌어서 놓기 / 클릭 = 선택에 추가)");
	if (ImGui::BeginChild("##UIPalette", ImVec2(0.0f, 0.0f)))
	{
		DrawPalette();
	}
	ImGui::EndChild();
}

void FWidgetEditor::DrawHierarchyNode(FUIWidget& Widget)
{
	const FPaletteEntry& Entry     = GetPaletteEntry(Widget.Type);
	const bool           bSelected = GetSelected() == &Widget;
	ImGuiTreeNodeFlags   Flags     = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth |
	                           ImGuiTreeNodeFlags_DefaultOpen;
	if (Widget.Children.empty())
	{
		Flags |= ImGuiTreeNodeFlags_Leaf;
	}
	if (bSelected)
	{
		Flags |= ImGuiTreeNodeFlags_Selected;
	}
	const bool bDim = Widget.Visibility == EUIVisibility::Collapsed || Widget.Visibility == EUIVisibility::Hidden;
	if (bDim)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::TextDim);
	}
	const bool bOpen = ImGui::TreeNodeEx(static_cast<void*>(&Widget), Flags, "%s %s", Entry.Icon, Widget.Name.c_str());
	if (bDim)
	{
		ImGui::PopStyleColor();
	}
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
	{
		Select(&Widget);
	}

	// 끌기 (루트 제외) / 놓기
	if (Widget.Parent != nullptr && ImGui::BeginDragDropSource())
	{
		GDraggedPath = GetPath(&Widget);
		ImGui::SetDragDropPayload(GWidgetPayload, nullptr, 0);
		ImGui::Text("%s %s", Entry.Icon, Widget.Name.c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget())
	{
		if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(GPalettePayload))
		{
			const EUIWidgetType Type = static_cast<EUIWidgetType>(*static_cast<const int32*>(Payload->Data));
			FUIWidget*          Into = &Widget;
			Defer([this, Type, Path = GetPath(Into)]() { AddWidget(Type, ResolvePath(Path), -1, nullptr); });
		}
		if (ImGui::AcceptDragDropPayload(GWidgetPayload))
		{
			// 자식을 받을 수 있으면 안으로, 아니면 이 위젯 앞으로
			Defer([this, From = GDraggedPath, To = GetPath(&Widget)]() {
				FUIWidget* Dragged = ResolvePath(From);
				FUIWidget* Onto    = ResolvePath(To);
				if (Dragged == nullptr || Onto == nullptr)
				{
					return;
				}
				if (Onto->CanAddChild() || Onto == Dragged->Parent)
				{
					MoveWidget(Dragged, Onto, -1); // 안으로 (자기 부모면 끝으로)
				}
				else if (Onto->Parent != nullptr)
				{
					MoveWidget(Dragged, Onto->Parent, Onto->Parent->GetChildIndex(Onto));
				}
			});
		}
		ImGui::EndDragDropTarget();
	}

	// 우클릭 메뉴
	if (ImGui::BeginPopupContextItem())
	{
		Select(&Widget);
		const std::vector<int32> ThisPath = GetPath(&Widget);
		const bool               bRoot    = Widget.Parent == nullptr;
		if (ImGui::MenuItem(ICON_FA_CLONE " 복제", "Ctrl+D", false, !bRoot && Widget.Parent->CanAddChild()))
		{
			Defer([this, ThisPath]() { DuplicateWidget(ResolvePath(ThisPath)); });
		}
		if (ImGui::MenuItem(ICON_FA_COPY " 복사", "Ctrl+C"))
		{
			ImGui::SetClipboardText(FUIAsset::WidgetToJsonString(Widget).c_str());
		}
		if (ImGui::MenuItem(ICON_FA_PASTE " 붙여넣기", "Ctrl+V"))
		{
			Defer([this, ThisPath]() { PasteInto(ResolvePath(ThisPath)); });
		}
		ImGui::Separator();
		const int32 Index = bRoot ? 0 : Widget.Parent->GetChildIndex(&Widget);
		if (ImGui::MenuItem(ICON_FA_ARROW_UP " 위로", nullptr, false, !bRoot && Index > 0))
		{
			Defer([this, ThisPath, Index]() {
				FUIWidget* Moving = ResolvePath(ThisPath);
				if (Moving != nullptr)
				{
					MoveWidget(Moving, Moving->Parent, Index - 1);
				}
			});
		}
		if (ImGui::MenuItem(ICON_FA_ARROW_DOWN " 아래로", nullptr, false, !bRoot && Index + 1 < static_cast<int32>(Widget.Parent->Children.size())))
		{
			Defer([this, ThisPath, Index]() {
				FUIWidget* Moving = ResolvePath(ThisPath);
				if (Moving != nullptr)
				{
					MoveWidget(Moving, Moving->Parent, Index + 2);
				}
			});
		}
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_FA_TRASH " 삭제", "Delete", false, !bRoot))
		{
			Defer([this, ThisPath]() { DeleteWidget(ResolvePath(ThisPath)); });
		}
		ImGui::EndPopup();
	}

	if (bOpen)
	{
		for (const std::unique_ptr<FUIWidget>& Child : Widget.Children)
		{
			DrawHierarchyNode(*Child);
		}
		ImGui::TreePop();
	}
}

void FWidgetEditor::DrawPalette()
{
	for (const FPaletteEntry& Entry : GPalette)
	{
		ImGui::PushID(static_cast<int32>(Entry.Type));
		if (ImGui::Selectable(std::format("{}  {}", Entry.Icon, Entry.Label).c_str(), false))
		{
			const EUIWidgetType Type = Entry.Type;
			Defer([this, Type]() { AddWidget(Type, GetSelected(), -1, nullptr); });
		}
		ImGui::SetItemTooltip("%s", Entry.Tooltip);
		if (ImGui::BeginDragDropSource())
		{
			const int32 Type = static_cast<int32>(Entry.Type);
			ImGui::SetDragDropPayload(GPalettePayload, &Type, sizeof(Type));
			ImGui::Text("%s %s", Entry.Icon, Entry.Label);
			ImGui::EndDragDropSource();
		}
		ImGui::PopID();
	}
}

void FWidgetEditor::DrawCanvas(FAssetEditorEnvironment& Env)
{
	const ImVec2 Origin = ImGui::GetCursorScreenPos();
	const ImVec2 Avail  = ImGui::GetContentRegionAvail();
	CanvasMin           = FVector2(Origin.x, Origin.y);
	CanvasSize          = FVector2(FMath::Max(Avail.x, 16.0f), FMath::Max(Avail.y, 16.0f));

	// 타깃을 캔버스 크기에 맞춘다 (바뀌면 이전 것은 지연 해제)
	const uint32 Width  = static_cast<uint32>(CanvasSize.X);
	const uint32 Height = static_cast<uint32>(CanvasSize.Y);
	if (Env.Rhi != nullptr && (!Target || Target->GetWidth() != Width || Target->GetHeight() != Height))
	{
		if (Target)
		{
			Target->ShutdownDeferred(*Env.Rhi);
		}
		FRenderTargetDesc Desc = FRenderTargetDesc::MakeLdrDisplay();
		Desc.bWithDepth        = false;
		std::memcpy(Desc.ClearColor, GCanvasClearColor, sizeof(GCanvasClearColor));
		Target = std::make_unique<FD3D12RenderTarget>();
		if (!Target->Init(Env.Rhi->GetDevice(), Env.Rhi->GetSrvAllocator(), Width, Height, L"UIDesignerCanvas", Desc))
		{
			Target.reset();
		}
	}

	// 사용자가 보기를 움직이기 전에는 캔버스 크기가 바뀔 때(창 배치가 잡히는 첫 프레임들, 창 크기 조절) 다시 맞춘다
	if (!bViewTouched && CanvasSize != LastCanvasSize)
	{
		bFitRequested = true;
	}
	LastCanvasSize = CanvasSize;
	if (bFitRequested)
	{
		const float Fit = FMath::Min((CanvasSize.X - 48.0f) / PreviewSize.X, (CanvasSize.Y - 48.0f) / PreviewSize.Y);
		Zoom            = FMath::Clamp(Fit, 0.05f, 4.0f);
		Pan             = (CanvasSize - PreviewSize * Zoom) * 0.5f;
		bFitRequested   = false;
		bViewTouched    = false; // 맞춘 뒤로는 창 크기를 따라간다
	}
	if (!AutoSelectName.empty() && CanvasSize == LastCanvasSize && ImGui::GetFrameCount() > 30)
	{
		UpdateLayout();
		if (FUIWidget* Widget = Asset.Root->FindByName(AutoSelectName))
		{
			Select(Widget);
			if (AutoZoom > 0.0f)
			{
				Zoom         = AutoZoom / GetUiScale();
				Pan          = CanvasSize * 0.5f - Widget->State.Geometry.GetCenter() * (Zoom * GetUiScale());
				bViewTouched = true;
			}
		}
		AutoSelectName.clear();
	}

	ImGui::InvisibleButton("##UICanvas", ImVec2(CanvasSize.X, CanvasSize.Y),
	                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	bCanvasHovered      = ImGui::IsItemHovered();
	const bool bActive  = ImGui::IsItemActive();

	UpdateLayout();

	// 팔레트/콘텐츠(이미지)를 캔버스에 놓기: 포인터 아래 위젯(또는 그 부모)에 추가
	if (ImGui::BeginDragDropTarget())
	{
		const ImVec2   Mouse = ImGui::GetIO().MousePos;
		const FVector2 Ui    = ScreenToUi(FVector2(Mouse.x, Mouse.y));
		if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(GPalettePayload))
		{
			const EUIWidgetType Type = static_cast<EUIWidgetType>(*static_cast<const int32*>(Payload->Data));
			FUIWidget*          Onto = PickWidget(*Asset.Root, Ui);
			Defer([this, Type, Ui, Path = GetPath(Onto != nullptr ? Onto : Asset.Root.get())]() { AddWidget(Type, ResolvePath(Path), -1, &Ui); });
		}
		if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
		{
			for (const std::filesystem::path& File : *Paths)
			{
				if (!IsImageExtension(ToLowerExtension(File)))
				{
					continue;
				}
				std::error_code             ErrorCode;
				const std::filesystem::path Relative = std::filesystem::relative(File, ContentDirectory, ErrorCode);
				const std::string           Texture  = FStringConv::ToUtf8((ErrorCode ? File : Relative).generic_wstring());
				FUIWidget*                  Onto     = PickWidget(*Asset.Root, Ui);
				Defer([this, Ui, Texture, Path = GetPath(Onto != nullptr ? Onto : Asset.Root.get())]() {
					if (FUIWidget* Image = AddWidget(EUIWidgetType::Image, ResolvePath(Path), -1, &Ui))
					{
						Image->Brush.Texture = Texture;
						Image->ImageSize     = FVector2(128.0f, 128.0f);
					}
				});
				break;
			}
		}
		ImGui::EndDragDropTarget();
	}

	if (bPreviewInput)
	{
		HandlePreviewInput(bCanvasHovered);
	}
	else
	{
		HandleCanvasInput(bCanvasHovered, bActive);
	}

	// 그리기 목록: 화면(프레임) 배경 → UI (프레임 밖은 잘린다 — 실제 화면과 같게)
	const float    Scale = Zoom * GetUiScale();
	FUITransform   Transform;
	Transform.Scale  = Scale;
	Transform.Offset = Pan;
	const FUIRect  TargetRect(FVector2::ZeroVector, CanvasSize);
	const FVector2 RootSize = PreviewSize / GetUiScale();
	const FUIRect  FrameUi(FVector2::ZeroVector, RootSize);
	DrawList.Clear();
	FUIBrush Background;
	Background.Color = FVector4(0.16f, 0.17f, 0.2f, 1.0f);
	FUIPainter::PaintBrush(Background, FrameUi, 1.0f, Transform, TargetRect, DrawList);
	FUIPainter::Paint(*Asset.Root, Transform, TargetRect.Intersect(Transform.ToPixels(FrameUi)), FUIFontLibrary::Get(), DrawList);
	bRenderThisFrame = Target != nullptr;

	ImDrawList* WindowDrawList = ImGui::GetWindowDrawList();
	const ImVec2 CanvasMax(CanvasMin.X + CanvasSize.X, CanvasMin.Y + CanvasSize.Y);
	WindowDrawList->PushClipRect(Origin, CanvasMax, true);
	if (Target)
	{
		WindowDrawList->AddImage(static_cast<ImTextureID>(Target->GetSrv().Gpu.ptr), Origin, CanvasMax);
	}
	DrawCanvasOverlay(WindowDrawList);
	if (bPreviewInput && !EventLog.empty())
	{
		float Y = CanvasMax.y - 8.0f - static_cast<float>(EventLog.size()) * ImGui::GetTextLineHeight();
		for (const std::string& Line : EventLog)
		{
			WindowDrawList->AddText(ImVec2(Origin.x + 8.0f, Y), ToColor(FEditorTheme::Success, 0.9f), Line.c_str());
			Y += ImGui::GetTextLineHeight();
		}
	}
	if (Env.UIRenderer == nullptr)
	{
		WindowDrawList->AddText(ImVec2(Origin.x + 8.0f, Origin.y + 8.0f), ToColor(FEditorTheme::Danger, 1.0f), "UI 렌더러를 초기화하지 못해 미리보기를 그릴 수 없습니다");
	}
	WindowDrawList->PopClipRect();
}

void FWidgetEditor::HandleCanvasInput(bool bHovered, bool bActive)
{
	const ImGuiIO& Io    = ImGui::GetIO();
	const FVector2 Mouse(Io.MousePos.x, Io.MousePos.y);
	const float    Scale = Zoom * GetUiScale();
	HoveredWidget        = bHovered && Drag == EDragMode::None ? PickWidget(*Asset.Root, ScreenToUi(Mouse)) : nullptr;

	// 휠 = 포인터 기준 확대/축소
	if (bHovered && Io.MouseWheel != 0.0f)
	{
		const float NewZoom = FMath::Clamp(Zoom * std::pow(1.15f, Io.MouseWheel), 0.05f, 8.0f);
		const FVector2 Local = Mouse - CanvasMin;
		Pan                  = Local - (Local - Pan) * (NewZoom / Zoom);
		Zoom                 = NewZoom;
		bViewTouched         = true;
	}
	// 가운데/오른쪽 드래그 = 화면 이동
	if (bActive && (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)))
	{
		Pan += FVector2(Io.MouseDelta.x, Io.MouseDelta.y);
		bViewTouched = bViewTouched || Io.MouseDelta.x != 0.0f || Io.MouseDelta.y != 0.0f;
	}

	FUIWidget* Selected = GetSelected();
	// 핸들 위인지 (선택한 캔버스 자식만)
	int32 HandleUnderMouse = -1;
	if (IsCanvasChild(Selected) && bHovered)
	{
		const FVector2 Min = UiToScreen(Selected->State.Geometry.Min);
		const FVector2 Max = UiToScreen(Selected->State.Geometry.Max);
		for (int32 Index = 0; Index < 8; ++Index)
		{
			const FVector2 Handle(FMath::Lerp(Min.X, Max.X, GHandleX[Index]), FMath::Lerp(Min.Y, Max.Y, GHandleY[Index]));
			if (FMath::Abs(Mouse.X - Handle.X) <= 5.0f && FMath::Abs(Mouse.Y - Handle.Y) <= 5.0f)
			{
				HandleUnderMouse = Index;
				break;
			}
		}
	}
	const int32 CursorHandle = Drag == EDragMode::Resize ? DragHandle : HandleUnderMouse;
	if (CursorHandle >= 0)
	{
		constexpr ImGuiMouseCursor Cursors[8] = { ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW,
			                                      ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW };
		ImGui::SetMouseCursor(Cursors[CursorHandle]);
	}

	if (bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		if (HandleUnderMouse >= 0)
		{
			Drag       = EDragMode::Resize;
			DragHandle = HandleUnderMouse;
		}
		else
		{
			FUIWidget* Picked = PickWidget(*Asset.Root, ScreenToUi(Mouse));
			Select(Picked);
			Selected = Picked;
			Drag     = IsCanvasChild(Picked) ? EDragMode::Move : EDragMode::None;
		}
		if (Drag != EDragMode::None && Selected != nullptr)
		{
			DragStartMouse = Mouse;
			DragStartRect  = Selected->State.Geometry;
			DragStartSlot  = Selected->Slot;
		}
	}

	if ((Drag == EDragMode::Move || Drag == EDragMode::Resize) && ImGui::IsMouseDown(ImGuiMouseButton_Left) && IsCanvasChild(Selected))
	{
		const float    Step  = FMath::Max(SnapStep, 1.0f);
		const FVector2 Raw   = (Mouse - DragStartMouse) / Scale;
		const FVector2 Delta(std::round(Raw.X / Step) * Step, std::round(Raw.Y / Step) * Step);
		FUISlot        Slot = DragStartSlot;
		if (Drag == EDragMode::Move)
		{
			FUILayout::MoveCanvasSlot(Slot, Delta);
		}
		else
		{
			FUIRect Rect = DragStartRect;
			const bool bLeft   = DragHandle == 0 || DragHandle == 6 || DragHandle == 7;
			const bool bRight  = DragHandle == 2 || DragHandle == 3 || DragHandle == 4;
			const bool bTop    = DragHandle == 0 || DragHandle == 1 || DragHandle == 2;
			const bool bBottom = DragHandle == 4 || DragHandle == 5 || DragHandle == 6;
			if (bLeft)
			{
				Rect.Min.X = FMath::Min(Rect.Min.X + Delta.X, Rect.Max.X - 1.0f);
			}
			if (bRight)
			{
				Rect.Max.X = FMath::Max(Rect.Max.X + Delta.X, Rect.Min.X + 1.0f);
			}
			if (bTop)
			{
				Rect.Min.Y = FMath::Min(Rect.Min.Y + Delta.Y, Rect.Max.Y - 1.0f);
			}
			if (bBottom)
			{
				Rect.Max.Y = FMath::Max(Rect.Max.Y + Delta.Y, Rect.Min.Y + 1.0f);
			}
			Slot.bAutoSize = false;
			FUILayout::SetCanvasSlotRect(Slot, Selected->Parent->State.Geometry, Rect);
		}
		if (!(Slot == Selected->Slot))
		{
			Selected->Slot = Slot;
			MarkEdited(Drag == EDragMode::Move ? "위젯 이동" : "위젯 크기");
		}
	}
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && (Drag == EDragMode::Move || Drag == EDragMode::Resize))
	{
		Drag       = EDragMode::None;
		DragHandle = -1;
	}
}

void FWidgetEditor::HandlePreviewInput(bool bHovered)
{
	const ImGuiIO& Io = ImGui::GetIO();
	HoveredWidget     = nullptr;
	if (bHovered && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
	{
		Pan += FVector2(Io.MouseDelta.x, Io.MouseDelta.y);
	}
	FUIPointerInput Pointer;
	Pointer.Position = ScreenToUi(FVector2(Io.MousePos.x, Io.MousePos.y));
	Pointer.bInside  = bHovered && FUIRect(FVector2::ZeroVector, PreviewSize / GetUiScale()).Contains(Pointer.Position);
	Pointer.bDown     = ImGui::IsMouseDown(ImGuiMouseButton_Left);
	Pointer.bPressed  = bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	Pointer.bReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
	Pointer.Wheel     = bHovered ? Io.MouseWheel : 0.0f;
	FUIKeyInput Keys;
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !Io.WantTextInput)
	{
		Keys.bActivate      = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false);
		Keys.bFocusNext     = ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !Io.KeyShift;
		Keys.bFocusPrevious = ImGui::IsKeyPressed(ImGuiKey_Tab, false) && Io.KeyShift;
	}
	std::vector<FUIEvent> Events;
	PreviewRouter.Process(*Asset.Root, Pointer, Keys, Events);
	for (const FUIEvent& Event : Events)
	{
		if (Event.Type == EUIEventType::Pressed || Event.Type == EUIEventType::Released)
		{
			continue; // 로그는 클릭/호버만
		}
		EventLog.push_back(std::format("{} {}", GEventLabels[static_cast<size_t>(Event.Type)], Event.WidgetName));
		if (EventLog.size() > 6)
		{
			EventLog.erase(EventLog.begin());
		}
	}
	if (Pointer.Wheel != 0.0f)
	{
		UpdateLayout();
	}
}

void FWidgetEditor::DrawCanvasOverlay(ImDrawList* Overlay)
{
	// 화면(프레임) 윤곽 + 크기
	const FVector2 RootSize = PreviewSize / GetUiScale();
	const FVector2 FrameMin = UiToScreen(FVector2::ZeroVector);
	const FVector2 FrameMax = UiToScreen(RootSize);
	Overlay->AddRect(ToImVec2(FrameMin), ToImVec2(FrameMax), IM_COL32(255, 255, 255, 90));
	Overlay->AddText(ImVec2(FrameMin.X, FrameMin.Y - ImGui::GetTextLineHeight() - 2.0f), IM_COL32(255, 255, 255, 120),
	                 std::format("{} x {}", static_cast<int32>(PreviewSize.X), static_cast<int32>(PreviewSize.Y)).c_str());

	if (bShowBounds)
	{
		static_cast<const FUIWidget&>(*Asset.Root).ForEach([&](const FUIWidget& Widget) {
			if (Widget.Parent != nullptr && Widget.Visibility != EUIVisibility::Collapsed)
			{
				Overlay->AddRect(ToImVec2(UiToScreen(Widget.State.Geometry.Min)), ToImVec2(UiToScreen(Widget.State.Geometry.Max)), IM_COL32(255, 255, 255, 28));
			}
		});
	}
	if (HoveredWidget != nullptr && HoveredWidget != GetSelected())
	{
		Overlay->AddRect(ToImVec2(UiToScreen(HoveredWidget->State.Geometry.Min)), ToImVec2(UiToScreen(HoveredWidget->State.Geometry.Max)),
		                 ToColor(FEditorTheme::Accent, 0.6f), 0.0f, 0, 1.0f);
	}

	FUIWidget* Selected = GetSelected();
	if (Selected == nullptr || bPreviewInput)
	{
		return;
	}
	const FVector2 Min = UiToScreen(Selected->State.Geometry.Min);
	const FVector2 Max = UiToScreen(Selected->State.Geometry.Max);
	Overlay->AddRect(ToImVec2(Min), ToImVec2(Max), ToColor(FEditorTheme::Accent, 1.0f), 0.0f, 0, 2.0f);
	Overlay->AddText(ImVec2(Min.X, Min.Y - ImGui::GetTextLineHeight() - 2.0f), ToColor(FEditorTheme::AccentHover, 1.0f), Selected->Name.c_str());

	if (!IsCanvasChild(Selected))
	{
		return;
	}
	// 앵커: 부모 영역 안 앵커 사각형(늘이기) 또는 점, 꽃 모양 표시 대신 마름모
	const FUIRect& Parent    = Selected->Parent->State.Geometry;
	const FUISlot& Slot      = Selected->Slot;
	const FVector2 AnchorMin = UiToScreen(Parent.Min + Parent.GetSize() * Slot.AnchorMin);
	const FVector2 AnchorMax = UiToScreen(Parent.Min + Parent.GetSize() * Slot.AnchorMax);
	const ImU32    AnchorColor = ToColor(FEditorTheme::Warning, 0.95f);
	if (!Slot.IsAnchorPointX() || !Slot.IsAnchorPointY())
	{
		Overlay->AddRect(ToImVec2(AnchorMin), ToImVec2(AnchorMax), ToColor(FEditorTheme::Warning, 0.5f));
	}
	const auto Diamond = [&](const FVector2& Center) {
		constexpr float Size = 6.0f;
		Overlay->AddQuadFilled(ImVec2(Center.X, Center.Y - Size), ImVec2(Center.X + Size, Center.Y), ImVec2(Center.X, Center.Y + Size), ImVec2(Center.X - Size, Center.Y),
		                       AnchorColor);
	};
	Diamond(AnchorMin);
	if (AnchorMax != AnchorMin)
	{
		Diamond(AnchorMax);
	}
	// 크기 핸들
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const ImVec2 Handle(FMath::Lerp(Min.X, Max.X, GHandleX[Index]), FMath::Lerp(Min.Y, Max.Y, GHandleY[Index]));
		Overlay->AddRectFilled(ImVec2(Handle.x - 4.0f, Handle.y - 4.0f), ImVec2(Handle.x + 4.0f, Handle.y + 4.0f), IM_COL32(255, 255, 255, 255));
		Overlay->AddRect(ImVec2(Handle.x - 4.0f, Handle.y - 4.0f), ImVec2(Handle.x + 4.0f, Handle.y + 4.0f), ToColor(FEditorTheme::Accent, 1.0f));
	}
}

void FWidgetEditor::HandleShortcuts()
{
	FUIWidget* Selected = GetSelected();
	const bool bCtrl    = ImGui::GetIO().KeyCtrl;
	if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !bCtrl && bCanvasHovered)
	{
		bFitRequested = true;
	}
	if (Selected == nullptr)
	{
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V))
		{
			Defer([this]() { PasteInto(nullptr); });
		}
		return;
	}
	const std::vector<int32> SelectedWidgetPath = GetPath(Selected);
	if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
	{
		Defer([this, SelectedWidgetPath]() { DeleteWidget(ResolvePath(SelectedWidgetPath)); });
	}
	else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D))
	{
		Defer([this, SelectedWidgetPath]() { DuplicateWidget(ResolvePath(SelectedWidgetPath)); });
	}
	else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C))
	{
		ImGui::SetClipboardText(FUIAsset::WidgetToJsonString(*Selected).c_str());
	}
	else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V))
	{
		Defer([this, SelectedWidgetPath]() { PasteInto(ResolvePath(SelectedWidgetPath)); });
	}
	// 방향키로 캔버스 자식 미세 이동 (Shift = 10)
	if (IsCanvasChild(Selected) && !bCtrl && !bPreviewInput)
	{
		const float Step = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
		FVector2    Delta;
		Delta.X += ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? Step : 0.0f;
		Delta.X -= ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? Step : 0.0f;
		Delta.Y += ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? Step : 0.0f;
		Delta.Y -= ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? Step : 0.0f;
		if (Delta != FVector2::ZeroVector)
		{
			FUILayout::MoveCanvasSlot(Selected->Slot, Delta);
			MarkEdited("위젯 이동");
		}
	}
}

void FWidgetEditor::RenderPreview(FAssetEditorEnvironment& Env)
{
	if (!bRenderThisFrame || !Target || Env.UIRenderer == nullptr || Env.Rhi == nullptr)
	{
		return;
	}
	ID3D12GraphicsCommandList* CommandList = Env.Rhi->GetCommandList();
	Target->Begin(CommandList, GCanvasClearColor);
	Env.UIRenderer->Render(DrawList, Target->GetOutput(), ContentDirectory);
	Target->End(CommandList);
	bRenderThisFrame = false;
}

// ---------------------------------------------------------------- 속성

void FWidgetEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	(void)Env;
	if (ImGui::CollapsingHeader(ICON_FA_DISPLAY " UI 설정"))
	{
		DrawAssetSettings();
	}
	FUIWidget* Selected = GetSelected();
	if (Selected == nullptr)
	{
		ImGui::Spacing();
		FAssetEditorWidgets::Hint("캔버스나 계층에서 위젯을 선택하세요. 팔레트의 위젯을 캔버스/계층으로 끌어 놓으면 추가됩니다.");
		return;
	}
	const FPaletteEntry& Entry = GetPaletteEntry(Selected->Type);
	ImGui::SeparatorText(std::format("{} {}", Entry.Icon, Entry.Label).c_str());
	ImGui::PushID(Selected);
	DrawCommonProperties(*Selected);
	if (Selected->Parent != nullptr &&
	    ImGui::CollapsingHeader(std::format("슬롯 (부모: {})###Slot", GetPaletteEntry(Selected->Parent->Type).Label).c_str(), ImGuiTreeNodeFlags_DefaultOpen))
	{
		DrawSlotProperties(*Selected);
	}
	DrawTypeProperties(*Selected);
	ImGui::PopID();
}

void FWidgetEditor::DrawAssetSettings()
{
	FVector2 Design = Asset.DesignSize;
	if (ImGui::DragFloat2("설계 해상도", &Design.X, 1.0f, 64.0f, 8192.0f, "%.0f"))
	{
		Asset.DesignSize = FVector2(FMath::Max(Design.X, 64.0f), FMath::Max(Design.Y, 64.0f));
		if (PreviewPreset == 0)
		{
			PreviewSize = Asset.DesignSize;
		}
		MarkEdited("설계 해상도");
	}
	if (EnumCombo("배율 규칙", Asset.ScaleMode, GScaleModeLabels))
	{
		MarkEdited("배율 규칙");
	}
	FAssetEditorWidgets::Hint("위젯 좌표는 설계 해상도 기준 UI 단위입니다. 실제 화면에서는 배율 규칙으로 늘리거나 줄입니다 (높이 맞춤: 화면 높이 / 설계 높이).");
}

void FWidgetEditor::DrawCommonProperties(FUIWidget& Widget)
{
	const std::vector<int32> ThisPath = GetPath(&Widget);
	if (ThisPath != NameBufferPath || !ImGui::IsAnyItemActive())
	{
		if (ThisPath != NameBufferPath || std::strcmp(NameBuffer, Widget.Name.c_str()) != 0)
		{
			std::snprintf(NameBuffer, sizeof(NameBuffer), "%s", Widget.Name.c_str());
			NameBufferPath = ThisPath;
		}
	}
	ImGui::InputText("이름", NameBuffer, sizeof(NameBuffer));
	if (ImGui::IsItemDeactivatedAfterEdit())
	{
		const std::string Unique = MakeUniqueName(NameBuffer, &Widget);
		if (Unique != Widget.Name)
		{
			Widget.Name = Unique;
			MarkEdited("이름");
		}
		std::snprintf(NameBuffer, sizeof(NameBuffer), "%s", Widget.Name.c_str());
	}
	ImGui::SetItemTooltip("Lua/C++에서 이 이름으로 찾습니다 (UI 안에서 겹치지 않게 유지)");
	if (EnumCombo("표시", Widget.Visibility, GVisibilityLabels))
	{
		MarkEdited("표시");
	}
	if (ImGui::Checkbox("활성", &Widget.bEnabled))
	{
		MarkEdited("활성");
	}
	if (ImGui::SliderFloat("불투명도", &Widget.RenderOpacity, 0.0f, 1.0f))
	{
		MarkEdited("불투명도");
	}
	if (ImGui::DragFloat2("최소 크기", &Widget.MinSize.X, 1.0f, 0.0f, 8192.0f, "%.0f"))
	{
		MarkEdited("최소 크기");
	}
}

void FWidgetEditor::DrawAnchorPresets(FUIWidget& Widget)
{
	// 4x4 (행: 위/가운데/아래/세로 늘이기, 열: 왼/가운데/오른/가로 늘이기) — UMG 앵커 메뉴
	constexpr float Mins[4] = { 0.0f, 0.5f, 1.0f, 0.0f };
	constexpr float Maxs[4] = { 0.0f, 0.5f, 1.0f, 1.0f };
	const float     Cell    = ImGui::GetFrameHeight() * 1.4f;
	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Col = 0; Col < 4; ++Col)
		{
			ImGui::PushID(Row * 4 + Col);
			if (Col > 0)
			{
				ImGui::SameLine();
			}
			const ImVec2 Pos = ImGui::GetCursorScreenPos();
			const bool   bClicked = ImGui::InvisibleButton("##Anchor", ImVec2(Cell, Cell));
			const bool   bHovered = ImGui::IsItemHovered();
			ImDrawList*  Draw     = ImGui::GetWindowDrawList();
			const ImVec2 BoxMin(Pos.x + 3.0f, Pos.y + 3.0f);
			const ImVec2 BoxMax(Pos.x + Cell - 3.0f, Pos.y + Cell - 3.0f);
			Draw->AddRectFilled(BoxMin, BoxMax, bHovered ? IM_COL32(70, 74, 84, 255) : IM_COL32(45, 47, 54, 255), 2.0f);
			Draw->AddRect(BoxMin, BoxMax, IM_COL32(110, 110, 120, 255), 2.0f);
			// 앵커 영역 표시
			const ImVec2 Size(BoxMax.x - BoxMin.x, BoxMax.y - BoxMin.y);
			const ImVec2 A(BoxMin.x + Size.x * Mins[Col], BoxMin.y + Size.y * Mins[Row]);
			const ImVec2 B(BoxMin.x + Size.x * Maxs[Col], BoxMin.y + Size.y * Maxs[Row]);
			const ImU32  Color = ToColor(FEditorTheme::Warning, 1.0f);
			if (A.x == B.x && A.y == B.y)
			{
				Draw->AddCircleFilled(A, 3.0f, Color);
			}
			else
			{
				Draw->AddRectFilled(ImVec2(A.x - 1.5f, A.y - 1.5f), ImVec2(B.x + 1.5f, B.y + 1.5f), ToColor(FEditorTheme::Warning, 0.6f));
			}
			if (bClicked)
			{
				const FVector2 NewMin(Mins[Col], Mins[Row]);
				const FVector2 NewMax(Maxs[Col], Maxs[Row]);
				const FUIRect& ParentRect = Widget.Parent->State.Geometry;
				FUIRect        Current    = Widget.State.Geometry;
				if (bAnchorMovesWidget)
				{
					// 피벗 = 앵커, 위치 = 앵커점 (점 축만), 늘이기 축은 여백 0
					FUISlot& Slot = Widget.Slot;
					Slot.AnchorMin = NewMin;
					Slot.AnchorMax = NewMax;
					Slot.Alignment = FVector2(NewMin.X == NewMax.X ? NewMin.X : 0.0f, NewMin.Y == NewMax.Y ? NewMin.Y : 0.0f);
					Slot.Offsets.Left = Slot.Offsets.Top = 0.0f;
					if (!Slot.IsAnchorPointX())
					{
						Slot.Offsets.Right = 0.0f;
					}
					else
					{
						Slot.Offsets.Right = Current.GetWidth();
					}
					if (!Slot.IsAnchorPointY())
					{
						Slot.Offsets.Bottom = 0.0f;
					}
					else
					{
						Slot.Offsets.Bottom = Current.GetHeight();
					}
				}
				else
				{
					FUILayout::SetAnchorsKeepRect(Widget.Slot, ParentRect, Current, NewMin, NewMax);
				}
				MarkEdited("앵커");
				ImGui::CloseCurrentPopup();
			}
			ImGui::PopID();
		}
	}
	ImGui::Checkbox("피벗/위치도 앵커로", &bAnchorMovesWidget);
	ImGui::SetItemTooltip("끄면 화면상 위치/크기를 유지한 채 앵커만 바꿉니다");
}

void FWidgetEditor::DrawSlotProperties(FUIWidget& Widget)
{
	FUISlot&            Slot       = Widget.Slot;
	const EUIWidgetType ParentType = Widget.Parent->Type;
	bool                bChanged   = false;
	if (ParentType == EUIWidgetType::Canvas)
	{
		if (ImGui::Button(ICON_FA_ANCHOR " 앵커 프리셋"))
		{
			ImGui::OpenPopup("##AnchorPresets");
		}
		if (ImGui::BeginPopup("##AnchorPresets"))
		{
			DrawAnchorPresets(Widget);
			ImGui::EndPopup();
		}
		bChanged |= ImGui::DragFloat2("앵커 최소", &Slot.AnchorMin.X, 0.01f, 0.0f, 1.0f, "%.2f");
		bChanged |= ImGui::DragFloat2("앵커 최대", &Slot.AnchorMax.X, 0.01f, 0.0f, 1.0f, "%.2f");
		// 축마다 점 앵커면 위치/크기, 늘이기면 양쪽 여백
		const bool bPointX = Slot.IsAnchorPointX();
		const bool bPointY = Slot.IsAnchorPointY();
		bChanged |= ImGui::DragFloat(bPointX ? "위치 X" : "왼쪽 여백", &Slot.Offsets.Left, 1.0f, 0.0f, 0.0f, "%.1f");
		bChanged |= ImGui::DragFloat(bPointY ? "위치 Y" : "위 여백", &Slot.Offsets.Top, 1.0f, 0.0f, 0.0f, "%.1f");
		ImGui::BeginDisabled(Slot.bAutoSize && bPointX);
		bChanged |= ImGui::DragFloat(bPointX ? "너비" : "오른쪽 여백", &Slot.Offsets.Right, 1.0f, bPointX ? 0.0f : -100000.0f, 100000.0f, "%.1f");
		ImGui::EndDisabled();
		ImGui::BeginDisabled(Slot.bAutoSize && bPointY);
		bChanged |= ImGui::DragFloat(bPointY ? "높이" : "아래 여백", &Slot.Offsets.Bottom, 1.0f, bPointY ? 0.0f : -100000.0f, 100000.0f, "%.1f");
		ImGui::EndDisabled();
		bChanged |= ImGui::DragFloat2("피벗 (정렬)", &Slot.Alignment.X, 0.01f, 0.0f, 1.0f, "%.2f");
		bChanged |= ImGui::Checkbox("내용 크기에 맞춤", &Slot.bAutoSize);
		bChanged |= ImGui::DragInt("Z 순서", &Slot.ZOrder, 0.1f);
	}
	else
	{
		bChanged |= ImGui::DragFloat4("여백", &Slot.Padding.Left, 0.5f, -1000.0f, 1000.0f, "%.1f");
		if (ParentType == EUIWidgetType::UniformGrid)
		{
			bChanged |= ImGui::DragInt("행", &Slot.Row, 0.1f, 0, 256);
			bChanged |= ImGui::DragInt("열", &Slot.Column, 0.1f, 0, 256);
		}
		bChanged |= EnumCombo("가로 정렬", Slot.HAlign, GHAlignLabels);
		bChanged |= EnumCombo("세로 정렬", Slot.VAlign, GVAlignLabels);
		if (ParentType == EUIWidgetType::HorizontalBox || ParentType == EUIWidgetType::VerticalBox)
		{
			bChanged |= EnumCombo("크기", Slot.SizeRule, GSizeRuleLabels);
			if (Slot.SizeRule == EUISizeRule::Fill)
			{
				bChanged |= ImGui::DragFloat("채우기 비율", &Slot.FillWeight, 0.05f, 0.0f, 100.0f, "%.2f");
			}
		}
	}
	if (bChanged)
	{
		MarkEdited("슬롯");
	}
}

bool FWidgetEditor::DrawTextureField(const char* Label, std::string& TexturePath)
{
	bool bChanged = FAssetEditorWidgets::TextureCombo(Label, TexturePath, ImageFiles, "(없음 — 단색)");
	if (ImGui::BeginDragDropTarget())
	{
		if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
		{
			for (const std::filesystem::path& File : *Paths)
			{
				if (IsImageExtension(ToLowerExtension(File)))
				{
					std::error_code             ErrorCode;
					const std::filesystem::path Relative = std::filesystem::relative(File, ContentDirectory, ErrorCode);
					TexturePath                          = FStringConv::ToUtf8((ErrorCode ? File : Relative).generic_wstring());
					bChanged                             = true;
					break;
				}
			}
		}
		ImGui::EndDragDropTarget();
	}
	return bChanged;
}

bool FWidgetEditor::DrawBrush(const char* Label, FUIBrush& Brush, bool bDefaultOpen)
{
	bool bChanged = false;
	ImGui::PushID(Label);
	if (ImGui::TreeNodeEx(Label, bDefaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0))
	{
		bChanged |= ImGui::ColorEdit4("색", &Brush.Color.X, ImGuiColorEditFlags_AlphaBar);
		bChanged |= DrawTextureField("텍스처", Brush.Texture);
		if (!Brush.Texture.empty())
		{
			bChanged |= EnumCombo("그리기", Brush.DrawAs, GDrawAsLabels);
		}
		if (!Brush.Texture.empty() && Brush.DrawAs == EUIBrushDrawAs::NineSlice)
		{
			// 9-slice: 가장자리는 원래 두께, 가운데만 늘어난다 (둥근 모서리/테두리 대신 텍스처 모양)
			bChanged |= ImGui::DragFloat4("여백 (비율)", &Brush.Margin.Left, 0.005f, 0.0f, 0.5f, "%.3f");
			ImGui::SetItemTooltip("텍스처 가장자리 비율 (왼/위/오른/아래, 0~0.5) — UMG Margin과 같음");
			bChanged |= ImGui::DragFloat2("원본 크기", &Brush.TextureSize.X, 1.0f, 1.0f, 8192.0f, "%.0f");
			ImGui::SetItemTooltip("화면 가장자리 두께 = 여백 × 원본 크기 (UI 단위)");
			if (ImGui::SmallButton("텍스처 크기로"))
			{
				const std::filesystem::path File = ContentDirectory / FStringConv::ToWide(Brush.Texture);
				if (FImage Image; FImageLoader::LoadFromFile(File, Image))
				{
					Brush.TextureSize = FVector2(static_cast<float>(Image.Width), static_cast<float>(Image.Height));
					bChanged          = true;
				}
			}
			ImGui::TreePop();
			ImGui::PopID();
			return bChanged;
		}
		bChanged |= ImGui::DragFloat("모서리 반지름", &Brush.CornerRadius, 0.25f, 0.0f, 1000.0f, "%.1f");
		bChanged |= ImGui::DragFloat("테두리 폭", &Brush.BorderWidth, 0.1f, 0.0f, 100.0f, "%.1f");
		if (Brush.BorderWidth > 0.0f)
		{
			bChanged |= ImGui::ColorEdit4("테두리 색", &Brush.BorderColor.X, ImGuiColorEditFlags_AlphaBar);
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
	return bChanged;
}

void FWidgetEditor::DrawTypeProperties(FUIWidget& Widget)
{
	bool bChanged = false;
	switch (Widget.Type)
	{
	case EUIWidgetType::Border:
		if (ImGui::CollapsingHeader("보더", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bChanged |= DrawBrush("배경", Widget.Brush, true);
			bChanged |= ImGui::DragFloat4("안쪽 여백", &Widget.ContentPadding.Left, 0.5f, 0.0f, 1000.0f, "%.1f");
		}
		break;
	case EUIWidgetType::Image:
		if (ImGui::CollapsingHeader("이미지", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bChanged |= DrawBrush("모양", Widget.Brush, true);
			bChanged |= ImGui::DragFloat2("이미지 크기", &Widget.ImageSize.X, 1.0f, 0.0f, 8192.0f, "%.0f");
		}
		break;
	case EUIWidgetType::Text:
		if (ImGui::CollapsingHeader("텍스트", ImGuiTreeNodeFlags_DefaultOpen))
		{
			char Buffer[2048];
			std::snprintf(Buffer, sizeof(Buffer), "%s", Widget.Text.c_str());
			if (ImGui::InputTextMultiline("내용", Buffer, sizeof(Buffer), ImVec2(0.0f, ImGui::GetTextLineHeight() * 4.0f)))
			{
				Widget.Text = Buffer;
				bChanged    = true;
			}
			const char* FontLabel = Widget.Font.empty() ? "(기본 글꼴 — Noto Sans KR)" : Widget.Font.c_str();
			if (ImGui::BeginCombo("글꼴", FontLabel))
			{
				if (ImGui::Selectable("(기본 글꼴 — Noto Sans KR)", Widget.Font.empty()))
				{
					Widget.Font.clear();
					bChanged = true;
				}
				for (const std::string& File : FontFiles)
				{
					if (ImGui::Selectable(File.c_str(), File == Widget.Font))
					{
						Widget.Font = File;
						bChanged    = true;
					}
				}
				ImGui::EndCombo();
			}
			bChanged |= ImGui::DragFloat("크기", &Widget.FontSize, 0.25f, 1.0f, 512.0f, "%.1f");
			bChanged |= ImGui::ColorEdit4("색", &Widget.TextColor.X, ImGuiColorEditFlags_AlphaBar);
			bChanged |= EnumCombo("정렬", Widget.Justify, GJustifyLabels);
			bChanged |= ImGui::Checkbox("자동 줄바꿈", &Widget.bWrap);
			bChanged |= ImGui::DragFloat("외곽선 폭", &Widget.OutlineWidth, 0.05f, 0.0f, 8.0f, "%.2f");
			if (Widget.OutlineWidth > 0.0f)
			{
				bChanged |= ImGui::ColorEdit4("외곽선 색", &Widget.OutlineColor.X, ImGuiColorEditFlags_AlphaBar);
			}
			bChanged |= ImGui::DragFloat2("그림자 위치", &Widget.ShadowOffset.X, 0.1f, -64.0f, 64.0f, "%.1f");
			if (Widget.ShadowOffset != FVector2::ZeroVector)
			{
				bChanged |= ImGui::ColorEdit4("그림자 색", &Widget.ShadowColor.X, ImGuiColorEditFlags_AlphaBar);
			}
			if (FUIFont* Font = FUIFontLibrary::Get().GetFont(Widget.Font); Font != nullptr)
			{
				ImGui::TextDisabled("아틀라스 %u x %u, 글자 %zu개", Font->GetAtlasSize(), Font->GetAtlasSize(), Font->GetGlyphCount());
			}
		}
		break;
	case EUIWidgetType::Button:
		if (ImGui::CollapsingHeader("버튼", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bChanged |= DrawBrush("기본", Widget.Brush, true);
			bChanged |= DrawBrush("호버", Widget.HoveredBrush, false);
			bChanged |= DrawBrush("눌림", Widget.PressedBrush, false);
			bChanged |= DrawBrush("비활성", Widget.DisabledBrush, false);
			bChanged |= ImGui::DragFloat4("안쪽 여백", &Widget.ContentPadding.Left, 0.5f, 0.0f, 1000.0f, "%.1f");
			FAssetEditorWidgets::Hint("스크립트: OnUIClicked_<이름> (2단계에서 연결). 미리보기 입력(▶)으로 호버/클릭 모양을 확인할 수 있습니다.");
		}
		break;
	case EUIWidgetType::ProgressBar:
		if (ImGui::CollapsingHeader("진행 막대", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bChanged |= ImGui::SliderFloat("비율", &Widget.Percent, 0.0f, 1.0f);
			bChanged |= EnumCombo("채우는 방향", Widget.FillDirection, GFillLabels);
			bChanged |= DrawBrush("배경", Widget.Brush, false);
			bChanged |= DrawBrush("채우기", Widget.FillBrush, true);
		}
		break;
	case EUIWidgetType::ScrollBox:
		if (ImGui::CollapsingHeader("스크롤 박스", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bChanged |= EnumCombo("방향", Widget.Orientation, GOrientationLabels);
			bChanged |= ImGui::DragFloat("막대 폭", &Widget.ScrollbarWidth, 0.25f, 0.0f, 64.0f, "%.1f");
			bChanged |= ImGui::ColorEdit4("막대 색", &Widget.ScrollbarColor.X, ImGuiColorEditFlags_AlphaBar);
		}
		break;
	case EUIWidgetType::UniformGrid:
		if (ImGui::CollapsingHeader("균일 그리드", ImGuiTreeNodeFlags_DefaultOpen))
		{
			bChanged |= ImGui::DragFloat2("최소 칸 크기", &Widget.MinCellSize.X, 1.0f, 0.0f, 4096.0f, "%.0f");
			FAssetEditorWidgets::Hint("자식 슬롯의 행/열로 칸을 정합니다.");
		}
		break;
	default:
		FAssetEditorWidgets::Hint("배치용 패널입니다. 자식 위젯의 슬롯 속성으로 배치를 조절하세요.");
		break;
	}
	if (bChanged)
	{
		MarkEdited("속성");
	}
}
