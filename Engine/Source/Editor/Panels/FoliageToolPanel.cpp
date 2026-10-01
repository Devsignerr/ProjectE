#include "Editor/Panels/FoliageToolPanel.h"

#include "Core/Input.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Renderer/Camera.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"
#include "Scene/Terrain.h"

#include <imgui.h>


#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace
{
	// std::string 입력 (imgui_stdlib는 빌드에 없어 버퍼로)
	bool InputString(const char* Label, std::string& Value)
	{
		char Buffer[512] = {};
		const size_t Length = std::min(Value.size(), sizeof(Buffer) - 1);
		std::copy_n(Value.data(), Length, Buffer);
		if (ImGui::InputText(Label, Buffer, sizeof(Buffer)))
		{
			Value = Buffer;
			return true;
		}
		return false;
	}

	const char* GMeshChoices[] = { "foliage:grass", "foliage:bush", "foliage:tree", "foliage:pine", "foliage:rock", "primitive:cube", "primitive:sphere" };

	FFoliageType MakeType(const char* Name, const char* Mesh, float Density, float MinScale, float MaxScale, float MaxSlope, float Cull, float Shadow,
	                      bool bAlign)
	{
		FFoliageType Type;
		Type.Name           = Name;
		Type.Mesh           = Mesh;
		Type.Density        = Density;
		Type.MinScale       = MinScale;
		Type.MaxScale       = MaxScale;
		Type.MaxSlope       = MaxSlope;
		Type.CullDistance   = Cull;
		Type.ShadowDistance = Shadow;
		Type.bAlignToNormal = bAlign;
		return Type;
	}
} // namespace

FFoliageAsset FFoliageToolPanel::MakeDefaultAsset()
{
	FFoliageAsset Asset;
	Asset.Types.push_back(MakeType("Grass", "foliage:grass", 150.0f, 0.7f, 1.3f, 35.0f, 4500.0f, 0.0f, true));
	Asset.Types.push_back(MakeType("Bush", "foliage:bush", 3.0f, 0.6f, 1.4f, 30.0f, 9000.0f, 4000.0f, true));
	FFoliageType Tree    = MakeType("Tree", "foliage:tree", 0.8f, 0.8f, 1.3f, 25.0f, 25000.0f, 6000.0f, false);
	Tree.bCollision      = true;
	Tree.CollisionRadius = 22.0f;
	Tree.CollisionHeight = 420.0f;
	Tree.ZOffset         = -10.0f;
	Asset.Types.push_back(Tree);
	FFoliageType Pine    = MakeType("Pine", "foliage:pine", 0.6f, 0.8f, 1.4f, 30.0f, 25000.0f, 6000.0f, false);
	Pine.bCollision      = true;
	Pine.CollisionRadius = 18.0f;
	Pine.CollisionHeight = 450.0f;
	Pine.ZOffset         = -10.0f;
	Asset.Types.push_back(Pine);
	FFoliageType Rock = MakeType("Rock", "foliage:rock", 1.0f, 0.4f, 1.6f, 45.0f, 12000.0f, 4000.0f, true);
	Rock.ZOffset      = -15.0f;
	Asset.Types.push_back(Rock);
	Asset.EnsureInstanceLists();
	return Asset;
}

