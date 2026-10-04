// ImGuizmo.h는 imgui.h를 먼저 요구하고, Windows.h 매크로(TEXT 등)와 열거형 이름이 충돌하므로 Windows 헤더보다 먼저 포함
#include <imgui.h>
#include <ImGuizmo.h>

#include "Editor/Panels/ViewportPanel.h"

#include "Core/Input.h"
#include "Editor/Editor2D/Collider2DOverlay.h"
#include "Editor/Editor2D/Editor2DMath.h"
#include "Editor/Editor2D/Editor2DScene.h"
#include "Editor/EditorCameraState.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Editor/EditorGrid.h"
#include "Editor/NavMeshDebugRenderer.h"
#include "Physics/PhysicsComponents.h"
#include "AI/AISystem.h"
#include "Editor/SceneEditOps.h"
#include "Editor/SelectionOutline.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "Renderer/Camera.h"
#include "Renderer/DebugDraw.h"
#include "Renderer/DebugDrawRenderer.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/StaticMesh.h"
#include "Core/StringConv.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/DecalMath.h"
#include "Renderer/SceneAssetResolver.h"
#include "Editor/EditorActions.h"
#include "Scene/IrradianceVolume.h"
#include "Scene/Particles.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scene/Sprite/Sprite2DComponents.h"
#include "Scene/Sprite/SpriteAsset.h"
#include "Scene/Sprite/TilesetAsset.h"
#include "Scene/Terrain.h"
#include "Renderer/UIRenderer.h"
#include "UI/UIComponent.h"
#include "UI/UISystem.h"

#include <algorithm>
#include <cwctype>
#include <format>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	constexpr uint32 GMinViewportSize = 16;

	// 광선에 닿는 가장 가까운 보이는 메시 엔티티 (모델 하위 노드 그대로). InOutDistance보다 먼 것은 무시하고, 찾으면 그 거리로 줄인다.
	//   경계 상자로 거른 뒤 삼각형으로 정확히 판정한다 — 상자만 쓰면 넓은 바닥/회전된 길/큰 지붕 상자가 그 위·안의 물체를 가린다.
	//   스킨 메시는 정점이 본으로 움직이므로(CPU 정점 = 바인드 포즈) 경계 상자로만 판정한다
	FEntity RaycastVisibleMeshes(FEditorContext& Context, const FRay& Ray, float& InOutDistance)
	{
		FEntity    Closest;
		FRegistry& Registry = Context.Scene->GetRegistry();
		Registry.View<FTransformComponent, FStaticMeshComponent>().Each([&](FEntity Entity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
			const FStaticMesh* Mesh = MeshComponent.bVisible ? Context.Resources->GetMesh(MeshComponent.Mesh) : nullptr;
			float              Distance = 0.0f;
			if (Mesh == nullptr || !Ray.Intersects(Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix), Distance) || Distance >= InOutDistance)
			{
				return;
			}
			const bool bUseBounds = Mesh->IsSkinned() || Registry.Has<FSkinComponent>(Entity) || Mesh->GetCpuIndices().empty();
			if (bUseBounds || Ray.IntersectsMesh(Transform.WorldMatrix, Mesh->GetCpuPositions(), Mesh->GetCpuIndices(), Distance, InOutDistance))
			{
				InOutDistance = Distance;
				Closest       = Entity;
			}
		});
		return Closest;
	}

	// 선택한 엔티티의 물리 관절: 연결 지점(+), 대상까지 선, 경첩 축과 원, 구 관절 원뿔 (Line/Circle은 아래 오버레이 함수의 투영 선 그리기)
	template <typename TLine, typename TCircle>
	void DrawJointShape(const FScene* Scene, FEntity Entity, const FTransformComponent& Transform, const TLine& Line, const TCircle& Circle)
	{
		const FRegistry& Registry = Scene->GetRegistry();
		const ImU32      Color    = ImGui::ColorConvertFloat4ToU32(ImVec4(FEditorTheme::Accent.x, FEditorTheme::Accent.y, FEditorTheme::Accent.z, 0.9f));
		const ImU32      Faint    = ImGui::ColorConvertFloat4ToU32(ImVec4(FEditorTheme::Accent.x, FEditorTheme::Accent.y, FEditorTheme::Accent.z, 0.45f));
		const FMatrix4x4& World   = Transform.WorldMatrix;

		auto TargetPoint = [&](FEntity Target, const FVector3& Local) {
			if (Target.IsValid() && Registry.IsValid(Target))
			{
				if (const FTransformComponent* TargetTransform = Registry.TryGet<FTransformComponent>(Target))
				{
					return TargetTransform->WorldMatrix.TransformPosition(Local);
				}
			}
			return Local; // 대상 없음 = 월드 위치
		};
		auto Anchor = [&](const FVector3& Local, FEntity Target, bool bLineToTarget) {
			constexpr float Size  = 6.0f;
			const FVector3  Point = World.TransformPosition(Local);
			Line(Point - FVector3(Size, 0.0f, 0.0f), Point + FVector3(Size, 0.0f, 0.0f), Color);
			Line(Point - FVector3(0.0f, Size, 0.0f), Point + FVector3(0.0f, Size, 0.0f), Color);
			Line(Point - FVector3(0.0f, 0.0f, Size), Point + FVector3(0.0f, 0.0f, Size), Color);
			if (bLineToTarget && Target.IsValid() && Registry.IsValid(Target))
			{
				Line(Point, TargetPoint(Target, FVector3()), Faint);
			}
			return Point;
		};
		auto Perpendicular = [](const FVector3& Axis) {
			const FVector3 Helper = FMath::Abs(Axis.Z) > 0.9f ? FVector3::ForwardVector : FVector3::UpVector;
			return FVector3::Cross(Helper, Axis).GetNormalized();
		};

		if (const FFixedJointComponent* Fixed = Registry.TryGet<FFixedJointComponent>(Entity))
		{
			Anchor(Fixed->Anchor, Fixed->Target, true);
		}
		if (const FHingeJointComponent* Hinge = Registry.TryGet<FHingeJointComponent>(Entity))
		{
			const FVector3 Point = Anchor(Hinge->Anchor, Hinge->Target, true);
			const FVector3 Axis  = World.TransformVector(Hinge->Axis).GetNormalized();
			const FVector3 U     = Perpendicular(Axis);
			Line(Point - Axis * 40.0f, Point + Axis * 40.0f, Color);
			Circle(Point, U, FVector3::Cross(Axis, U), 20.0f, Faint);
		}
		if (const FDistanceJointComponent* Distance = Registry.TryGet<FDistanceJointComponent>(Entity))
		{
			const FVector3 Point = Anchor(Distance->Anchor, Distance->Target, false);
			Line(Point, TargetPoint(Distance->Target, Distance->TargetAnchor), Color);
		}
		if (const FBallJointComponent* Ball = Registry.TryGet<FBallJointComponent>(Entity))
		{
			const FVector3 Point = Anchor(Ball->Anchor, Ball->Target, true);
			const FVector3 Axis  = World.TransformVector(Ball->Axis).GetNormalized();
			Line(Point, Point + Axis * 40.0f, Color);
			if (Ball->ConeAngle < 179.0f)
			{
				const float    Angle = FMath::DegreesToRadians(FMath::Clamp(Ball->ConeAngle, 0.0f, 179.0f));
				const FVector3 U     = Perpendicular(Axis);
				Circle(Point + Axis * (30.0f * FMath::Cos(Angle)), U, FVector3::Cross(Axis, U), 30.0f * FMath::Sin(Angle), Faint);
			}
		}
	}

	// 선택한 점광원/스포트라이트의 영향 반경과 원뿔, 데칼 상자를 뷰포트 위에 선으로 그린다 (ImGui 오버레이, 깊이 무시)
	void DrawSelectedLightShapes(FEditorContext& Context, const ImVec2& ImagePosition, const ImVec2& ImageSize)
	{
		const FMatrix4x4 ViewProjection = Context.Camera->GetViewProjectionMatrix();
		ImDrawList*      DrawList       = ImGui::GetWindowDrawList();
		const ImU32      OuterColor     = ImGui::ColorConvertFloat4ToU32(ImVec4(FEditorTheme::Warning.x, FEditorTheme::Warning.y, FEditorTheme::Warning.z, 0.85f));
		const ImU32      InnerColor     = ImGui::ColorConvertFloat4ToU32(ImVec4(FEditorTheme::Warning.x, FEditorTheme::Warning.y, FEditorTheme::Warning.z, 0.4f));

		auto Project = [&](const FVector3& P, ImVec2& Out) {
			const FVector3 Clip = ViewProjection.TransformPosition(P);
			const float    W    = P.X * ViewProjection.M[0][3] + P.Y * ViewProjection.M[1][3] + P.Z * ViewProjection.M[2][3] + ViewProjection.M[3][3];
			if (W <= 1.0f)
			{
				return false; // 카메라 뒤/근평면 근처
			}
			Out = ImVec2(ImagePosition.x + (Clip.X / W * 0.5f + 0.5f) * ImageSize.x, ImagePosition.y + (0.5f - Clip.Y / W * 0.5f) * ImageSize.y);
			return true;
		};
		auto Line = [&](const FVector3& A, const FVector3& B, ImU32 Color) {
			ImVec2 SA;
			ImVec2 SB;
			if (Project(A, SA) && Project(B, SB))
			{
				DrawList->AddLine(SA, SB, Color, 1.5f);
			}
		};
		auto Circle = [&](const FVector3& Center, const FVector3& AxisU, const FVector3& AxisV, float Radius, ImU32 Color) {
			constexpr int32 Segments = 48;
			FVector3        Previous = Center + AxisU * Radius;
			for (int32 Index = 1; Index <= Segments; ++Index)
			{
				const float    Angle = FMath::TwoPi * static_cast<float>(Index) / static_cast<float>(Segments);
				const FVector3 Point = Center + (AxisU * FMath::Cos(Angle) + AxisV * FMath::Sin(Angle)) * Radius;
				Line(Previous, Point, Color);
				Previous = Point;
			}
		};

		DrawList->PushClipRect(ImagePosition, ImVec2(ImagePosition.x + ImageSize.x, ImagePosition.y + ImageSize.y), true);
		const FRegistry& Registry = Context.Scene->GetRegistry();
		for (const FEntity Entity : Context.Selection.GetEntities())
		{
			const FTransformComponent* Transform = Registry.TryGet<FTransformComponent>(Entity);
			if (Transform == nullptr)
			{
				continue;
			}
			const FVector3 Position = Transform->GetWorldPosition();
			if (const FPointLightComponent* Point = Registry.TryGet<FPointLightComponent>(Entity))
			{
				Circle(Position, FVector3::ForwardVector, FVector3::RightVector, Point->Radius, OuterColor);
				Circle(Position, FVector3::ForwardVector, FVector3::UpVector, Point->Radius, OuterColor);
				Circle(Position, FVector3::RightVector, FVector3::UpVector, Point->Radius, OuterColor);
			}
			if (const FDecalComponent* Decal = Registry.TryGet<FDecalComponent>(Entity))
			{
				// 데칼 상자 12모서리 + 투영 방향(로컬 -Z) 화살표
				const FMatrix4x4 DecalToWorld = FDecalMath::MakeDecalToWorld(Transform->WorldMatrix, Decal->Size);
				FVector3         Corners[8];
				for (int32 Corner = 0; Corner < 8; ++Corner)
				{
					Corners[Corner] = DecalToWorld.TransformPosition(
						FVector3((Corner & 1) ? 0.5f : -0.5f, (Corner & 2) ? 0.5f : -0.5f, (Corner & 4) ? 0.5f : -0.5f));
				}
				const int32 Edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
				                             { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
				for (const auto& Edge : Edges)
				{
					Line(Corners[Edge[0]], Corners[Edge[1]], OuterColor);
				}
				const FVector3 Top    = DecalToWorld.TransformPosition(FVector3(0.0f, 0.0f, 0.5f));
				const FVector3 Bottom = DecalToWorld.TransformPosition(FVector3(0.0f, 0.0f, -0.5f));
				Line(Top, Bottom, InnerColor);
			}
			if (const FIrradianceVolumeComponent* Volume = Registry.TryGet<FIrradianceVolumeComponent>(Entity))
			{
				// DDGI 프로브 볼륨 상자 (월드 축 정렬, 엔티티 위치가 가운데 — 프로브 자체는 컴포넌트 DebugProbes / r.DDGI.ShowProbes로 렌더러가 그린다)
				const FVector3 Half = Volume->HalfExtents;
				FVector3       Corners[8];
				for (int32 Corner = 0; Corner < 8; ++Corner)
				{
					Corners[Corner] = Position + FVector3((Corner & 1) ? Half.X : -Half.X, (Corner & 2) ? Half.Y : -Half.Y, (Corner & 4) ? Half.Z : -Half.Z);
				}
				const int32 Edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
				for (const auto& Edge : Edges)
				{
					Line(Corners[Edge[0]], Corners[Edge[1]], OuterColor);
				}
			}
			if (const FSpotLightComponent* Spot = Registry.TryGet<FSpotLightComponent>(Entity))
			{
				const FVector3 Direction = Transform->GetWorldForward();
				const FVector3 Helper    = FMath::Abs(Direction.Z) > 0.99f ? FVector3::ForwardVector : FVector3::UpVector;
				const FVector3 AxisU     = FVector3::Cross(Helper, Direction).GetNormalized();
				const FVector3 AxisV     = FVector3::Cross(Direction, AxisU);
				const float    Cones[2]  = { Spot->OuterConeAngle, FMath::Min(Spot->InnerConeAngle, Spot->OuterConeAngle) };
				for (int32 Cone = 0; Cone < 2; ++Cone)
				{
					const float    Angle  = FMath::DegreesToRadians(FMath::Clamp(Cones[Cone], 0.0f, 80.0f));
					const FVector3 Center = Position + Direction * (Spot->Radius * FMath::Cos(Angle));
					const float    Radius = Spot->Radius * FMath::Sin(Angle);
					const ImU32    Color  = Cone == 0 ? OuterColor : InnerColor;
					Circle(Center, AxisU, AxisV, Radius, Color);
					if (Cone == 0)
					{
						for (int32 Edge = 0; Edge < 4; ++Edge)
						{
							const float EdgeAngle = FMath::HalfPi * static_cast<float>(Edge);
							Line(Position, Center + (AxisU * FMath::Cos(EdgeAngle) + AxisV * FMath::Sin(EdgeAngle)) * Radius, Color);
						}
					}
				}
			}
			if (const FAreaLightComponent* Area = Registry.TryGet<FAreaLightComponent>(Entity))
			{
				// 면광원 (Phase 52): 면 윤곽 + 법선 화살표(빛 나가는 쪽) + 문 덮개 원뿔 가장자리 4개 (면 크기 = 트랜스폼 스케일 무시)
				const FVector3 Forward = Transform->GetWorldForward();
				FVector3       Right   = Transform->WorldMatrix.GetAxisY();
				Right                  = (Right - Forward * FVector3::Dot(Right, Forward)).GetNormalized();
				const FVector3 Up      = FVector3::Cross(Forward, Right);
				const float    HalfW   = FMath::Max(Area->Width, 0.1f) * 0.5f;
				const float    HalfH   = FMath::Max(Area->Height, 0.1f) * 0.5f;
				if (Area->Shape == static_cast<int32>(EAreaLightShape::Disc))
				{
					constexpr int32 Segments = 48;
					FVector3        Previous = Position + Up * HalfH;
					for (int32 Index = 1; Index <= Segments; ++Index)
					{
						const float    Angle = FMath::TwoPi * static_cast<float>(Index) / static_cast<float>(Segments);
						const FVector3 Point = Position + Up * (FMath::Cos(Angle) * HalfH) + Right * (FMath::Sin(Angle) * HalfW);
						Line(Previous, Point, OuterColor);
						Previous = Point;
					}
				}
				else
				{
					const FVector3 Corners[4] = { Position - Right * HalfW - Up * HalfH, Position - Right * HalfW + Up * HalfH,
					                              Position + Right * HalfW + Up * HalfH, Position + Right * HalfW - Up * HalfH };
					for (int32 Corner = 0; Corner < 4; ++Corner)
					{
						Line(Corners[Corner], Corners[(Corner + 1) % 4], OuterColor);
					}
					Line(Corners[0], Corners[2], InnerColor);
					Line(Corners[1], Corners[3], InnerColor);
				}
				const float ArrowLength = FMath::Clamp(FMath::Max(HalfW, HalfH), 20.0f, 200.0f);
				Line(Position, Position + Forward * ArrowLength, OuterColor);
				if (Area->bTwoSided)
				{
					Line(Position, Position - Forward * ArrowLength, InnerColor);
				}
				if (Area->BarnDoorAngle < 90.0f)
				{
					const float Angle = FMath::DegreesToRadians(FMath::Clamp(Area->BarnDoorAngle, 0.0f, 89.0f));
					const float Reach = FMath::Max(Area->BarnDoorLength, ArrowLength);
					const FVector3 Sides[4] = { Right, -Right, Up, -Up };
					const FVector3 Edges[4] = { Right * HalfW, -Right * HalfW, Up * HalfH, -Up * HalfH };
					for (int32 Side = 0; Side < 4; ++Side)
					{
						const FVector3 Start = Position + Edges[Side];
						Line(Start, Start + (Forward * FMath::Cos(Angle) + Sides[Side] * FMath::Sin(Angle)) * Reach, InnerColor);
					}
				}
			}
			DrawJointShape(Context.Scene, Entity, *Transform, Line, Circle);
		}
		DrawList->PopClipRect();
	}
} // namespace

