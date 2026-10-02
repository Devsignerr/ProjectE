#pragma once

#include "Renderer/MaterialGraph.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ax::NodeEditor
{
	struct EditorContext;
}

// 머티리얼 그래프 노드 편집 화면 (Phase 51 사이드, 언리얼 머티리얼 에디터식, imgui-node-editor). FMaterialEditor가 소유한다.
//   - 노드: 제목(종류 · Id) + 설정 요약 + 왼쪽 입력 핀 / 오른쪽 출력 핀. 핀·연결 색 = 값 너비(float1~4, 분석 결과 — 모르면 회색)
//   - 출력 노드("@Output", 머티리얼 출력 8핀)는 항상 있고 지울 수 없다. 위치는 FMaterialGraph::OutputEditorPosition
//   - 노드 추가: 배경 우클릭/스페이스 → 검색 팔레트(노드 표 GetNodeInfos의 범주·순서 — 새 노드는 자동으로 나타남),
//     핀에서 링크를 빈 곳에 놓으면 팔레트 + 새 노드 첫 핀에 자동 연결
//   - 핀 끌기 = 연결(입력 핀 하나에 하나, 순환 거부), 링크 선택 Delete/우클릭 = 끊기, 노드 Delete = 삭제,
//     Ctrl+C/V/X/D = 복사/붙여넣기(마우스 위치)/잘라내기/복제(MaterialGraphEditing), 주석 상자(배경 메뉴 — 선택 노드를 감쌈)
//   - 오류: 컴파일 오류 노드는 빨간 테두리 + 메시지, 출력에서 닿지 않는 노드의 분석 오류는 노란 테두리
//   - 읽기 전용(머티리얼 인스턴스: 부모 그래프 보기): 선택·탐색만, 편집/위치 저장 없음
// 구조 변경은 그래프 순회가 끝난 뒤 적용하고(PendingChanges) OnEdited(라벨, 의미 변경 여부)로 알린다(위치/주석만 바뀌면 false).
class FMaterialGraphPanel
{
public:
	struct FContext
	{
		FMaterialGraph*                    Graph      = nullptr;
		std::vector<FMaterialParameter>*   Parameters = nullptr;
		bool                               bReadOnly  = false;
		const FMaterialGraphCompileResult* Compile    = nullptr; // 이 그래프의 컴파일 결과 (없으면 오류 표시 없음)
		const FMaterialGraphAnalysis*      Analysis   = nullptr;
		const std::vector<std::string>*    TextureFiles = nullptr; // 텍스처 선택 목록 (.emat 폴더 기준)
		std::filesystem::path              AssetDirectory;         // 텍스처 드롭 경로 기준
		std::function<void(std::string_view Label, bool bSemantic)> OnEdited;
	};

	FMaterialGraphPanel();
	~FMaterialGraphPanel();
	FMaterialGraphPanel(const FMaterialGraphPanel&)            = delete;
	FMaterialGraphPanel& operator=(const FMaterialGraphPanel&) = delete;

	// 남은 영역에 그래프를 그리고 편집을 처리한다
	void Draw(FContext& Context);
	// 상세 패널: 선택한 노드(또는 주석)의 설정과 입력 핀. 선택이 없으면 false (호출자가 머티리얼 설정을 그린다)
	bool DrawSelectionDetails(FContext& Context);

	// 에셋을 다시 읽었을 때(열기/되돌리기/실행 취소): 다음 프레임에 위치를 그래프에 넣는다. bFrame이면 전체 보기도
	void OnAssetReloaded(bool bFrame);
	void RequestFrame() { NavigateFrames = 1; }
	// 자동 검증 --matgraph-select: 열린 뒤 이 노드를 선택해 가운데에
	void SetAutoSelect(std::string NodeId) { AutoSelectId = std::move(NodeId); }
	void RequestPalette() { bPaletteRequestCenter = true; }
	const std::vector<std::string>& GetSelectedNodes() const { return SelectedNodes; }

