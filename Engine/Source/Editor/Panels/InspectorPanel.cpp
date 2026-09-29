#include "Editor/Panels/InspectorPanel.h"

#include "Core/Reflection/TypeInfo.h"
#include "Editor/EditorContext.h"
#include "Renderer/Material.h"
#include "Renderer/ResourceManager.h"
#include "Scene/Scene.h"

#include <imgui.h>

#include <cstring>
#include <format>

namespace
{
	// 컴포넌트 섹션 헤더 (+ 제거 버튼). 반환: 펼침 여부. OutRemove: 제거 요청
	bool DrawComponentHeader(const char* Label, bool bRemovable, bool& OutRemove)
	{
		OutRemove = false;
		ImGui::PushID(Label);
		const bool bExpanded = ImGui::CollapsingHeader(Label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (bRemovable)
		{
			ImGui::SameLine(ImGui::GetContentRegionAvail().x - 4.0f);
			OutRemove = ImGui::SmallButton("X");
		}
		ImGui::PopID();
		return bExpanded;
	}

	float DragSpeed(const FPropertyInfo& Property, float Default)
	{
		return Property.Step > 0.0f ? Property.Step : Default;
	}
} // namespace

void FInspectorPanel::Draw(FEditorContext& Context)
{
	if (!bOpen)
	{
		return;
	}

	if (ImGui::Begin("인스펙터", &bOpen))
	{
		const FEntity Entity   = Context.SelectedEntity;
		FRegistry&    Registry = Context.Scene->GetRegistry();

		if (!Registry.IsValid(Entity))
		{
			ImGui::TextDisabled("선택된 엔티티가 없습니다");
		}
		else
		{
			DrawNameField(Context, Entity);
			ImGui::Separator();

			// 등록 순서대로 컴포넌트 표시. 제거는 순회 후 적용.
			const FTypeInfo* PendingRemove = nullptr;
			FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
				if (Type.HasFlag(TF_HiddenInInspector) || !Type.HasComponent(Registry, Entity))
				{
					return;
				}
				void* Component = Type.GetComponent(Registry, Entity);
				bool  bRemove   = false;
				if (DrawComponentHeader(Type.DisplayName.c_str(), Type.bRemovable, bRemove))
				{
					DrawComponent(Context, Entity, Type, Component);
				}
				if (bRemove)
				{
					PendingRemove = &Type;
				}
			});
			if (PendingRemove != nullptr)
			{
				PendingRemove->RemoveComponent(Registry, Entity);
			}

			ImGui::Separator();
			DrawAddComponentMenu(Context, Entity);
		}
	}
	ImGui::End();
}

void FInspectorPanel::DrawNameField(FEditorContext& Context, FEntity Entity)
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

void FInspectorPanel::DrawComponent(FEditorContext& Context, FEntity Entity, const FTypeInfo& Type, void* Component)
{
	ImGui::PushID(Type.Name.c_str());
	for (const FPropertyInfo& Property : Type.Properties)
	{
		if (Property.HasFlag(PF_Hidden))
		{
			continue;
		}
		ImGui::PushID(Property.Name.c_str());
		ImGui::BeginDisabled(Property.HasFlag(PF_ReadOnly));
		DrawProperty(Property, Component, Entity);
		ImGui::EndDisabled();
		ImGui::PopID();
	}
	ImGui::PopID();

	// 타입별 추가 정보 (리플렉션으로 표현되지 않는 파생 값/연결 리소스)
	const FTypeRegistry& Registry = FTypeRegistry::Get();
	if (&Type == Registry.Find<FTransformComponent>())
	{
		DrawTransformExtras(Context, Entity);
	}
	else if (&Type == Registry.Find<FStaticMeshComponent>())
	{
		DrawStaticMeshExtras(Context, Entity);
	}
}