FViewportPanel::FViewportPanel()  = default;
FViewportPanel::~FViewportPanel() = default;

void FViewportPanel::Shutdown()
{
	ColliderOverlay.reset();
	UIRenderer.reset();
	NavMeshDebug.reset();
	DebugDrawRenderer.reset();
	Grid.reset();
	SelectionOutline.reset();
	RenderTarget.reset();
	DesiredWidth  = 0;
	DesiredHeight = 0;
}

void FViewportPanel::PrepareFrame(FEditorContext& Context)
{
	if (DesiredWidth < GMinViewportSize || DesiredHeight < GMinViewportSize)
	{
		return;
	}

	const bool bNeedsCreate = RenderTarget == nullptr;
	const bool bNeedsResize = !bNeedsCreate && (RenderTarget->GetWidth() != DesiredWidth || RenderTarget->GetHeight() != DesiredHeight);
	if (!bNeedsCreate && !bNeedsResize)
	{
		return;
	}

	// 이전 타깃은 진행 중인 프레임이 참조할 수 있으므로 지연 해제
	if (RenderTarget)
	{
		RenderTarget->ShutdownDeferred(*Context.Rhi);
	}
	RenderTarget = std::make_unique<FD3D12RenderTarget>();
	if (!RenderTarget->Init(Context.Rhi->GetDevice(), Context.Rhi->GetSrvAllocator(), DesiredWidth, DesiredHeight, L"EditorViewport"))
	{
		RenderTarget.reset();
		return;
	}
	Context.Camera->SetAspectRatio(static_cast<float>(DesiredWidth) / static_cast<float>(DesiredHeight));
}

