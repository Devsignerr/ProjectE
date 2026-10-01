#include "Editor/Panels/InspectorPanel.h"

#include "Core/Reflection/TypeInfo.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Editor/PropertyWidgets.h"
#include "Renderer/Material.h"
#include "Renderer/ResourceManager.h"
#include "Scene/AnimationSystem.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Scene.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Renderer/ModelLoader.h"
#include "Renderer/SceneAssetResolver.h"
#include "Scripting/ScriptSystem.h"

#include <imgui.h>

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <format>

namespace
{
	// 컴포넌트 섹션 헤더 (아이콘 + 굵은 이름 + 제거 버튼). 반환: 펼침 여부. OutRemove: 제거 요청
	// bPrefabAdded: 프리팹 인스턴스에서 추가한 컴포넌트 (제목에 표시)
	bool DrawComponentHeader(const FTypeInfo& Type, bool bPrefabAdded, bool& OutRemove)
	{
		OutRemove = false;
		ImGui::PushID(Type.Name.c_str());
		// 선택 강조색(파랑)이 아닌 회색 제목 줄
		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.26f, 0.26f, 0.26f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
		ImGui::PushFont(FEditorTheme::GetBoldFont(), 0.0f);
		const std::string Label = std::format("{}  {}{}", FEditorTheme::GetComponentIcon(Type.Name), Type.DisplayName, bPrefabAdded ? "  (+ 인스턴스에서 추가)" : "") + "###Header";
		const bool        bExpanded = ImGui::CollapsingHeader(Label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		ImGui::PopFont();
		ImGui::PopStyleColor(3);
		if (Type.bRemovable)
		{
			const float ButtonWidth = ImGui::GetFrameHeight();
			ImGui::SameLine(ImGui::GetContentRegionMax().x - ButtonWidth);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, FEditorTheme::Danger);
			OutRemove = ImGui::Button(ICON_FA_TRASH_CAN, ImVec2(ButtonWidth, 0.0f));
			ImGui::PopStyleColor(2);
			ImGui::SetItemTooltip("컴포넌트 제거");
		}
		ImGui::PopID();
		return bExpanded;
	}
} // namespace

void FInspectorPanel::Draw(FEditorContext& Context)
{
	if (!bOpen)
	{
		return;
	}

	if (ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_SLIDERS, "인스펙터", "Inspector").c_str(), &bOpen))
	{
		DrawContents(Context);
	}
	ImGui::End();
}

