#include "Editor/Panels/TerrainToolPanel.h"

#include "Core/Input.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Renderer/Camera.h"
#include "Renderer/TerrainHeightmapImage.h"
#include "Scene/Scene.h"

#include <commdlg.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <format>
#include <limits>

namespace
{
	constexpr uint32 GResolutions[]   = { 129, 257, 513, 1025 };
	const char*      GResolutionNames[] = { "129 x 129", "257 x 257", "513 x 513", "1025 x 1025" };

	std::filesystem::path ShowHeightmapDialog(const std::filesystem::path& InitialDirectory, bool bSave)
	{
		wchar_t            Buffer[MAX_PATH] = {};
		const std::wstring InitialDir       = InitialDirectory.wstring();
		OPENFILENAMEW      Dialog{};
		Dialog.lStructSize     = sizeof(Dialog);
		Dialog.hwndOwner       = GetActiveWindow();
		Dialog.lpstrFilter     = bSave ? L"16비트 PNG (*.png)\0*.png\0RAW 16비트 (*.raw;*.r16)\0*.raw;*.r16\0"
		                               : L"높이맵 (*.png;*.raw;*.r16)\0*.png;*.raw;*.r16\0모든 파일 (*.*)\0*.*\0";
		Dialog.lpstrFile       = Buffer;
		Dialog.nMaxFile        = MAX_PATH;
		Dialog.lpstrInitialDir = InitialDir.c_str();
		Dialog.lpstrDefExt     = L"png";
		Dialog.Flags           = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (bSave ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
		const BOOL bOk = bSave ? GetSaveFileNameW(&Dialog) : GetOpenFileNameW(&Dialog);
		return bOk ? std::filesystem::path(Buffer) : std::filesystem::path();
	}

	bool IsRawExtension(const std::filesystem::path& Path)
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Extension == L".raw" || Extension == L".r16";
	}

	const char* GetOpLabel(ETerrainBrushOp Op)
	{
		switch (Op)
		{
		case ETerrainBrushOp::Raise:   return "올리기";
		case ETerrainBrushOp::Lower:   return "내리기";
		case ETerrainBrushOp::Flatten: return "평탄화";
		case ETerrainBrushOp::Smooth:  return "부드럽게";
		case ETerrainBrushOp::Noise:   return "노이즈";
		case ETerrainBrushOp::Paint:   return "칠하기";
		default:                       return "?";
		}
	}

	const std::string& GetLayerMaterial(const FTerrainComponent& Terrain, uint32 Layer)
	{
		switch (Layer)
		{
		case 1:  return Terrain.Layer1Material;
		case 2:  return Terrain.Layer2Material;
		case 3:  return Terrain.Layer3Material;
		default: return Terrain.Layer0Material;
		}
	}
} // namespace

void FTerrainToolPanel::Notify(FEditorContext& Context, const std::string& Message, bool bError) const
{
	if (Context.Notify)
	{
		Context.Notify(Message, bError);
	}
}

void FTerrainToolPanel::Update(FEditorContext& Context)
{
	if (Context.Scene == nullptr || bStroking)
	{
		return;
	}
	// 같은 에셋을 여러 지형이 쓰면 처음 찾은 컴포넌트의 버전을 따른다
	std::vector<std::string> Done;
	FTerrainLibrary&         Library = FTerrainLibrary::Get();
	Context.Scene->GetRegistry().View<FTerrainComponent>().Each([&](FEntity, FTerrainComponent& Terrain) {
		if (Terrain.Asset.empty() || std::find(Done.begin(), Done.end(), Terrain.Asset) != Done.end())
		{
			return;
		}
		Done.push_back(Terrain.Asset);
		const std::shared_ptr<FTerrainData> Data = Library.Find(Terrain.Asset);
		if (Data && Data->Revision != Terrain.EditRevision && !History.Reconcile(Terrain.Asset, *Data, Terrain.EditRevision))
		{
			// 기록에 없는 버전 (다른 세션 저장본 등): 데이터를 그대로 두고 컴포넌트를 데이터에 맞춘다
			E_LOG(LogTerrain, Warning, "지형 편집 버전 {}을(를) 기록에서 찾지 못해 현재 데이터({})를 유지합니다: {}", Terrain.EditRevision, Data->Revision,
			      Terrain.Asset);
			Terrain.EditRevision = Data->Revision;
		}
	});
}