bool FInspectorPanel::DrawProperty(const FPropertyInfo& Property, void* Component, FEntity Entity)
{
	const char* Label = Property.DisplayName.c_str();

	switch (Property.Type)
	{
	case EPropertyType::Bool:
		return ImGui::Checkbox(Label, &Property.GetRef<bool>(Component));

	case EPropertyType::Int32:
	{
		int32& Value = Property.GetRef<int32>(Component);
		if (Property.HasRange())
		{
			return ImGui::SliderInt(Label, &Value, static_cast<int>(Property.MinValue), static_cast<int>(Property.MaxValue));
		}
		return ImGui::DragInt(Label, &Value, DragSpeed(Property, 1.0f));
	}

	case EPropertyType::UInt32:
	{
		uint32&      Value = Property.GetRef<uint32>(Component);
		const uint32 Min   = static_cast<uint32>(FMath::Max(Property.MinValue, 0.0f));
		const uint32 Max   = static_cast<uint32>(Property.MaxValue);
		return ImGui::DragScalar(Label, ImGuiDataType_U32, &Value, DragSpeed(Property, 1.0f),
		                         Property.HasRange() ? &Min : nullptr, Property.HasRange() ? &Max : nullptr);
	}

	case EPropertyType::Float:
	{
		float& Value = Property.GetRef<float>(Component);
		if (Property.HasRange())
		{
			return ImGui::DragFloat(Label, &Value, DragSpeed(Property, 0.05f), Property.MinValue, Property.MaxValue);
		}
		return ImGui::DragFloat(Label, &Value, DragSpeed(Property, 0.05f));
	}

	case EPropertyType::String:
	{
		std::string& Value = Property.GetRef<std::string>(Component);
		char         Buffer[256];
		strncpy_s(Buffer, sizeof(Buffer), Value.c_str(), _TRUNCATE);
		if (ImGui::InputText(Label, Buffer, sizeof(Buffer)))
		{
			Value = Buffer;
			return true;
		}
		return false;
	}

	case EPropertyType::Vector2:
		return ImGui::DragFloat2(Label, &Property.GetRef<FVector2>(Component).X, DragSpeed(Property, 0.05f));

	case EPropertyType::Vector3:
	{
		FVector3& Value = Property.GetRef<FVector3>(Component);
		if (Property.HasFlag(PF_Color))
		{
			return ImGui::ColorEdit3(Label, &Value.X);
		}
		if (Property.HasRange())
		{
			return ImGui::DragFloat3(Label, &Value.X, DragSpeed(Property, 0.05f), Property.MinValue, Property.MaxValue);
		}
		return ImGui::DragFloat3(Label, &Value.X, DragSpeed(Property, 0.05f));
	}

	case EPropertyType::Vector4:
	{
		FVector4& Value = Property.GetRef<FVector4>(Component);
		if (Property.HasFlag(PF_Color))
		{
			return ImGui::ColorEdit4(Label, &Value.X);
		}
		return ImGui::DragFloat4(Label, &Value.X, DragSpeed(Property, 0.05f));
	}

	case EPropertyType::Quat:
	{
		FQuat& Value = Property.GetRef<FQuat>(Component);
		// 외부(기즈모 등)에서 바뀌었으면 오일러 캐시 갱신
		if (EulerCacheEntity != Entity || EulerCacheProperty != &Property || !EulerCacheRotation.Equals(Value, 1.0e-6f))
		{
			Value.ToEuler(EulerCacheDegrees.X, EulerCacheDegrees.Y, EulerCacheDegrees.Z);
			EulerCacheEntity   = Entity;
			EulerCacheProperty = &Property;
			EulerCacheRotation = Value;
		}
		const bool bChanged = ImGui::DragFloat3(Label, &EulerCacheDegrees.X, 0.5f);
		ImGui::SetItemTooltip("Pitch / Yaw / Roll (도)");
		if (bChanged)
		{
			Value              = FQuat::FromEuler(EulerCacheDegrees.X, EulerCacheDegrees.Y, EulerCacheDegrees.Z);
			EulerCacheRotation = Value;
		}
		return bChanged;
	}

	case EPropertyType::Entity:
	{
		const FEntity& Value = Property.GetRef<FEntity>(Component);
		if (Value.IsValid())
		{
			ImGui::Text("%s: 엔티티 #%u", Label, Value.Index);
		}
		else
		{
			ImGui::Text("%s: 없음", Label);
		}
		return false;
	}

	case EPropertyType::ResourceHandle:
	{
		// 핸들은 공통 레이아웃(Index, Generation)이므로 인덱스만 표시
		const uint32 Index = *static_cast<const uint32*>(Property.GetPtr(Component));
		if (Index == ~0u)
		{
			ImGui::Text("%s: 없음", Label);
		}
		else
		{
			ImGui::Text("%s: #%u", Label, Index);
		}
		ImGui::SetItemTooltip("%s", Property.HandleTypeName.c_str());
		return false;
	}

	default:
		ImGui::TextDisabled("%s: (지원하지 않는 타입 %s)", Label, PropertyTypeToString(Property.Type));
		return false;
	}
}

