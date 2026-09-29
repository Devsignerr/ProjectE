#include "Editor/Panels/InspectorPanel.h"

#include "Editor/EditorContext.h"
#include "Renderer/Material.h"
#include "Renderer/ResourceManager.h"
#include "Scene/Scene.h"

#include <imgui.h>

#include <cstring>

void FInspectorPanel::Draw(FEditorContext& Context)
{
	if (!bOpen)
	{
		return;
	}

	if (ImGui::Begin("인스펙터", &bOpen))
	{
		const FEntity Entity = Context.SelectedEntity;
		if (!Context.Scene->GetRegistry().IsValid(Entity))
		{
			ImGui::TextDisabled("선택된 엔티티가 없습니다");
		}
		else
		{
			DrawNameComponent(Context, Entity);
			ImGui::Separator();
			DrawTransformComponent(Context, Entity);
			DrawStaticMeshComponent(Context, Entity);
			DrawDirectionalLightComponent(Context, Entity);
			ImGui::Separator();
			DrawAddComponentMenu(Context, Entity);
		}
	}
	ImGui::End();
}

void FInspectorPanel::DrawNameComponent(FEditorContext& Context, FEntity Entity)
{
	FNameComponent* Name = Context.Scene->GetRegistry().TryGet<FNameComponent>(Entity);
	if (Name == nullptr)
	{
		return;
	}

	char Buffer[128];
	strncpy_s(Buffer, sizeof(Buffer), Name->Name.c_str(), _TRUNCATE);
	ImGui::SetNextItemWidth(-1.0f);
	if (ImGui::InputText("##Name", Buffer, sizeof(Buffer)))
	{
		Name->Name = Buffer;
	}
	ImGui::TextDisabled("엔티티 #%u (세대 %u)", Entity.Index, Entity.Generation);
}

void FInspectorPanel::DrawTransformComponent(FEditorContext& Context, FEntity Entity)
{
	FTransformComponent* Transform = Context.Scene->GetRegistry().TryGet<FTransformComponent>(Entity);
	if (Transform == nullptr)
	{
		return;
	}
	if (!ImGui::CollapsingHeader("트랜스폼", ImGuiTreeNodeFlags_DefaultOpen))
	{
		return;
	}

	ImGui::DragFloat3("위치", &Transform->Position.X, 0.05f);

	// 외부(기즈모 등)에서 회전이 바뀌었으면 오일러 캐시 갱신
	if (EulerCacheEntity != Entity || !EulerCacheRotation.Equals(Transform->Rotation, 1.0e-6f))
	{
		Transform->Rotation.ToEuler(EulerCacheDegrees.X, EulerCacheDegrees.Y, EulerCacheDegrees.Z);
		EulerCacheEntity   = Entity;
		EulerCacheRotation = Transform->Rotation;
	}
	if (ImGui::DragFloat3("회전 (P/Y/R)", &EulerCacheDegrees.X, 0.5f))
	{
		Transform->Rotation = FQuat::FromEuler(EulerCacheDegrees.X, EulerCacheDegrees.Y, EulerCacheDegrees.Z);
		EulerCacheRotation  = Transform->Rotation;
	}

	ImGui::DragFloat3("스케일", &Transform->Scale.X, 0.02f, 0.001f, 1000.0f);

	const FVector3 WorldPosition = Transform->GetWorldPosition();
	ImGui::TextDisabled("월드 위치: %.2f, %.2f, %.2f", WorldPosition.X, WorldPosition.Y, WorldPosition.Z);
}

