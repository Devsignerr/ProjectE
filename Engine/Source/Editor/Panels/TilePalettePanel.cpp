#include <imgui.h>

#include "Editor/Panels/TilePalettePanel.h"

#include "Core/Input.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"
#include "Editor/Editor2D/Editor2DScene.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/Camera.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/SpriteDraw.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/Sprite2DLibrary.h"
#include "Scene/Sprite/TilemapCollision.h"
#include "Scene/Sprite/TilesetAsset.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace
{
	constexpr size_t GMaxPreviewCells = 4096; // 미리보기 고스트 상한 (넘으면 외곽선만)

	const char* GetToolName(FTilePalettePanel::ETool Tool)
	{
		switch (Tool)
		{
		case FTilePalettePanel::ETool::Brush:  return "브러시";
		case FTilePalettePanel::ETool::Eraser: return "지우개";
		case FTilePalettePanel::ETool::Rect:   return "사각형 채우기";
		case FTilePalettePanel::ETool::Flood:  return "흐름 채우기";
		case FTilePalettePanel::ETool::Line:   return "선";
		case FTilePalettePanel::ETool::Picker: return "스포이드";
		default:                               return "선택";
		}
	}

	// 타일셋 텍스처 (렌더러 수집기와 같은 용도: Point = 픽셀 아트 무압축, Linear = 색). 경로 캐시 적중이면 대기 없음
	FTextureHandle LoadTilesetTexture(FEditorContext& Context, const std::string& TilesetPath, const FTilesetAsset& Tileset)
	{
		const std::string ContentPath = FSprite2DLibrary::ResolveReference(TilesetPath, Tileset.Texture);
		if (ContentPath.empty() || Context.Resources == nullptr)
		{
			return FTextureHandle();
		}
		std::filesystem::path Path(FStringConv::ToWide(ContentPath));
		if (Path.is_relative())
		{
			Path = Context.ContentDirectory / Path;
		}
		return Context.Resources->LoadTexture(Path, Tileset.Filter == ESpriteFilter::Point ? ETextureUsage::PixelArt : ETextureUsage::Color);
	}

	// 뷰포트 오버레이 투영 (원근이면 카메라 뒤 점은 버린다)
	bool ProjectToImage(const FMatrix4x4& ViewProjection, const FVector2& ImageMin, const FVector2& ImageSize, const FVector3& Point, ImVec2& Out)
	{
		const FVector4 Clip = ViewProjection.TransformVector4(FVector4(Point, 1.0f));
		if (Clip.W <= 1.0e-4f)
		{
			return false;
		}
		Out = ImVec2(ImageMin.X + (Clip.X / Clip.W * 0.5f + 0.5f) * ImageSize.X, ImageMin.Y + (0.5f - Clip.Y / Clip.W * 0.5f) * ImageSize.Y);
		return true;
	}
} // namespace

FEntity FTilePalettePanel::FindTarget(const FEditorContext& Context)
{
	if (Context.Scene == nullptr)
	{
		return NullEntity;
	}
	const FRegistry& Registry = Context.Scene->GetRegistry();
	if (Registry.IsValid(Context.SelectedEntity) && Registry.Has<FTilemapComponent>(Context.SelectedEntity))
	{
		return Context.SelectedEntity;
	}
	for (const FEntity Entity : Context.Selection.GetEntities())
	{
		if (Registry.IsValid(Entity) && Registry.Has<FTilemapComponent>(Entity))
		{
			return Entity;
		}
	}
	return NullEntity;
}

Editor2DMath::FTileStamp FTilePalettePanel::GetEffectiveStamp() const
{
	return Editor2DMath::TransformStamp(BaseStamp, TransformFlags);
}

FTilemapComponent* FTilePalettePanel::GetStrokeTilemap(FEditorContext& Context) const
{
	FRegistry& Registry = Context.Scene->GetRegistry();
	if (!bStroking || !Registry.IsValid(StrokeEntity))
	{
		return nullptr;
	}
	return Registry.TryGet<FTilemapComponent>(StrokeEntity);
}

bool FTilePalettePanel::BeginStroke(FEditorContext& Context, FEntity Target)
{
	if (bStroking)
	{
		EndStroke(Context);
	}
	FRegistry& Registry = Context.Scene->GetRegistry();
	if (Context.bPlaying || !Registry.IsValid(Target) || !Registry.Has<FTilemapComponent>(Target))
	{
		return false;
	}
	Sprite2DRuntime::GetTilemapData(Registry.Get<FTilemapComponent>(Target)); // 지금 TileData로 디코딩해 둔다
	bStroking     = true;
	StrokeEntity  = Target;
	StrokeChanged  = 0;
	bStrokeHasLast = false;
	return true;
}