bool FFoliageToolPanel::QuerySurface(FEditorContext& Context, float X, float Y, float MaxZ, FVector3& OutPosition, FVector3& OutNormal)
{
	bool  bFound = false;
	float BestZ  = std::numeric_limits<float>::lowest();
	// 지형 (정확)
	std::vector<FTerrainInstance> Terrains;
	GatherTerrains(*Context.Scene, Terrains);
	for (const FTerrainInstance& Terrain : Terrains)
	{
		const FVector2 Grid = Terrain.Frame.WorldToGrid(X, Y);
		const float    Last = static_cast<float>(Terrain.Frame.Resolution - 1);
		if (Grid.X < 0.0f || Grid.Y < 0.0f || Grid.X > Last || Grid.Y > Last)
		{
			continue;
		}
		const float Z = TerrainMath::SampleWorldHeight(*Terrain.Data, Terrain.Frame, X, Y);
		if (Z <= MaxZ && Z > BestZ)
		{
			BestZ       = Z;
			OutPosition = FVector3(X, Y, Z);
			OutNormal   = TerrainMath::ComputeNormal(*Terrain.Data, Terrain.Frame, Grid.X, Grid.Y);
			bFound      = true;
		}
	}
	// 정적 메시: 월드 경계 상자 윗면 (근사)
	Context.Scene->GetRegistry().View<FTransformComponent, FStaticMeshComponent>().Each(
		[&](FEntity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
			const FStaticMesh* Mesh = MeshComponent.bVisible ? Context.Resources->GetMesh(MeshComponent.Mesh) : nullptr;
			if (Mesh == nullptr)
			{
				return;
			}
			const FBox Bounds = Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix);
			if (X < Bounds.Min.X || X > Bounds.Max.X || Y < Bounds.Min.Y || Y > Bounds.Max.Y || Bounds.Max.Z > MaxZ || Bounds.Max.Z <= BestZ)
			{
				return;
			}
			BestZ       = Bounds.Max.Z;
			OutPosition = FVector3(X, Y, Bounds.Max.Z);
			OutNormal   = FVector3::UpVector;
			bFound      = true;
		});
	return bFound;
}

void FFoliageToolPanel::Update(FEditorContext& Context)
{
	if (Context.Scene == nullptr || bStroking || bEditing)
	{
		return;
	}
	std::vector<std::string> Done;
	Context.Scene->GetRegistry().View<FFoliageComponent>().Each([&](FEntity, FFoliageComponent& Component) {
		if (Component.Asset.empty() || std::find(Done.begin(), Done.end(), Component.Asset) != Done.end())
		{
			return;
		}
		Done.push_back(Component.Asset);
		const std::shared_ptr<FFoliageAsset> Asset = FFoliageLibrary::Get().Find(Component.Asset);
		if (Asset && Asset->Revision != Component.EditRevision && !History.Reconcile(Component.Asset, *Asset, Component.EditRevision))
		{
			E_LOG(LogTerrain, Warning, "폴리지 편집 버전 {}을(를) 기록에서 찾지 못해 현재 데이터({})를 유지합니다: {}", Component.EditRevision, Asset->Revision,
			      Component.Asset);
			Component.EditRevision = Asset->Revision;
		}
	});
}

FEntity FFoliageToolPanel::FindTarget(FEditorContext& Context) const
{
	FRegistry& Registry = Context.Scene->GetRegistry();
	if (Context.SelectedEntity.IsValid() && Registry.IsValid(Context.SelectedEntity) && Registry.Has<FFoliageComponent>(Context.SelectedEntity))
	{
		return Context.SelectedEntity;
	}
	FEntity Found;
	Registry.View<FFoliageComponent>().Each([&](FEntity Entity, FFoliageComponent&) {
		if (!Found.IsValid())
		{
			Found = Entity;
		}
	});
	return Found;
}

FEntity FFoliageToolPanel::CreateFoliage(FEditorContext& Context)
{
	std::string Asset;
	for (int32 Index = 1; Index < 1000; ++Index)
	{
		Asset = std::format("Foliage/Foliage_{}.efoliage", Index);
		std::error_code ErrorCode;
		if (!std::filesystem::exists(Context.ContentDirectory / FStringConv::ToWide(Asset), ErrorCode) && !FFoliageLibrary::Get().Find(Asset))
		{
			break;
		}
	}
	FFoliageLibrary::Get().SetContentDirectory(Context.ContentDirectory);
	if (!FFoliageLibrary::Get().Create(Asset, MakeDefaultAsset()))
	{
		if (Context.Notify)
		{
			Context.Notify("폴리지 파일을 만들지 못했습니다", true);
		}
		return NullEntity;
	}
	const FEntity Entity = Context.Scene->CreateEntity("Foliage");
	Context.Scene->GetRegistry().Emplace<FFoliageComponent>(Entity).Asset = Asset;
	Context.Select(Entity);
	Context.MarkEdited("폴리지 추가");
	return Entity;
}