void FViewportPanel::Draw(FEditorContext& Context, const FInput& Input)
{
	bWasUsingGizmo = bUsingGizmo;
	bHovered       = false;
	bUsingGizmo    = false;
	bGizmoOver     = false;
	bFocused       = false;
	if (!bOpen)
	{
		return;
	}

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	const bool bVisible = ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_CAMERA, "뷰포트", "Viewport").c_str(), &bOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();

	bFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (bVisible)
	{
		const ImVec2 Avail = ImGui::GetContentRegionAvail();
		DesiredWidth       = static_cast<uint32>(FMath::Max(Avail.x, 1.0f));
		DesiredHeight      = static_cast<uint32>(FMath::Max(Avail.y, 1.0f));

		if (RenderTarget)
		{
			const ImVec2 ImagePosition = ImGui::GetCursorScreenPos();
			ImageMin                   = FVector2(ImagePosition.x, ImagePosition.y);
			const ImVec2 ImageSize(static_cast<float>(RenderTarget->GetWidth()), static_cast<float>(RenderTarget->GetHeight()));
			ImGui::Image(static_cast<ImTextureID>(RenderTarget->GetSrv().Gpu.ptr), ImageSize);
			bHovered = ImGui::IsItemHovered();
			// 콘텐츠 브라우저에서 끌어 놓기: 모델/파티클 배치, 머티리얼은 커서 아래 메시에 지정
			if (ImGui::BeginDragDropTarget())
			{
				if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
				{
					const ImVec2 Mouse = ImGui::GetMousePos();
					HandleAssetDrop(Context, *Paths, FVector2(Mouse.x - ImagePosition.x, Mouse.y - ImagePosition.y), FVector2(ImageSize.x, ImageSize.y));
				}
				ImGui::EndDragDropTarget();
			}

			// 2D 모드: 편집 카메라 고정 + 휠 줌/드래그 팬 (게임 카메라로 보는 중에는 하지 않는다)
			if (Is2DMode() && Context.Camera == EditCamera && Context.Camera != nullptr)
			{
				Camera2D.Enforce(*Context.Camera);
				if (bHovered)
				{
					Handle2DCamera(Context, FVector2(ImagePosition.x, ImagePosition.y), FVector2(ImageSize.x, ImageSize.y));
				}
			}
			Update2DGrid(Context);

			// 플레이 중 빙의(F8로 전환): 뷰포트 입력은 게임 것 — 라이트 모양/편집 도구/기즈모/클릭 선택/단축키/툴바 없음
			const bool bCanEdit = Context.CanEditInViewport();
			if (bCanEdit)
			{
				DrawSelectedLightShapes(Context, ImagePosition, ImageSize);
				// 충돌 모양 외곽선 (2D 콜라이더·타일맵 충돌·2D 관절·이동기 캡슐·3D 콜라이더) — 선택한 것, 토글이면 전부
				if (!ColliderOverlay)
				{
					ColliderOverlay = std::make_unique<FCollider2DOverlay>();
				}
				ColliderOverlay->Draw(Context, FVector2(ImagePosition.x, ImagePosition.y), FVector2(ImageSize.x, ImageSize.y), bShowAllColliders2D);
			}
			// 편집 도구(지형/폴리지 브러시)가 마우스를 가져가면 기즈모/클릭 선택을 하지 않는다
			const bool bToolCaptured = bCanEdit && ToolOverlay &&
			                           ToolOverlay(Context, Input, FVector2(ImagePosition.x, ImagePosition.y), FVector2(ImageSize.x, ImageSize.y), bHovered);
			if (bCanEdit && !bToolCaptured)
			{
				DrawGizmo(Context, FVector2(ImagePosition.x, ImagePosition.y), FVector2(ImageSize.x, ImageSize.y));
			}

			// 기즈모 위/사용 중이 아닌 곳에서 드래그 없이 좌클릭을 놓으면 선택 (툴바를 그린 뒤 처리)
			const ImVec2 DragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
			const bool   bDragged  = (DragDelta.x * DragDelta.x + DragDelta.y * DragDelta.y) > 16.0f;
			// 플레이 중 게임 UI가 포인터를 가져갔으면(버튼 클릭 등) 엔티티를 선택하지 않는다
			const bool   bPick     = bCanEdit && bHovered && !bGizmoOver && !bUsingGizmo && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !bDragged &&
			                   !Input.IsMouseButtonDown(EMouseButton::Right) && !(Context.bPlaying && bGameUIWantsPointer) && !bToolCaptured;

			// 단축키 (뷰포트 위, 카메라 조작 중 아님)
			// Ctrl 조합(Ctrl+R 셰이더 재로드, Ctrl+S 저장 등)은 에디터 단축키이므로 제외
			if (bCanEdit && bHovered && !Input.IsMouseButtonDown(EMouseButton::Right) && !ImGui::GetIO().KeyCtrl)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_W)) GizmoOperation = EGizmoOperation::Translate;
				if (ImGui::IsKeyPressed(ImGuiKey_E)) GizmoOperation = EGizmoOperation::Rotate;
				if (ImGui::IsKeyPressed(ImGuiKey_R)) GizmoOperation = EGizmoOperation::Scale;
				if (ImGui::IsKeyPressed(ImGuiKey_F)) FocusSelection(Context);
			}

			// 플레이 모드 표시: 뷰포트 테두리 (재생 초록, 일시정지 노랑, 빙의 해제 강조색)
			if (Context.bPlaying)
			{
				const ImU32 BorderColor = !Context.bPossessed ? ImGui::GetColorU32(FEditorTheme::Accent)
				                          : Context.bPaused   ? IM_COL32(255, 200, 70, 255)
				                                              : IM_COL32(90, 230, 110, 255);
				ImGui::GetWindowDrawList()->AddRect(ImagePosition, ImVec2(ImagePosition.x + ImageSize.x, ImagePosition.y + ImageSize.y),
				                                    BorderColor, 0.0f, 0, 3.0f);
			}

			// 툴바 오버레이 (빙의 중에는 게임 화면만)
			if (bCanEdit)
			{
				ImGui::SetCursorScreenPos(ImVec2(ImagePosition.x + 8.0f, ImagePosition.y + 8.0f));
				DrawToolbar(Context);
			}
			if (Context.bPlaying)
			{
				// 빙의 상태 안내 (오른쪽 아래)
				const char* const Label = Context.bPossessed ? ICON_FA_GAMEPAD " 빙의 중 — F8 빙의 해제" : ICON_FA_ARROW_POINTER " 빙의 해제됨 — F8 다시 빙의";
				const ImVec2      Size  = ImGui::CalcTextSize(Label);
				const ImVec2      Pos(ImagePosition.x + ImageSize.x - Size.x - 12.0f, ImagePosition.y + ImageSize.y - Size.y - 10.0f);
				ImDrawList* const List = ImGui::GetWindowDrawList();
				List->AddRectFilled(ImVec2(Pos.x - 6.0f, Pos.y - 3.0f), ImVec2(Pos.x + Size.x + 6.0f, Pos.y + Size.y + 3.0f), IM_COL32(0, 0, 0, 150), 4.0f);
				List->AddText(Pos, Context.bPossessed ? ImGui::GetColorU32(FEditorTheme::Success) : IM_COL32(235, 235, 235, 255), Label);
			}
			if (StatOverlay)
			{
				StatOverlay(FVector2(ImagePosition.x, ImagePosition.y), FVector2(ImageSize.x, ImageSize.y));
			}

			if (bPick && !ImGui::IsAnyItemHovered())
			{
				const ImVec2 Mouse = ImGui::GetMousePos();
				PickEntity(Context, FVector2(Mouse.x - ImagePosition.x, Mouse.y - ImagePosition.y), FVector2(ImageSize.x, ImageSize.y));
			}

			// 박스 선택: 기즈모·편집 도구(타일/지형 브러시)·툴바·게임 UI가 아닌 빈 곳에서 왼쪽을 눌러 끌기. Shift = 추가, Ctrl = 토글
			{
				const ImVec2   Mouse = ImGui::GetMousePos();
				const FVector2 LocalMouse(Mouse.x - ImagePosition.x, Mouse.y - ImagePosition.y);
				const bool     bCanBox = bCanEdit && !bToolCaptured && !bUsingGizmo && !(Context.bPlaying && bGameUIWantsPointer);
				if (bCanBox && bHovered && !bGizmoOver && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() &&
				    !Input.IsMouseButtonDown(EMouseButton::Right) && !ImGui::GetIO().KeyAlt)
				{
					bBoxPending  = true;
					bBoxDragging = false;
					BoxStart     = LocalMouse;
				}
				if (bBoxPending && !bCanBox)
				{
					bBoxPending  = false; // 기즈모/도구가 마우스를 가져감
					bBoxDragging = false;
				}
				if (bBoxPending)
				{
					const FVector2 Delta = LocalMouse - BoxStart;
					bBoxDragging         = bBoxDragging || Delta.X * Delta.X + Delta.Y * Delta.Y > 16.0f;
					const Editor2DMath::FScreenRect Rect = Editor2DMath::MakeScreenRect(BoxStart, LocalMouse);
					if (bBoxDragging)
					{
						ImDrawList* const List = ImGui::GetWindowDrawList();
						const ImVec2      Min(ImagePosition.x + Rect.Min.X, ImagePosition.y + Rect.Min.Y);
						const ImVec2      Max(ImagePosition.x + Rect.Max.X, ImagePosition.y + Rect.Max.Y);
						const ImVec4&     Accent = FEditorTheme::Accent;
						List->PushClipRect(ImagePosition, ImVec2(ImagePosition.x + ImageSize.x, ImagePosition.y + ImageSize.y), true);
						List->AddRectFilled(Min, Max, ImGui::ColorConvertFloat4ToU32(ImVec4(Accent.x, Accent.y, Accent.z, 0.12f)));
						List->AddRect(Min, Max, ImGui::ColorConvertFloat4ToU32(ImVec4(Accent.x, Accent.y, Accent.z, 0.9f)), 0.0f, 0, 1.5f);
						List->PopClipRect();
					}
					if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
					{
						if (bBoxDragging)
						{
							const ImGuiIO&                     IO   = ImGui::GetIO();
							const Editor2DMath::EBoxSelectMode Mode = IO.KeyCtrl    ? Editor2DMath::EBoxSelectMode::Toggle
							                                          : IO.KeyShift ? Editor2DMath::EBoxSelectMode::Add
							                                                        : Editor2DMath::EBoxSelectMode::Replace;
							BoxSelect(Context, Rect, FVector2(ImageSize.x, ImageSize.y), Mode);
						}
						bBoxPending  = false;
						bBoxDragging = false;
					}
				}
			}
		}
		else
		{
			ImGui::TextDisabled("렌더 타깃 준비 중...");
		}
	}
	ImGui::End();
}

