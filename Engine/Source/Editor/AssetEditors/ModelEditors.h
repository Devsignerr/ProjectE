#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "Renderer/ModelImportSettings.h"
#include "Scene/AnimNotify.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

struct FModelMetadata;
struct FModelResources;

// 모델(.glb/.gltf/.fbx) 편집 공통: 모델 배치, 정보(노드/메시/머티리얼/경계), 와이어프레임, 씬에 추가, 임포트 설정, 소켓.
// 편집 상태(저장/되돌리기/실행 취소 대상) = 원본 옆 .emeta (노티파이 + 소켓). 리소스 캐시의 공유 메타데이터를 직접 고치므로
// 열린 씬에 바로 반영되고, 저장하지 않고 닫으면 파일 상태로 되돌린다. 임포트 설정은 별도의 "적용" 버튼 (모델을 다시 가져옴)
class FModelEditorBase : public FAssetEditor
{
public:
	using FAssetEditor::FAssetEditor;

	// 애니메이션(또는 스킨 — 리타기팅 미리보기)이 있으면 애니메이션 편집기로 연다
	static bool HasAnimations(const std::filesystem::path& Path, FResourceManager& Resources);

	// 다시 가져온 뒤: 미리보기 모델을 새 리소스로 다시 배치 (저장 안 한 노티파이/소켓 편집은 유지)
	virtual void RebuildPreview(FAssetEditorEnvironment& Env);

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawPreviewToolbar(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewOverlay(FAssetEditorEnvironment& Env) override;
	void        RenderPreview(FAssetEditorEnvironment& Env) override;

	void DrawModelInfo(FAssetEditorEnvironment& Env);
	void DrawAddToScene(FAssetEditorEnvironment& Env);
	// 임포트 설정 (원본 옆 .eimport) 편집 + 다시 가져오기
	void DrawImportSettings(FAssetEditorEnvironment& Env);
	// 소켓 목록/속성 (속성 패널) + 표시/기즈모 (미리보기 오버레이)
	void DrawSockets();
	void DrawSocketOverlay();

	// 노드(뼈) 월드 행렬. Bone이 비면 모델 루트. 없으면 false
	bool GetBoneWorld(const std::string& Bone, FMatrix4x4& OutWorld);
	// .emeta 리타기팅 매핑이 바뀌었을 때 (편집/저장/되돌리기): 이 모델이 소스·대상인 리타기팅 결과를 다시 만들게 한다
	void InvalidateRetargeting() const;

	const FModelResources*          Model = nullptr; // FResourceManager 모델 캐시 (주소 고정)
	std::shared_ptr<FModelMetadata> Metadata;        // Model->Metadata (공유)
	FEntity                         ModelRoot;
	bool                            bWireframe = false;
	FModelImportSettings            ImportSettings;      // 편집 중 값
	FModelImportSettings            SavedImportSettings; // 파일에 있는 값 (바뀌었는지 비교)
	bool                            bImportSettingsLoaded = false;

	int32 SelectedSocket    = -1;
	bool  bSocketRotateMode = false; // 기즈모: 이동 / 회전
	char  SocketNameBuffer[64] = {};
	int32 SocketNameFor        = -1;
	FVector3 SocketEulerDegrees;      // 회전 입력 캐시 (드래그 중 오일러 변환 흔들림 방지)
	FQuat    SocketEulerRotation;
	int32    SocketEulerFor = -1;
};

class FStaticMeshEditor final : public FModelEditorBase
{
public:
	using FModelEditorBase::FModelEditorBase;
	const char* GetTypeName() const override { return "스태틱 메시"; }

protected:
	void DrawProperties(FAssetEditorEnvironment& Env) override;
};

// 스킨/애니메이션 모델: 클립 선택, 재생/일시정지/한 프레임, 타임라인 스크럽, 속도·루프, 뼈대 표시, 노티파이 트랙,
// 리타기팅 (휴머노이드 본 매핑 확인/수정 — .emeta, 다른 모델 클립을 이 모델에서 미리보기)
//   자동 검증: --retarget-preview <소스 모델(Content 기준)>[:<클립>] — 열 때 그 모델을 미리보기 소스로 (클립이 있으면 재생)
class FAnimationEditor final : public FModelEditorBase
{
public:
	using FModelEditorBase::FModelEditorBase;
	const char* GetTypeName() const override { return "애니메이션"; }

	void Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;

protected:
	bool LoadAsset(FAssetEditorEnvironment& Env) override;
	void DrawProperties(FAssetEditorEnvironment& Env) override;
	void DrawPreviewOverlay(FAssetEditorEnvironment& Env) override;
	void DrawPreviewToolbar(FAssetEditorEnvironment& Env) override;

private:
	void DrawBones();
	// 현재 클립 노티파이 트랙 (추가/끌기/삭제/이름) + 선택 항목 속성 + 최근 발생 이벤트
	void DrawNotifyTrack(const std::string& Clip, float Duration, float Time);
	// 리타기팅: 리그 뼈 → 노드 매핑 표 (자동/수동), 미리보기 소스 모델
	void DrawRetargeting(FAssetEditorEnvironment& Env);
	void SetPreviewSource(const std::string& Source);

	std::string PreviewSource; // 미리보기 리타기팅 소스 모델 (Content 기준, 비면 없음 — 파일에 저장하지 않음)
	bool        bCommandLineApplied = false;

	struct FRecentEvent
	{
		FAnimNotifyEvent Event;
		double           TimeSeconds = 0.0; // ImGui 시각
	};

	std::vector<std::string> ClipNames;
	std::vector<FEntity>     Joints; // 스킨 관절 엔티티 (뼈대 표시)
	bool                     bShowBones  = true;
	float                    StepSeconds = 1.0f / 30.0f;

	int32                    SelectedNotify = -1;
	std::string              SelectedNotifyClip;
	int32                    DragMode       = 0;    // 0 없음, 1 이동, 2 길이
	float                    DragGrabOffset = 0.0f; // 끌기 시작점과 항목 시작 시각의 차
	char                     NotifyNameBuffer[64] = {};
	int32                    NotifyNameFor        = -1;
	float                    ContextTime          = 0.0f; // 우클릭 메뉴를 연 시각
	int32                    ContextHit           = -1;
	std::deque<FRecentEvent> RecentEvents;
};