void FTilePalettePanel::StrokeTo(FEditorContext& Context, const FTileCoord& Cell, bool bErase)
{
	FTilemapComponent* Tilemap = GetStrokeTilemap(Context);
	if (Tilemap == nullptr)
	{
		return;
	}
	const Editor2DMath::FTileStamp Stamp = GetEffectiveStamp();
	// 첫 칸은 그 자리만, 이후는 직전 칸에서 선으로 (빠른 드래그에도 틈 없이)
	const FTileCoord From    = bStrokeHasLast ? StrokeLast : Cell;
	const int32      Changed = Editor2DMath::LineStamp(Tilemap->Runtime.Data, From, Cell, Stamp, bErase);
	StrokeLast               = Cell;
	bStrokeHasLast           = true;
	if (Changed > 0)
	{
		StrokeChanged += Changed;
		Sprite2DRuntime::MarkTilemapEdited(*Tilemap); // 렌더러·충돌은 다음 갱신에 반영, TileData 커밋은 스트로크 끝
	}
}

int32 FTilePalettePanel::ApplyRect(FEditorContext& Context, const FTileRect& Rect, bool bErase)
{
	FTilemapComponent* Tilemap = GetStrokeTilemap(Context);
	if (Tilemap == nullptr)
	{
		return 0;
	}
	const int32 Changed = Editor2DMath::FillRectPattern(Tilemap->Runtime.Data, Rect, GetEffectiveStamp(), bErase);
	if (Changed > 0)
	{
		StrokeChanged += Changed;
		Sprite2DRuntime::MarkTilemapEdited(*Tilemap);
	}
	return Changed;
}

int32 FTilePalettePanel::ApplyLine(FEditorContext& Context, const FTileCoord& From, const FTileCoord& To, bool bErase)
{
	FTilemapComponent* Tilemap = GetStrokeTilemap(Context);
	if (Tilemap == nullptr)
	{
		return 0;
	}
	const int32 Changed = Editor2DMath::LineStamp(Tilemap->Runtime.Data, From, To, GetEffectiveStamp(), bErase);
	if (Changed > 0)
	{
		StrokeChanged += Changed;
		Sprite2DRuntime::MarkTilemapEdited(*Tilemap);
	}
	return Changed;
}

int32 FTilePalettePanel::ApplyFlood(FEditorContext& Context, const FTileCoord& Start, bool bErase)
{
	FTilemapComponent* Tilemap = GetStrokeTilemap(Context);
	if (Tilemap == nullptr)
	{
		return 0;
	}
	FTilemapData&   Data    = Tilemap->Runtime.Data;
	const FTileRect Limit   = Editor2DMath::ComputeFloodLimit(Data, Start);
	const int32     Changed = Editor2DMath::FloodFillPattern(Data, Start, GetEffectiveStamp(), bErase, Limit);
	if (Changed > 0)
	{
		StrokeChanged += Changed;
		Sprite2DRuntime::MarkTilemapEdited(*Tilemap);
	}
	return Changed;
}

void FTilePalettePanel::EndStroke(FEditorContext& Context)
{
	FTilemapComponent* Tilemap = GetStrokeTilemap(Context);
	if (Tilemap != nullptr && StrokeChanged > 0)
	{
		// 스트로크 끝에만: TileData(저장·Undo 대상 문자열)로 커밋 → 씬 스냅샷 한 단계
		Sprite2DRuntime::CommitTilemapData(*Tilemap);
		Context.MarkEdited("타일 칠하기");
	}
	bStroking     = false;
	StrokeEntity  = NullEntity;
	StrokeChanged  = 0;
	bStrokeHasLast = false;
}

bool FTilePalettePanel::PickCell(FEditorContext& Context, FEntity Target, const FTileCoord& Cell)
{
	FRegistry& Registry = Context.Scene->GetRegistry();
	if (!Registry.IsValid(Target) || !Registry.Has<FTilemapComponent>(Target))
	{
		return false;
	}
	FTilemapComponent& Tilemap = Registry.Get<FTilemapComponent>(Target);
	const uint32       Value   = Sprite2DRuntime::GetTilemapData(Tilemap).Get(Cell.X, Cell.Y);
	if (TileCell::IsEmpty(Value))
	{
		return false;
	}
	const int32 TileId = TileCell::GetTileId(Value);
	BaseStamp          = Editor2DMath::FTileStamp::Single(TileCell::Make(TileId));
	TransformFlags     = TileCell::GetFlags(Value);
	if (const std::shared_ptr<const FTilesetAsset> Tileset = Sprite2DRuntime::ResolveTileset(Tilemap); Tileset != nullptr && Tileset->GetColumns() > 0)
	{
		SelectCol0 = SelectCol1 = TileId % Tileset->GetColumns();
		SelectRow0 = SelectRow1 = TileId / Tileset->GetColumns();
	}
	return true;
}