void FViewportPanel::DrawFrozen(const char* Message)
{
	bHovered    = false;
	bUsingGizmo = false;
	bGizmoOver  = false;
	bFocused    = false;
	if (!bOpen)
	{
		return;
	}
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	const bool bVisible = ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_CAMERA, "뷰포트", "Viewport").c_str(), &bOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	if (bVisible && RenderTarget)
	{
		const ImVec2 ImagePosition = ImGui::GetCursorScreenPos();
		const ImVec2 ImageSize(static_cast<float>(RenderTarget->GetWidth()), static_cast<float>(RenderTarget->GetHeight()));
		ImGui::Image(static_cast<ImTextureID>(RenderTarget->GetSrv().Gpu.ptr), ImageSize);
		ImDrawList* const List = ImGui::GetWindowDrawList();
		List->AddRectFilled(ImagePosition, ImVec2(ImagePosition.x + ImageSize.x, ImagePosition.y + ImageSize.y), IM_COL32(0, 0, 0, 70));
		List->AddRect(ImagePosition, ImVec2(ImagePosition.x + ImageSize.x, ImagePosition.y + ImageSize.y), IM_COL32(255, 200, 70, 255), 0.0f, 0, 3.0f);
		const ImVec2 Size = ImGui::CalcTextSize(Message);
		const ImVec2 Pos(ImagePosition.x + (ImageSize.x - Size.x) * 0.5f, ImagePosition.y + 16.0f);
		List->AddRectFilled(ImVec2(Pos.x - 10.0f, Pos.y - 5.0f), ImVec2(Pos.x + Size.x + 10.0f, Pos.y + Size.y + 5.0f), IM_COL32(0, 0, 0, 190), 4.0f);
		List->AddText(Pos, ImGui::GetColorU32(FEditorTheme::Warning), Message);
	}
	ImGui::End();
}

void FViewportPanel::RenderScene(FEditorContext& Context)
{
	if (!RenderTarget)
	{
		return;
	}
	ID3D12GraphicsCommandList* CommandList = Context.Rhi->GetCommandList();

	if (!SelectionOutline)
	{
		SelectionOutline = std::make_unique<FSelectionOutline>();
		if (!SelectionOutline->Init(*Context.Rhi, Context.Renderer->GetShaderLibrary()))
		{
			SelectionOutline.reset();
		}
		else
		{
			// 지형 선택 아웃라인: 씬 렌더러의 지형 렌더러가 이번 프레임 청크로 마스크를 그린다
			FTerrainRenderer& Terrain         = Context.Renderer->GetTerrainRenderer();
			SelectionOutline->ExtraMaskFilter = [&Terrain](const std::vector<FEntity>& Selected) { return Terrain.HasTerrain(Selected); };
			SelectionOutline->ExtraMask = [&Terrain](ID3D12GraphicsCommandList* List, const FMatrix4x4& ViewProjection, const std::vector<FEntity>& Selected) {
				Terrain.RenderMask(List, ViewProjection, Selected);
			};
		}
	}

	// 씬 렌더러가 HDR로 그린 뒤 톤매핑해 뷰포트 타깃(sRGB RTV)에 기록 → 선택 아웃라인 합성
	RenderTarget->Begin(CommandList, nullptr);
	Context.Renderer->Render(*Context.Scene, *Context.Camera, RenderTarget->GetOutput());
	if (bShowGrid && Context.CanEditInViewport()) // 빙의 중에는 게임 화면 그대로
	{
		RenderGrid(Context);
	}
	RenderNavMeshDebug(Context);
	RenderDebugDraw(Context);
	if (SelectionOutline && Context.CanEditInViewport()) // 빙의 중에는 선택 아웃라인 없음 (계층 창 선택은 유지)
	{
		SelectionOutline->Render(*Context.Scene, *Context.Resources, *Context.Camera, Context.Selection.GetEntities(), RenderTarget->GetOutput(),
		                         &Context.Renderer->GetSkinPalettes());
	}
	// 플레이 중: 게임 UI를 맨 위에 (레이아웃/입력은 OnUpdate의 FUISystem::Update가 이미 처리). 빙의 해제 중에는 편집 화면이므로 그리지 않는다
	if (Context.bPlaying && Context.bPossessed)
	{
		RenderGameUI(Context);
	}
	RenderTarget->End(CommandList);
}

float FViewportPanel::GetAspectRatio(float Fallback) const
{
	if (!RenderTarget || RenderTarget->GetHeight() == 0)
	{
		return Fallback;
	}
	return static_cast<float>(RenderTarget->GetWidth()) / static_cast<float>(RenderTarget->GetHeight());
}

bool FViewportPanel::ReloadShaders(bool bForceRecompile)
{
	const bool bOutlineOk = !SelectionOutline || SelectionOutline->ReloadShaders(bForceRecompile);
	const bool bGridOk    = !Grid || Grid->ReloadShaders(bForceRecompile);
	const bool bUIOk      = !UIRenderer || UIRenderer->ReloadShaders(bForceRecompile);
	const bool bNavOk     = !NavMeshDebug || NavMeshDebug->ReloadShaders(bForceRecompile);
	const bool bDebugOk   = !DebugDrawRenderer || DebugDrawRenderer->ReloadShaders(bForceRecompile);
	return bOutlineOk && bGridOk && bUIOk && bNavOk && bDebugOk;
}

void FViewportPanel::SetNavMeshTriangles(std::vector<FVector3> Triangles)
{
	PendingNavMeshTriangles = std::move(Triangles);
	bNavMeshTrianglesDirty  = true;
}

void FViewportPanel::RenderNavMeshDebug(FEditorContext& Context)
{
	if (!bShowNavMesh)
	{
		return;
	}
	if (!NavMeshDebug)
	{
		NavMeshDebug = std::make_unique<FNavMeshDebugRenderer>();
		if (!NavMeshDebug->Init(*Context.Rhi, Context.Renderer->GetShaderLibrary()))
		{
			NavMeshDebug.reset();
			bShowNavMesh = false; // 셰이더 오류 시 매 프레임 재시도하지 않는다
			return;
		}
		bNavMeshTrianglesDirty = true;
	}
	if (bNavMeshTrianglesDirty)
	{
		NavMeshDebug->SetNavMeshTriangles(PendingNavMeshTriangles);
		bNavMeshTrianglesDirty = false;
	}
	// 플레이 중: 이동 중인 엔티티의 경로
	if (Context.bPlaying && Context.AI != nullptr)
	{
		Context.Scene->GetRegistry().View<FTransformComponent>().Each([&](FEntity Entity, FTransformComponent&) {
			if (const std::vector<FVector3>* Path = Context.AI->GetMovePath(Entity))
			{
				NavMeshDebug->AddPath(*Path);
			}
		});
	}
	// 뷰포트 크기 깊이 (TAAU면 출력 해상도로 옮긴 깊이)
	const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = Context.Renderer->GetOverlayDepthDsv(RenderTarget->GetWidth(), RenderTarget->GetHeight());
	if (Dsv.ptr == 0)
	{
		return; // 씬 깊이를 쓸 수 없는 모드 (픽셀 아트 등)
	}
	NavMeshDebug->Render(*Context.Camera, RenderTarget->GetOutput(), Dsv, true);
}

void FViewportPanel::RenderDebugDraw(FEditorContext& Context)
{
	const FDebugDraw& Lines = FDebugDraw::Get();
	if (Lines.GetLines().empty())
	{
		return;
	}
	if (!DebugDrawRenderer)
	{
		DebugDrawRenderer = std::make_unique<FDebugDrawRenderer>();
		if (!DebugDrawRenderer->Init(*Context.Rhi, Context.Renderer->GetShaderLibrary()))
		{
			E_LOG(LogEditor, Error, "디버그 선 렌더러 초기화 실패");
		}
	}
	if (!DebugDrawRenderer->IsInitialized())
	{
		return; // 셰이더 오류: 매 프레임 다시 시도하지 않는다 (셰이더 다시 로드로는 복구 안 됨 — 에디터 재시작)
	}
	// 씬 깊이는 뷰포트와 같은 크기일 때만 (픽셀 아트 모드는 깊이 테스트 선을 그리지 않는다)
	DebugDrawRenderer->Render(Lines, *Context.Camera, RenderTarget->GetOutput(),
	                          Context.Renderer->GetOverlayDepthDsv(RenderTarget->GetWidth(), RenderTarget->GetHeight()));
}

FUIRect FViewportPanel::GetGameUIViewport() const
{
	if (!RenderTarget)
	{
		return FUIRect();
	}
	return FUIRect(FVector2::ZeroVector, FVector2(static_cast<float>(RenderTarget->GetWidth()), static_cast<float>(RenderTarget->GetHeight())));
}