void FInspectorPanel::DrawStaticMeshComponent(FEditorContext& Context, FEntity Entity)
{
	FRegistry&            Registry = Context.Scene->GetRegistry();
	FStaticMeshComponent* Mesh     = Registry.TryGet<FStaticMeshComponent>(Entity);
	if (Mesh == nullptr)
	{
		return;
	}

	ImGui::PushID("StaticMesh");
	const bool bExpanded = ImGui::CollapsingHeader("스태틱 메시", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 4.0f);
	const bool bRemove = ImGui::SmallButton("X");
	ImGui::PopID();
	if (bRemove)
	{
		Registry.Remove<FStaticMeshComponent>(Entity);
		return;
	}
	if (!bExpanded)
	{
		return;
	}

	ImGui::Checkbox("표시", &Mesh->bVisible);

	const FStaticMesh* StaticMesh = Context.Resources->GetMesh(Mesh->Mesh);
	if (StaticMesh != nullptr)
	{
		const FVector3 Size = StaticMesh->GetLocalBounds().GetSize();
		ImGui::Text("메시: #%u (정점 %u, 삼각형 %u)", Mesh->Mesh.Index, StaticMesh->GetVertexCount(), StaticMesh->GetIndexCount() / 3);
		ImGui::TextDisabled("로컬 크기: %.2f x %.2f x %.2f", Size.X, Size.Y, Size.Z);
	}
	else
	{
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "메시 핸들이 유효하지 않습니다");
	}

	const FMaterial& Material = Context.Resources->ResolveMaterial(Mesh->Material);
	ImGui::Text("머티리얼: %s%s", Material.Name.c_str(), Mesh->Material.IsValid() ? "" : " (기본값)");
	if (FMaterial* Editable = Context.Resources->GetMaterial(Mesh->Material))
	{
		ImGui::ColorEdit4("베이스 컬러 틴트", &Editable->Constants.BaseColorTint.X);
		ImGui::ColorEdit3("스페큘러 색", &Editable->Constants.SpecularColor.X);
		ImGui::DragFloat("광택", &Editable->Constants.Shininess, 1.0f, 1.0f, 1024.0f);
		ImGui::DragFloat("스페큘러 강도", &Editable->Constants.SpecularStrength, 0.01f, 0.0f, 4.0f);
	}
}

void FInspectorPanel::DrawDirectionalLightComponent(FEditorContext& Context, FEntity Entity)
{
	FRegistry&                  Registry = Context.Scene->GetRegistry();
	FDirectionalLightComponent* Light    = Registry.TryGet<FDirectionalLightComponent>(Entity);
	if (Light == nullptr)
	{
		return;
	}

	ImGui::PushID("DirectionalLight");
	const bool bExpanded = ImGui::CollapsingHeader("방향광", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 4.0f);
	const bool bRemove = ImGui::SmallButton("X");
	ImGui::PopID();
	if (bRemove)
	{
		Registry.Remove<FDirectionalLightComponent>(Entity);
		return;
	}
	if (!bExpanded)
	{
		return;
	}

	ImGui::ColorEdit3("색", &Light->Color.X);
	ImGui::DragFloat("강도", &Light->Intensity, 0.05f, 0.0f, 50.0f);
	if (const FTransformComponent* Transform = Registry.TryGet<FTransformComponent>(Entity))
	{
		const FVector3 Direction = Transform->GetWorldForward();
		ImGui::TextDisabled("방향(월드 Forward): %.2f, %.2f, %.2f", Direction.X, Direction.Y, Direction.Z);
	}
}

void FInspectorPanel::DrawAddComponentMenu(FEditorContext& Context, FEntity Entity)
{
	FRegistry& Registry = Context.Scene->GetRegistry();

	if (ImGui::Button("컴포넌트 추가", ImVec2(-1.0f, 0.0f)))
	{
		ImGui::OpenPopup("AddComponent");
	}
	if (ImGui::BeginPopup("AddComponent"))
	{
		if (!Registry.Has<FStaticMeshComponent>(Entity) && ImGui::MenuItem("스태틱 메시 (큐브)"))
		{
			FStaticMeshComponent& Mesh = Registry.Emplace<FStaticMeshComponent>(Entity);
			Mesh.Mesh = Context.DefaultCubeMesh;
		}
		if (!Registry.Has<FDirectionalLightComponent>(Entity) && ImGui::MenuItem("방향광"))
		{
			Registry.Emplace<FDirectionalLightComponent>(Entity);
		}
		ImGui::EndPopup();
	}
}