void FTilePalettePanel::HandleShortcuts()
{
	const ImGuiIO& IO = ImGui::GetIO();
	if (IO.KeyCtrl || IO.WantTextInput || ImGui::IsMouseDown(ImGuiMouseButton_Right))
	{
		return;
	}
	const auto SetPaintTool = [&](ETool NewTool) {
		Tool = NewTool;
		if (NewTool != ETool::Select && NewTool != ETool::Picker)
		{
			LastPaintTool = NewTool;
		}
	};
	if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) SetPaintTool(ETool::Select);
	if (ImGui::IsKeyPressed(ImGuiKey_B, false)) SetPaintTool(ETool::Brush);
	if (ImGui::IsKeyPressed(ImGuiKey_D, false)) SetPaintTool(ETool::Eraser);
	if (ImGui::IsKeyPressed(ImGuiKey_U, false)) SetPaintTool(ETool::Rect);
	if (ImGui::IsKeyPressed(ImGuiKey_G, false)) SetPaintTool(ETool::Flood);
	if (ImGui::IsKeyPressed(ImGuiKey_L, false)) SetPaintTool(ETool::Line);
	if (ImGui::IsKeyPressed(ImGuiKey_I, false)) SetPaintTool(ETool::Picker);
	if (ImGui::IsKeyPressed(ImGuiKey_X, false)) TransformFlags ^= TileCell::FlipXBit;
	if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) TransformFlags ^= TileCell::FlipYBit;
	if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) TransformFlags ^= TileCell::Rotate90Bit;
}

