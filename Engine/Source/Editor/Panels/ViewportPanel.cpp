// ImGuizmo.h는 imgui.h를 먼저 요구하고, Windows.h 매크로(TEXT 등)와 열거형 이름이 충돌하므로 Windows 헤더보다 먼저 포함
#include <imgui.h>
#include <ImGuizmo.h>

#include "Editor/Panels/ViewportPanel.h"

#include "Core/Input.h"
#include "Editor/EditorCameraState.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Editor/EditorGrid.h"
#include "Editor/SceneEditOps.h"
#include "Editor/SelectionOutline.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "Renderer/Camera.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/StaticMesh.h"
#include "Core/StringConv.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/SceneAssetResolver.h"
#include "Editor/EditorActions.h"
#include "Scene/Particles.h"
#include "Scene/Prefab.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cwctype>
#include <limits>

namespace
{
	constexpr uint32 GMinViewportSize = 16;
}

FViewportPanel::FViewportPanel()  = default;
FViewportPanel::~FViewportPanel() = default;

void FViewportPanel::Shutdown()
{
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

			DrawGizmo(Context, FVector2(ImagePosition.x, ImagePosition.y), FVector2(ImageSize.x, ImageSize.y));

			// 기즈모 위/사용 중이 아닌 곳에서 드래그 없이 좌클릭을 놓으면 선택 (툴바를 그린 뒤 처리)
			const ImVec2 DragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
			const bool   bDragged  = (DragDelta.x * DragDelta.x + DragDelta.y * DragDelta.y) > 16.0f;
			const bool   bPick     = bHovered && !bGizmoOver && !bUsingGizmo && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !bDragged &&
			                   !Input.IsMouseButtonDown(EMouseButton::Right);

			// 단축키 (뷰포트 위, 카메라 조작 중 아님)
			// Ctrl 조합(Ctrl+R 셰이더 재로드, Ctrl+S 저장 등)은 에디터 단축키이므로 제외
			if (bHovered && !Input.IsMouseButtonDown(EMouseButton::Right) && !ImGui::GetIO().KeyCtrl)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_W)) GizmoOperation = EGizmoOperation::Translate;
				if (ImGui::IsKeyPressed(ImGuiKey_E)) GizmoOperation = EGizmoOperation::Rotate;
				if (ImGui::IsKeyPressed(ImGuiKey_R)) GizmoOperation = EGizmoOperation::Scale;
				if (ImGui::IsKeyPressed(ImGuiKey_F)) FocusSelection(Context);
			}

			// 플레이 모드 표시: 뷰포트 테두리 (재생 초록, 일시정지 노랑)
			if (Context.bPlaying)
			{
				const ImU32 BorderColor = Context.bPaused ? IM_COL32(255, 200, 70, 255) : IM_COL32(90, 230, 110, 255);
				ImGui::GetWindowDrawList()->AddRect(ImagePosition, ImVec2(ImagePosition.x + ImageSize.x, ImagePosition.y + ImageSize.y),
				                                    BorderColor, 0.0f, 0, 3.0f);
			}

			// 툴바 오버레이
			ImGui::SetCursorScreenPos(ImVec2(ImagePosition.x + 8.0f, ImagePosition.y + 8.0f));
			DrawToolbar(Context);

			if (bPick && !ImGui::IsAnyItemHovered())
			{
				const ImVec2 Mouse = ImGui::GetMousePos();
				PickEntity(Context, FVector2(Mouse.x - ImagePosition.x, Mouse.y - ImagePosition.y), FVector2(ImageSize.x, ImageSize.y));
			}
		}
		else
		{
			ImGui::TextDisabled("렌더 타깃 준비 중...");
		}
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
	}

	// 씬 렌더러가 HDR로 그린 뒤 톤매핑해 뷰포트 타깃(sRGB RTV)에 기록 → 선택 아웃라인 합성
	RenderTarget->Begin(CommandList, nullptr);
	Context.Renderer->Render(*Context.Scene, *Context.Camera, RenderTarget->GetOutput());
	if (bShowGrid)
	{
		RenderGrid(Context);
	}
	if (SelectionOutline)
	{
		SelectionOutline->Render(*Context.Scene, *Context.Resources, *Context.Camera, Context.Selection.GetEntities(), RenderTarget->GetOutput(),
		                         &Context.Renderer->GetSkinPalettes());
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
	return bOutlineOk && bGridOk;
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
	// 씬 렌더러의 HDR 버퍼 깊이를 그대로 사용 (뷰포트와 같은 크기일 때만)
	const FD3D12RenderTarget* SceneColor = Context.Renderer->GetSceneColor();
	if (SceneColor == nullptr || !SceneColor->GetDesc().bWithDepth || SceneColor->GetWidth() != RenderTarget->GetWidth() ||
	    SceneColor->GetHeight() != RenderTarget->GetHeight())
	{
		return;
	}
	Grid->Render(*Context.Camera, RenderTarget->GetOutput(), SceneColor->GetDsv());
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
	ToggleButton(ICON_FA_MAGNET, Snap.bEnabled, "기즈모 스냅 (Ctrl을 누른 동안 일시 반전)");

	// 편집 카메라 투영 (플레이 중에는 게임 카메라 설정을 따른다)
	ImGui::SameLine();
	ImGui::BeginDisabled(Context.bPlaying);
	const bool bOrthographic = Context.Camera->IsOrthographic();
	if (ImGui::Button(bOrthographic ? ICON_FA_VECTOR_SQUARE " 직교" : ICON_FA_VIDEO " 원근"))
	{
		ToggleOrthographic(*Context.Camera);
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("편집 카메라 투영 전환 (직교: 휠로 확대/축소)");
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

	ImGuizmo::OPERATION Operation = ImGuizmo::TRANSLATE;
	switch (GizmoOperation)
	{
	case EGizmoOperation::Rotate: Operation = ImGuizmo::ROTATE; break;
	case EGizmoOperation::Scale:  Operation = ImGuizmo::SCALE; break;
	default:                      break;
	}
	// 스케일은 항상 로컬 기준
	const ImGuizmo::MODE Mode = (bGizmoLocal || GizmoOperation == EGizmoOperation::Scale) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

	// ImGuizmo는 float[16]을 행벡터 규약(이동이 [12..14])으로 다루므로 FMatrix4x4 메모리를 그대로 전달
	const FMatrix4x4 View       = Context.Camera->GetViewMatrix();
	const FMatrix4x4 Projection = Context.Camera->GetProjectionMatrix();
	const FMatrix4x4 OldWorld   = Transform->WorldMatrix;
	FMatrix4x4       World      = OldWorld;

	float        SnapValues[3] = {};
	const float* SnapPointer   = Snap.GetSnapValues(GizmoOperation, ImGui::GetIO().KeyCtrl, SnapValues);

	const bool bManipulated = ImGuizmo::Manipulate(&View.M[0][0], &Projection.M[0][0], Operation, Mode, &World.M[0][0], nullptr, SnapPointer);
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

	FEntity Closest;
	float   ClosestDistance = std::numeric_limits<float>::max();

	Context.Scene->GetRegistry().View<FTransformComponent, FStaticMeshComponent>().Each(
		[&](FEntity Entity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
			if (!MeshComponent.bVisible)
			{
				return;
			}
			const FStaticMesh* Mesh = Context.Resources->GetMesh(MeshComponent.Mesh);
			if (Mesh == nullptr)
			{
				return;
			}
			float Distance = 0.0f;
			if (Ray.Intersects(Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix), Distance) && Distance < ClosestDistance)
			{
				ClosestDistance = Distance;
				Closest         = Entity;
			}
		});

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
		// 메시가 없는 엔티티(빈 엔티티, 조명 등)는 위치만
		if (Meshes.empty())
		{
			Bounds.AddPoint(Scene.GetTransform(Entity).GetWorldPosition());
		}
	}
	if (!Bounds.IsValid())
	{
		return;
	}
	FCamera& Camera = *Context.Camera;
	Camera.SetPosition(FEditorCameraState::ComputeFramingPosition(Bounds, Camera.GetForwardVector(), Camera.GetFovYDegrees(), Camera.GetAspectRatio()));
	if (Camera.IsOrthographic())
	{
		Camera.SetOrthographic(FEditorCameraState::ComputeFramingOrthoHeight(Bounds, Camera.GetAspectRatio()), Camera.GetAspectRatio(),
		                       Camera.GetNearZ(), Camera.GetFarZ());
	}
}

