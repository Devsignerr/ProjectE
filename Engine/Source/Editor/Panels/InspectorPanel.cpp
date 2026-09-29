#include "Editor/Panels/InspectorPanel.h"

#include "Core/Reflection/TypeInfo.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Renderer/Material.h"
#include "Renderer/ResourceManager.h"
#include "Scene/AnimationSystem.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptSystem.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
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
				Context.MarkEdited("컴포넌트 제거");
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
		Context.MarkEdited("이름 변경");
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
		if (DrawProperty(Property, Component, Entity))
		{
			Context.MarkEdited(std::format("{} 편집", Property.DisplayName));
		}
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
	else if (&Type == Registry.Find<FScriptComponent>())
	{
		DrawScriptExtras(Context, Entity);
	}
	else if (&Type == Registry.Find<FAnimationComponent>())
	{
		DrawAnimationExtras(Context, Entity);
	}
}

void FInspectorPanel::DrawAnimationExtras(FEditorContext& Context, FEntity Entity)
{
	FAnimationComponent* Animation = Context.Scene->GetRegistry().TryGet<FAnimationComponent>(Entity);
	if (Animation == nullptr)
	{
		return;
	}
	const std::vector<std::string> Clips = FAnimationSystem::GetClipNames(*Context.Scene, Entity);
	if (Clips.empty())
	{
		ImGui::TextDisabled("클립 없음 (모델 로드 후 표시)");
		return;
	}

	// 선택하면 Clip 문자열을 바꾼다 → 애니메이션 시스템이 BlendTime 동안 크로스페이드
	const std::string Current = FAnimationSystem::GetCurrentClip(*Context.Scene, Entity);
	if (ImGui::BeginCombo("클립 선택", Current.empty() ? "(없음)" : Current.c_str()))
	{
		for (const std::string& Clip : Clips)
		{
			const bool bSelected = Clip == Current;
			if (ImGui::Selectable(Clip.c_str(), bSelected) && !bSelected)
			{
				Animation->Clip = Clip;
				Context.MarkEdited("애니메이션 클립 변경");
			}
			if (bSelected)
			{
				ImGui::SetItemDefaultFocus();
			}
		}
		ImGui::EndCombo();
	}
	ImGui::TextDisabled("클립 %zu개, 재생 시간 %.2f초", Clips.size(), Animation->Runtime.CurrentTime);
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

namespace
{
	// 스크립트 프로퍼티 값 위젯. 반환: 편집됨
	bool DrawScriptValue(const char* Label, FScriptValue& Value)
	{
		switch (Value.Type)
		{
		case EScriptValueType::Bool:
			return ImGui::Checkbox(Label, &Value.bBool);
		case EScriptValueType::Number:
			if (Value.bInteger)
			{
				int IntValue = static_cast<int>(Value.Number);
				if (ImGui::DragInt(Label, &IntValue))
				{
					Value.Number = IntValue;
					return true;
				}
				return false;
			}
			else
			{
				float FloatValue = static_cast<float>(Value.Number);
				if (ImGui::DragFloat(Label, &FloatValue, 0.1f))
				{
					Value.Number = FloatValue;
					return true;
				}
				return false;
			}
		case EScriptValueType::String:
		{
			char Buffer[256];
			strncpy_s(Buffer, sizeof(Buffer), Value.String.c_str(), _TRUNCATE);
			if (ImGui::InputText(Label, Buffer, sizeof(Buffer)))
			{
				Value.String = Buffer;
				return true;
			}
			return false;
		}
		case EScriptValueType::Vector3:
			return ImGui::DragFloat3(Label, &Value.Vector.X, 0.5f);
		default:
			ImGui::TextDisabled("%s: (지원하지 않는 타입)", Label);
			return false;
		}
	}

	std::vector<std::string> ScanScriptFiles(const std::filesystem::path& ContentDirectory)
	{
		std::vector<std::string> Files;
		std::error_code          ErrorCode;
		for (auto It = std::filesystem::recursive_directory_iterator(ContentDirectory, ErrorCode);
		     !ErrorCode && It != std::filesystem::recursive_directory_iterator(); It.increment(ErrorCode))
		{
			if (It->is_regular_file(ErrorCode) && It->path().extension() == L".lua")
			{
				Files.push_back(FStringConv::ToUtf8(std::filesystem::relative(It->path(), ContentDirectory, ErrorCode).generic_wstring()));
			}
		}
		std::sort(Files.begin(), Files.end());
		return Files;
	}
} // namespace

void FInspectorPanel::DrawScriptExtras(FEditorContext& Context, FEntity Entity)
{
	FScriptComponent* Script = Context.Scene->GetRegistry().TryGet<FScriptComponent>(Entity);
	if (Script == nullptr)
	{
		return;
	}

	if (ImGui::SmallButton("스크립트 선택..."))
	{
		ScriptFiles = ScanScriptFiles(Context.ContentDirectory);
		ImGui::OpenPopup("SelectScript");
	}
	if (ImGui::BeginPopup("SelectScript"))
	{
		if (ScriptFiles.empty())
		{
			ImGui::TextDisabled("Content에 .lua 파일이 없습니다");
		}
		for (const std::string& File : ScriptFiles)
		{
			if (ImGui::MenuItem(File.c_str(), nullptr, File == Script->ScriptAsset))
			{
				Script->ScriptAsset = File;
			}
		}
		ImGui::EndPopup();
	}

	if (Context.Scripts == nullptr || Script->ScriptAsset.empty())
	{
		return;
	}
	std::string                             Error;
	const std::vector<FScriptPropertyDecl>* Decls = Context.Scripts->GetPropertyDecls(Script->ScriptAsset, &Error);
	if (Decls == nullptr)
	{
		ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", Error.substr(0, Error.find('\n')).c_str());
		return;
	}
	if (Decls->empty())
	{
		ImGui::TextDisabled("선언된 Properties 없음");
		return;
	}

	// 플레이 중에는 실행 중인 인스턴스 값을 읽기 전용으로 보여 준다 (오버라이드 편집은 정지 후)
	const bool      bLive     = Context.bPlaying;
	FScriptValueMap Overrides = FScriptProperties::ParseOverrides(Script->PropertyOverrides);
	bool            bChanged  = false;

	ImGui::SeparatorText(bLive ? "Properties (실행 값)" : "Properties");
	for (const FScriptPropertyDecl& Decl : *Decls)
	{
		const auto   Found       = Overrides.find(Decl.Name);
		const bool   bOverridden = Found != Overrides.end() && FScriptProperties::IsCompatible(Decl.Default, Found->second);
		FScriptValue Value       = bOverridden ? Found->second : Decl.Default;
		Value.bInteger           = Decl.Default.bInteger;
		if (bLive)
		{
			if (FScriptValue Live = Context.Scripts->GetInstanceProperty(Entity, Decl.Name); !Live.IsNil())
			{
				Value = Live;
			}
		}

		ImGui::PushID(Decl.Name.c_str());
		ImGui::BeginDisabled(bLive);
		const bool bEdited = DrawScriptValue(Decl.Name.c_str(), Value);
		ImGui::EndDisabled();
		if (bEdited && !bLive)
		{
			Overrides[Decl.Name] = Value;
			bChanged             = true;
		}
		if (bOverridden && !bLive)
		{
			ImGui::SameLine();
			if (ImGui::SmallButton("기본값"))
			{
				Overrides.erase(Decl.Name);
				bChanged = true;
			}
			ImGui::SetItemTooltip("스크립트 기본값으로 되돌리기");
		}
		ImGui::PopID();
	}

	if (bChanged)
	{
		Script->PropertyOverrides = FScriptProperties::SerializeOverrides(Overrides);
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
				Context.MarkEdited("컴포넌트 추가");
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
