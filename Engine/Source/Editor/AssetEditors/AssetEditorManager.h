#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/UIRenderer.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class FEditorGrid;

// 열린 에셋 편집 창 관리: 파일 확장자로 편집기 종류를 고르고, 경로당 창 하나만 연다.
// 미리보기 전용 씬 렌더러를 하나 소유해 모든 창의 미리보기를 그린다 (메인 뷰포트 렌더러와 버퍼/통계 분리).
// 프레임 순서: Update(OnUpdate) → Draw(ImGui 프레임 안) → RenderPreviews(Rhi BeginFrame 이후, UI 기록 전)
class FAssetEditorManager
{
public:
	FAssetEditorManager();
	~FAssetEditorManager();

	// 편집 창이 생긴 뒤 처음 필요할 때 렌더러를 만든다 (에디터 시작 비용 없음)
	void Shutdown(FEditorContext& Context); // GPU 유휴 상태에서 호출

	// 지원하는 확장자인지 (콘텐츠 브라우저 더블클릭 등)
	static bool CanOpen(const std::filesystem::path& Path);
	// 이미 열려 있으면 앞으로 가져온다. 실패 시 false
	bool Open(FEditorContext& Context, const std::filesystem::path& Path);

	void Update(FEditorContext& Context, float DeltaSeconds);
	void Draw(FEditorContext& Context);
	void RenderPreviews(FEditorContext& Context);
	// ChangedFiles가 nullptr이면 전체 강제 재컴파일, 아니면 바뀐 셰이더 파일만 무효화 후 다시 로드
	bool ReloadShaders(const std::vector<std::filesystem::path>* ChangedFiles);

	// 지난 프레임에 편집 창이 포커스를 가졌는지 (메인 단축키/Delete를 창에 양보)
	// 모델을 다시 가져온 뒤 그 모델의 편집 창 미리보기를 새로 만든다
	void OnModelReimported(FEditorContext& Context, const std::filesystem::path& Path);

	// 파일 조작 전: Paths(폴더면 안쪽 전부)의 편집 창을 닫는다. 저장하지 않은 창이 하나라도 있으면 아무것도 닫지 않고 false
	bool CloseEditorsFor(FEditorContext& Context, const std::vector<std::filesystem::path>& Paths);

	// 자동 검증 (--verify-asset-close): 열린 편집 창마다 값을 바꾼 뒤 저장하지 않고 닫는다. 반환: 닫은 창 수
	uint32 VerifyCloseWithoutSave(FEditorContext& Context);

	// 편집 씬 미리보기(시퀀서): 씬 저장/스냅샷 앞뒤로 Swap(두 번 = 원상태), 플레이 시작·씬 교체 전 End
	void SwapScenePreviews(FEditorContext& Context);
	void EndScenePreviews(FEditorContext& Context);
	// 리소스 수거 루트: 열린 편집 창마다 (미리보기 씬 + 편집 중인 핸들)
	void CollectResourceRoots(FResourceRoots& Roots);

	bool HasFocusedEditor() const { return bEditorFocused; }
	size_t GetOpenCount() const { return Editors.size(); }

private:
	struct FOpenEditor
	{
		std::unique_ptr<FAssetEditor> Editor;
		std::wstring                  Key; // 정규화 경로
		bool                          bRequestFocus = true;
	};

	bool                    EnsureRenderer(FEditorContext& Context);
	FAssetEditorEnvironment MakeEnvironment(FEditorContext& Context);
	void                    HandleShortcuts(FAssetEditorEnvironment& Env, FAssetEditor& Editor);
	void                    DrawCloseConfirm(FEditorContext& Context);
	void                    CloseEditor(FEditorContext& Context, size_t Index);

	std::vector<FOpenEditor>     Editors;
	FSceneRenderer               PreviewRenderer;
	FUIRenderer                  UIRenderer; // UI 디자이너 미리보기
	std::unique_ptr<FEditorGrid> Grid;
	bool                         bRendererReady  = false;
	bool                         bRendererFailed = false;
	bool                         bEditorFocused  = false;
	std::wstring                 PendingCloseKey; // 저장 확인 대화상자 대상
};
