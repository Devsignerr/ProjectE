#include "Editor/AssetEditors/SpriteAtlasEditor.h"

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/AssetEditors/DataValueWidgets.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>

E_DECLARE_LOG_CATEGORY(LogEditor)

using namespace Sprite2DEditing;

namespace
{
	std::wstring& PendingGridDialogKey()
	{
		static std::wstring Key;
		return Key;
	}

	std::wstring MakePathKey(const std::filesystem::path& Path)
	{
		std::wstring Key = Path.lexically_normal().generic_wstring();
		std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Key;
	}

	constexpr ImU32 SliceColor         = IM_COL32(230, 230, 240, 150);
	constexpr ImU32 SelectedColor      = IM_COL32(0, 150, 255, 255);
	constexpr ImU32 PrimaryColor       = IM_COL32(60, 190, 255, 255);
	constexpr ImU32 PivotColor         = IM_COL32(255, 210, 60, 255);
	constexpr ImU32 BorderColor        = IM_COL32(80, 230, 120, 230);
	constexpr ImU32 CreateColor        = IM_COL32(255, 255, 255, 230);
	constexpr ImU32 GridPreviewColor   = IM_COL32(255, 200, 40, 200);
	constexpr float HandleScreenRadius = 6.0f;

	ImU32 ToU32(const ImVec4& Color) { return ImGui::ColorConvertFloat4ToU32(Color); }
} // namespace

void FSpriteAtlasEditor::RequestGridDialog(const std::filesystem::path& AssetPath)
{
	PendingGridDialogKey() = MakePathKey(AssetPath);
}

// ---------------------------------------------------------------- 에셋

bool FSpriteAtlasEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	std::string Text;
	if (!ReadAssetText(Text))
	{
		E_LOG(LogEditor, Error, "스프라이트 아틀라스를 읽을 수 없습니다: {}", GetDisplayName());
		return false;
	}
	FSpriteAsset             Loaded;
	std::vector<std::string> Warnings;
	std::string              Error;
	if (!FSpriteAsset::FromJsonString(Text, Loaded, &Warnings, &Error))
	{
		E_LOG(LogEditor, Error, "스프라이트 아틀라스 형식 오류 ({}): {}", GetDisplayName(), Error);
		return false;
	}
	for (const std::string& Warning : Warnings)
	{
		E_LOG(LogEditor, Warning, "[2D 편집기] {}: {}", GetDisplayName(), Warning);
	}
	Asset = std::move(Loaded);
	SyncTexture(Env);
	// 이미지 크기 자동 채움 (파일 값이 다르면 이번 상태의 기준값이 이미지 크기 — 저장하면 반영)
	if (Texture.IsValid())
	{
		Asset.TextureWidth  = Texture.GetWidth();
		Asset.TextureHeight = Texture.GetHeight();
	}
	ClampSelection();
	RememberDiskState();
	if (PendingGridDialogKey() == MakePathKey(Path))
	{
		PendingGridDialogKey().clear();
		bOpenGridDialog = true;
	}
	return true;
}

bool FSpriteAtlasEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	std::string Error;
	if (!FSprite2DLibrary::Get().SaveSprite(GetAssetPathString(), Asset, &Error))
	{
		E_LOG(LogEditor, Error, "스프라이트 아틀라스 저장 실패: {}", Error);
		return false;
	}
	AfterSaved();
	return true;
}

std::string FSpriteAtlasEditor::CaptureState() const
{
	return Asset.ToJsonString();
}

void FSpriteAtlasEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	FSpriteAsset Restored;
	if (FSpriteAsset::FromJsonString(State, Restored))
	{
		Asset = std::move(Restored);
		SyncTexture(Env);
	}
	RenamingIndex = -1;
	ClampSelection();
}

void FSpriteAtlasEditor::OnClose(FAssetEditorEnvironment& Env)
{
	ReleaseTexturePreview(Env, Texture);
	FSprite2DEditorBase::OnClose(Env);
}

void FSpriteAtlasEditor::Edited(std::string_view Label)
{
	MarkEdited(Label);
}

void FSpriteAtlasEditor::SyncTexture(FAssetEditorEnvironment& Env)
{
	UpdateTexturePreview(Env, Texture, ResolveReference(Asset.Texture));
}

// ---------------------------------------------------------------- 선택

bool FSpriteAtlasEditor::IsSelected(int32 Index) const
{
	return std::find(Selection.begin(), Selection.end(), Index) != Selection.end();
}

void FSpriteAtlasEditor::SelectOnly(int32 Index)
{
	Selection.clear();
	RenameError.clear();
	if (Index >= 0)
	{
		Selection.push_back(Index);
	}
}

void FSpriteAtlasEditor::ClampSelection()
{
	const int32 Count = static_cast<int32>(Asset.Slices.size());
	std::erase_if(Selection, [Count](int32 Index) { return Index < 0 || Index >= Count; });
}