bool FTilePalettePanel::HandleViewport(FEditorContext& Context, const FInput& Input, const FVector2& ImageMin, const FVector2& ImageSize, bool bHovered)
{
	bHasCell                = false;
	const FEntity  Target   = FindTarget(Context);
	FRegistry&     Registry = Context.Scene->GetRegistry();
	if (!IsActive() || Context.bPlaying || !Registry.IsValid(Target))
	{
		if (bStroking)
		{
			EndStroke(Context);
		}
		return false;
	}
	if (bHovered)
	{
		HandleShortcuts();
		if (!IsActive())
		{
			return false; // Q로 방금 껐다
		}
	}

	// 커서 셀
	FTilemapComponent& Tilemap = Registry.Get<FTilemapComponent>(Target);
	FVector2           CellSize;
	const ImVec2       Mouse = ImGui::GetMousePos();
	const FVector2     Pixel(Mouse.x - ImageMin.X, Mouse.y - ImageMin.Y);
	if (bHovered && Editor2DScene::GetTilemapCellSize(Tilemap, CellSize))
	{
		const FRay Ray = Editor2DScene::MakeRay(Context.Camera->GetViewProjectionMatrix(), Pixel, ImageSize);
		FVector2   Local;
		float      Distance = 0.0f;
		if (Editor2DScene::RayToEntityPlane(Context.Scene->GetTransform(Target).WorldMatrix, Ray, Local, Distance))
		{
			bHasCell  = true;
			HoverCell = TilemapMath::LocalToCell(Local, CellSize);
		}
	}
	else if (!bHovered && bAutomationCell)
	{
		bHasCell  = true;
		HoverCell = AutomationCell;
	}

	const ImGuiIO& IO          = ImGui::GetIO();
	const bool     bCameraDrag = Input.IsMouseButtonDown(EMouseButton::Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
	const ETool    Effective   = (IO.KeyAlt && (Tool == ETool::Brush || Tool == ETool::Rect || Tool == ETool::Line || Tool == ETool::Flood)) ? ETool::Picker : Tool;
	const bool     bErase      = Effective == ETool::Eraser || (IO.KeyShift && Effective != ETool::Picker);

	if (!bStroking && bHovered && bHasCell && !bCameraDrag && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
	{
		switch (Effective)
		{
		case ETool::Picker:
			if (PickCell(Context, Target, HoverCell) && Tool == ETool::Picker)
			{
				Tool = LastPaintTool; // 집은 뒤 바로 칠하기로 돌아간다
			}
			break;
		case ETool::Flood:
			if (BeginStroke(Context, Target))
			{
				if (ApplyFlood(Context, HoverCell, bErase) < 0 && Context.Notify)
				{
					Context.Notify("흐름 채우기 영역이 너무 큽니다 (칠한 영역 + 8칸 안, 최대 65536칸)", true);
				}
				EndStroke(Context);
			}
			break;
		case ETool::Brush:
		case ETool::Eraser:
			if (BeginStroke(Context, Target))
			{
				StrokeTool   = Effective;
				bStrokeErase = bErase;
				StrokeTo(Context, HoverCell, bErase);
			}
			break;
		case ETool::Rect:
		case ETool::Line:
			if (BeginStroke(Context, Target))
			{
				StrokeTool   = Effective;
				bStrokeErase = bErase;
				StrokeStart  = HoverCell;
				StrokeLast   = HoverCell;
			}
			break;
		default:
			break;
		}
	}
	else if (bStroking)
	{
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (bHasCell && !(HoverCell == StrokeLast))
			{
				if (StrokeTool == ETool::Brush || StrokeTool == ETool::Eraser)
				{
					StrokeTo(Context, HoverCell, bStrokeErase);
				}
				else
				{
					StrokeLast = HoverCell;
				}
			}
		}
		else
		{
			if (StrokeTool == ETool::Rect)
			{
				ApplyRect(Context, FTileRect::FromCorners(StrokeStart.X, StrokeStart.Y, StrokeLast.X, StrokeLast.Y), bStrokeErase);
			}
			else if (StrokeTool == ETool::Line)
			{
				ApplyLine(Context, StrokeStart, StrokeLast, bStrokeErase);
			}
			EndStroke(Context);
		}
	}

	DrawPreview(Context, Target, ImageMin, ImageSize);
	return bStroking || bHovered;
}

void FTilePalettePanel::DrawPreview(FEditorContext& Context, FEntity Target, const FVector2& ImageMin, const FVector2& ImageSize)
{
	FRegistry&         Registry = Context.Scene->GetRegistry();
	FTilemapComponent& Tilemap  = Registry.Get<FTilemapComponent>(Target);
	FVector2           CellSize;
	if (!Editor2DScene::GetTilemapCellSize(Tilemap, CellSize) || (!bHasCell && !bStroking))
	{
		return;
	}
	const FMatrix4x4& World = Context.Scene->GetTransform(Target).WorldMatrix;
	const ImGuiIO&    IO    = ImGui::GetIO();
	const ETool       Effective = bStroking ? StrokeTool : ((IO.KeyAlt && Tool != ETool::Eraser) ? ETool::Picker : Tool);
	const bool        bErase    = bStroking ? bStrokeErase : (Effective == ETool::Eraser || (IO.KeyShift && Effective != ETool::Picker));
	const Editor2DMath::FTileStamp Stamp = GetEffectiveStamp();

	// 칠할 셀 목록 (셀, 값)
	std::vector<std::pair<FTileCoord, uint32>> Cells;
	FTileRect                                  Outline;
	auto AddStampAt = [&](const FTileCoord& Cursor) {
		const FTileCoord Origin = Editor2DMath::GetStampOrigin(Stamp, Cursor);
		for (int32 Y = 0; Y < Stamp.Height && Cells.size() < GMaxPreviewCells; ++Y)
		{
			for (int32 X = 0; X < Stamp.Width; ++X)
			{
				if (const uint32 Cell = Stamp.Get(X, Y); !TileCell::IsEmpty(Cell))
				{
					Cells.push_back({ FTileCoord{ Origin.X + X, Origin.Y + Y }, Cell });
				}
			}
		}
	};
	switch (Effective)
	{
	case ETool::Brush:
	case ETool::Eraser:
		if (bHasCell)
		{
			AddStampAt(HoverCell);
		}
		break;
	case ETool::Rect:
		if (bStroking)
		{
			Outline = FTileRect::FromCorners(StrokeStart.X, StrokeStart.Y, StrokeLast.X, StrokeLast.Y);
			if (static_cast<size_t>(Outline.GetWidth()) * static_cast<size_t>(Outline.GetHeight()) <= GMaxPreviewCells)
			{
				for (int32 Y = Outline.MinY; Y <= Outline.MaxY; ++Y)
				{
					for (int32 X = Outline.MinX; X <= Outline.MaxX; ++X)
					{
						const uint32 Cell = Editor2DMath::SamplePattern(Stamp, X, Y, FTileCoord{ Outline.MinX, Outline.MinY });
						if (bErase || !TileCell::IsEmpty(Cell))
						{
							Cells.push_back({ FTileCoord{ X, Y }, Cell });
						}
					}
				}
			}
		}
		else if (bHasCell)
		{
			AddStampAt(HoverCell);
		}
		break;
	case ETool::Line:
		if (bStroking)
		{
			for (const FTileCoord& Cell : TilemapMath::LineCells(StrokeStart.X, StrokeStart.Y, StrokeLast.X, StrokeLast.Y))
			{
				AddStampAt(Cell);
			}
		}
		else if (bHasCell)
		{
			AddStampAt(HoverCell);
		}
		break;
	default: // 흐름 채우기·스포이드: 커서 칸만 외곽선
		if (bHasCell)
		{
			Outline = FTileRect{ HoverCell.X, HoverCell.Y, HoverCell.X, HoverCell.Y };
		}
		break;
	}

	// 칸 외곽선 (ImGui)
	const FMatrix4x4 ViewProjection = Context.Camera->GetViewProjectionMatrix();
	ImDrawList*      DrawList       = ImGui::GetWindowDrawList();
	const ImU32      Color          = ImGui::ColorConvertFloat4ToU32(bErase ? FEditorTheme::Danger : FEditorTheme::Accent);
	auto DrawCellRect = [&](int32 MinX, int32 MinY, int32 MaxX, int32 MaxY) {
		const FVector2 A = TilemapMath::CellToLocal(MinX, MinY, CellSize);
		const FVector2 B = TilemapMath::CellToLocal(MaxX + 1, MaxY + 1, CellSize);
		const FVector3 Corners[4] = { World.TransformPosition(FVector3(A.X, 0.0f, A.Y)), World.TransformPosition(FVector3(B.X, 0.0f, A.Y)),
		                              World.TransformPosition(FVector3(B.X, 0.0f, B.Y)), World.TransformPosition(FVector3(A.X, 0.0f, B.Y)) };
		ImVec2 Screen[4];
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (!ProjectToImage(ViewProjection, ImageMin, ImageSize, Corners[Index], Screen[Index]))
			{
				return;
			}
		}
		DrawList->AddPolyline(Screen, 4, Color, ImDrawFlags_Closed, 1.5f);
	};
	DrawList->PushClipRect(ImVec2(ImageMin.X, ImageMin.Y), ImVec2(ImageMin.X + ImageSize.X, ImageMin.Y + ImageSize.Y), true);
	if (Outline.IsValid())
	{
		DrawCellRect(Outline.MinX, Outline.MinY, Outline.MaxX, Outline.MaxY);
	}
	if (Cells.size() <= 256)
	{
		for (const auto& [Coord, Value] : Cells)
		{
			DrawCellRect(Coord.X, Coord.Y, Coord.X, Coord.Y);
		}
	}
	DrawList->PopClipRect();

	// 반투명 실제 타일 (지우기는 외곽선만)
	const std::shared_ptr<const FTilesetAsset> Tileset = Sprite2DRuntime::ResolveTileset(Tilemap);
	if (bErase || Tileset == nullptr || Cells.empty() || Context.Renderer == nullptr)
	{
		return;
	}
	const FTextureHandle Texture = LoadTilesetTexture(Context, Tilemap.Tileset, *Tileset);
	const FVector3       AxisX   = World.GetAxisX();
	const FVector3       AxisY   = World.GetAxisY();
	const FVector3       AxisZ   = World.GetAxisZ();
	std::vector<FSpriteDrawItem> Items;
	Items.reserve(Cells.size());
	for (const auto& [Coord, Value] : Cells)
	{
		const int32         TileId = TileCell::GetTileId(Value);
		const FSpriteUvRect Uv     = Tileset->ComputeTileUv(TileId);
		// 셀 변환(Rotate90 → FlipX → FlipY)을 사각형 축에 싣는다: 로컬 (u, v)의 텍셀이 셀 가운데 + T(u, v) × 셀 크기에 놓인다 (SpriteTiles와 같은 식)
		const uint32   Flags = TileCell::GetFlags(Value);
		const FVector2 TU    = TilemapMath::TransformTileUv(FVector2(1.0f, 0.0f), Flags);
		const FVector2 TV    = TilemapMath::TransformTileUv(FVector2(0.0f, 1.0f), Flags);
		const FVector2 Center = TilemapMath::CellCenterToLocal(Coord.X, Coord.Y, CellSize);
		FSpriteDrawItem Item;
		const FVector3 Row0 = AxisX * (TU.X * CellSize.X) + AxisZ * (TU.Y * CellSize.Y);
		const FVector3 Row2 = AxisX * (TV.X * CellSize.X) + AxisZ * (TV.Y * CellSize.Y);
		const FVector3 Origin = World.TransformPosition(FVector3(Center.X, 0.0f, Center.Y));
		Item.World            = FMatrix4x4::Identity;
		Item.World.M[0][0] = Row0.X; Item.World.M[0][1] = Row0.Y; Item.World.M[0][2] = Row0.Z;
		Item.World.M[1][0] = AxisY.X; Item.World.M[1][1] = AxisY.Y; Item.World.M[1][2] = AxisY.Z;
		Item.World.M[2][0] = Row2.X; Item.World.M[2][1] = Row2.Y; Item.World.M[2][2] = Row2.Z;
		Item.World.SetOrigin(Origin);
		Item.Size         = FVector2(1.0f, 1.0f);
		Item.Pivot        = FVector2(0.5f, 0.5f);
		Item.UVMin        = FVector2(Uv.U0, Uv.V0);
		Item.UVMax        = FVector2(Uv.U1, Uv.V1);
		Item.Texture      = Texture;
		Item.Color        = FVector4(1.0f, 1.0f, 1.0f, 0.6f);
		Item.SortLayer    = std::numeric_limits<int32>::max() / 2; // 모든 레이어 위
		Item.OrderInLayer = 0;
		Item.Filter       = Tileset->Filter;
		Items.push_back(Item);
	}
	Context.Renderer->SetSpriteDrawList(std::move(Items));
	bPreviewSet       = true;
	bPreviewThisFrame = true;
}

