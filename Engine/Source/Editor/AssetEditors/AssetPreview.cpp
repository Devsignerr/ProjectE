#include "Editor/AssetEditors/AssetPreview.h"

#include "Editor/EditorGrid.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/StaticMesh.h"

#include <imgui.h>

FAssetPreview::FAssetPreview()  = default;
FAssetPreview::~FAssetPreview() = default;

bool FAssetPreview::Init(FD3D12RHI& Rhi)
{
	Target = std::make_unique<FD3D12RenderTarget>();
	if (!Target->Init(Rhi.GetDevice(), Rhi.GetSrvAllocator(), Width, Height, L"AssetPreview"))
	{
		Target.reset();
		return false;
	}
	Camera.SetPerspective(45.0f, static_cast<float>(Width) / static_cast<float>(Height), 1.0f, 100000.0f);
	return true;
}

void FAssetPreview::Shutdown(FD3D12RHI& Rhi)
{
	if (Target)
	{
		Target->ShutdownDeferred(Rhi);
		Target.reset();
	}
	Scene.Clear();
}

void FAssetPreview::AddDefaultLight()
{
	const FEntity Sun = Scene.CreateEntity("PreviewLight");
	Scene.GetTransform(Sun).Rotation = FQuat::FromEuler(-45.0f, 40.0f, 0.0f);
	Scene.GetRegistry().Emplace<FDirectionalLightComponent>(Sun).Intensity = 3.0f;
	Scene.UpdateTransforms();
}

FBox FAssetPreview::ComputeMeshBounds(const FResourceManager& Resources)
{
	FBox Bounds;
	FRegistry& Registry = Scene.GetRegistry();
	Registry.View<FTransformComponent, FStaticMeshComponent>().Each([&](FEntity, FTransformComponent& Transform, FStaticMeshComponent& Mesh) {
		if (const FStaticMesh* StaticMesh = Resources.GetMesh(Mesh.Mesh); StaticMesh != nullptr && Mesh.bVisible)
		{
			Bounds.AddBox(StaticMesh->GetLocalBounds().TransformBy(Transform.WorldMatrix));
		}
	});
	return Bounds;
}

void FAssetPreview::FrameMeshes(const FResourceManager& Resources, const FBox& Fallback)
{
	const FBox Bounds = ComputeMeshBounds(Resources);
	Orbit.Frame(Bounds.IsValid() ? Bounds : Fallback, Camera.GetFovYDegrees(), Camera.GetAspectRatio());
}

void FAssetPreview::DrawViewport(const FVector2& Size)
{
	bDrawn   = true;
	bHovered = false;
	if (Size.X < 8.0f || Size.Y < 8.0f)
	{
		return;
	}

	// 마우스 입력을 받는 영역 (창 드래그로 새지 않도록 보이지 않는 버튼)
	const ImVec2 Origin = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##PreviewViewport", ImVec2(Size.X, Size.Y),
	                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	bHovered             = ImGui::IsItemHovered();
	ViewportMin          = FVector2(Origin.x, Origin.y);
	ViewportMax          = FVector2(Origin.x + Size.X, Origin.y + Size.Y);
	const bool bActive   = ImGui::IsItemActive();
	ImDrawList* DrawList = ImGui::GetWindowDrawList();
	DrawList->AddRectFilled(Origin, ImVec2(Origin.x + Size.X, Origin.y + Size.Y), IM_COL32(12, 12, 14, 255));

	// 종횡비 유지 맞춤 (가운데 정렬)
	const float TargetAspect = static_cast<float>(Width) / static_cast<float>(Height);
	FVector2    Fit          = Size;
	if (Size.X / Size.Y > TargetAspect)
	{
		Fit.X = Size.Y * TargetAspect;
	}
	else
	{
		Fit.Y = Size.X / TargetAspect;
	}
	ImageMin  = FVector2(Origin.x + (Size.X - Fit.X) * 0.5f, Origin.y + (Size.Y - Fit.Y) * 0.5f);
	ImageSize = Fit;
	if (Target)
	{
		DrawList->AddImage(static_cast<ImTextureID>(Target->GetSrv().Gpu.ptr), ImVec2(ImageMin.X, ImageMin.Y),
		                   ImVec2(ImageMin.X + Fit.X, ImageMin.Y + Fit.Y));
	}

	const ImGuiIO& Io = ImGui::GetIO();
	if (bActive && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right)))
	{
		Orbit.Orbit(Io.MouseDelta.x * 0.4f, -Io.MouseDelta.y * 0.4f);
	}
	if (bActive && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
	{
		Orbit.Pan(Io.MouseDelta.x, Io.MouseDelta.y, Camera.GetFovYDegrees(), Fit.Y);
	}
	if (bHovered && Io.MouseWheel != 0.0f)
	{
		Orbit.Zoom(Io.MouseWheel);
	}
	// 오버레이(뼈대 등)가 이번 프레임 카메라로 투영하도록 바로 반영
	SyncCamera();
}

void FAssetPreview::SyncCamera()
{
	// 가까이 볼 때도 잘리지 않도록 궤도 거리에 맞춰 클립 거리 조정
	Camera.SetPerspective(Camera.GetFovYDegrees(), static_cast<float>(Width) / static_cast<float>(Height),
	                      FMath::Clamp(Orbit.Distance * 0.01f, 0.1f, 10.0f), FMath::Max(Orbit.Distance * 50.0f, 20000.0f));
	Orbit.ApplyTo(Camera);
}

bool FAssetPreview::ProjectToScreen(const FVector3& WorldPosition, FVector2& OutScreen) const
{
	const FVector4 Clip = Camera.GetViewProjectionMatrix().TransformVector4(FVector4(WorldPosition.X, WorldPosition.Y, WorldPosition.Z, 1.0f));
	if (Clip.W <= 1.0e-4f)
	{
		return false;
	}
	const float NdcX = Clip.X / Clip.W;
	const float NdcY = Clip.Y / Clip.W;
	OutScreen        = FVector2(ImageMin.X + (NdcX * 0.5f + 0.5f) * ImageSize.X, ImageMin.Y + (0.5f - NdcY * 0.5f) * ImageSize.Y);
	return true;
}

void FAssetPreview::Render(FD3D12RHI& Rhi, FSceneRenderer& Renderer, FEditorGrid* Grid)
{
	if (!Target)
	{
		return;
	}
	SyncCamera();
	Scene.UpdateTransforms();

	ID3D12GraphicsCommandList* CommandList = Rhi.GetCommandList();
	Target->Begin(CommandList, nullptr);
	Renderer.Render(Scene, Camera, Target->GetOutput());
	const FD3D12RenderTarget* SceneColor = Renderer.GetSceneColor();
	if (bShowGrid && Grid != nullptr && SceneColor != nullptr && SceneColor->GetDesc().bWithDepth && SceneColor->GetWidth() == Width &&
	    SceneColor->GetHeight() == Height)
	{
		Grid->Render(Camera, Target->GetOutput(), SceneColor->GetDsv());
	}
	Target->End(CommandList);
	// 다음 프레임에 창이 그려지지 않으면(접힘/가려진 탭) 렌더도 건너뛴다
	bDrawn = false;
}
