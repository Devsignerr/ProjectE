#include "Editor/AssetEditors/TilesetEditor.h"

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorWidgets.h"
#include "Editor/AssetEditors/DataValueWidgets.h"
#include "Editor/AssetEditors/Sprite2DEditing.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Editor/ImGuiLayer.h"
#include "Physics/Physics2DMath.h"
#include "Scene/Sprite/Sprite2DLibrary.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <sstream>

E_DECLARE_LOG_CATEGORY(LogEditor)

using namespace Sprite2DEditing;

namespace
{
	constexpr ImU32 GridColor      = IM_COL32(255, 255, 255, 60);
	constexpr ImU32 SelectedColor  = IM_COL32(0, 150, 255, 255);
	constexpr ImU32 CollisionFill  = IM_COL32(255, 70, 60, 70);
	constexpr ImU32 CollisionLine  = IM_COL32(255, 90, 80, 230);
	constexpr ImU32 OneWayColor    = IM_COL32(80, 230, 120, 255);
	constexpr ImU32 TagColor       = IM_COL32(255, 230, 120, 255);
	constexpr ImU32 AnimationColor = IM_COL32(150, 200, 255, 255);
	constexpr ImU32 PointColor     = IM_COL32(255, 255, 255, 255);
	constexpr float PointRadius    = 7.0f; // 화면 px

	std::vector<FVector2> MakeRectanglePoints(int32 Width, int32 Height)
	{
		const float W = static_cast<float>(Width);
		const float H = static_cast<float>(Height);
		return { FVector2(0.0f, H), FVector2(W, H), FVector2(W, 0.0f), FVector2(0.0f, 0.0f) };
	}
} // namespace

template <typename TFunction>
void FTilesetEditor::ForEachSelectedTile(TFunction&& Function)
{
	for (const int32 Id : TileSelection)
	{
		FTileDefinition Tile = GetTile(Asset, Id);
		Function(Tile);
		SetTile(Asset, Tile);
	}
}

// ---------------------------------------------------------------- 에셋

bool FTilesetEditor::LoadAsset(FAssetEditorEnvironment& Env)
{
	std::string Text;
	if (!ReadAssetText(Text))
	{
		E_LOG(LogEditor, Error, "타일셋을 읽을 수 없습니다: {}", GetDisplayName());
		return false;
	}
	FTilesetAsset            Loaded;
	std::vector<std::string> Warnings;
	std::string              Error;
	if (!FTilesetAsset::FromJsonString(Text, Loaded, &Warnings, &Error))
	{
		E_LOG(LogEditor, Error, "타일셋 형식 오류 ({}): {}", GetDisplayName(), Error);
		return false;
	}
	for (const std::string& Warning : Warnings)
	{
		E_LOG(LogEditor, Warning, "[2D 편집기] {}: {}", GetDisplayName(), Warning);
	}
	Asset = std::move(Loaded);
	SyncTexture(Env);
	if (Texture.IsValid())
	{
		Asset.TextureWidth  = Texture.GetWidth();
		Asset.TextureHeight = Texture.GetHeight();
	}
	ClampSelection();
	RememberDiskState();
	return true;
}

bool FTilesetEditor::SaveAsset(FAssetEditorEnvironment& Env)
{
	(void)Env;
	std::string Error;
	if (!FSprite2DLibrary::Get().SaveTileset(GetAssetPathString(), Asset, &Error))
	{
		E_LOG(LogEditor, Error, "타일셋 저장 실패: {}", Error);
		return false;
	}
	AfterSaved();
	return true;
}

std::string FTilesetEditor::CaptureState() const
{
	return Asset.ToJsonString();
}

void FTilesetEditor::RestoreState(FAssetEditorEnvironment& Env, const std::string& State)
{
	FTilesetAsset Restored;
	if (FTilesetAsset::FromJsonString(State, Restored))
	{
		Asset = std::move(Restored);
		SyncTexture(Env);
	}
	DraggingPoint = -1;
	ClampSelection();
}

void FTilesetEditor::OnClose(FAssetEditorEnvironment& Env)
{
	ReleaseTexturePreview(Env, Texture);
	FSprite2DEditorBase::OnClose(Env);
}

