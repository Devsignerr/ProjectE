#pragma once

#include "Editor/AssetEditors/AssetPreview.h"
#include "Editor/UndoHistory.h"

#include <filesystem>
#include <string>

class FD3D12RHI;
class FEditorGrid;
class FResourceManager;
class FSceneRenderer;
class FUIRenderer;
struct FEditorContext;
struct FResourceRoots;

// 에셋 편집 창이 쓰는 공유 객체 (소유하지 않음)
struct FAssetEditorEnvironment
{
	FEditorContext*   Editor          = nullptr; // 메인 에디터 (열린 씬 반영 등)
	FD3D12RHI*        Rhi             = nullptr;
	FResourceManager* Resources       = nullptr;
	FSceneRenderer*   PreviewRenderer = nullptr; // 모든 미리보기 공용 전용 렌더러
	FEditorGrid*      Grid            = nullptr; // 미리보기 바닥 그리드 (없을 수 있음)
	FUIRenderer*      UIRenderer      = nullptr; // 게임 UI 미리보기 (UI 디자이너)
};

// 에셋 편집 창 공통 기반: 파일 하나 + 미리보기 + 창 안 실행 취소(에셋 상태 문자열 스냅샷) + 저장/되돌리기.
// 파생 클래스는 상태 캡처/복원(보통 에셋 JSON)과 편집 UI를 구현한다. 편집 코드는 변경 직후 MarkEdited를 호출한다.
class FAssetEditor
{
public:
	explicit FAssetEditor(std::filesystem::path InPath);
	virtual ~FAssetEditor();

	FAssetEditor(const FAssetEditor&)            = delete;
	FAssetEditor& operator=(const FAssetEditor&) = delete;

	// 에셋 로드 + 미리보기 준비. 실패하면 창을 열지 않는다
	bool Open(FAssetEditorEnvironment& Env);
	// 창 닫기: 저장하지 않은 변경은 파일 상태로 되돌린다 (실시간 반영된 공유 리소스 복구)
	void Close(FAssetEditorEnvironment& Env);

	virtual const char* GetTypeName() const = 0; // 창 제목 앞 에셋 종류 ("머티리얼")
	// 저장할 편집 상태가 있는지 (없으면 저장/되돌리기/실행 취소 버튼을 숨긴다)
	virtual bool        HasEditableState() const { return true; }
	// 속성 열 폭 비율 (미리보기 = 2). 편집 항목이 많은 창은 크게
	virtual float       GetPropertiesWidthWeight() const { return 1.0f; }
	// false면 3D 미리보기(렌더 타깃)를 만들지 않는다 — 왼쪽 열은 DrawPreviewArea가 채운다 (노드 그래프 편집기 등)
	virtual bool        UsesPreview() const { return true; }
	virtual void        Update(FAssetEditorEnvironment& Env, float DeltaSeconds);
	// 창 내용 (ImGui 창 Begin~End 사이에서 호출)
	void                Draw(FAssetEditorEnvironment& Env);
	virtual void        RenderPreview(FAssetEditorEnvironment& Env);

	bool Save(FAssetEditorEnvironment& Env);
	void RevertToSaved(FAssetEditorEnvironment& Env);
	void Undo(FAssetEditorEnvironment& Env);
	void Redo(FAssetEditorEnvironment& Env);
	bool CanUndo() const { return History.CanUndo(); }
	bool CanRedo() const { return History.CanRedo(); }

	// 편집 씬에 미리보기 값을 쓰는 편집기(시퀀서)용: 씬 저장·실행 취소 스냅샷 직전/직후에 원래 값과 맞바꾸고(두 번 = 원상태),
	// 플레이 시작·씬 교체 전에는 미리보기를 끝낸다 (FAssetEditorManager::SwapScenePreviews/EndScenePreviews)
	virtual void SwapScenePreview(FAssetEditorEnvironment& Env) { (void)Env; }
	virtual void EndScenePreview(FAssetEditorEnvironment& Env) { (void)Env; }

	// 리소스 수거 루트 (Phase 37): 기본 = 미리보기 씬. 씬 밖에서 핸들을 드는 편집기는 덧붙인다
	virtual void CollectResourceRoots(FResourceRoots& Roots);

	// 조작(드래그/텍스트 입력)이 끝나면 한 단계로 커밋. 창 Draw 뒤 매 프레임 호출
	void CommitPendingEdit(bool bInteractionActive);
	// 자동 검증용: 상태 JSON의 첫 숫자 값을 바꿔 편집 한 단계를 만든다 (저장 안 함 닫기 재현). 바꿀 값이 없으면 false
	bool ApplyTestEdit(FAssetEditorEnvironment& Env);
	void MarkEdited(std::string_view Label) { PendingEdit.Mark(Label); }

	bool                         IsDirty() const { return History.IsDirty() || PendingEdit.IsPending(); }
	const std::filesystem::path& GetPath() const { return Path; }
	std::string                  GetDisplayName() const;
	FAssetPreview&               GetPreview() { return Preview; }

protected:
	// 파일에서 에셋을 읽어 편집 상태/미리보기 구성. 되돌리기에서도 다시 불리므로 여러 번 호출해도 안전해야 한다
	virtual bool        LoadAsset(FAssetEditorEnvironment& Env) = 0;
	virtual bool        SaveAsset(FAssetEditorEnvironment& Env) = 0;
	virtual std::string CaptureState() const = 0;
	virtual void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) = 0;
	// 오른쪽 속성 패널 / 미리보기 위 오버레이
	virtual void DrawProperties(FAssetEditorEnvironment& Env) = 0;
	virtual void DrawPreviewOverlay(FAssetEditorEnvironment& Env);
	// 왼쪽 미리보기 열 전체 (기본: 도구 줄 + 3D 미리보기 + 오버레이). 3D가 아닌 편집기(UI 디자이너)가 바꾼다
	virtual void DrawPreviewArea(FAssetEditorEnvironment& Env);
	// 미리보기 위쪽 도구 줄 (기본: 그리드 토글, 화면 맞춤)
	virtual void DrawPreviewToolbar(FAssetEditorEnvironment& Env);
	// 화면 맞춤 (F)
	virtual void FramePreview(FAssetEditorEnvironment& Env);
	// 창을 닫기 직전 (편집기 소유 GPU 리소스 지연 해제)
	virtual void OnClose(FAssetEditorEnvironment& Env);

	FAssetPreview         Preview;
	std::filesystem::path Path;
	FUndoHistory          History;
	FPendingEdit          PendingEdit;
};