bool FFoliageToolPanel::BeginStroke(FEditorContext& Context, FEntity Foliage)
{
	FRegistry& Registry = Context.Scene->GetRegistry();
	if (bStroking || !Registry.IsValid(Foliage) || !Registry.Has<FFoliageComponent>(Foliage))
	{
		return false;
	}
	const std::string&                   AssetPath = Registry.Get<FFoliageComponent>(Foliage).Asset;
	const std::shared_ptr<FFoliageAsset> Asset     = FFoliageLibrary::Get().Load(AssetPath);
	if (!Asset)
	{
		return false;
	}
	Asset->EnsureInstanceLists();
	bStroking      = true;
	bStrokeChanged = false;
	StrokeEntity   = Foliage;
	StrokeAsset    = AssetPath;
	StrokeBefore   = FFoliageEditHistory::FSnapshot::Capture(*Asset);
	return true;
}

void FFoliageToolPanel::ApplyStroke(FEditorContext& Context, const FVector2& WorldXY, float DeltaSeconds, bool bErase)
{
	const std::shared_ptr<FFoliageAsset> Asset = bStroking ? FFoliageLibrary::Get().Find(StrokeAsset) : nullptr;
	if (!Asset)
	{
		return;
	}
	PaintTypes.resize(Asset->Types.size(), false);
	for (uint32 TypeIndex = 0; TypeIndex < Asset->Types.size(); ++TypeIndex)
	{
		if (!PaintTypes[TypeIndex])
		{
			continue;
		}
		if (bErase)
		{
			bStrokeChanged |= FoliageMath::Erase(*Asset, TypeIndex, WorldXY, BrushRadius) > 0;
			continue;
		}
		// 지면 질의: 브러시 중심 위쪽(+20m)부터 아래 가장 높은 면
		const float MaxZ = HitPoint.Z + 2000.0f;
		const FFoliageSurfaceQuery Surface = [&Context, MaxZ](float X, float Y, FVector3& OutPosition, FVector3& OutNormal) {
			return QuerySurface(Context, X, Y, MaxZ, OutPosition, OutNormal);
		};
		// 한 번에 다 채우지 않고 초당 일정량 (끌면서 고르게)
		const uint32 MaxAdd = std::max(1u, static_cast<uint32>(4000.0f * DeltaSeconds));
		bStrokeChanged |= FoliageMath::Paint(*Asset, TypeIndex, WorldXY, BrushRadius, DensityScale, MaxAdd, Surface, Random) > 0;
	}
}

void FFoliageToolPanel::EndStroke(FEditorContext& Context, const char* Label)
{
	if (!bStroking)
	{
		return;
	}
	bStroking                                 = false;
	const std::shared_ptr<FFoliageAsset> Asset = FFoliageLibrary::Get().Find(StrokeAsset);
	if (Asset && bStrokeChanged)
	{
		Commit(Context, *Asset, StrokeAsset, StrokeEntity, std::move(StrokeBefore), Label);
	}
	StrokeBefore = {};
}

void FFoliageToolPanel::Commit(FEditorContext& Context, FFoliageAsset& Asset, const std::string& AssetPath, FEntity Target,
                               FFoliageEditHistory::FSnapshot Before, const char* Label)
{
	FRegistry& Registry = Context.Scene->GetRegistry();
	if (!Registry.IsValid(Target) || !Registry.Has<FFoliageComponent>(Target))
	{
		return;
	}
	const uint32 Revision = History.MakeRevision();
	History.Record(AssetPath, Asset, Revision, std::move(Before));
	Registry.Get<FFoliageComponent>(Target).EditRevision = Revision;
	Context.MarkEdited(Label);
}