void FTilesetEditor::Edited(std::string_view Label)
{
	MarkEdited(Label);
}

void FTilesetEditor::SyncTexture(FAssetEditorEnvironment& Env)
{
	UpdateTexturePreview(Env, Texture, ResolveReference(Asset.Texture));
}

void FTilesetEditor::Update(FAssetEditorEnvironment& Env, float DeltaSeconds)
{
	FSprite2DEditorBase::Update(Env, DeltaSeconds);
	AnimTime += DeltaSeconds;
}

bool FTilesetEditor::IsTileSelected(int32 Id) const
{
	return std::find(TileSelection.begin(), TileSelection.end(), Id) != TileSelection.end();
}

void FTilesetEditor::ClampSelection()
{
	const int32 Count = Asset.GetTileCount();
	std::erase_if(TileSelection, [Count](int32 Id) { return Id < 0 || Id >= Count; });
}

int32 FTilesetEditor::FindTileAt(const FVector2& Point) const
{
	const int32 StrideX = Asset.TileWidth + Asset.Spacing;
	const int32 StrideY = Asset.TileHeight + Asset.Spacing;
	if (StrideX <= 0 || StrideY <= 0)
	{
		return -1;
	}
	const int32 Column = static_cast<int32>(std::floor((Point.X - static_cast<float>(Asset.Margin)) / static_cast<float>(StrideX)));
	const int32 Row    = static_cast<int32>(std::floor((Point.Y - static_cast<float>(Asset.Margin)) / static_cast<float>(StrideY)));
	if (Column < 0 || Row < 0 || Column >= Asset.GetColumns() || Row >= Asset.GetRows())
	{
		return -1;
	}
	const int32        Id   = Row * Asset.GetColumns() + Column;
	const FSpriteSlice Rect = Asset.GetTileRect(Id);
	// 간격(Spacing) 위는 타일 아님
	if (Point.X >= static_cast<float>(Rect.X + Rect.W) || Point.Y >= static_cast<float>(Rect.Y + Rect.H))
	{
		return -1;
	}
	return Id;
}

ImVec2 FTilesetEditor::TileUv(int32 Id, bool bMax) const
{
	const FSpriteUvRect Uv = Asset.ComputeTileUv(Id);
	return bMax ? ImVec2(Uv.U1, Uv.V1) : ImVec2(Uv.U0, Uv.V0);
}

// ---------------------------------------------------------------- 캔버스

void FTilesetEditor::DrawPreviewArea(FAssetEditorEnvironment& Env)
{
	DrawExternalChangeBanner(Env);
	SyncTexture(Env);
	if (!bCheckedArgs)
	{
		bCheckedArgs = true;
		if (const std::wstring Select = FCommandLine::FromProcess().GetValue(L"--tileset-select"); !Select.empty())
		{
			std::stringstream Stream(FStringConv::ToUtf8(Select));
			std::string       Item;
			while (std::getline(Stream, Item, ','))
			{
				TileSelection.push_back(std::atoi(Item.c_str()));
			}
			ClampSelection();
		}
	}
	ImGui::Checkbox("충돌", &bShowCollision);
	ImGui::SameLine();
	ImGui::Checkbox("원웨이", &bShowOneWay);
	ImGui::SameLine();
	ImGui::Checkbox(ICON_FA_TAGS " 태그", &bShowTags);
	ImGui::SameLine();
	ImGui::Checkbox(ICON_FA_FILM " 애니메이션", &bShowAnimation);
	ImGui::SameLine();
	ImGui::Checkbox("번호", &bShowIds);
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_EXPAND " 화면 맞춤 (F)"))
	{
		Canvas.Frame(Asset.TextureWidth, Asset.TextureHeight);
	}
	if (Canvas.IsHovered())
	{
		const int32 Hovered = FindTileAt(Canvas.GetMouseImage());
		ImGui::SameLine();
		ImGui::TextDisabled(Hovered >= 0 ? "타일 %d" : "", Hovered);
	}
	DrawCanvas(Env);
}