void FTilePalettePanel::FinishFrame(FEditorContext& Context)
{
	if (bPreviewSet && !bPreviewThisFrame && Context.Renderer != nullptr)
	{
		Context.Renderer->ClearSpriteDrawList();
		bPreviewSet = false;
	}
	bPreviewThisFrame = false;
}

void FTilePalettePanel::DrawToolbar()
{
	struct FToolInfo
	{
		ETool       Tool;
		const char* Icon;
		const char* Tooltip;
	};
	static constexpr FToolInfo Tools[] = {
		{ ETool::Select, ICON_FA_ARROW_POINTER, "선택 (Q) — 칠하기 끔, 뷰포트 클릭 = 엔티티 선택" },
		{ ETool::Brush, ICON_FA_PAINTBRUSH, "브러시 (B) — 스탬프 칠하기, Shift = 지우기, Alt = 스포이드" },
		{ ETool::Eraser, ICON_FA_ERASER, "지우개 (D) — 스탬프 모양만큼 지우기" },
		{ ETool::Rect, ICON_FA_VECTOR_SQUARE, "사각형 채우기 (U) — 끌어서 사각형, 스탬프 반복" },
		{ ETool::Flood, ICON_FA_FILL_DRIP, "흐름 채우기 (G) — 같은 칸 연결 영역 (칠한 영역 + 8칸 안)" },
		{ ETool::Line, ICON_FA_SLASH, "선 (L) — 끌어서 직선" },
		{ ETool::Picker, ICON_FA_EYE_DROPPER, "스포이드 (I) — 칸의 타일·회전/반전 집기" },
	};
	for (const FToolInfo& Info : Tools)
	{
		if (FEditorTheme::ToolButton(Info.Icon, Info.Tooltip, Tool == Info.Tool))
		{
			Tool = Info.Tool;
			if (Info.Tool != ETool::Select && Info.Tool != ETool::Picker)
			{
				LastPaintTool = Info.Tool;
			}
		}
		ImGui::SameLine();
	}
	ImGui::TextDisabled("|");
	ImGui::SameLine();
	struct FFlagInfo
	{
		uint32      Bit;
		const char* Icon;
		const char* Tooltip;
	};
	static constexpr FFlagInfo Flags[] = {
		{ TileCell::FlipXBit, ICON_FA_ARROWS_LEFT_RIGHT, "좌우 반전 (X)" },
		{ TileCell::FlipYBit, ICON_FA_ARROWS_UP_DOWN, "상하 반전 (Y)" },
		{ TileCell::Rotate90Bit, ICON_FA_ROTATE_RIGHT, "반시계 90도 회전 (Z) — 변환 순서: 회전 → 좌우 → 상하" },
	};
	for (const FFlagInfo& Info : Flags)
	{
		if (FEditorTheme::ToolButton(Info.Icon, Info.Tooltip, (TransformFlags & Info.Bit) != 0))
		{
			TransformFlags ^= Info.Bit;
		}
		ImGui::SameLine();
	}
	ImGui::NewLine();
}