void FViewportPanel::RenderGameUI(FEditorContext& Context)
{
	if (!UIRenderer)
	{
		UIRenderer = std::make_unique<FUIRenderer>();
		if (!UIRenderer->Init(*Context.Rhi, Context.Renderer->GetShaderLibrary(), *Context.Resources, FD3D12RHI::RenderTargetFormat))
		{
			UIRenderer.reset();
			return;
		}
	}
	GameUIDrawList.Clear();
	FUISystem::Paint(*Context.Scene, GameUIDrawList);
	UIRenderer->Render(GameUIDrawList, RenderTarget->GetOutput(), Context.ContentDirectory);
}

void FViewportPanel::RenderGrid(FEditorContext& Context)
{
	if (!Grid)
	{
		Grid = std::make_unique<FEditorGrid>();
		if (!Grid->Init(*Context.Rhi, Context.Renderer->GetShaderLibrary()))
		{
			Grid.reset();
			bShowGrid = false; // 셰이더 오류 시 매 프레임 재시도하지 않는다
			return;
		}
	}
	// 씬 렌더러의 깊이를 그대로 사용 (뷰포트와 같은 크기일 때만 — TAAU면 출력 해상도로 옮긴 깊이)
	const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = Context.Renderer->GetOverlayDepthDsv(RenderTarget->GetWidth(), RenderTarget->GetHeight());
	if (Dsv.ptr == 0)
	{
		return;
	}
	// 2D 모드(편집 카메라로 볼 때): X-Z 평면 격자 — 칸·원점·도트는 Update2DGrid가 정한 값
	Grid->bPlaneXZ     = Is2DMode() && Context.Camera == EditCamera;
	Grid->Cell2D       = Grid2DCell;
	Grid->Origin2D     = Grid2DOrigin;
	Grid->PixelStep2D  = Grid2DPixel;
	Grid->PlaneDepth2D = Grid2DDepth;
	Grid->Render(*Context.Camera, RenderTarget->GetOutput(), Dsv);
}

void FViewportPanel::Set2DMode(FEditorContext& Context, bool bEnable)
{
	if (EditCamera == nullptr || bEnable == Is2DMode())
	{
		return;
	}
	FCamera& Camera = *EditCamera; // 편집 카메라 (앱 소유, 패널이 조작한다 — 직교 토글과 같음)
	if (bEnable)
	{
		Camera2D.Enter(Camera);
		if (!Context.Selection.GetEntities().empty())
		{
			FocusSelection(Context);
		}
	}
	else
	{
		Camera2D.Exit(Camera);
	}
}

void FViewportPanel::Handle2DCamera(FEditorContext& Context, const FVector2& ImagePosition, const FVector2& ImageSize)
{
	FCamera&       Camera = *Context.Camera;
	const ImGuiIO& IO     = ImGui::GetIO();
	if (IO.MouseWheel != 0.0f && !IO.KeyCtrl)
	{
		Camera2D.Zoom(Camera, IO.MouseWheel, FVector2(IO.MousePos.x - ImagePosition.X, IO.MousePos.y - ImagePosition.Y), ImageSize);
	}
	if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))
	{
		Camera2D.Pan(Camera, FVector2(IO.MouseDelta.x, IO.MouseDelta.y), ImageSize);
	}
}

void FViewportPanel::Update2DGrid(FEditorContext& Context)
{
	// 기본: 설정 칸, 원점 0, 도트 없음. 선택 타일맵이 있으면 그 셀(원점 = 엔티티 위치), 선택 스프라이트면 도트 간격
	Grid2DCell   = FVector2(FMath::Max(Grid2DCellSize, 1.0f), FMath::Max(Grid2DCellSize, 1.0f));
	Grid2DOrigin = FVector2::ZeroVector;
	Grid2DPixel  = FVector2::ZeroVector;
	Grid2DDepth  = 0.0f;
	FRegistry&    Registry = Context.Scene->GetRegistry();
	const FEntity Primary  = Context.SelectedEntity;
	if (!Registry.IsValid(Primary) || !Registry.Has<FTransformComponent>(Primary))
	{
		return;
	}
	const FMatrix4x4& World = Context.Scene->GetTransform(Primary).WorldMatrix;
	const FVector2    Scale(World.GetAxisX().Length(), World.GetAxisZ().Length());
	if (FTilemapComponent* Tilemap = Registry.TryGet<FTilemapComponent>(Primary))
	{
		FVector2 CellSize;
		if (Editor2DScene::GetTilemapCellSize(*Tilemap, CellSize))
		{
			Grid2DCell   = CellSize * Scale;
			Grid2DOrigin = FVector2(World.GetOrigin().X, World.GetOrigin().Z);
			Grid2DDepth  = World.GetOrigin().Y;
			if (const std::shared_ptr<const FTilesetAsset> Tileset = Sprite2DRuntime::ResolveTileset(*Tilemap);
			    Tileset != nullptr && Tileset->TileWidth > 0 && Tileset->TileHeight > 0)
			{
				Grid2DPixel = FVector2(Grid2DCell.X / static_cast<float>(Tileset->TileWidth), Grid2DCell.Y / static_cast<float>(Tileset->TileHeight));
			}
		}
	}
	else if (FSpriteComponent* Sprite = Registry.TryGet<FSpriteComponent>(Primary))
	{
		const Sprite2DRuntime::FSpriteDisplay Display = Sprite2DRuntime::ResolveSprite(*Sprite);
		FVector2                              Quad[4];
		if (Display.Asset != nullptr && Display.SliceIndex >= 0 && Editor2DScene::GetSpriteLocalQuad(*Sprite, Quad))
		{
			const FSpriteSlice& Slice = Display.Asset->Slices[static_cast<size_t>(Display.SliceIndex)];
			const FVector2      Size  = SpriteMath::ComputeSize(Slice, Display.Asset->UnitsPerPixel, Sprite->Size);
			if (Slice.W > 0 && Slice.H > 0)
			{
				Grid2DPixel = FVector2(Size.X / static_cast<float>(Slice.W), Size.Y / static_cast<float>(Slice.H)) * Scale;
				// 도트 격자 원점 = 스프라이트 사각형 왼쪽 아래 모서리 (텍셀 경계에 맞춤)
				FVector2 Min = Quad[0];
				for (const FVector2& Corner : Quad)
				{
					Min = FVector2(FMath::Min(Min.X, Corner.X), FMath::Min(Min.Y, Corner.Y));
				}
				const FVector3 Corner = World.TransformPosition(FVector3(Min.X, 0.0f, Min.Y));
				Grid2DOrigin          = FVector2(Corner.X, Corner.Z);
				Grid2DDepth           = World.GetOrigin().Y;
			}
		}
	}
}

void FViewportPanel::ToggleOrthographic(FCamera& Camera)
{
	if (Camera.IsOrthographic())
	{
		Camera.SetPerspectiveMode();
		return;
	}
	// 지금 보이는 크기를 유지: 아래를 보고 있으면 바닥(Z=0)까지, 아니면 10m를 초점 거리로
	const FVector3 Forward       = Camera.GetForwardVector();
	float          FocusDistance = 1000.0f;
	if (Forward.Z < -0.05f && Camera.GetPosition().Z > 0.0f)
	{
		FocusDistance = FMath::Clamp(-Camera.GetPosition().Z / Forward.Z, 100.0f, 20000.0f);
	}
	Camera.SetOrthographic(FEditorCameraState::ComputeMatchingOrthoHeight(Camera.GetFovYDegrees(), FocusDistance), Camera.GetAspectRatio(),
	                       Camera.GetNearZ(), Camera.GetFarZ());
}