bool FFoliageToolPanel::RaycastSurface(FEditorContext& Context, const FVector2& LocalPixel, const FVector2& ImageSize, FVector3& OutHit) const
{
	if (ImageSize.X <= 0.0f || ImageSize.Y <= 0.0f)
	{
		return false;
	}
	const FRay Ray = FRay::FromNdc((LocalPixel.X / ImageSize.X) * 2.0f - 1.0f, 1.0f - (LocalPixel.Y / ImageSize.Y) * 2.0f,
	                               Context.Camera->GetViewProjectionMatrix().GetInverse());
	float Best = std::numeric_limits<float>::max();
	std::vector<FTerrainInstance> Terrains;
	GatherTerrains(*Context.Scene, Terrains);
	for (const FTerrainInstance& Terrain : Terrains)
	{
		float Distance = 0.0f;
		if (TerrainMath::Raycast(*Terrain.Data, Terrain.Frame, Ray.Origin, Ray.Direction, Best, Distance) && Distance < Best)
		{
			Best = Distance;
		}
	}
	Context.Scene->GetRegistry().View<FTransformComponent, FStaticMeshComponent>().Each(
		[&](FEntity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
			const FStaticMesh* Mesh     = MeshComponent.bVisible ? Context.Resources->GetMesh(MeshComponent.Mesh) : nullptr;
			float              Distance = 0.0f;
			if (Mesh != nullptr && Ray.Intersects(Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix), Distance) && Distance < Best)
			{
				Best = Distance;
			}
		});
	if (Best == std::numeric_limits<float>::max())
	{
		return false;
	}
	OutHit = Ray.GetPoint(Best);
	return true;
}

void FFoliageToolPanel::DrawBrushRing(FEditorContext& Context, const FVector2& ImageMin, const FVector2& ImageSize) const
{
	if (!bHasHit)
	{
		return;
	}
	const FMatrix4x4 ViewProjection = Context.Camera->GetViewProjectionMatrix();
	ImDrawList*      DrawList       = ImGui::GetWindowDrawList();
	auto             Project        = [&](const FVector3& P, ImVec2& Out) {
        const FVector3 Clip = ViewProjection.TransformPosition(P);
        const float    W    = P.X * ViewProjection.M[0][3] + P.Y * ViewProjection.M[1][3] + P.Z * ViewProjection.M[2][3] + ViewProjection.M[3][3];
        if (W <= 1.0f)
        {
            return false;
        }
        Out = ImVec2(ImageMin.X + (Clip.X / W * 0.5f + 0.5f) * ImageSize.X, ImageMin.Y + (0.5f - Clip.Y / W * 0.5f) * ImageSize.Y);
        return true;
	};
	const ImVec4 Color = Mode == EMode::Erase || ImGui::GetIO().KeyShift ? FEditorTheme::Danger : FEditorTheme::Success;
	const ImU32  Line  = ImGui::ColorConvertFloat4ToU32(ImVec4(Color.x, Color.y, Color.z, 0.95f));
	DrawList->PushClipRect(ImVec2(ImageMin.X, ImageMin.Y), ImVec2(ImageMin.X + ImageSize.X, ImageMin.Y + ImageSize.Y), true);
	constexpr int32 Segments = 64;
	ImVec2          Previous;
	bool            bPrevious = false;
	for (int32 Index = 0; Index <= Segments; ++Index)
	{
		const float Angle = FMath::TwoPi * static_cast<float>(Index) / static_cast<float>(Segments);
		const float X     = HitPoint.X + FMath::Cos(Angle) * BrushRadius;
		const float Y     = HitPoint.Y + FMath::Sin(Angle) * BrushRadius;
		FVector3    Point(X, Y, HitPoint.Z);
		FVector3    Normal;
		QuerySurface(Context, X, Y, HitPoint.Z + 2000.0f, Point, Normal);
		ImVec2     Screen;
		const bool bVisible = Project(Point + FVector3(0.0f, 0.0f, 3.0f), Screen);
		if (bVisible && bPrevious)
		{
			DrawList->AddLine(Previous, Screen, Line, 2.0f);
		}
		Previous  = Screen;
		bPrevious = bVisible;
	}
	DrawList->PopClipRect();
}