FEntity FTerrainToolPanel::FindTarget(FEditorContext& Context) const
{
	FRegistry& Registry = Context.Scene->GetRegistry();
	if (Context.SelectedEntity.IsValid() && Registry.IsValid(Context.SelectedEntity) && Registry.Has<FTerrainComponent>(Context.SelectedEntity))
	{
		return Context.SelectedEntity;
	}
	FEntity Found;
	Registry.View<FTerrainComponent>().Each([&](FEntity Entity, FTerrainComponent&) {
		if (!Found.IsValid())
		{
			Found = Entity;
		}
	});
	return Found;
}

FEntity FTerrainToolPanel::CreateTerrain(FEditorContext& Context, uint32 Resolution, float Size)
{
	FScene& Scene = *Context.Scene;
	// Content/Terrain/Terrain_N.eterrain (겹치지 않는 이름)
	std::string Asset;
	for (int32 Index = 1; Index < 1000; ++Index)
	{
		Asset = std::format("Terrain/Terrain_{}.eterrain", Index);
		std::error_code ErrorCode;
		if (!std::filesystem::exists(Context.ContentDirectory / FStringConv::ToWide(Asset), ErrorCode) && !FTerrainLibrary::Get().Find(Asset))
		{
			break;
		}
	}
	FTerrainLibrary::Get().SetContentDirectory(Context.ContentDirectory);
	if (!FTerrainLibrary::Get().Create(Asset, Resolution))
	{
		Notify(Context, "지형 파일을 만들지 못했습니다", true);
		return NullEntity;
	}
	const FEntity      Entity  = Scene.CreateEntity("Terrain");
	FTerrainComponent& Terrain = Scene.GetRegistry().Emplace<FTerrainComponent>(Entity);
	Terrain.Asset              = Asset;
	Terrain.Size               = FVector2(Size, Size);
	Scene.UpdateTransforms();
	Context.Select(Entity);
	Context.MarkEdited("지형 추가");
	Notify(Context, std::format("지형 추가: {} ({}x{})", Asset, Resolution, Resolution), false);
	return Entity;
}

bool FTerrainToolPanel::RaycastTerrains(FEditorContext& Context, const FVector2& LocalPixel, const FVector2& ImageSize, FEntity& OutEntity,
                                        FVector3& OutHit) const
{
	if (ImageSize.X <= 0.0f || ImageSize.Y <= 0.0f)
	{
		return false;
	}
	const float NdcX = (LocalPixel.X / ImageSize.X) * 2.0f - 1.0f;
	const float NdcY = 1.0f - (LocalPixel.Y / ImageSize.Y) * 2.0f;
	const FRay  Ray  = FRay::FromNdc(NdcX, NdcY, Context.Camera->GetViewProjectionMatrix().GetInverse());

	std::vector<FTerrainInstance> Terrains;
	GatherTerrains(*Context.Scene, Terrains);
	float Best = std::numeric_limits<float>::max();
	for (const FTerrainInstance& Terrain : Terrains)
	{
		float Distance = 0.0f;
		if (TerrainMath::Raycast(*Terrain.Data, Terrain.Frame, Ray.Origin, Ray.Direction, 1.0e7f, Distance) && Distance < Best)
		{
			Best      = Distance;
			OutEntity = Terrain.Entity;
			OutHit    = Ray.GetPoint(Distance);
		}
	}
	return Best < std::numeric_limits<float>::max();
}