int32 FSpriteAtlasEditor::FindSliceAt(const FVector2& Point) const
{
	// 뒤(나중에 그린 것)부터 — 겹치면 위에 보이는 것
	for (size_t Index = Asset.Slices.size(); Index-- > 0;)
	{
		const FSpriteSlice& Slice = Asset.Slices[Index];
		if (Point.X >= static_cast<float>(Slice.X) && Point.X < static_cast<float>(Slice.X + Slice.W) && Point.Y >= static_cast<float>(Slice.Y) &&
		    Point.Y < static_cast<float>(Slice.Y + Slice.H))
		{
			return static_cast<int32>(Index);
		}
	}
	return -1;
}

void FSpriteAtlasEditor::DeleteSelected()
{
	if (Selection.empty())
	{
		return;
	}
	std::vector<int32> Sorted = Selection;
	std::sort(Sorted.begin(), Sorted.end());
	for (auto It = Sorted.rbegin(); It != Sorted.rend(); ++It)
	{
		Asset.Slices.erase(Asset.Slices.begin() + *It);
	}
	Selection.clear();
	Edited("슬라이스 삭제");
}

// ---------------------------------------------------------------- 캔버스

void FSpriteAtlasEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	DrawExternalChangeBanner(Env);
	SyncTexture(Env);
	if (!bCheckedArgs)
	{
		bCheckedArgs = true;
		const FCommandLine& Args = FCommandLine::FromProcess();
		if (const std::wstring Select = Args.GetValue(L"--sprite-select"); !Select.empty())
		{
			SelectOnly(Asset.FindSlice(FStringConv::ToUtf8(Select)));
		}
		if (Args.HasFlag(L"--sprite-grid-dialog"))
		{
			bOpenGridDialog = true;
		}
	}

	// 도구 줄
	ImGui::Checkbox(ICON_FA_CROSSHAIRS " 피벗", &bShowPivots);
	ImGui::SameLine();
	ImGui::Checkbox(ICON_FA_BORDER_ALL " 9-슬라이스", &bShowBorders);
	ImGui::SameLine();
	ImGui::Checkbox("이름", &bShowNames);
	ImGui::SameLine();
	ImGui::Checkbox(ICON_FA_TABLE_CELLS " 픽셀 격자", &bShowGrid);
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_SCISSORS " 격자로 자르기"))
	{
		bOpenGridDialog = true;
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_EXPAND " 화면 맞춤 (F)"))
	{
		Canvas.Frame(Asset.TextureWidth, Asset.TextureHeight);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%.0f%%", Canvas.GetZoom() * 100.0f);
	if (Canvas.IsHovered())
	{
		const FVector2 Mouse = Canvas.GetMouseImage();
		ImGui::SameLine();
		ImGui::TextDisabled("(%d, %d)", static_cast<int32>(std::floor(Mouse.X)), static_cast<int32>(std::floor(Mouse.Y)));
	}

	DrawCanvas(Env);
	DrawGridDialog();
}