FEntity FViewportPanel::RaycastMesh(FEditorContext& Context, const FVector2& LocalPixel, const FVector2& ImageSize, FRay& OutRay, float& OutDistance) const
{
	const float NdcX = (LocalPixel.X / ImageSize.X) * 2.0f - 1.0f;
	const float NdcY = 1.0f - (LocalPixel.Y / ImageSize.Y) * 2.0f;
	OutRay           = FRay::FromNdc(NdcX, NdcY, Context.Camera->GetViewProjectionMatrix().GetInverse());
	OutDistance      = std::numeric_limits<float>::max();

	FEntity Closest;
	Context.Scene->GetRegistry().View<FTransformComponent, FStaticMeshComponent>().Each(
		[&](FEntity Entity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
			const FStaticMesh* Mesh = MeshComponent.bVisible ? Context.Resources->GetMesh(MeshComponent.Mesh) : nullptr;
			float              Distance = 0.0f;
			if (Mesh != nullptr && OutRay.Intersects(Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix), Distance) && Distance < OutDistance)
			{
				OutDistance = Distance;
				Closest     = Entity;
			}
		});
	return Closest;
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
	// 놓을 위치: 메시에 닿으면 그 표면(경계 상자) 근처, 아니면 바닥면(Z = 0), 하늘을 향하면 카메라 앞 5m
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