bool FTerrainToolPanel::BeginStroke(FEditorContext& Context, FEntity Terrain)
{
	FRegistry& Registry = Context.Scene->GetRegistry();
	if (bStroking || !Registry.IsValid(Terrain) || !Registry.Has<FTerrainComponent>(Terrain))
	{
		return false;
	}
	const FTerrainComponent&            Component = Registry.Get<FTerrainComponent>(Terrain);
	const std::shared_ptr<FTerrainData> Data      = FTerrainLibrary::Get().Load(Component.Asset);
	if (!Data || !Data->IsValid())
	{
		return false;
	}
	bStroking     = true;
	StrokeEntity  = Terrain;
	StrokeAsset   = Component.Asset;
	StrokeHeights = Data->Heights;
	StrokeWeights = Data->Weights;
	StrokeRect    = FTerrainRect{};
	++Brush.Seed;
	return true;
}

void FTerrainToolPanel::ApplyStroke(FEditorContext& Context, const FVector2& WorldXY, float DeltaSeconds)
{
	if (!bStroking)
	{
		return;
	}
	FRegistry& Registry = Context.Scene->GetRegistry();
	if (!Registry.IsValid(StrokeEntity) || !Registry.Has<FTerrainComponent>(StrokeEntity))
	{
		bStroking = false;
		return;
	}
	const FTerrainComponent&            Component = Registry.Get<FTerrainComponent>(StrokeEntity);
	const std::shared_ptr<FTerrainData> Data      = FTerrainLibrary::Get().Find(StrokeAsset);
	if (!Data || Data->Heights.size() != StrokeHeights.size())
	{
		bStroking = false;
		return;
	}
	const FTerrainFrame Frame   = FTerrainFrame::Make(Context.Scene->GetTransform(StrokeEntity).GetWorldPosition(), Component, Data->Resolution);
	const FTerrainRect  Changed = ApplyTerrainBrush(*Data, Frame, Brush, WorldXY, DeltaSeconds);
	if (!Changed.IsEmpty())
	{
		Data->MarkChanged(Changed);
		StrokeRect.Add(Changed);
	}
}

void FTerrainToolPanel::EndStroke(FEditorContext& Context, const char* Label)
{
	if (!bStroking)
	{
		return;
	}
	bStroking                                 = false;
	const std::shared_ptr<FTerrainData> Data  = FTerrainLibrary::Get().Find(StrokeAsset);
	FRegistry&                          Registry = Context.Scene->GetRegistry();
	if (StrokeRect.IsEmpty() || !Data || !Registry.IsValid(StrokeEntity) || !Registry.Has<FTerrainComponent>(StrokeEntity))
	{
		return;
	}
	FTerrainRegion Before    = FTerrainRegion::CaptureFrom(StrokeHeights, StrokeWeights, Data->Resolution, StrokeRect);
	FTerrainRegion After     = FTerrainRegion::Capture(*Data, StrokeRect);
	const uint32   Revision  = History.MakeRevision();
	History.Record(StrokeAsset, *Data, Revision, std::move(Before), std::move(After));
	Registry.Get<FTerrainComponent>(StrokeEntity).EditRevision = Revision;
	Context.MarkEdited(Label);
	StrokeHeights.clear();
	StrokeWeights.clear();
}

