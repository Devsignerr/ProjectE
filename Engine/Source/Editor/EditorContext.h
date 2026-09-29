#pragma once

#include "Core/ECS/Entity.h"
#include "Editor/EntitySelection.h"
#include "Editor/UndoHistory.h"
#include "Scene/ResourceHandles.h"

#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>

class FCamera;
class FD3D12RHI;
class FResourceManager;
class FScene;
class FSceneRenderer;

// 패널들이 공유하는 에디터 상태 (소유하지 않음)
struct FEditorContext
{
	FD3D12RHI*        Rhi       = nullptr;
	FResourceManager* Resources = nullptr;
	FSceneRenderer*   Renderer  = nullptr;
	FScene*           Scene     = nullptr;
	FCamera*          Camera    = nullptr;

	// 주 선택 (기즈모/인스펙터 대상). 읽기만 하고 변경은 Select* 함수로 한다 (Selection과 동기화)
	FEntity SelectedEntity;
	// 다중 선택 전체 (주 선택 포함)
	FEntitySelection Selection;

	std::filesystem::path ContentDirectory;
	FMeshHandle           DefaultCubeMesh; // "큐브 추가" 등에 사용 (MeshAsset "primitive:cube")

	// 패널 → 애플리케이션 요청 (씬 파일 열기 등)
	std::function<void(const std::filesystem::path&)> OpenSceneRequest;

	// 씬 편집 알림: 편집이 끝나면(활성 위젯/기즈모 조작 없음) 애플리케이션이 Undo 단계로 커밋한다.
	// 조작이 이어지는 동안 여러 번 호출돼도 첫 라벨 하나로 병합된다.
	FPendingEdit PendingEdit;
	void         MarkEdited(std::string_view Label) { PendingEdit.Mark(Label); }

	void Select(FEntity Entity)
	{
		Selection.Set(Entity);
		SelectedEntity = Selection.GetPrimary();
	}
	void ClearSelection()
	{
		Selection.Clear();
		SelectedEntity = NullEntity;
	}
	void ToggleSelection(FEntity Entity)
	{
		Selection.Toggle(Entity);
		SelectedEntity = Selection.GetPrimary();
	}
	void AddToSelection(FEntity Entity)
	{
		Selection.Add(Entity);
		SelectedEntity = Selection.GetPrimary();
	}
	void SelectMany(const std::vector<FEntity>& Entities, FEntity Primary)
	{
		Selection.SetMany(Entities, Primary);
		SelectedEntity = Selection.GetPrimary();
	}
	bool IsSelected(FEntity Entity) const { return Selection.Contains(Entity); }
	// Pred(Entity)가 true인 선택 제거 (파괴된 엔티티 정리 등)
	template <typename TPred>
	void DeselectIf(TPred&& Pred)
	{
		Selection.RemoveIf(Pred);
		SelectedEntity = Selection.GetPrimary();
	}
};