void FInspectorPanel::DrawContents(FEditorContext& Context)
{
	const FEntity Entity   = Context.SelectedEntity;
	FRegistry&    Registry = Context.Scene->GetRegistry();

	if (!Registry.IsValid(Entity))
	{
		ImGui::TextDisabled("선택된 엔티티가 없습니다");
		return;
	}

	UpdatePrefabView(Context, Entity);
	DrawPrefabHeader(Context, Entity);
	DrawNameField(Context, Entity);
	ImGui::Separator();

	// 등록 순서대로 컴포넌트 표시. 제거는 순회 후 적용.
	const FTypeInfo* PendingRemove = nullptr;
	FTypeRegistry::Get().ForEachComponentType([&](const FTypeInfo& Type) {
		if (Type.HasFlag(TF_HiddenInInspector) || !Type.HasComponent(Registry, Entity))
		{
			return;
		}
		void*      Component = Type.GetComponent(Registry, Entity);
		bool       bRemove   = false;
		const auto Found     = PrefabView.bMember ? PrefabView.Overrides.Entities.find(PrefabView.Id) : PrefabView.Overrides.Entities.end();
		const bool bAdded    = Found != PrefabView.Overrides.Entities.end() && Found->second.AddedComponents.contains(Type.Name);
		if (DrawComponentHeader(Type, bAdded, bRemove))
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
	ApplyPendingPrefabAction(Context, Entity);
}

// ---------------------------------------------------------------- 프리팹

void FInspectorPanel::UpdatePrefabView(FEditorContext& Context, FEntity Entity)
{
	PrefabView            = FPrefabView{};
	const FScene&   Scene = *Context.Scene;
	const FEntity   Root  = FPrefabLibrary::FindInstanceRoot(Scene, Entity);
	const FRegistry& Registry = Scene.GetRegistry();
	if (!Root.IsValid())
	{
		return;
	}
	PrefabView.bMember  = true;
	PrefabView.bPlaying = Context.bPlaying; // 플레이 중에는 되돌리기 메뉴 없음 (플레이 씬 편집은 정지 시 버려짐)
	PrefabView.Root     = Root;
	if (const FPrefabLinkComponent* Link = Registry.TryGet<FPrefabLinkComponent>(Entity))
	{
		PrefabView.Id = Link->Id;
	}
	const FPrefabInstanceComponent& Instance = Registry.Get<FPrefabInstanceComponent>(Root);
	PrefabView.Asset                         = Instance.Asset;
	PrefabView.Overrides                     = FPrefabOverrides::Parse(Instance.Overrides);
}

void FInspectorPanel::DrawPrefabHeader(FEditorContext& Context, FEntity Entity)
{
	if (!PrefabView.bMember)
	{
		return;
	}
	const FRegistry& Registry = Context.Scene->GetRegistry();
	ImGui::PushStyleColor(ImGuiCol_Text, FEditorTheme::PrefabText);
	ImGui::TextUnformatted(ICON_FA_BOXES_STACKED);
	ImGui::SameLine();
	ImGui::TextWrapped("프리팹: %s", PrefabView.Asset.c_str());
	ImGui::PopStyleColor();
	if (Entity != PrefabView.Root)
	{
		const FNameComponent* RootName = Registry.TryGet<FNameComponent>(PrefabView.Root);
		ImGui::TextDisabled("인스턴스 '%s'의 일부", RootName ? RootName->Name.c_str() : "");
		ImGui::SameLine();
		if (ImGui::SmallButton("루트 선택"))
		{
			Context.Select(PrefabView.Root);
		}
	}
	if (const FPrefabInstanceComponent* Nested = Registry.TryGet<FPrefabInstanceComponent>(Entity); Nested && Entity != PrefabView.Root)
	{
		ImGui::TextDisabled("중첩 프리팹: %s", Nested->Asset.c_str());
	}

	const size_t OverrideCount = PrefabView.Overrides.Count();
	if (ImGui::SmallButton(ICON_FA_PEN_TO_SQUARE " 원본 열기") && Context.OpenAssetEditorRequest)
	{
		Context.OpenAssetEditorRequest(FPrefabLibrary::Get().ResolveAssetPath(PrefabView.Asset));
	}
	ImGui::SetItemTooltip("프리팹 편집 창에서 원본을 고칩니다. 저장하면 이 씬의 모든 인스턴스에 반영됩니다");
	ImGui::BeginDisabled(Context.bPlaying);
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT " 되돌리기"))
	{
		PendingPrefabAction = EPrefabAction::RevertAll;
	}
	ImGui::SetItemTooltip("인스턴스에서 바꾼 값, 추가/삭제한 컴포넌트와 엔티티를 모두 원본대로 되돌립니다");
	if (ImGui::SmallButton(ICON_FA_UPLOAD " 원본에 적용"))
	{
		PendingPrefabAction = EPrefabAction::Apply;
	}
	ImGui::SetItemTooltip("이 인스턴스의 현재 상태를 원본 파일에 저장합니다 (루트 위치/회전/이름 제외). 다른 인스턴스에도 반영됩니다");
	ImGui::SameLine();
	if (ImGui::SmallButton(ICON_FA_LINK_SLASH " 연결 해제"))
	{
		PendingPrefabAction = EPrefabAction::Unpack;
	}
	ImGui::SetItemTooltip("원본과의 연결을 끊고 일반 엔티티로 만듭니다");
	ImGui::EndDisabled();
	ImGui::TextDisabled("오버라이드 %zu개", OverrideCount);
	ImGui::SetItemTooltip("왼쪽 하늘색 막대가 있는 값이 원본과 다른 값입니다. 우클릭하면 원본 값으로 되돌립니다");

	// 이 엔티티에서 제거한 원본 컴포넌트 (다시 추가하려면 되돌리기)
	if (const auto Found = PrefabView.Overrides.Entities.find(PrefabView.Id); Found != PrefabView.Overrides.Entities.end())
	{
		for (const std::string& Removed : Found->second.RemovedComponents)
		{
			const FTypeInfo* Type = FTypeRegistry::Get().Find(Removed);
			ImGui::PushID(Removed.c_str());
			ImGui::TextColored(FEditorTheme::Warning, "- %s (인스턴스에서 제거)", Type ? Type->DisplayName.c_str() : Removed.c_str());
			ImGui::SameLine();
			ImGui::BeginDisabled(Context.bPlaying);
			if (ImGui::SmallButton("되돌리기"))
			{
				PendingPrefabAction = EPrefabAction::RevertComponent;
				PendingPrefabKey    = Removed;
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
	}
	ImGui::Separator();
}

void FInspectorPanel::DrawOverrideMarker(const std::string& Key)
{
	if (!PrefabView.bMember || !PrefabView.Overrides.IsPropertyOverridden(PrefabView.Id, Key))
	{
		return;
	}
	// 유니티처럼 왼쪽 가장자리에 하늘색 막대
	const ImVec2 Min = ImGui::GetItemRectMin();
	const ImVec2 Max = ImGui::GetItemRectMax();
	const float  X   = ImGui::GetWindowPos().x + 2.0f;
	ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(X, Min.y), ImVec2(X + 3.0f, Max.y), ImGui::GetColorU32(FEditorTheme::PrefabText));
	if (!PrefabView.bPlaying && ImGui::BeginPopupContextItem("##PrefabOverride"))
	{
		ImGui::TextDisabled("프리팹 오버라이드");
		if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT " 원본 값으로 되돌리기"))
		{
			PendingPrefabAction = EPrefabAction::RevertProperty;
			PendingPrefabKey    = Key;
		}
		ImGui::EndPopup();
	}
}