	// 파라미터 값 위젯 (상세·파라미터 패널 공용). 반환: 값이 바뀌었으면 true (bOutTexture = 텍스처 경로/용도).
	// bEditUsage = false면 텍스처 용도를 고치지 않는다 (인스턴스 — 타입·용도는 부모 것)
	static bool DrawParameterValue(FMaterialParameter& Parameter, const char* Label, const std::vector<std::string>* TextureFiles,
	                               const std::filesystem::path& AssetDirectory, bool& bOutTexture, bool bEditUsage = true);
	// 너비별 핀 색 (float1~4, 0 = 모름)
	static uint32 GetWidthColor(uint32 Width);

private:
	struct FGraphDeleter
	{
		void operator()(ax::NodeEditor::EditorContext* Editor) const;
	};

	uint32      GetKey(const std::string& NodeId);
	std::string FindNodeIdByKey(uint32 Key) const;
	void        BuildStatus(const FContext& Context);
	void        DrawComments(FContext& Context);
	void        DrawNode(FContext& Context, const FMaterialGraphNode& Node);
	void        DrawOutputNode(FContext& Context);
	void        DrawLinks(FContext& Context);
	void        HandleCreateAndDelete(FContext& Context);
	void        HandleShortcuts(FContext& Context);
	void        DrawContextMenus(FContext& Context);
	void        DrawPalette(FContext& Context);
	void        SyncFromGraph(FContext& Context);
	void        ApplyPositions(FContext& Context);
	void        Edit(FContext& Context, std::string_view Label, bool bSemantic, std::function<void()> Change);
	uint32      GetSourceWidth(const FContext& Context, const FMaterialGraphInput& Input) const;
	std::string MakeSummary(const FContext& Context, const FMaterialGraphNode& Node) const;
	void        CopySelection(FContext& Context, bool bCut);
	void        PasteAt(FContext& Context, const std::string& Text, const FVector2& CanvasPosition);
	void        AddCommentAroundSelection(FContext& Context, const FVector2& FallbackPosition);
	bool        DrawNodeDetails(FContext& Context, FMaterialGraphNode& Node);
	bool        DrawPinInputs(FContext& Context, const std::string& NodeId, const std::vector<FMaterialGraphPinInfo>& Pins);

	std::unique_ptr<ax::NodeEditor::EditorContext, FGraphDeleter> Editor;
	std::unordered_map<std::string, uint32>                        Keys; // 노드 Id → 편집기 키 (세션 동안 다시 쓰지 않음)
	uint32                                                         NextKey = 2; // 1 = 출력 노드
	std::vector<std::function<void()>>                             PendingChanges;
	std::vector<std::string>                                       SelectedNodes;
	std::vector<std::string>                                       PendingSelection; // 다음 프레임에 고를 노드 (붙여넣기/추가 직후)
	int32                                                          SelectedComment = -1;
	std::vector<FVector2>                                          CommentNodeSizes; // 주석 노드 전체 크기 (지난 프레임 — 크기 조절 추적)
	bool                                                           bApplyPositions = true;
	int32                                                          NavigateFrames  = 2;
	std::string                                                    AutoSelectId;
	int32                                                          AutoSelectFrames = 3;

	// 상태 (프레임마다 결과에서 다시 만든다): 노드 Id → 오류 메시지, 오류 수준(2 = 컴파일 오류, 1 = 분석만)
	std::unordered_map<std::string, std::vector<std::string>> NodeMessages;
	std::unordered_map<std::string, int32>                    NodeSeverity;

	// 팔레트
	bool        bOpenPalette          = false;
	bool        bPaletteRequestCenter = false;
	FVector2    PaletteSpawn          = FVector2::ZeroVector; // 캔버스 좌표
	uintptr_t   PalettePin            = 0;                    // 링크를 빈 곳에 놓아 연 팔레트: 시작 핀 (0 = 없음)
	char        PaletteFilter[64]     = {};
	// 우클릭 메뉴 대상
	uintptr_t   ContextNode = 0;
	uintptr_t   ContextPin  = 0;
	uintptr_t   ContextLink = 0;
	FVector2    ContextCanvas = FVector2::ZeroVector;
	// 파라미터 이름 편집 중 (상세 패널)
	std::string ParameterNameEdit;
	std::string ParameterNameEditOwner;
};
