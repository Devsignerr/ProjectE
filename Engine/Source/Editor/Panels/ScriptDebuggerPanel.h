#pragma once

#include "Scripting/ScriptDebugger.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct FEditorContext;

// Lua 스크립트 디버거 패널 (Phase 47 사이드): 소스 보기 + 여백 클릭 중단점, 정지 줄 강조, 툴바(계속 F5 / 단계 F10 / 들어가기 F11 /
// 나가기 Shift+F11 / 플레이 정지), 호출 스택·지역 변수·조사식·중단점 탭. 디버거 코어(FScriptDebugger)를 소유한다.
//
// 정지하면 디버거가 OnPaused(앱의 중첩 루프)를 부른다 — 앱은 그 안에서 이 패널을 그리고, 재개 명령이 내려지면 돌아온다.
// 중단점·조사식·오류 시 멈춤은 프로젝트 사용자 상태(설정 섹션 "ScriptDebugger", <Saved>/Config/ScriptDebugger.json)에 저장한다
// (자동 검증 실행은 읽지도 쓰지도 않는다 — Initialize(false)).
class FScriptDebuggerPanel
{
public:
	// bPersistent: 저장된 중단점을 읽고 바뀌면 저장 (자동 검증은 false)
	void Initialize(bool bPersistent);
	void Draw(FEditorContext& Context);
	// 정지 중 단축키 (F5 계속, F10 넘기기, F11 들어가기, Shift+F11 나가기) — 앱이 정지 프레임마다 부른다
	void HandlePausedShortcuts();

	FScriptDebugger& GetDebugger() { return Debugger; }
	// 콘텐츠 브라우저 더블클릭 / 메뉴: 소스 열기 + 창 앞으로 (절대 경로 또는 Content 기준)
	void OpenFile(const FEditorContext& Context, const std::filesystem::path& Path);
	// 핫 리로드로 파일이 바뀜: 열린 소스 다시 읽기 + 줄 수를 넘는 중단점 제거
	void OnScriptFileChanged(const FEditorContext& Context, const std::filesystem::path& AbsolutePath);

	// 앱 연결: 정지하면 부른다 (중첩 루프 — 재개 명령이 내려질 때까지 돌아오지 않는다)
	std::function<void()> OnPaused;
	// 툴바 "플레이 정지" (정지 중이면 재개 후 정지)
	std::function<void()> OnStopPlay;
	bool bOpen = true;

private:
	struct FSourceFile
	{
		std::string              Asset; // Content 기준 ('/' 구분)
		std::vector<std::string> Lines;
	};
	struct FWatch
	{
		std::string     Expression;
		FScriptVariable Result;
		bool            bOk = false;
	};

	void HandlePause(); // 디버거 정지 처리기
	void SavePersistentState();
	FSourceFile* FindOrLoad(const FEditorContext& Context, const std::string& Asset);
	static bool  LoadLines(const std::filesystem::path& Path, std::vector<std::string>& OutLines);

	void DrawToolbar(FEditorContext& Context);
	void DrawSource(FEditorContext& Context);
	void DrawCallStack(FEditorContext& Context);
	void DrawVariables();
	void DrawWatches();
	void DrawBreakpoints(FEditorContext& Context);
	void DrawVariableRow(const FScriptVariable& Variable, int32 Depth);
	void RefreshPausedViews(); // 프레임 선택이 바뀌거나 새로 정지했을 때 지역 변수/조사식 다시 조회

	FScriptDebugger           Debugger;
	bool                      bPersistent = false;
	uint32                    SavedRevision = 0;
	std::vector<FSourceFile>  Files;
	std::string               ActiveFile;       // 소스 탭 (Content 기준)
	int32                     ScrollToLine = 0; // 다음 그리기에서 이 줄로 스크롤 (0 = 없음)
	bool                      bSelectActiveTab = false; // ActiveFile 탭을 코드로 고름 (반영될 때까지)
	bool                      bFocusWindow = false;
	bool                      bSelectLocalsTab = false;
	std::vector<std::string>  ScriptList;       // 열기 콤보 (Content의 .lua)
	std::vector<std::string>  WatchExpressions; // 저장되는 조사식
	char                      NewWatch[256] = {};
	char                      ConditionBuffer[256] = {};
	int32                     ConditionLine = 0;

	// 정지 중 조회 결과 (정지/프레임 선택이 바뀔 때만 다시 조회 — 핸들은 조회마다 새로 발급되므로 매 프레임 부르지 않는다)
	uint64                                                    PauseSerial    = 0;
	uint64                                                    ViewSerial     = ~0ull;
	int32                                                     SelectedFrame  = 0;
	int32                                                     ViewFrame      = -1;
	std::vector<FScriptVariable>                              Locals;
	std::vector<FScriptVariable>                              Upvalues;
	std::vector<FWatch>                                       Watches;
	std::unordered_map<uint32, std::vector<FScriptVariable>> Children; // 핸들 → 펼친 항목
};
