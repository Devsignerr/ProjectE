#pragma once

#include "Core/CoreTypes.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class FThumbnailCache;
struct ImGuiSelectionBasicStorage;
struct FAssetMove;
struct FEditorContext;

// 콘텐츠 브라우저 (언리얼 풍): 왼쪽 폴더 트리 | 위 경로 표시줄·검색·보기 전환 | 썸네일 타일(또는 목록).
//   다중 선택(Ctrl/Shift/드래그 박스), 드래그 앤 드롭(폴더로 이동, 뷰포트/계층/인스펙터로), 우클릭 메뉴,
//   이름 바꾸기(F2)/복제/삭제(휴지통)/새 폴더/새 에셋. 이동·이름 변경은 다른 에셋·열린 씬의 경로 참조를 함께 고친다.
class FContentBrowserPanel
{
public:
	FContentBrowserPanel();
	~FContentBrowserPanel();

	void Draw(FEditorContext& Context);
	// Rhi BeginFrame 이후 (UI 기록 전): 보이는 썸네일 몇 개씩 그리기
	void RenderThumbnails(FEditorContext& Context);
	void Shutdown(FEditorContext& Context); // GPU 유휴 상태에서
	bool ReloadShaders(const std::vector<std::filesystem::path>* ChangedFiles);

	// 윈도우 탐색기에서 끌어 놓은 파일/폴더를 현재 폴더로 복사
	void ImportExternalFiles(FEditorContext& Context, const std::vector<std::filesystem::path>& Files);

	// 파일/폴더 이동, 이름 바꾸기 (참조 갱신 포함). 드래그 앤 드롭/메뉴와 자동 검증이 쓴다
	bool MoveAssets(FEditorContext& Context, const std::vector<std::filesystem::path>& Sources, const std::filesystem::path& DestinationDirectory);
	bool RenameAsset(FEditorContext& Context, const std::filesystem::path& Source, const std::wstring& NewName);

	// 썸네일을 다음에 다시 그리게 한다 (다시 가져오기 등)
	void InvalidateThumbnail(const std::filesystem::path& Path);

	// 현재 폴더 지정 (자동 검증 인자 --content-dir)
	void SetCurrentDirectory(const std::filesystem::path& Directory) { Navigate(Directory); }

	// 지난 프레임에 창이 포커스를 가졌는지 (Delete 등 단축키를 메인 씬에 넘기지 않기 위해)
	bool IsFocused() const { return bFocused; }

	bool bOpen = true;

private:
	struct FEntry
	{
		std::filesystem::path Path;
		std::string           DisplayName;
		std::string           Extension; // 소문자
		bool                  bDirectory  = false;
		uint64                SizeInBytes = 0;
		uint32                Id          = 0; // 선택 저장용 (경로 해시, ImGuiID)
	};
	struct FFolderNode
	{
		std::filesystem::path    Path;
		std::string              Name;
		std::vector<FFolderNode> Children;
	};

	void Refresh(const std::filesystem::path& Root);
	void Navigate(const std::filesystem::path& Directory);

	void DrawToolbar(FEditorContext& Context);
	void DrawBreadcrumb(FEditorContext& Context);
	void DrawFolderNode(FEditorContext& Context, const FFolderNode& Node, bool bRoot);
	void DrawItems(FEditorContext& Context);
	void DrawTile(FEditorContext& Context, const FEntry& Entry, float TileWidth);
	void DrawListRow(FEditorContext& Context, const FEntry& Entry);
	void HandleItemInteraction(FEditorContext& Context, const FEntry& Entry);
	void DrawItemContextMenu(FEditorContext& Context, const FEntry& Entry);
	void DrawBackgroundContextMenu(FEditorContext& Context);
	void DrawRenamePopup(FEditorContext& Context);
	void DrawDeletePopup(FEditorContext& Context);
	void HandleShortcuts(FEditorContext& Context);

	void OpenEntry(FEditorContext& Context, const FEntry& Entry);
	void AddToScene(FEditorContext& Context, const FEntry& Entry);
	// 폴더에 놓인 콘텐츠 페이로드 → 이동 (직전 항목이 드롭 대상)
	void AcceptMoveDrop(FEditorContext& Context, const std::filesystem::path& DestinationDirectory);
	void MoveSidecar(const std::filesystem::path& Source, const std::filesystem::path& NewPath, std::vector<FAssetMove>& Moves);
	void Duplicate(FEditorContext& Context, const std::vector<std::filesystem::path>& Sources);
	void RequestDelete(FEditorContext& Context, const std::vector<std::filesystem::path>& Targets);
	void BeginRename(const std::filesystem::path& Path);
	void CreateAsset(FEditorContext& Context, const std::string& BaseName, const std::wstring& Extension);
	void CreateDataFile(FEditorContext& Context, const std::wstring& Extension, const std::string& StructPath); // .etable/.edata (구조체 지정)
	void CreateFolder(FEditorContext& Context);

	std::vector<std::filesystem::path> GetSelectedPaths() const;
	bool                               PassesFilter(const FEntry& Entry) const;
	void                               Notify(FEditorContext& Context, const std::string& Message, bool bError) const;

	std::filesystem::path CurrentDirectory;
	std::filesystem::path Root;
	std::vector<FEntry>   Entries;
	std::vector<size_t>   Visible; // 필터를 통과한 Entries 인덱스 (다중 선택 인덱스)
	FFolderNode           FolderTree;
	bool                  bNeedsRefresh = true;
	std::chrono::steady_clock::time_point LastRefresh;

	std::unique_ptr<ImGuiSelectionBasicStorage> Selection; // 다중 선택 (ImGui 기본 저장소)
	char                       SearchText[128] = {};
	int32                      TypeFilter      = 0; // 0 = 전체
	float                      TileSize        = 88.0f;
	bool                       bListView       = false;
	bool                       bFocused        = false;

	// 이름 바꾸기 / 삭제 대화상자
	std::filesystem::path              RenameTarget;
	char                               RenameBuffer[256] = {};
	bool                               bOpenRenamePopup  = false;
	std::vector<std::filesystem::path> DeleteTargets;
	size_t                             DeleteReferenceCount = 0;
	bool                               bOpenDeletePopup     = false;

	std::unique_ptr<FThumbnailCache> Thumbnails;
};