void FSpriteAtlasEditor::DrawCanvas(FAssetEditorEnvironment& Env)
{
	const ImVec2 Avail = ImGui::GetContentRegionAvail();
	ImGui::PushID("SpriteCanvas");
	Canvas.Begin("##Canvas", Avail, Asset.TextureWidth, Asset.TextureHeight);
	const bool bNearest = Asset.Filter == ESpriteFilter::Point;
	Canvas.DrawImage(GetImTexture(Env, Texture), Asset.TextureWidth, Asset.TextureHeight, bNearest);
	if (bShowGrid)
	{
		Canvas.DrawPixelGrid(Asset.TextureWidth, Asset.TextureHeight);
	}

	HandleCanvasInput();

	ImDrawList* DrawList = Canvas.GetDrawList();
	const int32 Primary  = GetPrimary();
	for (size_t Index = 0; Index < Asset.Slices.size(); ++Index)
	{
		const FSpriteSlice& Slice    = Asset.Slices[Index];
		const bool          bSel     = IsSelected(static_cast<int32>(Index));
		const ImVec2        Min      = Canvas.ImageToScreen(FVector2(static_cast<float>(Slice.X), static_cast<float>(Slice.Y)));
		const ImVec2        Max      = Canvas.ImageToScreen(FVector2(static_cast<float>(Slice.X + Slice.W), static_cast<float>(Slice.Y + Slice.H)));
		const ImU32         Color    = static_cast<int32>(Index) == Primary ? PrimaryColor : (bSel ? SelectedColor : SliceColor);
		DrawList->AddRect(Min, Max, Color, 0.0f, 0, bSel ? 2.0f : 1.0f);
		if (bSel)
		{
			DrawList->AddRectFilled(Min, Max, IM_COL32(0, 150, 255, 30));
		}
		if (bShowNames && Max.x - Min.x > 36.0f && Max.y - Min.y > 16.0f)
		{
			DrawList->AddText(ImVec2(Min.x + 3.0f, Min.y + 2.0f), Color, Slice.Name.c_str());
		}
		if (bShowBorders && Slice.HasBorder() && (bSel || static_cast<int32>(Index) == Primary))
		{
			const float L = static_cast<float>(Slice.X + Slice.BorderLeft);
			const float R = static_cast<float>(Slice.X + Slice.W - Slice.BorderRight);
			const float T = static_cast<float>(Slice.Y + Slice.BorderTop);
			const float B = static_cast<float>(Slice.Y + Slice.H - Slice.BorderBottom);
			DrawList->AddLine(Canvas.ImageToScreen(FVector2(L, static_cast<float>(Slice.Y))), Canvas.ImageToScreen(FVector2(L, static_cast<float>(Slice.Y + Slice.H))), BorderColor);
			DrawList->AddLine(Canvas.ImageToScreen(FVector2(R, static_cast<float>(Slice.Y))), Canvas.ImageToScreen(FVector2(R, static_cast<float>(Slice.Y + Slice.H))), BorderColor);
			DrawList->AddLine(Canvas.ImageToScreen(FVector2(static_cast<float>(Slice.X), T)), Canvas.ImageToScreen(FVector2(static_cast<float>(Slice.X + Slice.W), T)), BorderColor);
			DrawList->AddLine(Canvas.ImageToScreen(FVector2(static_cast<float>(Slice.X), B)), Canvas.ImageToScreen(FVector2(static_cast<float>(Slice.X + Slice.W), B)), BorderColor);
		}
	}

	// 주 선택: 크기 핸들, 테두리 핸들(마름모), 피벗
	if (Primary >= 0)
	{
		const FSpriteSlice& Slice = Asset.Slices[static_cast<size_t>(Primary)];
		const float         X0    = static_cast<float>(Slice.X);
		const float         Y0    = static_cast<float>(Slice.Y);
		const float         X1    = static_cast<float>(Slice.X + Slice.W);
		const float         Y1    = static_cast<float>(Slice.Y + Slice.H);
		const float         XM    = (X0 + X1) * 0.5f;
		const float         YM    = (Y0 + Y1) * 0.5f;
		const FVector2      Handles[] = { { X0, Y0 }, { XM, Y0 }, { X1, Y0 }, { X0, YM }, { X1, YM }, { X0, Y1 }, { XM, Y1 }, { X1, Y1 } };
		for (const FVector2& Handle : Handles)
		{
			const ImVec2 Center = Canvas.ImageToScreen(Handle);
			DrawList->AddRectFilled(ImVec2(Center.x - 3.5f, Center.y - 3.5f), ImVec2(Center.x + 3.5f, Center.y + 3.5f), IM_COL32(255, 255, 255, 255));
			DrawList->AddRect(ImVec2(Center.x - 3.5f, Center.y - 3.5f), ImVec2(Center.x + 3.5f, Center.y + 3.5f), PrimaryColor);
		}
		if (bShowBorders)
		{
			const FVector2 BorderHandles[] = {
				{ static_cast<float>(Slice.X + Slice.BorderLeft), Y0 + (Y1 - Y0) * 0.25f },
				{ X0 + (X1 - X0) * 0.25f, static_cast<float>(Slice.Y + Slice.BorderTop) },
				{ static_cast<float>(Slice.X + Slice.W - Slice.BorderRight), Y0 + (Y1 - Y0) * 0.75f },
				{ X0 + (X1 - X0) * 0.75f, static_cast<float>(Slice.Y + Slice.H - Slice.BorderBottom) },
			};
			for (const FVector2& Handle : BorderHandles)
			{
				const ImVec2 C = Canvas.ImageToScreen(Handle);
				DrawList->AddQuadFilled(ImVec2(C.x, C.y - 5.0f), ImVec2(C.x + 5.0f, C.y), ImVec2(C.x, C.y + 5.0f), ImVec2(C.x - 5.0f, C.y), BorderColor);
			}
		}
		if (bShowPivots)
		{
			const ImVec2 P = Canvas.ImageToScreen(PivotToImagePoint(GetRect(Slice), Slice.Pivot));
			DrawList->AddCircle(P, 6.0f, PivotColor, 16, 2.0f);
			DrawList->AddLine(ImVec2(P.x - 10.0f, P.y), ImVec2(P.x + 10.0f, P.y), PivotColor);
			DrawList->AddLine(ImVec2(P.x, P.y - 10.0f), ImVec2(P.x, P.y + 10.0f), PivotColor);
		}
	}

	// 새 슬라이스 끌기 미리보기
	if (Drag == EDrag::Create)
	{
		const FPixelRect Rect = MakeRectFromDrag(DragStart, Canvas.GetMouseImage(), Asset.TextureWidth, Asset.TextureHeight);
		if (Rect.W > 0 && Rect.H > 0)
		{
			DrawList->AddRect(Canvas.ImageToScreen(FVector2(static_cast<float>(Rect.X), static_cast<float>(Rect.Y))),
			                  Canvas.ImageToScreen(FVector2(static_cast<float>(Rect.X + Rect.W), static_cast<float>(Rect.Y + Rect.H))), CreateColor, 0.0f, 0, 1.5f);
			const ImVec2 Label = Canvas.ImageToScreen(FVector2(static_cast<float>(Rect.X + Rect.W), static_cast<float>(Rect.Y + Rect.H)));
			DrawList->AddText(ImVec2(Label.x + 4.0f, Label.y + 2.0f), CreateColor, std::format("{} x {}", Rect.W, Rect.H).c_str());
		}
	}

	// 격자 대화 미리보기
	if (bGridDialogOpen)
	{
		const FImageView                Image{ Texture.GetWidth(), Texture.GetHeight(), Texture.Image.Pixels };
		const std::vector<FSpriteSlice> Cells = SliceGridWithOptions(Asset.TextureWidth, Asset.TextureHeight, GridOptions, Image);
		for (const FSpriteSlice& Cell : Cells)
		{
			DrawList->AddRect(Canvas.ImageToScreen(FVector2(static_cast<float>(Cell.X), static_cast<float>(Cell.Y))),
			                  Canvas.ImageToScreen(FVector2(static_cast<float>(Cell.X + Cell.W), static_cast<float>(Cell.Y + Cell.H))), GridPreviewColor);
		}
	}
	Canvas.End();
	ImGui::PopID();
}

