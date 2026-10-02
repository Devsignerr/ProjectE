#pragma once

#include "Core/ECS/Entity.h"
#include "Editor/EntitySelection.h"
#include "Editor/UndoHistory.h"
#include "Scene/ResourceHandles.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class FAISystem;
class FCamera;
class FD3D12RHI;
class FPlayInEditorNet;
class FResourceManager;
class FScene;
class FSceneRenderer;
class FScriptSystem;
struct FAssetMove;

// 패널들이 공유하는 에디터 상태 (소유하지 않음)
struct FEditorContext
{
	FD3D12RHI*        Rhi       = nullptr;
	FResourceManager* Resources = nullptr;
	FSceneRenderer*   Renderer  = nullptr;
	FScene*           Scene     = nullptr;
	FCamera*          Camera    = nullptr;
	FScriptSystem*    Scripts   = nullptr; // 스크립트 Properties 선언 조회 (인스펙터)
	FAISystem*        AI        = nullptr; // 플레이 중 비헤이비어 트리 디버그 (FGameWorld 소유, 항상 유효)
	FPlayInEditorNet* NetPlay   = nullptr; // 네트워크 플레이 설정/상태, LAN 세션 (네트워크 패널)

	// 주 선택 (기즈모/인스펙터 대상). 읽기만 하고 변경은 Select* 함수로 한다 (Selection과 동기화)
	FEntity SelectedEntity;
	// 다중 선택 전체 (주 선택 포함)
	FEntitySelection Selection;

	// 플레이 모드 (FPlayMode가 갱신). 플레이 중 Scene은 복제된 플레이 씬을 가리킨다
	bool bPlaying = false;
	bool bPaused  = false;
	// 플레이 중 뷰포트 빙의 (언리얼 PIE Possess/Eject — FPlayMode가 갱신, F8로 전환. 플레이 시작 = 빙의).
	//   빙의: 뷰포트 입력이 게임으로 가고 게임 카메라로 본다 — 클릭 선택/기즈모/선택 아웃라인/편집 단축키/편집 카메라 없음
	//   빙의 해제: 게임은 계속 돌지만 입력을 받지 않고, 편집 카메라로 플레이 씬을 고르고 고칠 수 있다
	bool bPossessed = false;
	// 뷰포트에서 씬 편집 상호작용(클릭 선택, 기즈모, 선택 아웃라인, W/E/R/F, 편집 도구)을 허용하는가: 편집 중이거나 빙의 해제된 플레이
	bool CanEditInViewport() const { return !bPlaying || !bPossessed; }

	std::filesystem::path ContentDirectory;
	std::string           EntityClipboard; // 복사한 엔티티 (FSceneEditOps::Copy 형식). 씬을 바꿔도 유지
	FMeshHandle           DefaultCubeMesh; // "큐브 추가" 등에 사용 (MeshAsset "primitive:cube")

	// 패널 → 애플리케이션 요청 (씬 파일 열기 등)
	std::function<void(const std::filesystem::path&)> OpenSceneRequest;
	std::function<void(const std::filesystem::path&)> OpenAssetEditorRequest; // 에셋 편집 창 열기 (지원하지 않는 형식이면 무시)
	// 에셋 파일 조작 전: 해당 경로(폴더면 안쪽 전부)의 편집 창을 닫는다. 저장 안 한 창이 있으면 닫지 않고 false
	std::function<bool(const std::vector<std::filesystem::path>&)> PrepareAssetChange;
	// 에셋 이동/이름 변경 후: 열린 씬·실행 취소 기록·리소스 캐시·현재 씬 경로를 새 경로로
	std::function<void(const std::vector<FAssetMove>&)> AssetsMoved;
	// 모델 다시 가져오기 (임포트 설정 저장 후): 캐시 교체 → 열린 씬 인스턴스·편집 창·썸네일 갱신. 실패하면 false
	std::function<bool(const std::filesystem::path&)> ReimportModel;
	// 프리팹 원본을 바꾸는 작업(편집 창 저장, 원본에 적용)을 감싼다: 열린 씬 오버라이드 기록(옛 원본 기준) → Change →
	// 원본 캐시 비우기 → 열린 씬 인스턴스를 새 원본에 맞춤 + 에셋 해석. 반환: Change 결과. 없으면 Change만 실행
	std::function<bool(const std::function<bool()>&)> ChangePrefab;
	// 화면 알림 (bError면 빨간색)
	std::function<void(const std::string&, bool)> Notify;

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