void FViewportPanel::DrawToolbar(FEditorContext& Context)
{
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
	ImGui::BeginGroup();
	// 뷰포트 위에 떠 있는 도구 막대: 반투명 어두운 버튼
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.08f, 0.08f, 0.78f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.22f, 0.90f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, FEditorTheme::Accent);

	const auto ToolButton = [&](const char* Label, EGizmoOperation Operation, const char* Tooltip) {
		const bool bActive = GizmoOperation == Operation;
		if (bActive)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		}
		if (ImGui::Button(Label))
		{
			GizmoOperation = Operation;
		}
		if (bActive)
		{
			ImGui::PopStyleColor();
		}
		ImGui::SetItemTooltip("%s", Tooltip);
		ImGui::SameLine();
	};
	ToolButton(ICON_FA_UP_DOWN_LEFT_RIGHT, EGizmoOperation::Translate, "이동 (W)");
	ToolButton(ICON_FA_ROTATE, EGizmoOperation::Rotate, "회전 (E)");
	ToolButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, EGizmoOperation::Scale, "스케일 (R)");

	if (ImGui::Button(bGizmoLocal ? ICON_FA_CUBE " 로컬" : ICON_FA_GLOBE " 월드"))
	{
		bGizmoLocal = !bGizmoLocal;
	}
	ImGui::SetItemTooltip("기즈모 기준 좌표계 전환");

	const auto ToggleButton = [&](const char* Label, bool& bValue, const char* Tooltip) {
		ImGui::SameLine();
		if (bValue)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		}
		const bool bClicked = ImGui::Button(Label);
		if (bValue)
		{
			ImGui::PopStyleColor();
		}
		if (bClicked)
		{
			bValue = !bValue;
		}
		ImGui::SetItemTooltip("%s", Tooltip);
	};
	ToggleButton(ICON_FA_BORDER_ALL, bShowGrid, "그리드/월드 축 표시 (주 100cm, 보조 10cm)");
	ToggleButton(ICON_FA_ROUTE, bShowNavMesh, "내비메시 표시 (플레이 중에는 이동 경로도) — 굽기: 도구 → 내비메시 굽기");
	ToggleButton(ICON_FA_MAGNET, Snap.bEnabled, "기즈모 스냅 (Ctrl을 누른 동안 일시 반전)");
	ToggleButton(ICON_FA_DRAW_POLYGON, bShowAllColliders2D, "모든 충돌 모양 외곽선 (끄면 선택한 엔티티만) — 2D 콜라이더·타일맵 충돌·2D 관절·이동기, 3D 콜라이더");

	// 2D 모드 (편집 카메라를 +Y에서 -Y를 보는 직교로 고정)
	ImGui::SameLine();
	{
		const bool b2D = Is2DMode();
		ImGui::BeginDisabled(!Context.CanEditInViewport() || Context.Camera != EditCamera);
		if (b2D)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		}
		if (ImGui::Button("2D"))
		{
			Set2DMode(Context, !b2D);
		}
		if (b2D)
		{
			ImGui::PopStyleColor();
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("2D 모드: +Y에서 -Y를 보는 직교 카메라 (휠 = 커서 기준 확대, 가운데/오른쪽 드래그 = 이동, F = 선택 맞춤), X-Z 격자, 평면 기즈모");
	}

	// 편집 카메라 투영 (플레이 중에는 게임 카메라 설정을 따른다)
	ImGui::SameLine();
	ImGui::BeginDisabled(!Context.CanEditInViewport() || Is2DMode()); // 빙의 해제 중에는 편집 카메라, 2D 모드는 항상 직교
	const bool bOrthographic = Context.Camera->IsOrthographic();
	if (ImGui::Button(bOrthographic ? ICON_FA_VECTOR_SQUARE " 직교" : ICON_FA_VIDEO " 원근"))
	{
		ToggleOrthographic(*Context.Camera);
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("편집 카메라 투영 전환 (직교: 휠로 확대/축소)");

	// 편집 카메라 이동 속도 (우클릭 + 휠과 같은 값, 범위도 FFlyCameraController와 같다)
	if (CameraSpeed != nullptr)
	{
		ImGui::SameLine();
		const std::string SpeedLabel = std::format(ICON_FA_GAUGE_HIGH " {:.0f}##CameraSpeed", *CameraSpeed);
		if (ImGui::Button(SpeedLabel.c_str()))
		{
			ImGui::OpenPopup("CameraSpeed");
		}
		ImGui::SetItemTooltip("편집 카메라 이동 속도 (cm/초) — 우클릭 + 휠로도 바꾼다, Shift = 가속");
		if (ImGui::BeginPopup("CameraSpeed"))
		{
			ImGui::SetNextItemWidth(220.0f);
			ImGui::SliderFloat("속도 (cm/초)", CameraSpeed, 10.0f, 50000.0f, "%.0f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
			constexpr float Presets[] = { 100.0f, 500.0f, 1000.0f, 2500.0f, 5000.0f, 10000.0f };
			for (size_t Index = 0; Index < std::size(Presets); ++Index)
			{
				if (Index > 0)
				{
					ImGui::SameLine();
				}
				const std::string PresetLabel = std::format("{:.0f}", Presets[Index]);
				if (ImGui::Button(PresetLabel.c_str()))
				{
					*CameraSpeed = Presets[Index];
				}
			}
			ImGui::EndPopup();
		}
	}
	ImGui::SameLine();
	if (ImGui::ArrowButton("##SnapOptions", ImGuiDir_Down))
	{
		ImGui::OpenPopup("SnapOptions");
	}
	ImGui::SetItemTooltip("스냅 간격");
	if (ImGui::BeginPopup("SnapOptions"))
	{
		ImGui::SetNextItemWidth(120.0f);
		ImGui::DragFloat("이동 (cm)", &Snap.TranslateStep, 1.0f, 0.1f, 10000.0f, "%.1f");
		ImGui::SetNextItemWidth(120.0f);
		ImGui::DragFloat("회전 (도)", &Snap.RotateStepDegree, 0.5f, 0.1f, 180.0f, "%.1f");
		ImGui::SetNextItemWidth(120.0f);
		ImGui::DragFloat("스케일", &Snap.ScaleStep, 0.01f, 0.001f, 10.0f, "%.3f");
		ImGui::SeparatorText("2D");
		int32             Snap2DIndex   = static_cast<int32>(Snap2D);
		const char* const Snap2DNames[] = { "끔", "격자 칸", "픽셀 (도트)" };
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::Combo("2D 이동 스냅", &Snap2DIndex, Snap2DNames, 3))
		{
			Snap2D = static_cast<ESnap2D>(Snap2DIndex);
		}
		ImGui::SetItemTooltip("2D 모드 이동 기즈모를 절대 위치로 맞춘다: 격자 칸(선택 타일맵 셀 또는 아래 값) / 선택 스프라이트·타일맵의 도트(UnitsPerPixel)");
		ImGui::SetNextItemWidth(120.0f);
		ImGui::DragFloat("2D 격자 (cm)", &Grid2DCellSize, 1.0f, 1.0f, 10000.0f, "%.1f");
		ImGui::SetItemTooltip("타일맵을 고르지 않았을 때 2D 격자 칸 크기");
		ImGui::EndPopup();
	}

	ImGui::EndGroup();
	ImGui::PopStyleColor(3);
	ImGui::PopStyleVar();
}

void FViewportPanel::DrawGizmo(FEditorContext& Context, const FVector2& ImagePosition, const FVector2& ImageSize)
{
	FScene& Scene  = *Context.Scene;
	FEntity Entity = Context.SelectedEntity;
	if (!Scene.GetRegistry().IsValid(Entity))
	{
		return;
	}
	FTransformComponent* Transform = Scene.GetRegistry().TryGet<FTransformComponent>(Entity);
	if (Transform == nullptr)
	{
		return;
	}

	ImGuizmo::SetOrthographic(Context.Camera->IsOrthographic());
	ImGuizmo::SetDrawlist();
	ImGuizmo::SetRect(ImagePosition.X, ImagePosition.Y, ImageSize.X, ImageSize.Y);

	// 2D 모드: 평면 제약 (이동 X/Z, 회전 Y축 = 2D 각, 스케일 X/Z)
	const bool          b2D       = Is2DMode() && Context.Camera == EditCamera;
	ImGuizmo::OPERATION Operation = b2D ? static_cast<ImGuizmo::OPERATION>(ImGuizmo::TRANSLATE_X | ImGuizmo::TRANSLATE_Z) : ImGuizmo::TRANSLATE;
	switch (GizmoOperation)
	{
	case EGizmoOperation::Rotate: Operation = b2D ? ImGuizmo::ROTATE_Y : ImGuizmo::ROTATE; break;
	case EGizmoOperation::Scale:  Operation = b2D ? static_cast<ImGuizmo::OPERATION>(ImGuizmo::SCALE_X | ImGuizmo::SCALE_Z) : ImGuizmo::SCALE; break;
	default:                      break;
	}
	// 스케일은 항상 로컬 기준
	const ImGuizmo::MODE Mode = (bGizmoLocal || GizmoOperation == EGizmoOperation::Scale) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
	// 2D 이동 스냅은 절대 위치(격자 칸/도트)로 아래에서 맞춘다 — ImGuizmo 스냅(시작점 기준 간격)은 쓰지 않는다
	const bool bAbsoluteSnap2D = b2D && GizmoOperation == EGizmoOperation::Translate && Snap2D != ESnap2D::Off;

	// ImGuizmo는 float[16]을 행벡터 규약(이동이 [12..14])으로 다루므로 FMatrix4x4 메모리를 그대로 전달
	const FMatrix4x4 View       = Context.Camera->GetViewMatrix();
	const FMatrix4x4 Projection = Context.Camera->GetProjectionMatrix();
	const FMatrix4x4 OldWorld   = Transform->WorldMatrix;
	FMatrix4x4       World      = OldWorld;

	float        SnapValues[3] = {};
	const float* SnapPointer   = bAbsoluteSnap2D ? nullptr : Snap.GetSnapValues(GizmoOperation, ImGui::GetIO().KeyCtrl, SnapValues);

	bool bManipulated = ImGuizmo::Manipulate(&View.M[0][0], &Projection.M[0][0], Operation, Mode, &World.M[0][0], nullptr, SnapPointer);
	if (bManipulated && bAbsoluteSnap2D && !ImGui::GetIO().KeyCtrl) // Ctrl = 일시 해제
	{
		// 격자 칸 = 엔티티 위치를 월드 칸 배수로, 도트 = 스프라이트 사각형 왼쪽 아래(도트 격자 원점)를 월드 도트 배수로 (엔티티와 함께 움직이는 원점은 기준으로 쓰지 않는다)
		const bool     bPixel = Snap2D == ESnap2D::Pixel && Grid2DPixel.X > 0.0f && Grid2DPixel.Y > 0.0f;
		const FVector2 Step   = bPixel ? Grid2DPixel : Grid2DCell;
		const FVector2 Offset = bPixel ? Grid2DOrigin - FVector2(OldWorld.M[3][0], OldWorld.M[3][2]) : FVector2::ZeroVector;
		World.M[3][0]         = Editor2DMath::SnapToStep(World.M[3][0] + Offset.X, Step.X) - Offset.X;
		World.M[3][2]         = Editor2DMath::SnapToStep(World.M[3][2] + Offset.Y, Step.Y) - Offset.Y;
	}
	bUsingGizmo             = ImGuizmo::IsUsing();
	bGizmoOver              = ImGuizmo::IsOver();

	// Alt+드래그 (언리얼과 동일): 조작을 시작하는 순간 선택을 복제하고 복제본을 움직인다. 원본은 제자리에 남는다.
	// 복제본의 월드 행렬이 원본과 같으므로 기즈모 조작은 끊기지 않고, 복제+이동이 Undo 한 단계로 합쳐진다
	if (bUsingGizmo && !bWasUsingGizmo && ImGui::GetIO().KeyAlt)
	{
		FEditorActions::DuplicateSelection(Context);
		Entity    = Context.SelectedEntity;
		Transform = Scene.GetRegistry().TryGet<FTransformComponent>(Entity);
		if (Transform == nullptr)
		{
			return;
		}
	}

	if (bManipulated)
	{
		// 주 선택의 월드 변화량을 다른 최상위 선택에도 적용: NewWorld = World * Inverse(OldPrimary) * NewPrimary
		// (주 선택의 조상이 함께 선택됐다면 조상만 움직여도 주 선택은 정확히 World가 된다)
		const FMatrix4x4 Delta = OldWorld.GetInverse() * World;
		for (FEntity Target : FSceneEditOps::GetTopLevel(Scene, Context.Selection.GetEntities()))
		{
			FTransformComponent* TargetTransform = Scene.GetRegistry().TryGet<FTransformComponent>(Target);
			if (TargetTransform == nullptr)
			{
				continue;
			}
			const FMatrix4x4 NewWorld = Target == Entity ? World : TargetTransform->WorldMatrix * Delta;

			// 월드 → 로컬: Local = World * Inverse(ParentWorld)
			const FMatrix4x4 Local = NewWorld * Scene.GetParentWorldMatrix(Target).GetInverse(); // 소켓 부착이면 소켓 기준
			Local.Decompose(TargetTransform->Position, TargetTransform->Rotation, TargetTransform->Scale);
		}
		Scene.UpdateTransforms();

		static constexpr const char* Labels[] = { "이동", "회전", "스케일" };
		Context.MarkEdited(Labels[static_cast<int32>(GizmoOperation)]);
	}
}

