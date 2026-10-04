#pragma once

#include "Editor/AssetEditors/Sprite2DCanvas.h"
#include "Editor/AssetEditors/Sprite2DEditorBase.h"
#include "Scene/Sprite/FlipbookAsset.h"
#include "Scene/Sprite/SpriteAsset.h"

#include <memory>
#include <string>
#include <vector>

// 플립북 편집기 (.eflipbook — Phase 56-5b).
//   왼쪽: 미리보기 캔버스(현재 프레임을 피벗 기준으로, 점 필터 확대, 반전 미리보기) + 재생 도구(재생/정지, 한 프레임씩, 속도) +
//         타임라인(프레임 폭 = 길이, 끌어서 스크러빙, 이벤트 표시 — 재생 중 지나간 이벤트는 잠깐 강조)
//   오른쪽: 대상 .esprite(드롭/목록), Fps, Loop, 아틀라스 슬라이스 목록(여러 개 골라 프레임으로 추가 — 프레임 표로 끌어도 됨),
//          프레임 표(끌어서 순서 바꾸기·여러 개 선택·복제·삭제, 프레임별 Duration/Scale), 이벤트 표(프레임 + 이름)
//   재생 시간은 편집기 Update의 dt (자동 검증 --fixed-delta면 결정적), 시간축·이벤트 규칙은 FlipbookMath 그대로.
//   자동 검증 --verify-flipbook-roundtrip, --flipbook-time <초>(재생 멈추고 그 시각)
class FFlipbookEditor : public FSprite2DEditorBase
{
public:
	using FSprite2DEditorBase::FSprite2DEditorBase;

	const char* GetTypeName() const override { return "플립북"; }
	void        Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;

protected:
	bool        LoadAsset(FAssetEditorEnvironment& Env) override;
	bool        SaveAsset(FAssetEditorEnvironment& Env) override;
	std::string CaptureState() const override;
	void        RestoreState(FAssetEditorEnvironment& Env, const std::string& State) override;
	void        DrawProperties(FAssetEditorEnvironment& Env) override;
	void        DrawPreviewArea(FAssetEditorEnvironment& Env) override;
	void        OnClose(FAssetEditorEnvironment& Env) override;
	void        OnSprite2DLibraryChanged(FAssetEditorEnvironment& Env) override;

	const wchar_t* GetVerifyRoundTripFlag() const override { return L"--verify-flipbook-roundtrip"; }
	bool           ApplyVerifyEdits(FAssetEditorEnvironment& Env) override;
	bool           SupportsLivePreview() const override { return true; }
	void           PushLivePreview() override;
	std::string    CaptureLibraryState() const override;

private:
	struct FRecentEvent
	{
		std::string Name;
		int32       Frame = 0;
		float       Age   = 0.0f;
	};

	void Edited(std::string_view Label);
	void SyncAtlas(FAssetEditorEnvironment& Env, bool bForce);
	void DrawToolbar();
	void DrawCanvas(FAssetEditorEnvironment& Env);
	void DrawTimeline();
	void DrawSliceList();
	void DrawFrameTable();
	void DrawEventTable();
	void AddSelectedSlicesAsFrames(int32 InsertBefore);
	int32 GetCurrentFrame() const;
	float GetFrameStart(int32 Frame) const;
	void  SeekFrame(int32 Frame);
	void  ClampSelection();
	bool  IsFrameSelected(int32 Frame) const;

	FFlipbookAsset                      Asset;
	std::shared_ptr<const FSpriteAsset> Atlas;
	std::string                         AtlasPath; // Content 기준
	FTexturePreview                     Texture;
	FSprite2DCanvas                     Canvas;

	// 재생
	float                     Time     = 0.0f;
	float                     Speed    = 1.0f;
	bool                      bPlaying = true;
	bool                      bFlipX   = false;
	bool                      bFlipY   = false;
	std::vector<FRecentEvent> RecentEvents;

	std::vector<int32>       FrameSelection; // 프레임 번호 (마지막 = 주 선택)
	std::vector<std::string> SliceSelection; // 아틀라스 슬라이스 이름
	bool                     bCheckedArgs = false;
};
