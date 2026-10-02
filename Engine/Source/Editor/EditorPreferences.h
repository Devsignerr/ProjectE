#pragma once

#include "Core/CoreTypes.h"

#include <string>

// ---- 에디터 환경설정 (개인 설정, 커밋하지 않음). 새 항목은 필드 + EditorPreferences.cpp 등록만 하면 창/저장에 나타난다

// "EditorGeneral" — %LOCALAPPDATA%/ProjectE/EditorPreferences/EditorGeneral.json (모든 프로젝트 공통)
struct FEditorGeneralSettings
{
	bool   bLoadLastSceneOnStartup = true;  // 시작 시 마지막으로 연 씬 (프로젝트 설정 "에디터 시작 맵"보다 우선, --scene이 가장 우선)
	bool   bAutoSave               = true;
	float  AutoSaveIntervalMinutes = 10.0f; // 저장 안 한 변경이 있을 때 이 간격으로 <Saved>/Autosaves/에 사본 (원본 파일은 건드리지 않는다)
	uint32 AutoSaveKeepCount       = 5;     // 씬마다 남길 자동 저장 사본 수
	bool   bAutoReloadGameModule   = true;  // C++ 게임 모듈 DLL이 다시 빌드되면(외부 빌드 포함) 감지해 자동으로 다시 로드
};

// "EditorViewport" — 공통
struct FEditorViewportSettings
{
	float  DefaultCameraSpeed = 500.0f; // cm/초 — 저장된 에디터 카메라가 없을 때 (휠로 바꾼 속도는 프로젝트별로 기억)
	float  MouseSensitivity   = 0.15f;  // 도/픽셀
	float  FieldOfView        = 60.0f;  // 원근 수직 시야각 (도)
	bool   bSnapEnabled       = false;
	float  TranslateSnap      = 10.0f;  // cm
	float  RotateSnap         = 15.0f;  // 도
	float  ScaleSnap          = 0.1f;
	bool   bVSync             = false;  // 켜면 프레임이 모니터 주사율에 묶인다 (끄면 테어링 지원 시 제한 없음)
};

// "EditorPerProjectUserSettings" — <Saved>/Config/ (프로젝트별 개인 상태, 창에 표시하지 않음)
struct FEditorProjectState
{
	std::string LastOpenedScene; // Content 기준
};

// 에디터 환경설정 전체 (에디터 프로세스 하나). 에디터 시작 시 Register + Load
class FEditorPreferences
{
public:
	static FEditorPreferences& Get();

	FEditorGeneralSettings  General;
	FEditorViewportSettings Viewport;
	FEditorProjectState     ProjectState;

	// 섹션 등록 + 파일 읽기 (FPaths 초기화 후, 한 번)
	void Initialize();
	bool SaveProjectState() const;
	bool SaveViewport() const;

private:
	FEditorPreferences() = default;
	bool bInitialized = false;
};