void FViewportPanel::PickEntity(FEditorContext& Context, const FVector2& LocalPixel, const FVector2& ImageSize)
{
	if (ImageSize.X <= 0.0f || ImageSize.Y <= 0.0f)
	{
		return;
	}

	const float NdcX = (LocalPixel.X / ImageSize.X) * 2.0f - 1.0f;
	const float NdcY = 1.0f - (LocalPixel.Y / ImageSize.Y) * 2.0f;
	const FRay  Ray  = FRay::FromNdc(NdcX, NdcY, Context.Camera->GetViewProjectionMatrix().GetInverse());

	float   ClosestDistance = std::numeric_limits<float>::max();
	FEntity Closest         = RaycastVisibleMeshes(Context, Ray, ClosestDistance);

	// 지형: 높이맵 레이캐스트 (메시보다 가까우면 지형)
	std::vector<FTerrainInstance> Terrains;
	GatherTerrains(*Context.Scene, Terrains);
	for (const FTerrainInstance& Terrain : Terrains)
	{
		float Distance = 0.0f;
		if (TerrainMath::Raycast(*Terrain.Data, Terrain.Frame, Ray.Origin, Ray.Direction, ClosestDistance, Distance) && Distance < ClosestDistance)
		{
			ClosestDistance = Distance;
			Closest         = Terrain.Entity;
		}
	}

	// 2D: 스프라이트 사각형·칠한 타일맵 칸·2D 콜라이더 (정렬 레이어/순번/깊이로 맨 앞). 2D 모드는 메시보다 우선, 아니면 광선 거리로 비교
	{
		const float EdgeTolerance = Context.Camera->IsOrthographic() ? Context.Camera->GetOrthoHeight() / FMath::Max(ImageSize.Y, 1.0f) * 6.0f : 5.0f;
		float       Distance2D    = 0.0f;
		const FEntity Hit2D       = Editor2DScene::Pick(*Context.Scene, Ray, EdgeTolerance, Distance2D);
		if (Hit2D.IsValid() && ((Is2DMode() && Context.Camera == EditCamera) || Distance2D <= ClosestDistance))
		{
			ClosestDistance = Distance2D;
			Closest         = Hit2D;
		}
	}

	// 모델에서 생성된 하위 노드(저장되지 않음)를 찍으면 모델 루트를 선택한다 (언리얼의 액터 선택과 같은 동작)
	FRegistry& Registry = Context.Scene->GetRegistry();
	while (Closest.IsValid() && Registry.Has<FTransientComponent>(Closest))
	{
		const FEntity Parent = Context.Scene->GetParent(Closest);
		if (!Parent.IsValid())
		{
			break;
		}
		Closest = Parent;
	}

	if (ImGui::GetIO().KeyCtrl)
	{
		if (Closest.IsValid())
		{
			Context.ToggleSelection(Closest);
		}
	}
	else
	{
		Context.Select(Closest);
	}
}

std::vector<FEntity> FViewportPanel::CollectBoxSelection(FEditorContext& Context, const Editor2DMath::FScreenRect& Rect, const FVector2& ImageSize) const
{
	std::vector<FEntity> Result;
	if (Context.Scene == nullptr || Context.Camera == nullptr || ImageSize.X <= 0.0f || ImageSize.Y <= 0.0f)
	{
		return Result;
	}
	FScene&          Scene          = *Context.Scene;
	FRegistry&       Registry       = Scene.GetRegistry();
	const FMatrix4x4 ViewProjection = Context.Camera->GetViewProjectionMatrix();

	// 선택 단위(모델 하위 노드 → 모델 루트 — 클릭 선택과 같은 규칙)별 화면 경계 합
	struct FCandidate
	{
		Editor2DMath::FScreenRect Rect;
		bool                      bValid = false;
	};
	std::vector<FEntity>                    Order;
	std::unordered_map<FEntity, FCandidate> Candidates;
	const auto Add = [&](FEntity Entity, const FBox& Bounds) {
		while (Entity.IsValid() && Registry.Has<FTransientComponent>(Entity))
		{
			const FEntity Parent = Scene.GetParent(Entity);
			if (!Parent.IsValid())
			{
				break;
			}
			Entity = Parent;
		}
		Editor2DMath::FScreenRect Screen;
		if (!Editor2DMath::ProjectBoundsToScreen(Bounds, ViewProjection, ImageSize, Screen))
		{
			return;
		}
		auto [It, bInserted] = Candidates.try_emplace(Entity);
		if (bInserted)
		{
			Order.push_back(Entity);
		}
		Editor2DMath::UnionScreenRect(It->second.Rect, It->second.bValid, Screen);
	};

	Registry.View<FTransformComponent, FStaticMeshComponent>().Each([&](FEntity Entity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
		if (const FStaticMesh* Mesh = MeshComponent.bVisible ? Context.Resources->GetMesh(MeshComponent.Mesh) : nullptr)
		{
			Add(Entity, Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix));
		}
	});
	Registry.View<FTransformComponent>().Each([&](FEntity Entity, FTransformComponent&) {
		if (const FSpriteComponent* Sprite = Registry.TryGet<FSpriteComponent>(Entity); Sprite != nullptr && !Sprite->bVisible)
		{
			return; // 숨긴 스프라이트는 클릭 선택처럼 고르지 않는다
		}
		FBox Bounds;
		if (Editor2DScene::AddBounds(Scene, Entity, Bounds))
		{
			Add(Entity, Bounds);
		}
	});

	for (const FEntity Entity : Order)
	{
		const FCandidate& Candidate = Candidates[Entity];
		if (Candidate.bValid && Editor2DMath::IsScreenRectInside(Candidate.Rect, Rect))
		{
			Result.push_back(Entity);
		}
	}
	return Result;
}

void FViewportPanel::BoxSelect(FEditorContext& Context, const Editor2DMath::FScreenRect& Rect, const FVector2& ImageSize, Editor2DMath::EBoxSelectMode Mode)
{
	const std::vector<FEntity> Hits     = CollectBoxSelection(Context, Rect, ImageSize);
	const std::vector<FEntity> Combined = Editor2DMath::CombineBoxSelection(Context.Selection.GetEntities(), Hits, Mode);
	// 주 선택: 추가/토글에서 기존 주 선택이 남아 있으면 유지, 아니면 결과의 마지막
	const bool    bKeepPrimary = Mode != Editor2DMath::EBoxSelectMode::Replace &&
	                          std::find(Combined.begin(), Combined.end(), Context.SelectedEntity) != Combined.end();
	const FEntity Primary      = bKeepPrimary ? Context.SelectedEntity : (Combined.empty() ? NullEntity : Combined.back());
	Context.SelectMany(Combined, Primary);
}

void FViewportPanel::DropAssets(FEditorContext& Context, const std::vector<std::filesystem::path>& Paths, const FVector2& LocalPixel)
{
	HandleAssetDrop(Context, Paths, LocalPixel, GetImageSize());
}

FVector2 FViewportPanel::GetImageSize() const
{
	return RenderTarget ? FVector2(static_cast<float>(RenderTarget->GetWidth()), static_cast<float>(RenderTarget->GetHeight())) : FVector2::ZeroVector;
}

void FViewportPanel::FocusSelection(FEditorContext& Context)
{
	FScene& Scene = *Context.Scene;
	FBox    Bounds;
	std::vector<FEntity> Meshes;
	for (FEntity Entity : Context.Selection.GetEntities())
	{
		if (!Scene.GetRegistry().IsValid(Entity))
		{
			continue;
		}
		Meshes.clear();
		FSelectionOutline::CollectOutlinedEntities(Scene, Entity, Meshes);
		for (FEntity MeshEntity : Meshes)
		{
			if (const FStaticMesh* Mesh = Context.Resources->GetMesh(Scene.GetRegistry().Get<FStaticMeshComponent>(MeshEntity).Mesh))
			{
				Bounds.AddBox(Mesh->GetLocalBounds().TransformBy(Scene.GetTransform(MeshEntity).WorldMatrix));
			}
		}
		// 2D 표시(스프라이트·타일맵·2D 콜라이더) 경계, 그것도 없으면(빈 엔티티, 조명 등) 위치만
		if (!Editor2DScene::AddBounds(Scene, Entity, Bounds) && Meshes.empty())
		{
			Bounds.AddPoint(Scene.GetTransform(Entity).GetWorldPosition());
		}
	}
	if (!Bounds.IsValid())
	{
		return;
	}
	FCamera& Camera = *Context.Camera;
	if (Is2DMode() && Context.Camera == EditCamera)
	{
		Camera2D.FitBounds(Camera, Bounds); // 2D: 가운데 + 직교 높이 (회전·깊이 고정)
		return;
	}
	Camera.SetPosition(FEditorCameraState::ComputeFramingPosition(Bounds, Camera.GetForwardVector(), Camera.GetFovYDegrees(), Camera.GetAspectRatio()));
	if (Camera.IsOrthographic())
	{
		Camera.SetOrthographic(FEditorCameraState::ComputeFramingOrthoHeight(Bounds, Camera.GetAspectRatio()), Camera.GetAspectRatio(),
		                       Camera.GetNearZ(), Camera.GetFarZ());
	}
}