void FTilesetEditor::DrawCanvas(FAssetEditorEnvironment& Env)
{
	ImGui::PushID("TilesetCanvas");
	Canvas.Begin("##Canvas", ImGui::GetContentRegionAvail(), Asset.TextureWidth, Asset.TextureHeight);
	ImDrawList* DrawList = Canvas.GetDrawList();
	Canvas.DrawImage(GetImTexture(Env, Texture), Asset.TextureWidth, Asset.TextureHeight, Asset.Filter == ESpriteFilter::Point);

	// 입력: 클릭 = 선택, Ctrl = 토글, 끌기 = 상자 선택
	const FVector2 Mouse = Canvas.GetMouseImage();
	if (Canvas.IsLeftClicked())
	{
		bBoxSelecting = true;
		BoxStart      = Mouse;
	}
	if (bBoxSelecting && Canvas.IsLeftReleased())
	{
		bBoxSelecting     = false;
		const bool bCtrl  = ImGui::GetIO().KeyCtrl;
		const bool bClick = (Mouse - BoxStart).Length() * Canvas.GetZoom() < 4.0f;
		if (!bCtrl)
		{
			TileSelection.clear();
		}
		if (bClick)
		{
			const int32 Id = FindTileAt(Mouse);
			if (Id >= 0)
			{
				if (bCtrl && IsTileSelected(Id))
				{
					std::erase(TileSelection, Id);
				}
				else if (!IsTileSelected(Id))
				{
					TileSelection.push_back(Id);
				}
			}
		}
		else
		{
			const float X0 = FMath::Min(BoxStart.X, Mouse.X);
			const float X1 = FMath::Max(BoxStart.X, Mouse.X);
			const float Y0 = FMath::Min(BoxStart.Y, Mouse.Y);
			const float Y1 = FMath::Max(BoxStart.Y, Mouse.Y);
			for (int32 Id = 0; Id < Asset.GetTileCount(); ++Id)
			{
				const FSpriteSlice Rect = Asset.GetTileRect(Id);
				if (static_cast<float>(Rect.X) < X1 && static_cast<float>(Rect.X + Rect.W) > X0 && static_cast<float>(Rect.Y) < Y1 &&
				    static_cast<float>(Rect.Y + Rect.H) > Y0 && !IsTileSelected(Id))
				{
					TileSelection.push_back(Id);
				}
			}
		}
		DraggingPoint = -1;
	}

	// 격자
	for (int32 Id = 0; Id < Asset.GetTileCount(); ++Id)
	{
		const FSpriteSlice Rect = Asset.GetTileRect(Id);
		const ImVec2       Min  = Canvas.ImageToScreen(FVector2(static_cast<float>(Rect.X), static_cast<float>(Rect.Y)));
		const ImVec2       Max  = Canvas.ImageToScreen(FVector2(static_cast<float>(Rect.X + Rect.W), static_cast<float>(Rect.Y + Rect.H)));
		DrawList->AddRect(Min, Max, GridColor);
		if (bShowIds && Max.x - Min.x > 22.0f)
		{
			DrawList->AddText(ImVec2(Min.x + 2.0f, Min.y + 1.0f), IM_COL32(255, 255, 255, 170), std::to_string(Id).c_str());
		}
	}
	for (const FTileDefinition& Tile : Asset.Tiles)
	{
		DrawTileOverlay(Tile);
	}
	for (const int32 Id : TileSelection)
	{
		const FSpriteSlice Rect = Asset.GetTileRect(Id);
		DrawList->AddRect(Canvas.ImageToScreen(FVector2(static_cast<float>(Rect.X), static_cast<float>(Rect.Y))),
		                  Canvas.ImageToScreen(FVector2(static_cast<float>(Rect.X + Rect.W), static_cast<float>(Rect.Y + Rect.H))), SelectedColor, 0.0f, 0, 2.5f);
	}
	if (bBoxSelecting)
	{
		DrawList->AddRect(Canvas.ImageToScreen(BoxStart), Canvas.ImageToScreen(Mouse), IM_COL32(255, 255, 255, 200));
	}
	Canvas.End();
	ImGui::PopID();
}

