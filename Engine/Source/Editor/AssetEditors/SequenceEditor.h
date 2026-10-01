#pragma once

#include "Editor/AssetEditors/AssetEditor.h"
#include "Renderer/Camera.h"
#include "Scene/Sequence.h"
#include "Scene/SequencePlayer.h"

#include <memory>
#include <optional>
#include <string>

class FScene;

// .esequence 컷신 편집기 (언리얼 시퀀서식 타임라인).
//   - 왼쪽: 도구 줄(재생/정지/반복, 시각, 뷰포트·카메라 미리보기, 트랙 추가) + 타임라인 (눈금 클릭/끌기 = 재생 헤드, 휠 = 좌우 이동,
//     Ctrl+휠 = 확대, 가운데 끌기 = 이동). 트랙마다 키(마름모)/구간(막대): 클릭 선택, 끌기 이동(프레임 스냅, Alt = 스냅 끔),
//     구간 오른쪽 끝 끌기 = 길이, 빈 곳 더블클릭 = 그 시각에 현재 값으로 키, 우클릭 = 키/트랙 메뉴
//   - 단축키 (창 포커스): Space 재생/일시정지, K 선택 트랙에 재생 헤드 시각 키, Delete 키 삭제, Ctrl+C/V 키 복사·붙여넣기(재생 헤드),
//     ←/→ 한 프레임, Home/End 처음/끝
//   - 오른쪽: 시퀀스 길이/프레임, 선택 트랙(대상 이름 경로, "선택한 엔티티로", 프로퍼티 고르기), 선택 키 값/보간
//   - 뷰포트 미리보기: 재생 헤드 시점을 편집 씬에 적용한다 (FSequenceSystem::Evaluate). 바인딩 기준은 이 에셋을 쓰는
//     SequencePlayerComponent 엔티티. 끄거나 창을 닫거나 플레이를 시작하면 원래 값으로 되돌리고, 씬 저장·실행 취소 기록에는 원래 값이 들어간다
//     (FAssetEditor::SwapScenePreview). 미리보기 중 시퀀스가 움직이는 값을 직접 고치면 다음 평가 때 덮인다 — 키로 넣는다
//   - 카메라 미리보기: 지금 컷 카메라 시점으로 뷰포트를 본다 (편집 카메라는 그대로)
class FSequenceEditor final : public FAssetEditor
{
public:
	explicit FSequenceEditor(std::filesystem::path InPath);
	~FSequenceEditor() override;

	const char* GetTypeName() const override { return "시퀀스"; }
	bool        UsesPreview() const override { return false; }
	float       GetPropertiesWidthWeight() const override { return 0.85f; }
	void        Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;
	void        SwapScenePreview(FAssetEditorEnvironment& Env) override;
	void        EndScenePreview(FAssetEditorEnvironment& Env) override;

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        OnClose(FAssetEditorEnvironment& Env) override;

private:
	enum class EDrag : uint8
	{
		None,
		Playhead,
		Key,        // 키/컷/이벤트/구간 통째로
		SectionEnd, // 애니메이션 구간 끝
		Pan,
	};

	// 편집 씬 / 바인딩 기준 (플레이 중이 아니면 Context.Scene)
	FScene* GetEditScene(FAssetEditorEnvironment& Env) const;
	FEntity FindPlayerEntity(const FScene& Scene) const;
	FEntity ResolveTarget(FAssetEditorEnvironment& Env, const std::string& Path) const;
	void    PublishIfChanged();
	void    EndPreview();

	// 타임라인
	void DrawToolbar(FAssetEditorEnvironment& Env);
	void DrawTimeline(FAssetEditorEnvironment& Env);
	void DrawAddTrackMenu(FAssetEditorEnvironment& Env);
	void HandleShortcuts(FAssetEditorEnvironment& Env);
	std::string TrackLabel(const FSequenceTrack& Track) const;

	// 키 조작
	float Snap(float Seconds) const;
	int32 AddKeyAtTime(FAssetEditorEnvironment& Env, int32 TrackIndex, float Seconds); // 같은 시각 키가 있으면 값만 갱신. 반환: 키 번호
	bool  CaptureTransform(FAssetEditorEnvironment& Env, const FSequenceTrack& Track, float Seconds, FSequenceTransformKey& Key) const;
	bool  CaptureValue(FAssetEditorEnvironment& Env, const FSequenceTrack& Track, FVector4& OutValue) const;
	void  SetKeyTime(FSequenceTrack& Track, int32& InOutKey, float Seconds); // 정렬 유지 (키 번호 갱신)
	void  DeleteSelectedKey();
	void  CopySelectedKey();
	void  PasteKey(float Seconds);

	// 속성
	void DrawTrackProperties(FAssetEditorEnvironment& Env, FSequenceTrack& Track);
	void DrawKeyProperties(FAssetEditorEnvironment& Env, FSequenceTrack& Track, int32 Key);

	FSequenceAsset                        Asset;
	std::shared_ptr<const FSequenceAsset> Published; // 미리보기가 평가하는 편집 상태 사본
	std::string                           PublishedKey;

	float Time             = 0.0f;
	bool  bTimelinePlaying = false;
	bool  bLoopPreview     = true;
	bool  bViewportPreview = true;
	bool  bCameraPreview   = false;

	// 편집 씬 미리보기
	FSequenceEvalState PreviewState;
	FScene*            PreviewScene   = nullptr; // 적용한 씬 (되돌릴 곳)
	bool               bPreviewApplied = false;
	float              AppliedTime     = -1.0f;
	std::string        AppliedKey;
	FEntity            AppliedPlayer;
	FEntity            PreviewPlayer; // 바인딩 기준 (이 에셋의 재생 컴포넌트 엔티티)
	FCamera            PreviewCamera;
	float              DebugTime = -1.0f; // 플레이 중 재생 엔티티의 시각 (없으면 -1)
	std::string        DebugLabel;

	// 타임라인 보기
	float ViewStart       = 0.0f;
	float PixelsPerSecond = 120.0f;
	bool  bFitView        = true;
	int32 SelectedTrack   = -1;
	int32 SelectedKey     = -1;
	EDrag Drag            = EDrag::None;
	int32 DragTrack       = -1;
	float DragGrabOffset  = 0.0f; // 끄는 키 시각 - 잡은 시각
	int32 ContextTrack    = -1;
	int32 ContextKey      = -1;
	float ContextTime     = 0.0f;
	bool  bTimelineFocused = false;

	std::optional<FSequenceTrack> KeyClipboard; // 키 하나만 든 트랙 (같은 종류 트랙에만 붙여넣기)
};