void FViewportPanel::FrameAll2D(FEditorContext& Context)
{
	if (!Is2DMode() || Context.Camera != EditCamera)
	{
		return;
	}
	FBox Bounds;
	Context.Scene->GetRegistry().View<FTransformComponent>().Each([&](FEntity Entity, FTransformComponent&) {
		Editor2DScene::AddBounds(*Context.Scene, Entity, Bounds);
	});
	Camera2D.FitBounds(*Context.Camera, Bounds);
}

bool FViewportPanel::VerifyPick(FEditorContext& Context, FEntity Target, bool bFocus, FEntity& OutPicked)
{
	OutPicked = FEntity();
	if (!RenderTarget || !Context.Scene->GetRegistry().IsValid(Target))
	{
		return false;
	}
	if (bFocus)
	{
		Context.Select(Target);
		FocusSelection(Context);
	}

	// 대상 메시들의 월드 경계 중심 → 화면 픽셀
	FBox                 Bounds;
	std::vector<FEntity> Meshes;
	FSelectionOutline::CollectOutlinedEntities(*Context.Scene, Target, Meshes);
	for (FEntity MeshEntity : Meshes)
	{
		if (const FStaticMesh* Mesh = Context.Resources->GetMesh(Context.Scene->GetRegistry().Get<FStaticMeshComponent>(MeshEntity).Mesh))
		{
			Bounds.AddBox(Mesh->GetLocalBounds().TransformBy(Context.Scene->GetTransform(MeshEntity).WorldMatrix));
		}
	}
	if (!Bounds.IsValid())
	{
		return false;
	}
	const FVector2 ImageSize(static_cast<float>(RenderTarget->GetWidth()), static_cast<float>(RenderTarget->GetHeight()));
	const FVector4 Clip = Context.Camera->GetViewProjectionMatrix().TransformVector4(FVector4(Bounds.GetCenter(), 1.0f));
	if (Clip.W <= 0.0f)
	{
		return false;
	}
	const FVector2 Pixel((Clip.X / Clip.W * 0.5f + 0.5f) * ImageSize.X, (0.5f - Clip.Y / Clip.W * 0.5f) * ImageSize.Y);
	if (Pixel.X < 0.0f || Pixel.Y < 0.0f || Pixel.X >= ImageSize.X || Pixel.Y >= ImageSize.Y)
	{
		return false;
	}
	Context.ClearSelection();
	PickEntity(Context, Pixel, ImageSize);
	OutPicked = Context.SelectedEntity;
	return true;
}

FEntity FViewportPanel::RaycastMesh(FEditorContext& Context, const FVector2& LocalPixel, const FVector2& ImageSize, FRay& OutRay, float& OutDistance) const
{
	const float NdcX = (LocalPixel.X / ImageSize.X) * 2.0f - 1.0f;
	const float NdcY = 1.0f - (LocalPixel.Y / ImageSize.Y) * 2.0f;
	OutRay           = FRay::FromNdc(NdcX, NdcY, Context.Camera->GetViewProjectionMatrix().GetInverse());
	OutDistance      = std::numeric_limits<float>::max();

	return RaycastVisibleMeshes(Context, OutRay, OutDistance);
}

void FViewportPanel::HandleAssetDrop(FEditorContext& Context, const std::vector<std::filesystem::path>& Paths, const FVector2& LocalPixel,
                                     const FVector2& ImageSize)
{
	const auto Notify = [&](const std::string& Message, bool bError) {
		if (Context.Notify)
		{
			Context.Notify(Message, bError);
		}
	};
	if (Context.bPlaying)
	{
		Notify("플레이 중에는 에셋을 배치할 수 없습니다", true);
		return;
	}
	if (ImageSize.X <= 0.0f || ImageSize.Y <= 0.0f)
	{
		return;
	}

	FRay          Ray;
	float         HitDistance = 0.0f;
	const FEntity Hit         = RaycastMesh(Context, LocalPixel, ImageSize, Ray, HitDistance);
	// 놓을 위치: 메시에 닿으면 그 표면(스킨 메시는 경계 상자), 아니면 바닥면(Z = 0), 하늘을 향하면 카메라 앞 5m
	FVector3 DropPoint = Ray.GetPoint(500.0f);
	if (Hit.IsValid())
	{
		DropPoint = Ray.GetPoint(HitDistance);
	}
	else if (Ray.Direction.Z < -1.0e-4f)
	{
		DropPoint = Ray.GetPoint(-Ray.Origin.Z / Ray.Direction.Z);
	}

	FScene&    Scene    = *Context.Scene;
	FRegistry& Registry = Scene.GetRegistry();
	std::vector<FEntity> Placed;
	bool                 bEdited = false;
	// 2D 에셋은 Y = 0 평면 (2D 모드면 커서 아래, 아니면 커서 광선과 평면 교차 — 평행하면 위 위치에 깊이만 0)
	FVector3 DropPoint2D = DropPoint;
	if (!Editor2DScene::RayToPlaneY(Ray, 0.0f, DropPoint2D))
	{
		DropPoint2D.Y = 0.0f;
	}
	for (size_t Index = 0; Index < Paths.size(); ++Index)
	{
		const std::filesystem::path& Path      = Paths[Index];
		std::wstring                 Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		const FVector3 Position = DropPoint + FVector3(0.0f, 150.0f * static_cast<float>(Index), 0.0f); // 여러 개면 옆으로 나란히

		if (Extension == L".glb" || Extension == L".gltf" || Extension == L".fbx")
		{
			const FEntity Root = FModelLoader::LoadIntoScene(Path, Scene, *Context.Resources);
			if (Registry.IsValid(Root))
			{
				Scene.GetTransform(Root).Position = Position;
				Placed.push_back(Root);
			}
		}
		else if (Extension == L".eparticle")
		{
			const FEntity Emitter = Scene.CreateEntity(FStringConv::ToUtf8(Path.stem().wstring()));
			Scene.GetTransform(Emitter).Position                           = Position;
			Registry.Emplace<FParticleSystemComponent>(Emitter).Asset = FModelLoader::MakeAssetPath(Path);
			Placed.push_back(Emitter);
		}
		else if (Extension == L".esprite" || Extension == L".eflipbook" || Extension == L".etileset")
		{
			// 2D: 스프라이트(첫 슬라이스) / 스프라이트 + 플립북 / 빈 타일맵 — 여러 개면 오른쪽(+X)으로 나란히
			const std::string AssetPath = FModelLoader::MakeAssetPath(Path);
			const FVector3    Position2D = DropPoint2D + FVector3(150.0f * static_cast<float>(Index), 0.0f, 0.0f);
			const FEntity     Created    = Extension == L".esprite"   ? Editor2DScene::CreateSprite(Scene, AssetPath, Position2D)
			                               : Extension == L".eflipbook" ? Editor2DScene::CreateFlipbook(Scene, AssetPath, Position2D)
			                                                            : Editor2DScene::CreateTilemap(Scene, AssetPath, Position2D);
			Placed.push_back(Created);
		}
		else if (Extension == L".eui")
		{
			// 화면 UI: 위치와 무관 (플레이 중 화면 전체 위에 그려진다)
			const FEntity UIEntity = Scene.CreateEntity(FStringConv::ToUtf8(Path.stem().wstring()));
			Registry.Emplace<FUIComponent>(UIEntity).Asset = FModelLoader::MakeAssetPath(Path);
			Placed.push_back(UIEntity);
		}
		else if (Extension == FPrefabLibrary::Extension)
		{
			const FEntity Root = FEditorActions::InstantiatePrefab(Context, Path, NullEntity);
			if (Registry.IsValid(Root))
			{
				Scene.GetTransform(Root).Position = Position;
				Placed.push_back(Root);
			}
		}
		else if (Extension == L".emat")
		{
			// 모델에서 생성된 하위 메시는 저장되지 않으므로 바꾸지 않는다
			if (!Hit.IsValid())
			{
				Notify("머티리얼은 메시 위에 놓으세요", true);
			}
			else if (Registry.Has<FTransientComponent>(Hit))
			{
				Notify("모델 안의 메시는 머티리얼을 바꿀 수 없습니다 (모델 파일의 내장 머티리얼)", true);
			}
			else
			{
				FStaticMeshComponent& Mesh = Registry.Get<FStaticMeshComponent>(Hit);
				Mesh.MaterialAsset         = FModelLoader::MakeAssetPath(Path);
				Mesh.Material              = FMaterialHandle{};
				FSceneAssetResolver::Resolve(Scene, *Context.Resources, Context.ContentDirectory);
				Context.Select(Hit);
				bEdited = true;
			}
		}
		else
		{
			Notify("뷰포트에 놓을 수 없는 에셋입니다: " + FStringConv::ToUtf8(Path.filename().wstring()), true);
		}
	}

	if (!Placed.empty())
	{
		Scene.UpdateTransforms();
		Context.SelectMany(Placed, Placed.back());
		bEdited = true;
	}
	if (bEdited)
	{
		Context.MarkEdited("에셋 배치");
	}
}