void FTilesetEditor::DrawTileOverlay(const FTileDefinition& Tile)
{
	ImDrawList*        DrawList = Canvas.GetDrawList();
	const FSpriteSlice Rect     = Asset.GetTileRect(Tile.Id);
	if (Rect.W <= 0)
	{
		return;
	}
	const ImVec2 Min = Canvas.ImageToScreen(FVector2(static_cast<float>(Rect.X), static_cast<float>(Rect.Y)));
	const ImVec2 Max = Canvas.ImageToScreen(FVector2(static_cast<float>(Rect.X + Rect.W), static_cast<float>(Rect.Y + Rect.H)));
	if (bShowCollision)
	{
		if (Tile.Collision == ETileCollision::Full)
		{
			DrawList->AddRectFilled(Min, Max, CollisionFill);
			DrawList->AddRect(Min, Max, CollisionLine);
		}
		else if (Tile.Collision == ETileCollision::Polygon && Tile.Points.size() >= 2)
		{
			std::vector<ImVec2> Points;
			Points.reserve(Tile.Points.size());
			for (const FVector2& Point : Tile.Points)
			{
				Points.push_back(Canvas.ImageToScreen(FVector2(static_cast<float>(Rect.X) + Point.X, static_cast<float>(Rect.Y) + Point.Y)));
			}
			if (Points.size() >= 3)
			{
				DrawList->AddConcavePolyFilled(Points.data(), static_cast<int>(Points.size()), CollisionFill);
			}
			DrawList->AddPolyline(Points.data(), static_cast<int>(Points.size()), CollisionLine, 1.5f, ImDrawFlags_Closed);
		}
	}
	if (bShowOneWay && Tile.bOneWay)
	{
		DrawList->AddLine(ImVec2(Min.x, Min.y + 1.0f), ImVec2(Max.x, Min.y + 1.0f), OneWayColor, 3.0f);
		const float CX = (Min.x + Max.x) * 0.5f;
		const float CY = (Min.y + Max.y) * 0.5f;
		const float S  = FMath::Min(Max.x - Min.x, Max.y - Min.y) * 0.18f;
		DrawList->AddTriangleFilled(ImVec2(CX, CY - S), ImVec2(CX + S, CY + S * 0.6f), ImVec2(CX - S, CY + S * 0.6f), OneWayColor);
	}
	if (bShowTags && !Tile.Tags.empty() && Max.x - Min.x > 24.0f)
	{
		std::string Text = Tile.Tags.front();
		if (Tile.Tags.size() > 1)
		{
			Text += std::format(" +{}", Tile.Tags.size() - 1);
		}
		DrawList->PushClipRect(Min, Max, true);
		DrawList->AddText(ImVec2(Min.x + 2.0f, Max.y - ImGui::GetTextLineHeight() - 1.0f), TagColor, Text.c_str());
		DrawList->PopClipRect();
	}
	if (bShowAnimation && !Tile.Animation.empty())
	{
		DrawList->AddText(ImVec2(Max.x - ImGui::CalcTextSize(ICON_FA_FILM).x - 2.0f, Min.y + 1.0f), AnimationColor, ICON_FA_FILM);
	}
}

// ---------------------------------------------------------------- 속성

void FTilesetEditor::DrawProperties(FAssetEditorEnvironment& Env)
{
	DrawGridSettings(Env);
	DrawTileProperties(Env);
}