void FSpriteAtlasEditor::HandleCanvasInput()
{
	const FVector2 Mouse   = Canvas.GetMouseImage();
	const float    Radius  = Canvas.ScreenToImageDistance(HandleScreenRadius);
	const bool     bCtrl   = ImGui::GetIO().KeyCtrl;
	const int32    Primary = GetPrimary();

	if (Canvas.IsLeftClicked())
	{
		Drag = EDrag::None;
		if (Primary >= 0)
		{
			const FSpriteSlice& Slice = Asset.Slices[static_cast<size_t>(Primary)];
			const FPixelRect    Rect  = GetRect(Slice);
			// 1) 피벗
			if (bShowPivots && (PivotToImagePoint(Rect, Slice.Pivot) - Mouse).Length() <= Radius * 1.4f)
			{
				Drag = EDrag::Pivot;
			}
			// 2) 9-슬라이스 테두리 마름모
			if (Drag == EDrag::None && bShowBorders)
			{
				const float    X0 = static_cast<float>(Slice.X);
				const float    Y0 = static_cast<float>(Slice.Y);
				const float    W  = static_cast<float>(Slice.W);
				const float    H  = static_cast<float>(Slice.H);
				const FVector2 BorderHandles[] = {
					{ X0 + static_cast<float>(Slice.BorderLeft), Y0 + H * 0.25f },
					{ X0 + W * 0.25f, Y0 + static_cast<float>(Slice.BorderTop) },
					{ X0 + W - static_cast<float>(Slice.BorderRight), Y0 + H * 0.75f },
					{ X0 + W * 0.75f, Y0 + H - static_cast<float>(Slice.BorderBottom) },
				};
				for (int32 Side = 0; Side < 4; ++Side)
				{
					if ((BorderHandles[Side] - Mouse).Length() <= Radius)
					{
						Drag           = EDrag::Border;
						DragBorderSide = Side;
						break;
					}
				}
			}
			// 3) 크기/이동 핸들
			if (Drag == EDrag::None && !bCtrl)
			{
				const ERectHandle Handle = HitTestRect(Rect, Mouse, Radius);
				// 안쪽(Move)은 다른 슬라이스가 위에 겹쳐 있으면 그쪽을 고른다
				if (Handle != ERectHandle::None && (Handle != ERectHandle::Move || FindSliceAt(Mouse) == Primary))
				{
					Drag       = EDrag::Rect;
					DragHandle = Handle;
				}
			}
		}
		if (Drag == EDrag::None)
		{
			const int32 Hit = FindSliceAt(Mouse);
			if (Hit >= 0)
			{
				if (bCtrl)
				{
					if (IsSelected(Hit))
					{
						std::erase(Selection, Hit);
					}
					else
					{
						Selection.push_back(Hit);
					}
				}
				else
				{
					SelectOnly(Hit);
					Drag       = EDrag::Rect;
					DragHandle = ERectHandle::Move;
				}
			}
			else
			{
				if (!bCtrl)
				{
					Selection.clear();
				}
				Drag = EDrag::Create;
			}
		}
		DragStart = Mouse;
		if (GetPrimary() >= 0)
		{
			DragOriginalRect = GetRect(Asset.Slices[static_cast<size_t>(GetPrimary())]);
		}
	}

	const int32 Target = GetPrimary();
	if (Canvas.IsLeftDown() && Target >= 0)
	{
		FSpriteSlice& Slice = Asset.Slices[static_cast<size_t>(Target)];
		if (Drag == EDrag::Rect)
		{
			const FPixelRect NewRect = ApplyRectDrag(DragOriginalRect, DragHandle, Mouse - DragStart, Asset.TextureWidth, Asset.TextureHeight);
			if (!(NewRect == GetRect(Slice)))
			{
				SetRect(Slice, NewRect);
				Edited(DragHandle == ERectHandle::Move ? "슬라이스 이동" : "슬라이스 크기 조절");
			}
		}
		else if (Drag == EDrag::Pivot)
		{
			const FVector2 Pivot = PivotFromImagePoint(GetRect(Slice), Mouse, !ImGui::GetIO().KeyAlt);
			if (!(Pivot == Slice.Pivot))
			{
				Slice.Pivot = Pivot;
				Edited("피벗 이동");
			}
		}
		else if (Drag == EDrag::Border)
		{
			const FSpriteSlice Before = Slice;
			ApplyBorderDrag(Slice, DragBorderSide, Mouse);
			if (!(Before == Slice))
			{
				Edited("9-슬라이스 테두리");
			}
		}
	}
	if (Canvas.IsLeftReleased())
	{
		if (Drag == EDrag::Create)
		{
			const FPixelRect Rect = MakeRectFromDrag(DragStart, Mouse, Asset.TextureWidth, Asset.TextureHeight);
			if (Rect.W > 0 && Rect.H > 0)
			{
				SelectOnly(AddSlice(Asset, Rect, "Slice"));
				Edited("슬라이스 추가");
			}
		}
		Drag = EDrag::None;
	}

	// 키보드: Delete = 삭제, 화살표 = 1px 이동 (Shift = 크기)
	if ((Canvas.IsHovered() || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) && !ImGui::GetIO().WantTextInput && Drag == EDrag::None)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
		{
			DeleteSelected();
		}
		const int32 DX = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? -1 : (ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : 0);
		const int32 DY = ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0);
		if ((DX != 0 || DY != 0) && !Selection.empty())
		{
			const bool bResize = ImGui::GetIO().KeyShift;
			for (const int32 Index : Selection)
			{
				FSpriteSlice& Slice = Asset.Slices[static_cast<size_t>(Index)];
				SetRect(Slice, ApplyRectDrag(GetRect(Slice), bResize ? ERectHandle::BottomRight : ERectHandle::Move, FVector2(static_cast<float>(DX), static_cast<float>(DY)),
				                             Asset.TextureWidth, Asset.TextureHeight));
			}
			Edited(bResize ? "슬라이스 크기 조절" : "슬라이스 이동");
		}
	}
}

