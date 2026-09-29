// ImGuizmo.h는 imgui.h를 먼저 요구하고, Windows.h 매크로(TEXT 등)와 열거형 이름이 충돌하므로 Windows 헤더보다 먼저 포함
#include <imgui.h>
#include <ImGuizmo.h>

#include "Editor/Panels/ViewportPanel.h"

#include "Core/Input.h"
#include "Editor/EditorContext.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "Renderer/Camera.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

namespace
{
	constexpr uint32 GMinViewportSize = 16;
}

FViewportPanel::FViewportPanel()  = default;
FViewportPanel::~FViewportPanel() = default;

void FViewportPanel::Shutdown()
{
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
	bHovered    = false;
	bUsingGizmo = false;
	bGizmoOver  = false;
	if (!bOpen)
	{
		return;
	}

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	const bool bVisible = ImGui::Begin("뷰포트", &bOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();

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

			DrawGizmo(Context, FVector2(ImagePosition.x, ImagePosition.y), FVector2(ImageSize.x, ImageSize.y));

			// 기즈모 위/사용 중이 아닌 곳에서 드래그 없이 좌클릭을 놓으면 선택
			const ImVec2 DragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
			const bool   bDragged  = (DragDelta.x * DragDelta.x + DragDelta.y * DragDelta.y) > 16.0f;
			if (bHovered && !bGizmoOver && !bUsingGizmo && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !bDragged &&
			    !Input.IsMouseButtonDown(EMouseButton::Right))
			{
				const ImVec2 Mouse = ImGui::GetMousePos();
				PickEntity(Context, FVector2(Mouse.x - ImagePosition.x, Mouse.y - ImagePosition.y), FVector2(ImageSize.x, ImageSize.y));
			}

			// 단축키 (뷰포트 위, 카메라 조작 중 아님)
			// Ctrl 조합(Ctrl+R 셰이더 재로드, Ctrl+S 저장 등)은 에디터 단축키이므로 제외
			if (bHovered && !Input.IsMouseButtonDown(EMouseButton::Right) && !ImGui::GetIO().KeyCtrl)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_W)) GizmoOperation = EGizmoOperation::Translate;
				if (ImGui::IsKeyPressed(ImGuiKey_E)) GizmoOperation = EGizmoOperation::Rotate;
				if (ImGui::IsKeyPressed(ImGuiKey_R)) GizmoOperation = EGizmoOperation::Scale;
			}

			// 툴바 오버레이
			ImGui::SetCursorScreenPos(ImVec2(ImagePosition.x + 8.0f, ImagePosition.y + 8.0f));
			DrawToolbar();
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

	const float ClearColor[4] = { 0.12f, 0.2f, 0.36f, 1.0f };
	RenderTarget->Begin(CommandList, ClearColor);
	Context.Renderer->Render(*Context.Scene, *Context.Camera);
	RenderTarget->End(CommandList);
}

void FViewportPanel::DrawToolbar()
{
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
	ImGui::BeginGroup();

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
	ToolButton("이동", EGizmoOperation::Translate, "이동 (W)");
	ToolButton("회전", EGizmoOperation::Rotate, "회전 (E)");
	ToolButton("스케일", EGizmoOperation::Scale, "스케일 (R)");

	if (ImGui::Button(bGizmoLocal ? "로컬" : "월드"))
	{
		bGizmoLocal = !bGizmoLocal;
	}
	ImGui::SetItemTooltip("기즈모 기준 좌표계 전환");

	ImGui::EndGroup();
	ImGui::PopStyleVar();
}

void FViewportPanel::DrawGizmo(FEditorContext& Context, const FVector2& ImagePosition, const FVector2& ImageSize)
{
	FScene&       Scene  = *Context.Scene;
	const FEntity Entity = Context.SelectedEntity;
	if (!Scene.GetRegistry().IsValid(Entity))
	{
		return;
	}
	FTransformComponent* Transform = Scene.GetRegistry().TryGet<FTransformComponent>(Entity);
	if (Transform == nullptr)
	{
		return;
	}

	ImGuizmo::SetOrthographic(false);
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
	FMatrix4x4       World      = Transform->WorldMatrix;

	const bool bManipulated = ImGuizmo::Manipulate(&View.M[0][0], &Projection.M[0][0], Operation, Mode, &World.M[0][0]);
	bUsingGizmo             = ImGuizmo::IsUsing();
	bGizmoOver              = ImGuizmo::IsOver();

	if (bManipulated)
	{
		// 월드 → 로컬: Local = World * Inverse(ParentWorld)
		FMatrix4x4 Local = World;
		if (const FEntity Parent = Scene.GetParent(Entity); Parent.IsValid())
		{
			Local = World * Scene.GetTransform(Parent).WorldMatrix.GetInverse();
		}
		Local.Decompose(Transform->Position, Transform->Rotation, Transform->Scale);
		Scene.UpdateTransforms();
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

	Context.Select(Closest);
}