void FTilesetEditor::DrawGridSettings(FAssetEditorEnvironment& Env)
{
	ImGui::SeparatorText(ICON_FA_IMAGE " 텍스처 / 격자");
	const std::vector<std::string>& Images =
		FAssetEditorWidgets::ScanImageFiles(Env.Editor != nullptr ? Env.Editor->ContentDirectory : GetAssetDirectory(), GetAssetDirectory());
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
		ClampSelection();
		Canvas.RequestFrame();
		Edited("텍스처 변경");
	}
	if (Texture.bFailed)
	{
		ImGui::TextColored(FEditorTheme::Danger, ICON_FA_TRIANGLE_EXCLAMATION " 이미지를 읽을 수 없습니다");
	}
	ImGui::TextDisabled("크기 %d x %d px (이미지에서 자동)", Asset.TextureWidth, Asset.TextureHeight);
	int32 TileSize[2] = { Asset.TileWidth, Asset.TileHeight };
	if (ImGui::InputInt2("타일 W, H (px)", TileSize))
	{
		Asset.TileWidth  = FMath::Clamp(TileSize[0], 1, 4096);
		Asset.TileHeight = FMath::Clamp(TileSize[1], 1, 4096);
		ClampSelection();
		Edited("타일 크기");
	}
	int32 Layout[2] = { Asset.Margin, Asset.Spacing };
	if (ImGui::InputInt2("여백, 간격 (px)", Layout))
	{
		Asset.Margin  = FMath::Clamp(Layout[0], 0, 4096);
		Asset.Spacing = FMath::Clamp(Layout[1], 0, 4096);
		ClampSelection();
		Edited("여백/간격");
	}
	ImGui::TextDisabled("%d열 x %d행 = 타일 %d개", Asset.GetColumns(), Asset.GetRows(), Asset.GetTileCount());
	if (ImGui::DragFloat("UnitsPerPixel", &Asset.UnitsPerPixel, 0.01f, 0.001f, 1000.0f, "%.3f cm/px"))
	{
		Asset.UnitsPerPixel = FMath::Max(Asset.UnitsPerPixel, 0.001f);
		Edited("UnitsPerPixel");
	}
	int32 Filter = static_cast<int32>(Asset.Filter);
	if (ImGui::Combo("필터", &Filter, "Point (픽셀 아트)\0Linear\0"))
	{
		Asset.Filter = static_cast<ESpriteFilter>(Filter);
		Edited("필터");
	}
}

void FTilesetEditor::DrawTileProperties(FAssetEditorEnvironment& Env)
{
	if (TileSelection.empty())
	{
		ImGui::SeparatorText("타일");
		FAssetEditorWidgets::Hint("캔버스에서 타일을 고르세요 (Ctrl+클릭 = 여러 개, 끌기 = 상자 선택)");
		return;
	}
	const int32           Primary = TileSelection.back();
	const FTileDefinition First   = GetTile(Asset, Primary);
	if (TileSelection.size() == 1)
	{
		ImGui::SeparatorText(std::format("타일 {}", Primary).c_str());
	}
	else
	{
		ImGui::SeparatorText(std::format("타일 {}개 (주: {})", TileSelection.size(), Primary).c_str());
	}

	int32 Collision = static_cast<int32>(First.Collision);
	if (ImGui::Combo("충돌", &Collision, "None\0Full\0Polygon\0"))
	{
		ForEachSelectedTile([this, Collision](FTileDefinition& Tile) {
			Tile.Collision = static_cast<ETileCollision>(Collision);
			if (Tile.Collision == ETileCollision::Polygon && Tile.Points.empty())
			{
				Tile.Points = MakeRectanglePoints(Asset.TileWidth, Asset.TileHeight);
			}
			if (Tile.Collision != ETileCollision::Polygon)
			{
				Tile.Points.clear();
			}
		});
		Edited("타일 충돌");
	}
	bool bOneWay = First.bOneWay;
	if (ImGui::Checkbox("원웨이 (위에서만 막음)", &bOneWay))
	{
		ForEachSelectedTile([bOneWay](FTileDefinition& Tile) { Tile.bOneWay = bOneWay; });
		Edited("원웨이");
	}
	DrawTags();
	if (TileSelection.size() == 1)
	{
		if (First.Collision == ETileCollision::Polygon)
		{
			DrawPolygonEditor(Env, Primary);
		}
		DrawAnimation(Env, Primary);
	}
}