// ---------------------------------------------------------------- 속성

void FSpriteAtlasEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	ImGui::SeparatorText(ICON_FA_IMAGE " 텍스처");
	const std::vector<std::string>& Images = FAssetEditorWidgets::ScanImageFiles(Env.Editor != nullptr ? Env.Editor->ContentDirectory : GetAssetDirectory(), GetAssetDirectory());
	std::string TexturePath = Asset.Texture;
	bool        bTexture    = FAssetEditorWidgets::TextureCombo("텍스처", TexturePath, Images, "(없음)");
	bTexture |= FAssetEditorWidgets::AcceptTextureDrop(TexturePath, GetAssetDirectory());
	if (bTexture && TexturePath != Asset.Texture)
	{
		Asset.Texture = TexturePath;
		SyncTexture(Env);
		if (Texture.IsValid())
		{
			Asset.TextureWidth  = Texture.GetWidth();
			Asset.TextureHeight = Texture.GetHeight();
		}
		Edited("텍스처 변경");
	}
	if (Texture.bFailed)
	{
		ImGui::TextColored(FEditorTheme::Danger, ICON_FA_TRIANGLE_EXCLAMATION " 이미지를 읽을 수 없습니다");
	}
	ImGui::TextDisabled("크기 %d x %d px (이미지에서 자동)", Asset.TextureWidth, Asset.TextureHeight);
	if (ImGui::DragFloat("UnitsPerPixel", &Asset.UnitsPerPixel, 0.01f, 0.001f, 1000.0f, "%.3f cm/px"))
	{
		Asset.UnitsPerPixel = FMath::Max(Asset.UnitsPerPixel, 0.001f);
		Edited("UnitsPerPixel");
	}
	ImGui::SetItemTooltip("1px = N cm");
	int32 Filter = static_cast<int32>(Asset.Filter);
	if (ImGui::Combo("필터", &Filter, "Point (픽셀 아트)\0Linear\0"))
	{
		Asset.Filter = static_cast<ESpriteFilter>(Filter);
		Edited("필터");
	}

	DrawSliceList();
	DrawSliceDetails();
}