void FTilePalettePanel::DrawPalette(FEditorContext& Context, FEntity Target)
{
	FTilemapComponent&                         Tilemap = Context.Scene->GetRegistry().Get<FTilemapComponent>(Target);
	const std::shared_ptr<const FTilesetAsset> Tileset = Sprite2DRuntime::ResolveTileset(Tilemap);
	if (Tileset == nullptr)
	{
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 타일셋을 읽을 수 없습니다: %s", Tilemap.Tileset.empty() ? "(비어 있음)" : Tilemap.Tileset.c_str());
		ImGui::TextWrapped("인스펙터에서 TilemapComponent.Tileset에 .etileset을 지정하세요.");
		return;
	}
	const int32 Columns = Tileset->GetColumns();
	const int32 Rows    = Tileset->GetRows();

	const FTextureHandle Handle  = LoadTilesetTexture(Context, Tilemap.Tileset, *Tileset);
	const FD3D12Texture* Texture = Context.Resources != nullptr ? Context.Resources->GetTexture(Handle) : nullptr;
	const float TextureWidth  = static_cast<float>(Tileset->TextureWidth > 0 ? Tileset->TextureWidth : (Texture != nullptr ? static_cast<int32>(Texture->GetWidth()) : 0));
	const float TextureHeight = static_cast<float>(Tileset->TextureHeight > 0 ? Tileset->TextureHeight : (Texture != nullptr ? static_cast<int32>(Texture->GetHeight()) : 0));

	const Editor2DMath::FTileStamp Stamp = GetEffectiveStamp();
	ImGui::Text("%s  ·  %s  ·  스탬프 %dx%d", ICON_FA_TABLE_CELLS, GetToolName(Tool), Stamp.Width, Stamp.Height);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::SliderFloat("##Zoom", &Zoom, 1.0f, 16.0f, "확대 %.1fx", ImGuiSliderFlags_Logarithmic);
	ImGui::TextDisabled("%s  (%dx%d 타일, %dx%d px)", Tilemap.Tileset.c_str(), Columns, Rows, Tileset->TileWidth, Tileset->TileHeight);

	if (Columns <= 0 || Rows <= 0 || TextureWidth <= 0.0f || TextureHeight <= 0.0f)
	{
		ImGui::TextDisabled("타일셋 격자가 비어 있습니다");
		return;
	}

	ImGui::BeginChild("PaletteCanvas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	const ImVec2 Origin = ImGui::GetCursorScreenPos();
	const ImVec2 CanvasSize(TextureWidth * Zoom, TextureHeight * Zoom);
	ImGui::InvisibleButton("##Tiles", ImVec2(std::max(CanvasSize.x, 1.0f), std::max(CanvasSize.y, 1.0f)));
	const bool  bCanvasHovered = ImGui::IsItemHovered();
	ImDrawList* DrawList       = ImGui::GetWindowDrawList();
	const ImVec2 CanvasMax(Origin.x + CanvasSize.x, Origin.y + CanvasSize.y);
	DrawList->AddRectFilled(Origin, CanvasMax, IM_COL32(40, 40, 44, 255));
	if (Texture != nullptr && Texture->IsReady())
	{
		DrawList->AddImage(static_cast<ImTextureID>(Texture->GetSrv().Gpu.ptr), Origin, CanvasMax);
	}
	else
	{
		DrawList->AddText(ImVec2(Origin.x + 6.0f, Origin.y + 6.0f), IM_COL32(200, 200, 200, 255), "텍스처 읽는 중...");
	}

	auto TileScreenRect = [&](int32 Col, int32 Row, ImVec2& OutMin, ImVec2& OutMax) {
		const FSpriteSlice Rect = Tileset->GetTileRect(Row * Columns + Col);
		OutMin                  = ImVec2(Origin.x + static_cast<float>(Rect.X) * Zoom, Origin.y + static_cast<float>(Rect.Y) * Zoom);
		OutMax                  = ImVec2(OutMin.x + static_cast<float>(Rect.W) * Zoom, OutMin.y + static_cast<float>(Rect.H) * Zoom);
	};
	// 격자
	const ImU32 GridColor = IM_COL32(255, 255, 255, 40);
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		for (int32 Col = 0; Col < Columns; ++Col)
		{
			ImVec2 Min;
			ImVec2 Max;
			TileScreenRect(Col, Row, Min, Max);
			DrawList->AddRect(Min, Max, GridColor);
		}
	}

	// 마우스 아래 타일
	int32 HoverCol = -1;
	int32 HoverRow = -1;
	if (bCanvasHovered)
	{
		const ImVec2 Mouse = ImGui::GetMousePos();
		const float  PixelX = (Mouse.x - Origin.x) / Zoom - static_cast<float>(Tileset->Margin);
		const float  PixelY = (Mouse.y - Origin.y) / Zoom - static_cast<float>(Tileset->Margin);
		const int32  Col    = static_cast<int32>(std::floor(PixelX / static_cast<float>(std::max(Tileset->TileWidth + Tileset->Spacing, 1))));
		const int32  Row    = static_cast<int32>(std::floor(PixelY / static_cast<float>(std::max(Tileset->TileHeight + Tileset->Spacing, 1))));
		if (Col >= 0 && Col < Columns && Row >= 0 && Row < Rows)
		{
			HoverCol = Col;
			HoverRow = Row;
		}
		// Ctrl + 휠 = 확대/축소
		if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f)
		{
			Zoom = std::clamp(Zoom * std::pow(1.2f, ImGui::GetIO().MouseWheel), 1.0f, 16.0f);
		}
	}
	if (HoverCol >= 0 && ImGui::IsItemClicked(ImGuiMouseButton_Left))
	{
		bSelecting = true;
		SelectCol0 = SelectCol1 = HoverCol;
		SelectRow0 = SelectRow1 = HoverRow;
	}
	if (bSelecting)
	{
		if (HoverCol >= 0)
		{
			SelectCol1 = HoverCol;
			SelectRow1 = HoverRow;
		}
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			bSelecting = false;
			BaseStamp  = Editor2DMath::MakeStampFromTileset(Columns, SelectCol0, SelectRow0, SelectCol1, SelectRow1);
			if (Tool == ETool::Select || Tool == ETool::Eraser || Tool == ETool::Picker)
			{
				Tool = ETool::Brush; // 타일을 고르면 바로 칠한다
				LastPaintTool = ETool::Brush;
			}
		}
	}

	// 선택 사각형 + 마우스 아래 칸
	{
		ImVec2 MinA;
		ImVec2 MaxA;
		ImVec2 MinB;
		ImVec2 MaxB;
		TileScreenRect(std::min(SelectCol0, SelectCol1), std::min(SelectRow0, SelectRow1), MinA, MaxA);
		TileScreenRect(std::max(SelectCol0, SelectCol1), std::max(SelectRow0, SelectRow1), MinB, MaxB);
		DrawList->AddRect(MinA, MaxB, ImGui::ColorConvertFloat4ToU32(FEditorTheme::Accent), 0.0f, 0, 2.5f);
	}
	if (HoverCol >= 0)
	{
		ImVec2 Min;
		ImVec2 Max;
		TileScreenRect(HoverCol, HoverRow, Min, Max);
		DrawList->AddRect(Min, Max, IM_COL32(255, 255, 255, 180), 0.0f, 0, 1.5f);
		const int32            TileId = HoverRow * Columns + HoverCol;
		const FTileDefinition* Def    = Tileset->FindTile(TileId);
		std::string            Tip    = std::format("타일 {}", TileId);
		if (Def != nullptr)
		{
			Tip += std::format(" · 충돌 {}{}", ToString(Def->Collision), Def->bOneWay ? " (원웨이)" : "");
			for (const std::string& Tag : Def->Tags)
			{
				Tip += " #" + Tag;
			}
			if (!Def->Animation.empty())
			{
				Tip += std::format(" · 애니메이션 {}프레임", Def->Animation.size());
			}
		}
		ImGui::SetItemTooltip("%s", Tip.c_str());
	}
	ImGui::EndChild();
}