void FTilesetEditor::DrawTags()
{
	ImGui::SeparatorText(ICON_FA_TAGS " 태그");
	// 선택 타일들의 태그와 가진 타일 수
	std::map<std::string, int32> Counts;
	for (const int32 Id : TileSelection)
	{
		for (const std::string& Tag : GetTile(Asset, Id).Tags)
		{
			++Counts[Tag];
		}
	}
	std::string Remove;
	for (const auto& [Tag, Count] : Counts)
	{
		ImGui::PushID(Tag.c_str());
		if (ImGui::SmallButton(ICON_FA_XMARK))
		{
			Remove = Tag;
		}
		ImGui::SameLine();
		if (Count == static_cast<int32>(TileSelection.size()))
		{
			ImGui::TextUnformatted(Tag.c_str());
		}
		else
		{
			ImGui::TextDisabled("%s (%d/%zu)", Tag.c_str(), Count, TileSelection.size());
		}
		ImGui::PopID();
	}
	if (!Remove.empty())
	{
		ForEachSelectedTile([&Remove](FTileDefinition& Tile) { std::erase(Tile.Tags, Remove); });
		Edited("태그 제거");
	}
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
	const bool bEnter = DataValueWidgets::InputString("##NewTag", NewTag, ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	if ((ImGui::SmallButton(ICON_FA_PLUS " 태그 추가") || bEnter) && !NewTag.empty())
	{
		ForEachSelectedTile([this](FTileDefinition& Tile) {
			if (!Tile.HasTag(NewTag))
			{
				Tile.Tags.push_back(NewTag);
			}
		});
		NewTag.clear();
		Edited("태그 추가");
	}
}

void FTilesetEditor::DrawAnimation(FAssetEditorEnvironment& Env, int32 Id)
{
	ImGui::SeparatorText(ICON_FA_FILM " 애니메이션");
	FTileDefinition Tile     = GetTile(Asset, Id);
	bool            bChanged = false;
	int32           Remove   = -1;
	for (size_t Index = 0; Index < Tile.Animation.size(); ++Index)
	{
		FTileAnimFrame& Frame = Tile.Animation[Index];
		ImGui::PushID(static_cast<int>(Index));
		ImGui::SetNextItemWidth(90.0f);
		if (ImGui::InputInt("타일", &Frame.TileId))
		{
			Frame.TileId = FMath::Clamp(Frame.TileId, 0, FMath::Max(Asset.GetTileCount() - 1, 0));
			bChanged     = true;
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90.0f);
		if (ImGui::DragFloat("초", &Frame.Duration, 0.005f, 0.01f, 60.0f, "%.3f"))
		{
			Frame.Duration = FMath::Max(Frame.Duration, 0.01f);
			bChanged       = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_TRASH_CAN))
		{
			Remove = static_cast<int32>(Index);
		}
		ImGui::PopID();
	}
	if (Remove >= 0)
	{
		Tile.Animation.erase(Tile.Animation.begin() + Remove);
		bChanged = true;
	}
	if (ImGui::SmallButton(ICON_FA_PLUS " 프레임 추가"))
	{
		// 처음이면 자기 자신, 아니면 마지막 다음 타일
		const int32 Next = Tile.Animation.empty() ? Id : FMath::Min(Tile.Animation.back().TileId + 1, FMath::Max(Asset.GetTileCount() - 1, 0));
		Tile.Animation.push_back(FTileAnimFrame{ Next, Tile.Animation.empty() ? 0.2f : Tile.Animation.back().Duration });
		bChanged = true;
	}
	if (bChanged)
	{
		SetTile(Asset, Tile);
		Edited("타일 애니메이션");
	}
	// 미리보기 (편집기 시간)
	if (!Tile.Animation.empty())
	{
		float Total = 0.0f;
		for (const FTileAnimFrame& Frame : Tile.Animation)
		{
			Total += Frame.Duration;
		}
		float Local   = Total > 0.0f ? std::fmod(AnimTime, Total) : 0.0f;
		int32 ShownId = Tile.Animation.front().TileId;
		for (const FTileAnimFrame& Frame : Tile.Animation)
		{
			if (Local < Frame.Duration)
			{
				ShownId = Frame.TileId;
				break;
			}
			Local -= Frame.Duration;
		}
		const ImTextureID TextureId = GetImTexture(Env, Texture);
		if (TextureId != ImTextureID{})
		{
			const float  Size = 64.0f;
			const ImVec2 Min  = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(Size, Size));
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			DrawList->AddRectFilled(Min, ImVec2(Min.x + Size, Min.y + Size), IM_COL32(58, 58, 64, 255));
			if (Asset.Filter == ESpriteFilter::Point)
			{
				FImGuiLayer::BeginNearestSampling(DrawList);
			}
			DrawList->AddImage(TextureId, Min, ImVec2(Min.x + Size, Min.y + Size), TileUv(ShownId, false), TileUv(ShownId, true));
			if (Asset.Filter == ESpriteFilter::Point)
			{
				FImGuiLayer::EndNearestSampling(DrawList);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("미리보기: 타일 %d", ShownId);
		}
	}
}