void FTerrainToolPanel::DrawBrushRing(FEditorContext& Context, const FVector2& ImageMin, const FVector2& ImageSize) const
{
	FRegistry& Registry = Context.Scene->GetRegistry();
	if (!bHasHit || !Registry.IsValid(HitEntity) || !Registry.Has<FTerrainComponent>(HitEntity))
	{
		return;
	}
	const FTerrainComponent&            Component = Registry.Get<FTerrainComponent>(HitEntity);
	const std::shared_ptr<FTerrainData> Data      = FTerrainLibrary::Get().Find(Component.Asset);
	if (!Data)
	{
		return;
	}
	const FTerrainFrame Frame          = FTerrainFrame::Make(Context.Scene->GetTransform(HitEntity).GetWorldPosition(), Component, Data->Resolution);
	const FMatrix4x4    ViewProjection = Context.Camera->GetViewProjectionMatrix();
	ImDrawList*         DrawList       = ImGui::GetWindowDrawList();

	auto Project = [&](const FVector3& P, ImVec2& Out) {
		const FVector3 Clip = ViewProjection.TransformPosition(P);
		const float    W    = P.X * ViewProjection.M[0][3] + P.Y * ViewProjection.M[1][3] + P.Z * ViewProjection.M[2][3] + ViewProjection.M[3][3];
		if (W <= 1.0f)
		{
			return false;
		}
		Out = ImVec2(ImageMin.X + (Clip.X / W * 0.5f + 0.5f) * ImageSize.X, ImageMin.Y + (0.5f - Clip.Y / W * 0.5f) * ImageSize.Y);
		return true;
	};
	auto Ring = [&](float Radius, ImU32 Color, float Thickness) {
		constexpr int32 Segments = 64;
		ImVec2          Previous;
		bool            bPrevious = false;
		for (int32 Index = 0; Index <= Segments; ++Index)
		{
			const float    Angle = FMath::TwoPi * static_cast<float>(Index) / static_cast<float>(Segments);
			const float    X     = HitPoint.X + FMath::Cos(Angle) * Radius;
			const float    Y     = HitPoint.Y + FMath::Sin(Angle) * Radius;
			const FVector3 Point(X, Y, TerrainMath::SampleWorldHeight(*Data, Frame, X, Y) + 2.0f); // 지형에 묻히지 않게 살짝 위
			ImVec2         Screen;
			const bool     bVisible = Project(Point, Screen);
			if (bVisible && bPrevious)
			{
				DrawList->AddLine(Previous, Screen, Color, Thickness);
			}
			Previous  = Screen;
			bPrevious = bVisible;
		}
	};
	const ImVec4 Accent = Mode == EMode::Paint ? FEditorTheme::Success : FEditorTheme::Accent;
	DrawList->PushClipRect(ImVec2(ImageMin.X, ImageMin.Y), ImVec2(ImageMin.X + ImageSize.X, ImageMin.Y + ImageSize.Y), true);
	Ring(Brush.Radius, ImGui::ColorConvertFloat4ToU32(ImVec4(Accent.x, Accent.y, Accent.z, 0.95f)), 2.0f);
	if (Brush.Falloff > 0.01f && Brush.Falloff < 0.99f)
	{
		Ring(Brush.Radius * (1.0f - Brush.Falloff), ImGui::ColorConvertFloat4ToU32(ImVec4(Accent.x, Accent.y, Accent.z, 0.45f)), 1.0f);
	}
	ImVec2 Center;
	if (Project(HitPoint, Center))
	{
		DrawList->AddCircleFilled(Center, 3.0f, ImGui::ColorConvertFloat4ToU32(Accent));
	}
	DrawList->PopClipRect();
}