void FTilePalettePanel::Draw(FEditorContext& Context)
{
	if (!bOpen)
	{
		return;
	}
	if (bRequestFocus)
	{
		FocusFrames   = 10;
		bRequestFocus = false;
	}
	if (FocusFrames > 0)
	{
		ImGui::SetNextWindowFocus();
		--FocusFrames;
	}
	if (!ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_TABLE_CELLS, "타일 팔레트", "TilePalette").c_str(), &bOpen))
	{
		ImGui::End();
		return;
	}
	const FEntity Target = FindTarget(Context);
	if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && Target.IsValid())
	{
		HandleShortcuts();
	}
	DrawToolbar();
	ImGui::Separator();
	if (!Target.IsValid())
	{
		ImGui::TextColored(FEditorTheme::TextDim, ICON_FA_CIRCLE_INFO " 칠할 타일맵을 선택하세요");
		ImGui::TextWrapped("계층 창이나 뷰포트에서 TilemapComponent가 있는 엔티티를 고르면 그 타일셋이 여기에 나옵니다. "
		                   "콘텐츠 브라우저에서 .etileset을 뷰포트로 끌어 놓거나 엔티티 메뉴 → 2D 타일맵으로 새 타일맵을 만들 수 있습니다.");
		ImGui::End();
		return;
	}
	if (Context.bPlaying)
	{
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 플레이 중에는 칠할 수 없습니다");
	}
	DrawPalette(Context, Target);
	ImGui::End();
}