void FInspectorPanel::DrawTransformExtras(FEditorContext& Context, FEntity Entity)
{
	if (const FTransformComponent* Transform = Context.Scene->GetRegistry().TryGet<FTransformComponent>(Entity))
	{
		const FVector3 WorldPosition = Transform->GetWorldPosition();
		ImGui::TextDisabled("월드 위치: %.2f, %.2f, %.2f", WorldPosition.X, WorldPosition.Y, WorldPosition.Z);
	}
}

void FInspectorPanel::DrawStaticMeshExtras(FEditorContext& Context, FEntity Entity)
{
	FStaticMeshComponent* Mesh = Context.Scene->GetRegistry().TryGet<FStaticMeshComponent>(Entity);
	if (Mesh == nullptr)
	{
		return;
	}

	const FStaticMesh* StaticMesh = Context.Resources->GetMesh(Mesh->Mesh);
	if (StaticMesh != nullptr)
	{
		const FVector3 Size = StaticMesh->GetLocalBounds().GetSize();
		ImGui::TextDisabled("정점 %u, 삼각형 %u, 로컬 크기 %.2f x %.2f x %.2f", StaticMesh->GetVertexCount(),
		                    StaticMesh->GetIndexCount() / 3, Size.X, Size.Y, Size.Z);
	}
	else
	{
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "메시 핸들이 유효하지 않습니다");
	}

	// 머티리얼은 별도 리소스라 여기서 직접 편집 (머티리얼 에셋 에디터 도입 전까지)
	const FMaterial& Material = Context.Resources->ResolveMaterial(Mesh->Material);
	ImGui::SeparatorText(std::format("머티리얼: {}{}", Material.Name, Mesh->Material.IsValid() ? "" : " (기본값)").c_str());
	if (FMaterial* Editable = Context.Resources->GetMaterial(Mesh->Material))
	{
		ImGui::PushID("Material");
		ImGui::ColorEdit4("베이스 컬러", &Editable->Constants.BaseColorFactor.X);
		ImGui::SliderFloat("금속성", &Editable->Constants.Metallic, 0.0f, 1.0f);
		ImGui::SliderFloat("거칠기", &Editable->Constants.Roughness, 0.0f, 1.0f);
		ImGui::ColorEdit3("발광", &Editable->Constants.EmissiveFactor.X, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
		ImGui::DragFloat("노멀 강도", &Editable->Constants.NormalScale, 0.01f, 0.0f, 4.0f);
		ImGui::SliderFloat("AO 강도", &Editable->Constants.OcclusionStrength, 0.0f, 1.0f);
		ImGui::PopID();
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
		bool bAny = false;
		FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
			if (Type.HasFlag(TF_HiddenInInspector) || !Type.bRemovable || Type.HasComponent(Registry, Entity))
			{
				return;
			}
			bAny = true;
			if (ImGui::MenuItem(Type.DisplayName.c_str()))
			{
				void* Added = Type.AddComponent(Registry, Entity);
				// 스태틱 메시는 기본 큐브로 시작해 바로 보이게 한다
				if (&Type == FTypeRegistry::Get().Find<FStaticMeshComponent>())
				{
					FStaticMeshComponent* Mesh = static_cast<FStaticMeshComponent*>(Added);
					Mesh->Mesh      = Context.DefaultCubeMesh;
					Mesh->MeshAsset = "primitive:cube";
				}
			}
		});
		if (!bAny)
		{
			ImGui::TextDisabled("추가할 수 있는 컴포넌트가 없습니다");
		}
		ImGui::EndPopup();
	}
}