void FTilesetEditor::DrawPolygonEditor(FAssetEditorEnvironment& Env, int32 Id)
{
	ImGui::SeparatorText(ICON_FA_DRAW_POLYGON " 충돌 다각형");
	FTileDefinition Tile = GetTile(Asset, Id);
	ImGui::Checkbox("픽셀 스냅", &bSnapPoints);
	ImGui::SameLine();
	if (ImGui::SmallButton("사각형으로"))
	{
		Tile.Points = MakeRectanglePoints(Asset.TileWidth, Asset.TileHeight);
		SetTile(Asset, Tile);
		Edited("다각형 초기화");
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("볼록 껍질로 정리"))
	{
		std::vector<FVector2> Hull = Physics2DMath::ReduceConvexPolygon(Physics2DMath::ComputeConvexHull(Tile.Points), MaxPhysicsPolygonPoints);
		if (Hull.size() >= 3)
		{
			Tile.Points = std::move(Hull);
			SetTile(Asset, Tile);
			Edited("다각형 볼록 껍질");
		}
	}
	const std::string Warning = GetPolygonWarning(Tile.Points);
	if (!Warning.empty())
	{
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " %s", Warning.c_str());
	}

	const float Side = FMath::Clamp(ImGui::GetContentRegionAvail().x, 120.0f, 320.0f);
	ImGui::PushID("PolygonCanvas");
	TileCanvas.Begin("##TileCanvas", ImVec2(Side, Side), Asset.TileWidth, Asset.TileHeight);
	TileCanvas.DrawImage(GetImTexture(Env, Texture), Asset.TileWidth, Asset.TileHeight, Asset.Filter == ESpriteFilter::Point, TileUv(Id, false), TileUv(Id, true));
	TileCanvas.DrawPixelGrid(Asset.TileWidth, Asset.TileHeight);

	const FVector2 Mouse  = TileCanvas.GetMouseImage();
	const float    Radius = TileCanvas.ScreenToImageDistance(PointRadius);
	bool           bEdit  = false;
	const char*    Label  = "";
	if (TileCanvas.IsLeftClicked())
	{
		DraggingPoint = FindNearestPoint(Tile.Points, Mouse, Radius);
		if (DraggingPoint < 0)
		{
			DraggingPoint = InsertPointOnNearestEdge(Tile.Points, SnapTilePoint(Mouse, Asset.TileWidth, Asset.TileHeight, bSnapPoints));
			bEdit         = true;
			Label         = "다각형 점 추가";
		}
	}
	if (TileCanvas.IsLeftDown() && DraggingPoint >= 0 && DraggingPoint < static_cast<int32>(Tile.Points.size()))
	{
		const FVector2 Snapped = SnapTilePoint(Mouse, Asset.TileWidth, Asset.TileHeight, bSnapPoints);
		if (!(Snapped == Tile.Points[static_cast<size_t>(DraggingPoint)]))
		{
			Tile.Points[static_cast<size_t>(DraggingPoint)] = Snapped;
			bEdit                                          = true;
			Label                                          = "다각형 점 이동";
		}
	}
	if (TileCanvas.IsLeftReleased())
	{
		DraggingPoint = -1;
	}
	if (TileCanvas.IsRightClickedWithoutDrag())
	{
		const int32 Hit = FindNearestPoint(Tile.Points, Mouse, Radius);
		if (Hit >= 0)
		{
			Tile.Points.erase(Tile.Points.begin() + Hit);
			bEdit = true;
			Label = "다각형 점 삭제";
		}
	}
	if (bEdit)
	{
		SetTile(Asset, Tile);
		Edited(Label);
	}

	ImDrawList*         DrawList = TileCanvas.GetDrawList();
	std::vector<ImVec2> Points;
	for (const FVector2& Point : Tile.Points)
	{
		Points.push_back(TileCanvas.ImageToScreen(Point));
	}
	if (Points.size() >= 3)
	{
		DrawList->AddConcavePolyFilled(Points.data(), static_cast<int>(Points.size()), CollisionFill);
	}
	if (Points.size() >= 2)
	{
		DrawList->AddPolyline(Points.data(), static_cast<int>(Points.size()), CollisionLine, 2.0f, ImDrawFlags_Closed);
	}
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		const bool bActive = static_cast<int32>(Index) == DraggingPoint;
		DrawList->AddCircleFilled(Points[Index], bActive ? 6.0f : 4.5f, bActive ? SelectedColor : PointColor);
		DrawList->AddText(ImVec2(Points[Index].x + 6.0f, Points[Index].y - 16.0f), IM_COL32(255, 255, 255, 200), std::to_string(Index).c_str());
	}
	TileCanvas.End();
	ImGui::PopID();
	FAssetEditorWidgets::Hint("빈 곳 클릭 = 가까운 변에 점 추가, 점 끌기 = 이동, 오른쪽 클릭 = 삭제 (가운데 끌기 = 화면 이동, 휠 = 확대)");
}