void FSpriteAtlasEditor::DrawSliceList()
{
	ImGui::SeparatorText(std::format(ICON_FA_TABLE_CELLS " 슬라이스 ({})", Asset.Slices.size()).c_str());
	if (ImGui::SmallButton(ICON_FA_PLUS " 전체 이미지"))
	{
		SelectOnly(AddSlice(Asset, FPixelRect{ 0, 0, FMath::Max(Asset.TextureWidth, 1), FMath::Max(Asset.TextureHeight, 1) }, "Slice"));
		Edited("슬라이스 추가");
	}
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_SCISSORS " 격자로 자르기…"))
	{
		bOpenGridDialog = true;
	}
	ImGui::SameLine();
	const auto SortBy = [this](ESliceSort Sort) {
		std::vector<std::string> SelectedNames;
		for (const int32 Index : Selection)
		{
			SelectedNames.push_back(Asset.Slices[static_cast<size_t>(Index)].Name);
		}
		SortSlices(Asset, Sort);
		Selection.clear();
		for (const std::string& Name : SelectedNames)
		{
			Selection.push_back(Asset.FindSlice(Name));
		}
		Edited("슬라이스 정렬");
	};
	if (ImGui::SmallButton(ICON_FA_ARROW_DOWN_A_Z " 이름순"))
	{
		SortBy(ESliceSort::Name);
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("위치순"))
	{
		SortBy(ESliceSort::Position);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(Selection.empty());
	if (ImGui::SmallButton(ICON_FA_TRASH_CAN " 삭제"))
	{
		DeleteSelected();
	}
	ImGui::EndDisabled();

	const float ListHeight = FMath::Clamp(static_cast<float>(Asset.Slices.size()) * ImGui::GetTextLineHeightWithSpacing() + 8.0f, 60.0f, 220.0f);
	if (ImGui::BeginChild("##SliceList", ImVec2(0.0f, ListHeight), ImGuiChildFlags_Borders))
	{
		for (size_t Index = 0; Index < Asset.Slices.size(); ++Index)
		{
			const int32   SliceIndex = static_cast<int32>(Index);
			FSpriteSlice& Slice      = Asset.Slices[Index];
			ImGui::PushID(SliceIndex);
			if (RenamingIndex == SliceIndex)
			{
				if (bFocusRename)
				{
					ImGui::SetKeyboardFocusHere();
					bFocusRename = false;
				}
				ImGui::SetNextItemWidth(-1.0f);
				const bool bEnter = DataValueWidgets::InputString("##Rename", RenameBuffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
				RenameError       = ValidateRename(Asset, SliceIndex, RenameBuffer);
				if (bEnter || ImGui::IsItemDeactivated())
				{
					if (RenameError.empty() && RenameBuffer != Slice.Name)
					{
						RenameSlice(Asset, SliceIndex, RenameBuffer);
						Edited("슬라이스 이름 바꾸기");
					}
					RenamingIndex = -1;
				}
				if (!RenameError.empty() && RenameBuffer != Slice.Name)
				{
					ImGui::TextColored(FEditorTheme::Danger, "%s", RenameError.c_str());
				}
			}
			else
			{
				const std::string Label = std::format("{}##Slice", Slice.Name);
				if (ImGui::Selectable(Label.c_str(), IsSelected(SliceIndex), ImGuiSelectableFlags_AllowDoubleClick))
				{
					if (ImGui::GetIO().KeyCtrl)
					{
						if (IsSelected(SliceIndex))
						{
							std::erase(Selection, SliceIndex);
						}
						else
						{
							Selection.push_back(SliceIndex);
						}
					}
					else if (ImGui::GetIO().KeyShift && GetPrimary() >= 0)
					{
						const int32 From = GetPrimary();
						Selection.clear();
						for (int32 Each = FMath::Min(From, SliceIndex); Each <= FMath::Max(From, SliceIndex); ++Each)
						{
							if (Each != SliceIndex)
							{
								Selection.push_back(Each);
							}
						}
						Selection.push_back(SliceIndex);
					}
					else
					{
						SelectOnly(SliceIndex);
					}
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						RenamingIndex = SliceIndex;
						RenameBuffer  = Slice.Name;
						bFocusRename  = true;
					}
				}
				ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.6f);
				ImGui::TextDisabled("%d,%d %dx%d", Slice.X, Slice.Y, Slice.W, Slice.H);
			}
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	FAssetEditorWidgets::Hint("캔버스 빈 곳 끌기 = 새 슬라이스, Ctrl+클릭 = 여러 개, 더블 클릭 = 이름 바꾸기, Delete = 삭제, 화살표 = 1px 이동(Shift = 크기)");
}

void FSpriteAtlasEditor::DrawSliceDetails()
{
	const int32 Primary = GetPrimary();
	if (Primary < 0)
	{
		return;
	}
	FSpriteSlice& Slice = Asset.Slices[static_cast<size_t>(Primary)];
	ImGui::SeparatorText(std::format("선택: {}", Slice.Name).c_str());

	// 이름 (Enter/포커스 해제 때 검사 후 반영 — 중복/빈 이름이면 그대로 두고 오류 표시)
	if (std::string NewName; DataValueWidgets::InputTextCommit("이름", Slice.Name, NewName) && NewName != Slice.Name)
	{
		std::string Error;
		if (RenameSlice(Asset, Primary, NewName, &Error))
		{
			Edited("슬라이스 이름 바꾸기");
			RenameError.clear();
		}
		else
		{
			RenameError = Error;
		}
	}
	if (!RenameError.empty() && RenamingIndex < 0)
	{
		ImGui::TextColored(FEditorTheme::Danger, "%s", RenameError.c_str());
	}

	int32 Rect[4] = { Slice.X, Slice.Y, Slice.W, Slice.H };
	if (ImGui::DragInt4("X, Y, W, H", Rect, 0.2f))
	{
		const FPixelRect Clamped = ClampRect(NormalizeRect(Rect[0], Rect[1], Rect[0] + Rect[2], Rect[1] + Rect[3]), Asset.TextureWidth, Asset.TextureHeight);
		SetRect(Slice, Clamped);
		Edited("슬라이스 사각형");
	}
	float Pivot[2] = { Slice.Pivot.X, Slice.Pivot.Y };
	if (ImGui::DragFloat2("피벗", Pivot, 0.005f, 0.0f, 1.0f, "%.3f"))
	{
		Slice.Pivot = FVector2(FMath::Clamp(Pivot[0], 0.0f, 1.0f), FMath::Clamp(Pivot[1], 0.0f, 1.0f));
		Edited("피벗");
	}
	ImGui::SetItemTooltip("0~1, X 왼쪽 0 → 오른쪽 1, Y 아래 0 → 위 1");
	int32 PresetIndex = 0;
	for (const FPivotPreset& Preset : GetPivotPresets())
	{
		if (PresetIndex++ % 5 != 0)
		{
			ImGui::SameLine();
		}
		if (ImGui::SmallButton(Preset.Label))
		{
			for (const int32 Index : Selection)
			{
				Asset.Slices[static_cast<size_t>(Index)].Pivot = Preset.Pivot;
			}
			Edited("피벗 프리셋");
		}
	}
	int32 Border[4] = { Slice.BorderLeft, Slice.BorderTop, Slice.BorderRight, Slice.BorderBottom };
	if (ImGui::DragInt4("테두리 L,T,R,B", Border, 0.2f, 0, 4096))
	{
		Slice.BorderLeft   = FMath::Clamp(Border[0], 0, Slice.W);
		Slice.BorderRight  = FMath::Clamp(Border[2], 0, Slice.W - Slice.BorderLeft);
		Slice.BorderTop    = FMath::Clamp(Border[1], 0, Slice.H);
		Slice.BorderBottom = FMath::Clamp(Border[3], 0, Slice.H - Slice.BorderTop);
		Edited("9-슬라이스 테두리");
	}
	ImGui::SetItemTooltip("9-슬라이스 테두리 (px). 모두 0 = 9-슬라이스 아님. 캔버스의 초록 마름모를 끌어도 된다");
}

void FSpriteAtlasEditor::DrawGridDialog()
{
	if (bOpenGridDialog)
	{
		ImGui::OpenPopup("격자로 자르기##SpriteGrid");
		bOpenGridDialog = false;
	}
	bGridDialogOpen = false;
	ImGui::SetNextWindowPos(ImVec2(Canvas.GetMax().x - 12.0f, Canvas.GetMin().y + 12.0f), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
	if (!ImGui::BeginPopup("격자로 자르기##SpriteGrid"))
	{
		return;
	}
	bGridDialogOpen = true;
	ImGui::TextUnformatted(ICON_FA_SCISSORS " 격자로 자르기");
	ImGui::Separator();
	int32 Mode = GridOptions.bByCount ? 1 : 0;
	ImGui::RadioButton("셀 크기", &Mode, 0);
	ImGui::SameLine();
	ImGui::RadioButton("개수", &Mode, 1);
	GridOptions.bByCount = Mode == 1;
	ImGui::PushItemWidth(160.0f);
	if (GridOptions.bByCount)
	{
		int32 Count[2] = { GridOptions.Columns, GridOptions.Rows };
		if (ImGui::InputInt2("열, 행", Count))
		{
			GridOptions.Columns = FMath::Clamp(Count[0], 1, 1024);
			GridOptions.Rows    = FMath::Clamp(Count[1], 1, 1024);
		}
	}
	else
	{
		int32 Cell[2] = { GridOptions.CellWidth, GridOptions.CellHeight };
		if (ImGui::InputInt2("셀 W, H (px)", Cell))
		{
			GridOptions.CellWidth  = FMath::Clamp(Cell[0], 1, 8192);
			GridOptions.CellHeight = FMath::Clamp(Cell[1], 1, 8192);
		}
	}
	if (ImGui::InputInt("여백 (px)", &GridOptions.Margin))
	{
		GridOptions.Margin = FMath::Clamp(GridOptions.Margin, 0, 4096);
	}
	if (ImGui::InputInt("간격 (px)", &GridOptions.Spacing))
	{
		GridOptions.Spacing = FMath::Clamp(GridOptions.Spacing, 0, 4096);
	}
	DataValueWidgets::InputString("이름 접두사", GridOptions.NamePrefix);
	ImGui::PopItemWidth();
	ImGui::BeginDisabled(!Texture.IsValid());
	ImGui::Checkbox("빈 칸(완전 투명) 건너뛰기", &GridOptions.bSkipEmpty);
	ImGui::EndDisabled();
	ImGui::Checkbox("기존 슬라이스 교체", &bGridReplace);
	ImGui::SetItemTooltip("끄면 기존 슬라이스 뒤에 덧붙인다 (이름이 겹치면 _번호)");

	int32 CellWidth  = 0;
	int32 CellHeight = 0;
	ComputeGridCellSize(Asset.TextureWidth, Asset.TextureHeight, GridOptions, CellWidth, CellHeight);
	const FImageView                Image{ Texture.GetWidth(), Texture.GetHeight(), Texture.Image.Pixels };
	std::vector<FSpriteSlice>       Cells = SliceGridWithOptions(Asset.TextureWidth, Asset.TextureHeight, GridOptions, Image);
	ImGui::TextDisabled("셀 %d x %d px → 슬라이스 %zu개", CellWidth, CellHeight, Cells.size());
	ImGui::BeginDisabled(Cells.empty());
	if (ImGui::Button(bGridReplace ? "자르기 (교체)" : "자르기 (추가)"))
	{
		ApplyGridSlices(Asset, std::move(Cells), bGridReplace);
		Selection.clear();
		Edited("격자로 자르기");
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("취소"))
	{
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

// ---------------------------------------------------------------- 자동 검증

bool FSpriteAtlasEditor::ApplyVerifyEdits(FAssetEditorEnvironment& Env)
{
	(void)Env;
	const auto Step = [this](std::string_view Label) {
		Edited(Label);
		CommitPendingEdit(false);
	};
	// 1) 끌기로 새 슬라이스 (음수 방향 끌기 → 정규화)
	const FPixelRect Created = MakeRectFromDrag(FVector2(9.6f, 9.4f), FVector2(1.2f, 1.4f), Asset.TextureWidth, Asset.TextureHeight);
	SelectOnly(AddSlice(Asset, Created, "Slice"));
	Step("슬라이스 추가");
	// 2) 오른쪽 아래 핸들로 크기 조절 + 이동
	FSpriteSlice& Slice = Asset.Slices[static_cast<size_t>(GetPrimary())];
	SetRect(Slice, ApplyRectDrag(GetRect(Slice), ERectHandle::BottomRight, FVector2(4.4f, 2.6f), Asset.TextureWidth, Asset.TextureHeight));
	Step("슬라이스 크기 조절");
	SetRect(Slice, ApplyRectDrag(GetRect(Slice), ERectHandle::Move, FVector2(3.0f, 1.0f), Asset.TextureWidth, Asset.TextureHeight));
	Step("슬라이스 이동");
	// 3) 이름 바꾸기 (중복 거부 확인) + 피벗 프리셋 + 테두리
	const bool bDuplicateRejected = Asset.Slices.size() < 2 || !RenameSlice(Asset, GetPrimary(), Asset.Slices[0].Name);
	RenameSlice(Asset, GetPrimary(), MakeUniqueName(Asset, "VerifySlice"));
	Step("슬라이스 이름 바꾸기");
	Asset.Slices[static_cast<size_t>(GetPrimary())].Pivot = GetPivotPresets()[1].Pivot;
	Step("피벗 프리셋");
	ApplyBorderDrag(Asset.Slices[static_cast<size_t>(GetPrimary())], 0, FVector2(static_cast<float>(Asset.Slices[static_cast<size_t>(GetPrimary())].X + 2), 0.0f));
	Step("9-슬라이스 테두리");
	// 4) 격자로 자르기 (덧붙임) + 정렬 + 삭제
	FGridSliceOptions Options;
	Options.CellWidth  = 16;
	Options.CellHeight = 16;
	Options.NamePrefix = "VerifyGrid_";
	const FImageView Image{ Texture.GetWidth(), Texture.GetHeight(), Texture.Image.Pixels };
	ApplyGridSlices(Asset, SliceGridWithOptions(Asset.TextureWidth, Asset.TextureHeight, Options, Image), false);
	Step("격자로 자르기");
	SortSlices(Asset, ESliceSort::Name);
	Step("슬라이스 정렬");
	SelectOnly(Asset.FindSlice("VerifySlice"));
	DeleteSelected();
	CommitPendingEdit(false);
	return bDuplicateRejected;
}