void FInspectorPanel::ApplyPendingPrefabAction(FEditorContext& Context, FEntity Entity)
{
	const EPrefabAction Action = PendingPrefabAction;
	PendingPrefabAction        = EPrefabAction::None;
	if (Action == EPrefabAction::None || !PrefabView.bMember)
	{
		return;
	}
	FPrefabLibrary& Library = FPrefabLibrary::Get();
	FScene&         Scene   = *Context.Scene;
	const FEntity   Root    = PrefabView.Root;
	std::string     Error;
	bool            bOk     = true;
	const char*     Label   = "프리팹 되돌리기";
	switch (Action)
	{
	case EPrefabAction::RevertProperty:  bOk = Library.RevertProperty(Scene, Entity, PendingPrefabKey); break;
	case EPrefabAction::RevertComponent: bOk = Library.RevertComponent(Scene, Entity, PendingPrefabKey); break;
	case EPrefabAction::RevertAll:       bOk = Library.RevertAll(Scene, Root); break;
	case EPrefabAction::Unpack:
		Library.Unpack(Scene, Root);
		Label = "프리팹 연결 해제";
		break;
	case EPrefabAction::Apply:
	{
		const auto Apply = [&] { return Library.ApplyToPrefab(Scene, Root, &Error); };
		bOk              = Context.ChangePrefab ? Context.ChangePrefab(Apply) : Apply();
		Label            = "원본에 적용";
		if (bOk && Context.Notify)
		{
			Context.Notify("원본에 적용: " + PrefabView.Asset, false);
		}
		break;
	}
	default:
		break;
	}
	if (!bOk)
	{
		if (!Error.empty() && Context.Notify)
		{
			Context.Notify(Error, true);
		}
		return;
	}
	// 동기화로 경로가 바뀐 컴포넌트의 리소스 핸들을 다시 해석
	if (Context.Resources != nullptr)
	{
		FSceneAssetResolver::Resolve(Scene, *Context.Resources, Context.ContentDirectory);
	}
	Context.DeselectIf([&Scene](FEntity Selected) { return !Scene.GetRegistry().IsValid(Selected); });
	Context.MarkEdited(Label);
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
	DrawOverrideMarker("NameComponent.Name");
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
		// 에셋 경로 칸: 콘텐츠 브라우저에서 끌어 놓아 지정
		if (!Property.AssetFilter.empty() && Property.Type == EPropertyType::String)
		{
			if (DrawAssetSlot(Context, Type, Property, Component))
			{
				Context.MarkEdited(std::format("{} 지정", Property.DisplayName));
			}
			DrawOverrideMarker(Type.Name + "." + Property.Name);
			ImGui::PopID();
			continue;
		}
		ImGui::BeginDisabled(Property.HasFlag(PF_ReadOnly));
		if (DrawProperty(Property, Component, Entity))
		{
			Context.MarkEdited(std::format("{} 편집", Property.DisplayName));
		}
		ImGui::EndDisabled();
		DrawOverrideMarker(Type.Name + "." + Property.Name);
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
	else if (&Type == Registry.Find<FSocketAttachmentComponent>())
	{
		DrawSocketAttachmentExtras(Context, Entity);
	}
}