// ---------------------------------------------------------------- 자동 검증

bool FTilesetEditor::ApplyVerifyEdits(FAssetEditorEnvironment& Env)
{
	(void)Env;
	const auto Step = [this](std::string_view Label) {
		Edited(Label);
		CommitPendingEdit(false);
	};
	const int32 Count = Asset.GetTileCount();
	if (Count < 2)
	{
		return false;
	}
	const int32 Target = Count - 1;
	// 1) 여러 타일에 충돌 Full + 태그
	TileSelection = { 0, Target };
	ForEachSelectedTile([](FTileDefinition& Tile) { Tile.Collision = ETileCollision::Full; });
	Step("타일 충돌");
	ForEachSelectedTile([](FTileDefinition& Tile) { Tile.Tags.push_back("VerifyTag"); });
	Step("태그 추가");
	// 2) 다각형: 사각형 → 점 추가(오목) → 이동 → 삭제
	TileSelection = { Target };
	FTileDefinition Tile = GetTile(Asset, Target);
	Tile.Collision       = ETileCollision::Polygon;
	Tile.Points          = MakeRectanglePoints(Asset.TileWidth, Asset.TileHeight);
	SetTile(Asset, Tile);
	Step("다각형 초기화");
	const int32 Inserted = InsertPointOnNearestEdge(Tile.Points, SnapTilePoint(FVector2(static_cast<float>(Asset.TileWidth) * 0.5f, 2.2f), Asset.TileWidth, Asset.TileHeight, true));
	SetTile(Asset, Tile);
	Step("다각형 점 추가");
	const bool bConcaveWarned = !GetPolygonWarning(Tile.Points).empty();
	Tile.Points[static_cast<size_t>(Inserted)] = SnapTilePoint(FVector2(static_cast<float>(Asset.TileWidth) * 0.5f, -3.0f), Asset.TileWidth, Asset.TileHeight, true);
	SetTile(Asset, Tile);
	Step("다각형 점 이동");
	Tile.Points.erase(Tile.Points.begin() + Inserted);
	SetTile(Asset, Tile);
	Step("다각형 점 삭제");
	// 3) 원웨이 + 애니메이션 + 간격
	Tile         = GetTile(Asset, Target);
	Tile.bOneWay = true;
	Tile.Animation = { FTileAnimFrame{ Target, 0.25f }, FTileAnimFrame{ 0, 0.25f } };
	SetTile(Asset, Tile);
	Step("타일 애니메이션");
	Asset.Spacing += 1;
	Step("여백/간격");
	return bConcaveWarned;
}