bool FFoliageToolPanel::HandleViewport(FEditorContext& Context, const FInput& Input, const FVector2& ImageMin, const FVector2& ImageSize, bool bHovered)
{
	bHasHit = false;
	if (!IsActive() || Context.bPlaying)
	{
		EndStroke(Context, "폴리지 칠하기");
		return false;
	}
	const ImVec2 Mouse = ImGui::GetMousePos();
	if (bHovered)
	{
		bHasHit = RaycastSurface(Context, FVector2(Mouse.x - ImageMin.X, Mouse.y - ImageMin.Y), ImageSize, HitPoint);
	}
	else if (bAutomationCursor)
	{
		FVector3 Normal;
		bHasHit = QuerySurface(Context, AutomationCursor.X, AutomationCursor.Y, 1.0e7f, HitPoint, Normal);
	}
	if (bHovered && !ImGui::GetIO().KeyCtrl)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
		{
			BrushRadius = std::max(10.0f, BrushRadius / 1.2f);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_RightBracket))
		{
			BrushRadius = std::min(100000.0f, BrushRadius * 1.2f);
		}
	}
	const bool bErase = Mode == EMode::Erase || ImGui::GetIO().KeyShift;
	if (!bStroking && bHovered && bHasHit && !Input.IsMouseButtonDown(EMouseButton::Right) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
	    !ImGui::IsAnyItemHovered())
	{
		BeginStroke(Context, FindTarget(Context));
	}
	if (bStroking)
	{
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			EndStroke(Context, bErase ? "폴리지 지우기" : "폴리지 칠하기");
		}
		else if (bHasHit)
		{
			ApplyStroke(Context, FVector2(HitPoint.X, HitPoint.Y), std::min(ImGui::GetIO().DeltaTime, 0.1f), bErase);
		}
	}
	DrawBrushRing(Context, ImageMin, ImageSize);
	return bStroking || (bHovered && bHasHit);
}

void FFoliageToolPanel::TrackEdit(FEditorContext& Context, FFoliageAsset& Asset, const std::string& AssetPath, FEntity Target)
{
	if (ImGui::IsItemActivated() && !bEditing)
	{
		bEditing   = true;
		EditBefore = FFoliageEditHistory::FSnapshot::Capture(Asset);
	}
	if (ImGui::IsItemEdited())
	{
		Asset.MarkChanged(); // 렌더러/충돌이 바로 반영
	}
	if (ImGui::IsItemDeactivated() && bEditing)
	{
		bEditing = false;
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			Commit(Context, Asset, AssetPath, Target, std::move(EditBefore), "폴리지 타입 편집");
		}
		EditBefore = {};
	}
}