void FInspectorPanel::DrawSocketAttachmentExtras(FEditorContext& Context, FEntity Entity)
{
	FScene&                     Scene      = *Context.Scene;
	FRegistry&                  Registry   = Scene.GetRegistry();
	FSocketAttachmentComponent* Attachment = Registry.TryGet<FSocketAttachmentComponent>(Entity);
	if (Attachment == nullptr)
	{
		return;
	}
	const auto EntityLabel = [&](FEntity Target) -> std::string {
		if (!Registry.IsValid(Target))
		{
			return "(없음)";
		}
		const FNameComponent* Name = Registry.TryGet<FNameComponent>(Target);
		return Name != nullptr ? Name->Name : std::format("엔티티 #{}", Target.Index);
	};
	// 붙이면 소켓 위치로 맞춘다 (로컬 위치/회전 = 0)
	const auto SnapToSocket = [&]() {
		FTransformComponent& Transform = Scene.GetTransform(Entity);
		Transform.Position             = FVector3::ZeroVector;
		Transform.Rotation             = FQuat::Identity;
		Scene.UpdateTransforms();
	};

	// 대상 모델: 씬의 모델 루트 중 선택 (자기 자신/자기 하위 제외)
	if (ImGui::BeginCombo("대상 모델", EntityLabel(Attachment->Target).c_str()))
	{
		std::vector<FEntity> Models;
		Registry.View<FModelComponent>().Each([&](FEntity Model, FModelComponent&) {
			if (Model != Entity && !Scene.IsAncestorOf(Entity, Model))
			{
				Models.push_back(Model);
			}
		});
		for (const FEntity Model : Models)
		{
			ImGui::PushID(static_cast<int>(Model.Index));
			if (ImGui::Selectable(EntityLabel(Model).c_str(), Model == Attachment->Target))
			{
				Attachment->Target = Model;
				Context.MarkEdited("소켓 대상 변경");
			}
			ImGui::PopID();
		}
		if (Models.empty())
		{
			ImGui::TextDisabled("씬에 모델이 없습니다");
		}
		ImGui::EndCombo();
	}

	const FModelComponent* Model = Registry.IsValid(Attachment->Target) ? Registry.TryGet<FModelComponent>(Attachment->Target) : nullptr;
	const FModelMetadata*  Meta  = Model != nullptr ? Model->Runtime.Metadata.get() : nullptr;
	if (ImGui::BeginCombo("소켓", Attachment->Socket.empty() ? "(선택)" : Attachment->Socket.c_str()))
	{
		if (Meta != nullptr)
		{
			for (const FModelSocket& Socket : Meta->Sockets)
			{
				const std::string Label = Socket.Bone.empty() ? Socket.Name : Socket.Name + "  (" + Socket.Bone + ")";
				if (ImGui::Selectable(Label.c_str(), Socket.Name == Attachment->Socket))
				{
					Attachment->Socket = Socket.Name;
					SnapToSocket();
					Context.MarkEdited("소켓 선택");
				}
			}
		}
		if (Meta == nullptr || Meta->Sockets.empty())
		{
			ImGui::TextDisabled("소켓이 없습니다 — 모델 편집 창의 '소켓'에서 추가하세요");
		}
		ImGui::EndCombo();
	}

	if (Model == nullptr)
	{
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 대상 모델을 고르세요");
	}
	else if (!Scene.IsSocketAttached(Entity))
	{
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 대상이 이 엔티티의 하위라 붙일 수 없습니다");
	}
	else
	{
		FMatrix4x4 SocketWorld;
		if (Scene.GetSocketWorldMatrix(Attachment->Target, Attachment->Socket, SocketWorld))
		{
			ImGui::TextColored(FEditorTheme::Success, ICON_FA_LINK " 소켓을 따라갑니다 (트랜스폼 = 소켓 기준)");
			if (ImGui::SmallButton("소켓 위치로 맞추기"))
			{
				SnapToSocket();
				Context.MarkEdited("소켓 위치로 맞추기");
			}
		}
		else
		{
			ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 소켓을 찾지 못해 계층을 따릅니다");
		}
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

	// 값 타입(불/정수/enum/실수/문자열/벡터)은 설정 창과 같은 공용 위젯
	if (FPropertyWidgets::IsValueType(Property.Type))
	{
		return FPropertyWidgets::DrawValue(Property, Component);
	}

	switch (Property.Type)
	{
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

	const FMaterial& Material = Context.Resources->ResolveMaterial(Mesh->Material);
	ImGui::SeparatorText(std::format("머티리얼: {}{}", Material.Name, Mesh->Material.IsValid() ? "" : " (기본값)").c_str());
	// .emat 에셋은 머티리얼 편집기에서 고친다 (저장/실행 취소 지원, 같은 머티리얼을 쓰는 모든 메시에 반영)
	if (!Mesh->MaterialAsset.empty())
	{
		ImGui::TextDisabled("%s", Mesh->MaterialAsset.c_str());
		if (ImGui::Button("머티리얼 편집기에서 열기") && Context.OpenAssetEditorRequest)
		{
			const std::filesystem::path AssetPath = FStringConv::ToWide(Mesh->MaterialAsset);
			Context.OpenAssetEditorRequest(AssetPath.is_absolute() ? AssetPath : Context.ContentDirectory / AssetPath);
		}
		return;
	}
	// 파일이 없는 머티리얼(모델 내장 등)은 여기서 직접 조정한다 (저장되지 않음)
	if (FMaterial* Editable = Context.Resources->GetMaterial(Mesh->Material))
	{
		ImGui::TextDisabled("모델 내장 머티리얼: 값 변경은 저장되지 않습니다");
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
	// Filter(";" 구분 확장자, 비면 모두 허용)가 Path의 확장자를 받는가
	bool MatchesAssetFilter(const std::string& Filter, const std::filesystem::path& Path)
	{
		if (Filter.empty())
		{
			return true;
		}
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return (";" + Filter + ";").find(";" + FStringConv::ToUtf8(Extension) + ";") != std::string::npos;
	}

	// 스크립트 에셋 값 칸: 콘텐츠 브라우저에서 끌어 놓기 + 비우기. 반환: 편집됨
	bool DrawScriptAssetValue(FEditorContext& Context, const char* Label, FScriptValue& Value)
	{
		bool              bChanged = false;
		const std::string Filter   = Value.AssetFilter;
		const FEditorTheme::FAssetStyle Style =
			FEditorTheme::GetAssetStyle(Value.String.empty() ? std::string() : FStringConv::ToUtf8(std::filesystem::path(FStringConv::ToWide(Value.String)).extension().wstring()), false);
		const std::string Text = std::format("{} {}", Value.String.empty() ? ICON_FA_CIRCLE_XMARK : Style.Icon, Value.String.empty() ? "(없음)" : Value.String.c_str());
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
		ImGui::Button(Text.c_str(), ImVec2(ImGui::CalcItemWidth(), 0.0f));
		ImGui::PopStyleVar();
		ImGui::SetItemTooltip("콘텐츠 브라우저에서 %s 파일을 끌어 놓습니다", Filter.empty() ? "에셋" : Filter.c_str());
		if (ImGui::BeginDragDropTarget())
		{
			if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
			{
				if (MatchesAssetFilter(Filter, Paths->front()))
				{
					Value.String = FModelLoader::MakeAssetPath(Paths->front());
					bChanged     = true;
				}
				else if (Context.Notify)
				{
					Context.Notify(std::format("{} 칸에는 {} 파일만 놓을 수 있습니다", Label, Filter), true);
				}
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		if (!Value.String.empty() && ImGui::SmallButton(ICON_FA_XMARK))
		{
			Value.String.clear();
			bChanged = true;
		}
		ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		ImGui::TextUnformatted(Label);
		return bChanged;
	}

	// 스크립트 프로퍼티 값 위젯. 반환: 편집됨
	bool DrawScriptValue(FEditorContext& Context, const char* Label, FScriptValue& Value)
	{
		switch (Value.Type)
		{
		case EScriptValueType::Asset:
			return DrawScriptAssetValue(Context, Label, Value);
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
				Context.MarkEdited("스크립트 지정");
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
	DrawOverrideMarker("ScriptComponent.PropertyOverrides");
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
		Value.AssetFilter  = Decl.Default.AssetFilter;
		const bool bEdited = DrawScriptValue(Context, Decl.Name.c_str(), Value);
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
		Context.MarkEdited("스크립트 프로퍼티 편집");
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

bool FInspectorPanel::DrawAssetSlot(FEditorContext& Context, const FTypeInfo& Type, const FPropertyInfo& Property, void* Component)
{
	std::string& Value    = Property.GetRef<std::string>(Component);
	bool         bChanged = false;

	// 읽기 전용(머티리얼 등)은 버튼 모양 칸 (누르면 편집 창), 아니면 직접 입력도 가능
	const std::filesystem::path Extension = FStringConv::ToWide(Value).empty() ? std::filesystem::path() : std::filesystem::path(FStringConv::ToWide(Value)).extension();
	const FEditorTheme::FAssetStyle Style = FEditorTheme::GetAssetStyle(FStringConv::ToUtf8(Extension.wstring()), false);
	if (Property.HasFlag(PF_ReadOnly))
	{
		const std::string Text = std::format("{} {}", Value.empty() ? ICON_FA_CIRCLE_XMARK : Style.Icon, Value.empty() ? "(없음)" : Value.c_str());
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
		if (ImGui::Button(Text.c_str(), ImVec2(ImGui::CalcItemWidth(), 0.0f)) && !Value.empty() && Context.OpenAssetEditorRequest)
		{
			const std::filesystem::path AssetPath = FStringConv::ToWide(Value);
			Context.OpenAssetEditorRequest(AssetPath.is_absolute() ? AssetPath : Context.ContentDirectory / AssetPath);
		}
		ImGui::PopStyleVar();
		ImGui::SetItemTooltip("콘텐츠 브라우저에서 %s 파일을 끌어 놓아 바꿉니다. 누르면 편집 창", Property.AssetFilter.c_str());
	}
	else
	{
		char Buffer[256];
		strncpy_s(Buffer, sizeof(Buffer), Value.c_str(), _TRUNCATE);
		ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
		if (ImGui::InputText("##AssetPath", Buffer, sizeof(Buffer)))
		{
			Value    = Buffer;
			bChanged = true;
		}
		ImGui::SetItemTooltip("콘텐츠 브라우저에서 %s 파일을 끌어 놓을 수 있습니다", Property.AssetFilter.c_str());
	}

	if (ImGui::BeginDragDropTarget())
	{
		if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
		{
			std::wstring Dropped = Paths->front().extension().wstring();
			std::transform(Dropped.begin(), Dropped.end(), Dropped.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
			const std::string Filter = ";" + Property.AssetFilter + ";";
			if (Filter.find(";" + FStringConv::ToUtf8(Dropped) + ";") != std::string::npos)
			{
				Value    = FModelLoader::MakeAssetPath(Paths->front());
				bChanged = true;
				// 정적 메시 머티리얼은 핸들을 비우고 새 경로로 다시 해석
				if (Type.Name == "StaticMeshComponent")
				{
					static_cast<FStaticMeshComponent*>(Component)->Material = FMaterialHandle{};
					FSceneAssetResolver::Resolve(*Context.Scene, *Context.Resources, Context.ContentDirectory);
				}
			}
			else if (Context.Notify)
			{
				Context.Notify(std::format("{} 칸에는 {} 파일만 놓을 수 있습니다", Property.DisplayName, Property.AssetFilter), true);
			}
		}
		ImGui::EndDragDropTarget();
	}
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	ImGui::TextUnformatted(Property.DisplayName.c_str());
	return bChanged;
}