bool FTerrainToolPanel::HandleViewport(FEditorContext& Context, const FInput& Input, const FVector2& ImageMin, const FVector2& ImageSize, bool bHovered)
{
	bHasHit = false;
	if (!IsActive() || Context.bPlaying)
	{
		if (bStroking)
		{
			EndStroke(Context, "지형 편집");
		}
		return false;
	}

	const ImVec2   Mouse = ImGui::GetMousePos();
	const FVector2 Local(Mouse.x - ImageMin.X, Mouse.y - ImageMin.Y);
	if (bHovered)
	{
		bHasHit = RaycastTerrains(Context, Local, ImageSize, HitEntity, HitPoint);
	}
	else if (bAutomationCursor)
	{
		std::vector<FTerrainInstance> Terrains;
		GatherTerrains(*Context.Scene, Terrains);
		if (!Terrains.empty())
		{
			bHasHit   = true;
			HitEntity = Terrains.front().Entity;
			HitPoint  = FVector3(AutomationCursor.X, AutomationCursor.Y,
                                TerrainMath::SampleWorldHeight(*Terrains.front().Data, Terrains.front().Frame, AutomationCursor.X, AutomationCursor.Y));
		}
	}

	// 반경 단축키
	if (bHovered && !ImGui::GetIO().KeyCtrl)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
		{
			Brush.Radius = std::max(10.0f, Brush.Radius / 1.2f);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_RightBracket))
		{
			Brush.Radius = std::min(100000.0f, Brush.Radius * 1.2f);
		}
	}

	const bool bCameraLook = Input.IsMouseButtonDown(EMouseButton::Right);
	const bool bInvert     = ImGui::GetIO().KeyShift;
	const bool bMouseDown  = ImGui::IsMouseDown(ImGuiMouseButton_Left);

	// 스트로크 시작: 커서 아래 지형 위 좌클릭
	if (!bStroking && bHovered && bHasHit && !bCameraLook && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
	{
		if (BeginStroke(Context, HitEntity))
		{
			Brush.Op = Mode == EMode::Paint ? ETerrainBrushOp::Paint : SculptOp;
			if (Brush.Op == ETerrainBrushOp::Flatten)
			{
				Brush.FlattenHeight = HitPoint.Z; // 시작점 높이로
			}
		}
	}

	if (bStroking)
	{
		if (!bMouseDown)
		{
			EndStroke(Context, Mode == EMode::Paint ? "지형 칠하기" : "지형 스컬프트");
		}
		else if (bHasHit && HitEntity == StrokeEntity)
		{
			const ETerrainBrushOp BaseOp = Mode == EMode::Paint ? ETerrainBrushOp::Paint : SculptOp;
			FTerrainBrush         Saved  = Brush;
			Brush.Op                     = BaseOp;
			if (bInvert && BaseOp == ETerrainBrushOp::Raise)
			{
				Brush.Op = ETerrainBrushOp::Lower;
			}
			else if (bInvert && BaseOp == ETerrainBrushOp::Lower)
			{
				Brush.Op = ETerrainBrushOp::Raise;
			}
			else if (bInvert && BaseOp == ETerrainBrushOp::Paint)
			{
				Brush.Layer = 0; // 지우개: 바탕 레이어로
			}
			ApplyStroke(Context, FVector2(HitPoint.X, HitPoint.Y), std::min(ImGui::GetIO().DeltaTime, 0.1f));
			const uint32 Seed = Brush.Seed;
			Brush             = Saved;
			Brush.Seed        = Seed;
		}
	}

	DrawBrushRing(Context, ImageMin, ImageSize);
	// 모드 중에는 지형 위에서 왼쪽 버튼을 가져간다 (지형 밖 클릭은 평소처럼 선택)
	return bStroking || (bHovered && bHasHit);
}

void FTerrainToolPanel::ImportHeightmap(FEditorContext& Context, FEntity Terrain, const std::filesystem::path& Path)
{
	FRegistry&                          Registry  = Context.Scene->GetRegistry();
	const FTerrainComponent&            Component = Registry.Get<FTerrainComponent>(Terrain);
	const std::shared_ptr<FTerrainData> Data      = FTerrainLibrary::Get().Load(Component.Asset);
	if (!Data)
	{
		return;
	}
	std::vector<uint16> Pixels;
	uint32              Width  = 0;
	uint32              Height = 0;
	bool                bOk    = false;
	if (IsRawExtension(Path))
	{
		bOk    = TerrainIO::ReadRaw16(Path, Pixels, Width);
		Height = Width;
	}
	else
	{
		bOk = FTerrainHeightmapImage::Load(Path, Pixels, Width, Height);
	}
	if (!bOk)
	{
		Notify(Context, "높이맵을 읽지 못했습니다 (PNG/정사각형 RAW16)", true);
		return;
	}
	if (!BeginStroke(Context, Terrain))
	{
		return;
	}
	Data->Heights = TerrainIO::ResampleHeights(Pixels, Width, Height, Data->Resolution);
	StrokeRect    = FTerrainRect::Full(static_cast<int32>(Data->Resolution));
	Data->MarkChanged(StrokeRect);
	EndStroke(Context, "높이맵 가져오기");
	Notify(Context, std::format("높이맵 가져오기: {}x{} → {}x{}", Width, Height, Data->Resolution, Data->Resolution), false);
}