void FFoliageToolPanel::DrawTypeEditor(FEditorContext& Context, FFoliageAsset& Asset, const std::string& AssetPath, FEntity Target)
{
	Asset.EnsureInstanceLists();
	PaintTypes.resize(Asset.Types.size(), false);
	if (std::none_of(PaintTypes.begin(), PaintTypes.end(), [](bool bValue) { return bValue; }) && !Asset.Types.empty())
	{
		PaintTypes[std::clamp(SelectedType, 0, static_cast<int32>(Asset.Types.size()) - 1)] = true;
	}
	ImGui::SeparatorText("타입");
	for (int32 Index = 0; Index < static_cast<int32>(Asset.Types.size()); ++Index)
	{
		ImGui::PushID(Index);
		bool bPaint = PaintTypes[Index];
		if (ImGui::Checkbox("##Paint", &bPaint))
		{
			PaintTypes[Index] = bPaint;
		}
		ImGui::SetItemTooltip("칠하기/지우기 대상");
		ImGui::SameLine();
		const std::string Label = std::format("{}  ({}개)", Asset.Types[Index].Name, Asset.Instances[Index].size());
		if (ImGui::Selectable(Label.c_str(), SelectedType == Index))
		{
			SelectedType = Index;
		}
		ImGui::PopID();
	}
	if (ImGui::Button(ICON_FA_PLUS " 타입 추가"))
	{
		FFoliageEditHistory::FSnapshot Before = FFoliageEditHistory::FSnapshot::Capture(Asset);
		Asset.Types.push_back(FFoliageType{});
		Asset.MarkChanged();
		SelectedType = static_cast<int32>(Asset.Types.size()) - 1;
		Commit(Context, Asset, AssetPath, Target, std::move(Before), "폴리지 타입 추가");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(Asset.Types.empty());
	if (ImGui::Button(ICON_FA_TRASH " 타입 삭제") && SelectedType < static_cast<int32>(Asset.Types.size()))
	{
		FFoliageEditHistory::FSnapshot Before = FFoliageEditHistory::FSnapshot::Capture(Asset);
		Asset.Types.erase(Asset.Types.begin() + SelectedType);
		Asset.Instances.erase(Asset.Instances.begin() + SelectedType);
		Asset.TypeCounters.clear();
		PaintTypes.clear();
		Asset.MarkChanged();
		SelectedType = std::max(0, SelectedType - 1);
		Commit(Context, Asset, AssetPath, Target, std::move(Before), "폴리지 타입 삭제");
	}
	ImGui::EndDisabled();
	if (SelectedType >= static_cast<int32>(Asset.Types.size()))
	{
		return;
	}

	FFoliageType& Type = Asset.Types[SelectedType];
	ImGui::SeparatorText("타입 설정");
	auto Track = [&]() { TrackEdit(Context, Asset, AssetPath, Target); };
	InputString("이름", Type.Name);
	Track();
	if (ImGui::BeginCombo("메시", Type.Mesh.c_str()))
	{
		for (const char* Choice : GMeshChoices)
		{
			if (ImGui::Selectable(Choice, Type.Mesh == Choice))
			{
				FFoliageEditHistory::FSnapshot Before = FFoliageEditHistory::FSnapshot::Capture(Asset);
				Type.Mesh                             = Choice;
				Asset.MarkChanged();
				Commit(Context, Asset, AssetPath, Target, std::move(Before), "폴리지 메시 바꾸기");
			}
		}
		ImGui::EndCombo();
	}
	InputString("머티리얼 (.emat)", Type.Material);
	Track();
	ImGui::SetItemTooltip("Content 기준 경로. 비우면 기본 머티리얼 (내장 메시는 정점 색)");
	ImGui::DragFloat("밀도 (100m²당)", &Type.Density, 0.1f, 0.0f, 10000.0f, "%.1f");
	Track();
	ImGui::DragFloatRange2("크기", &Type.MinScale, &Type.MaxScale, 0.01f, 0.01f, 100.0f, "%.2f");
	Track();
	ImGui::SliderFloat("최대 경사 (도)", &Type.MaxSlope, 0.0f, 90.0f, "%.0f");
	Track();
	ImGui::DragFloatRange2("높이 제한 (cm)", &Type.MinHeight, &Type.MaxHeight, 10.0f, -1.0e7f, 1.0e7f, "%.0f");
	Track();
	ImGui::SetItemTooltip("최소 >= 최대이면 제한 없음");
	ImGui::Checkbox("지면 정렬", &Type.bAlignToNormal);
	Track();
	ImGui::SameLine();
	ImGui::Checkbox("무작위 회전", &Type.bRandomYaw);
	Track();
	ImGui::DragFloat("높이 보정 (cm)", &Type.ZOffset, 1.0f, -1000.0f, 1000.0f, "%.0f");
	Track();
	ImGui::DragFloat("컬링 거리 (cm)", &Type.CullDistance, 50.0f, 100.0f, 1.0e6f, "%.0f");
	Track();
	ImGui::DragFloat("그림자 거리 (cm)", &Type.ShadowDistance, 50.0f, 0.0f, 1.0e6f, "%.0f");
	Track();
	ImGui::SetItemTooltip("0 = 그림자 없음. 방향광 그림자 거리(그림자 창)보다 먼 그림자는 어차피 안 그린다");
	ImGui::Checkbox("충돌 (캡슐)", &Type.bCollision);
	Track();
	if (Type.bCollision)
	{
		ImGui::DragFloat("충돌 반지름 (cm)", &Type.CollisionRadius, 0.5f, 1.0f, 1000.0f, "%.0f");
		Track();
		ImGui::DragFloat("충돌 높이 (cm)", &Type.CollisionHeight, 1.0f, 2.0f, 10000.0f, "%.0f");
		Track();
	}
}

void FFoliageToolPanel::Draw(FEditorContext& Context)
{
	if (!bOpen)
	{
		EndStroke(Context, "폴리지 칠하기");
		return;
	}
	if (bRequestFocus)
	{
		ImGui::SetNextWindowFocus();
		bRequestFocus = false;
	}
	if (!ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_TREE, "폴리지", "Foliage").c_str(), &bOpen))
	{
		ImGui::End();
		return;
	}
	FFoliageLibrary::Get().SetContentDirectory(Context.ContentDirectory);
	ImGui::BeginDisabled(Context.bPlaying);
	const FEntity Target   = FindTarget(Context);
	FRegistry&    Registry = Context.Scene->GetRegistry();

	ImGui::SeparatorText("모드");
	if (FEditorTheme::ToolButton(ICON_FA_ARROW_POINTER, "선택 (브러시 끔)", Mode == EMode::None))
	{
		Mode = EMode::None;
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_SEEDLING, "칠하기 (좌클릭 드래그, Shift = 지우기)", Mode == EMode::Paint))
	{
		Mode = EMode::Paint;
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_ERASER, "지우기", Mode == EMode::Erase))
	{
		Mode = EMode::Erase;
	}
	ImGui::DragFloat("브러시 반경 (cm)", &BrushRadius, 5.0f, 10.0f, 100000.0f, "%.0f");
	ImGui::SliderFloat("밀도 배율", &DensityScale, 0.05f, 4.0f, "%.2f");

	if (!Target.IsValid())
	{
		ImGui::TextDisabled("씬에 폴리지가 없습니다");
		if (ImGui::Button(ICON_FA_PLUS " 새 폴리지"))
		{
			CreateFoliage(Context);
		}
	}
	else
	{
		const std::string                    AssetPath = Registry.Get<FFoliageComponent>(Target).Asset;
		const std::shared_ptr<FFoliageAsset> Asset     = FFoliageLibrary::Get().Load(AssetPath);
		if (!Asset)
		{
			ImGui::TextColored(FEditorTheme::Danger, "데이터를 읽지 못했습니다: %s", AssetPath.c_str());
		}
		else
		{
			ImGui::Text("%s %s  —  인스턴스 %zu개", ICON_FA_TREE, AssetPath.c_str(), Asset->GetInstanceCount());
			if (Asset->bUnsaved)
			{
				ImGui::SameLine();
				ImGui::TextColored(FEditorTheme::Warning, "저장 안 됨");
			}
			if (ImGui::Button(ICON_FA_FLOPPY_DISK " 저장"))
			{
				FFoliageLibrary::Get().Save(AssetPath);
			}
			DrawTypeEditor(Context, *Asset, AssetPath, Target);
		}
	}
	ImGui::TextDisabled("되돌리기 기록 %zu개 (%.1f MB)", History.GetRecordCount(), static_cast<double>(History.GetMemorySize()) / (1024.0 * 1024.0));
	ImGui::EndDisabled();
	ImGui::End();
}