void FTerrainToolPanel::ExportHeightmap(FEditorContext& Context, FEntity Terrain, const std::filesystem::path& Path)
{
	const FTerrainComponent&            Component = Context.Scene->GetRegistry().Get<FTerrainComponent>(Terrain);
	const std::shared_ptr<FTerrainData> Data      = FTerrainLibrary::Get().Load(Component.Asset);
	if (!Data)
	{
		return;
	}
	const bool bOk = IsRawExtension(Path) ? TerrainIO::WriteRaw16(Path, *Data) : TerrainIO::WritePng16(Path, *Data);
	Notify(Context, bOk ? "높이맵 내보내기 완료: " + FStringConv::ToUtf8(Path.filename().wstring()) : std::string("높이맵 내보내기 실패"), !bOk);
}

void FTerrainToolPanel::Draw(FEditorContext& Context)
{
	if (!bOpen)
	{
		if (bStroking)
		{
			EndStroke(Context, "지형 편집");
		}
		return;
	}
	if (bRequestFocus)
	{
		ImGui::SetNextWindowFocus(); // 탭 묶음에서 앞으로
		bRequestFocus = false;
	}
	if (!ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_MOUNTAIN_SUN, "지형", "Terrain").c_str(), &bOpen))
	{
		ImGui::End();
		return;
	}
	FTerrainLibrary::Get().SetContentDirectory(Context.ContentDirectory);
	ImGui::BeginDisabled(Context.bPlaying);

	// ---- 대상
	const FEntity Target   = FindTarget(Context);
	FRegistry&    Registry = Context.Scene->GetRegistry();
	if (!Target.IsValid())
	{
		ImGui::TextDisabled("씬에 지형이 없습니다");
	}
	else
	{
		const FTerrainComponent&            Component = Registry.Get<FTerrainComponent>(Target);
		const std::shared_ptr<FTerrainData> Data      = FTerrainLibrary::Get().Load(Component.Asset);
		const FNameComponent*               Name      = Registry.TryGet<FNameComponent>(Target);
		ImGui::Text("%s %s", ICON_FA_MOUNTAIN, Name != nullptr ? Name->Name.c_str() : "지형");
		if (Data)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%ux%u, %.0f x %.0f m", Data->Resolution, Data->Resolution, Component.Size.X / 100.0f, Component.Size.Y / 100.0f);
			if (Data->bUnsaved)
			{
				ImGui::SameLine();
				ImGui::TextColored(FEditorTheme::Warning, "저장 안 됨");
			}
		}
		else
		{
			ImGui::TextColored(FEditorTheme::Danger, "데이터를 읽지 못했습니다: %s", Component.Asset.c_str());
		}
	}

	// ---- 모드
	ImGui::SeparatorText("모드");
	if (FEditorTheme::ToolButton(ICON_FA_ARROW_POINTER, "선택 (브러시 끔)", Mode == EMode::None))
	{
		Mode = EMode::None;
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_MOUNTAIN, "스컬프트 (좌클릭 드래그, Shift = 반대)", Mode == EMode::Sculpt))
	{
		Mode = EMode::Sculpt;
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_PAINTBRUSH, "레이어 칠하기 (Shift = 레이어 0으로 지우기)", Mode == EMode::Paint))
	{
		Mode = EMode::Paint;
	}

	if (Mode == EMode::Sculpt)
	{
		const ETerrainBrushOp Ops[] = { ETerrainBrushOp::Raise, ETerrainBrushOp::Lower, ETerrainBrushOp::Flatten, ETerrainBrushOp::Smooth, ETerrainBrushOp::Noise };
		for (size_t Index = 0; Index < std::size(Ops); ++Index)
		{
			if (Index > 0)
			{
				ImGui::SameLine();
			}
			if (ImGui::RadioButton(GetOpLabel(Ops[Index]), SculptOp == Ops[Index]))
			{
				SculptOp = Ops[Index];
			}
		}
	}
	else if (Mode == EMode::Paint)
	{
		const FTerrainComponent* Component = Target.IsValid() ? &Registry.Get<FTerrainComponent>(Target) : nullptr;
		for (uint32 Layer = 0; Layer < TerrainMaxLayers; ++Layer)
		{
			const std::string& Material = Component != nullptr ? GetLayerMaterial(*Component, Layer) : std::string();
			const std::string  Label    = std::format("레이어 {}  {}", Layer, Material.empty() ? "(기본)" : Material.c_str());
			if (ImGui::RadioButton(Label.c_str(), Brush.Layer == Layer))
			{
				Brush.Layer = Layer;
			}
		}
	}

	// ---- 브러시
	ImGui::SeparatorText("브러시");
	ImGui::DragFloat("반경 (cm)", &Brush.Radius, 5.0f, 10.0f, 100000.0f, "%.0f");
	ImGui::SetItemTooltip("뷰포트에서 [ / ] 로도 바꿀 수 있다");
	ImGui::SliderFloat("세기", &Brush.Strength, 0.01f, 1.0f, "%.2f");
	ImGui::SliderFloat("감쇠", &Brush.Falloff, 0.0f, 1.0f, "%.2f");
	ImGui::SetItemTooltip("가장자리에서 부드럽게 줄어드는 폭 (0 = 단단한 원)");
	if (Mode == EMode::Sculpt && SculptOp == ETerrainBrushOp::Noise)
	{
		ImGui::DragFloat("노이즈 크기 (cm)", &Brush.NoiseScale, 10.0f, 10.0f, 100000.0f, "%.0f");
	}

	// ---- 지형 파일
	ImGui::SeparatorText("지형");
	ImGui::Combo("해상도", &NewResolutionIndex, GResolutionNames, IM_ARRAYSIZE(GResolutionNames));
	ImGui::DragFloat("크기 (cm)", &NewSize, 100.0f, 100.0f, 1000000.0f, "%.0f");
	if (ImGui::Button(ICON_FA_PLUS " 새 지형"))
	{
		CreateTerrain(Context, GResolutions[std::clamp(NewResolutionIndex, 0, 3)], NewSize);
	}
	ImGui::BeginDisabled(!Target.IsValid());
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_FLOPPY_DISK " 저장") && Target.IsValid())
	{
		const std::string& Asset = Registry.Get<FTerrainComponent>(Target).Asset;
		Notify(Context, FTerrainLibrary::Get().Save(Asset) ? "지형 저장: " + Asset : "지형 저장 실패: " + Asset, false);
	}
	if (ImGui::Button(ICON_FA_FILE_IMPORT " 높이맵 가져오기") && Target.IsValid())
	{
		if (const std::filesystem::path Path = ShowHeightmapDialog(Context.ContentDirectory, false); !Path.empty())
		{
			ImportHeightmap(Context, Target, Path);
		}
	}
	ImGui::SetItemTooltip("16비트/8비트 PNG 또는 정사각형 RAW16 (리틀 엔디언). 해상도가 다르면 늘이거나 줄인다");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_FILE_EXPORT " 내보내기") && Target.IsValid())
	{
		if (const std::filesystem::path Path = ShowHeightmapDialog(Context.ContentDirectory, true); !Path.empty())
		{
			ExportHeightmap(Context, Target, Path);
		}
	}
	ImGui::EndDisabled();
	ImGui::TextDisabled("되돌리기 기록 %zu개 (%.1f MB)", History.GetRecordCount(), static_cast<double>(History.GetMemorySize()) / (1024.0 * 1024.0));

	ImGui::EndDisabled();
	ImGui::End();
}
