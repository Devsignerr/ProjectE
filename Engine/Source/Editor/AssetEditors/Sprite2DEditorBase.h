#pragma once

#include "Editor/AssetEditors/DataEditorBase.h"
#include "Renderer/Image.h"
#include "Scene/ResourceHandles.h"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <vector>

// 2D 에셋 편집기(.esprite/.eflipbook/.etileset — Phase 56-5b) 공통 기반.
//   - 디스크 감시: FDataEditorBase (쓰기 시각 — 저장 안 한 변경이 없으면 다시 읽고, 있으면 알림 띠)
//   - FSprite2DLibrary 세대가 바뀌면(다른 2D 에셋 저장·핫 리로드) OnSprite2DLibraryChanged (플립북이 참조 아틀라스를 다시 읽는 등)
//   - 저장: FSprite2DLibrary::Save*(디스크 쓰기 + 그 경로 Invalidate → 사용 중 컴포넌트가 다음 갱신에 다시 해석). 편집 중에는 라이브러리를
//     고치지 않으므로(씬은 저장된 내용만 본다) 저장하지 않고 닫으면 파일 상태 그대로다 — 닫을 때 바뀐 게 있었으면 한 번 더 Invalidate
//   - 텍스처 미리보기: 이미지를 CPU로 읽어(FFileSystem + FImageLoader — 빈 칸 판정·크기 자동 채움에도 씀) 편집기 소유 UNORM 텍스처
//     (CreateTexture, 밉 1개)로 올린다. ImGui는 UNORM 백버퍼에 그리므로 sRGB 텍스처를 쓰면 어둡게 보인다 — 원본 픽셀 값 그대로 보이게 UNORM.
//     편집기 소유라 수거 대상이 아니며 닫을 때·경로가 바뀔 때 DestroyTexture(지연 해제)
//   - 씬에 실시간 반영(아틀라스·플립북, 기본 끔 — 도구 줄 토글): 켜면 상태가 바뀔 때마다 PushLivePreview가 FSprite2DLibrary 항목을
//     저장 안 한 사본으로 바꾼다(SetSpritePreview/SetFlipbookPreview — 씬 뷰포트·다른 편집기에 바로 보임). 끄거나 닫으면 Invalidate(경로)로
//     파일 상태로 돌아간다(공유 리소스를 실시간으로 고치는 편집기 규칙). 자기 미리보기로 오른 세대는 변경 알림으로 보지 않는다.
//     자동 검증 --sprite-live-preview = 열 때 켬 + 왕복 검증에서 "편집 = 라이브러리 상태", "되돌리고 끔 = 파일 상태" 확인
//   - 자동 검증 --verify-<종류>-roundtrip: 20프레임째 열기 상태 → ApplyVerifyEdits(편집기별 프로그램 편집) → 실행 취소 끝까지 = 처음 상태 →
//     다시 실행 끝까지 = 편집 상태 → 저장 안 함 닫기(RevertToSaved) = 처음 상태 + 파일 바이트 불변. 실패는 Error 로그 (로그 "[2D 편집기 왕복]")
class FSprite2DEditorBase : public FDataEditorBase
{
public:
	using FDataEditorBase::FDataEditorBase;

	float GetPropertiesWidthWeight() const override { return 1.1f; }
	void  Update(FAssetEditorEnvironment& Env, float DeltaSeconds) override;

protected:
	// 텍스처 하나의 CPU 이미지 + 편집기 소유 GPU 텍스처
	struct FTexturePreview
	{
		std::filesystem::path Path;   // 읽은 절대 경로 (비면 없음)
		FImage                Image;  // RGBA8 (실패면 비어 있음)
		FTextureHandle        Handle; // 편집기 소유
		bool                  bFailed = false;

		int32       GetWidth() const { return static_cast<int32>(Image.Width); }
		int32       GetHeight() const { return static_cast<int32>(Image.Height); }
		bool        IsValid() const { return Image.IsValid(); }
	};

	// ContentPath = Content 기준(또는 절대) 이미지 경로 — 에셋 안 상대 경로는 ResolveReference로 바꿔 넘긴다. 바뀌었을 때만 다시 읽는다.
	// 반환: 이번에 다시 읽었으면 true
	bool        UpdateTexturePreview(FAssetEditorEnvironment& Env, FTexturePreview& Target, const std::string& ContentPath);
	void        ReleaseTexturePreview(FAssetEditorEnvironment& Env, FTexturePreview& Target);
	ImTextureID GetImTexture(FAssetEditorEnvironment& Env, const FTexturePreview& Target) const;

	// 디스크에서 텍스트 읽기 (FFileSystem)
	bool ReadAssetText(std::string& OutText) const;
	// 이 파일 폴더 기준 상대 경로 → Content 기준 경로(라이브러리 키)
	std::string ResolveReference(const std::string& Reference) const;
	std::filesystem::path GetAssetDirectory() const { return Path.parent_path(); }

	// 같은 폴더 기준 상대 경로를 콘텐츠 브라우저 드롭으로 받는다 (확장자 소문자 "." 포함, 맞으면 true + 경로)
	bool AcceptAssetDrop(std::string& InOutReference, const wchar_t* Extension) const;
	// Content 아래 Extension 파일 목록 (이 파일 폴더 기준 상대 경로, 2초마다 다시 훑음)
	const std::vector<std::string>& GetContentFiles(FAssetEditorEnvironment& Env, const wchar_t* Extension);

	void OnClose(FAssetEditorEnvironment& Env) override;
	virtual void OnSprite2DLibraryChanged(FAssetEditorEnvironment& Env) { (void)Env; }
	// 저장 성공 직후 (라이브러리 저장 + 디스크 상태 기억은 파생이 SaveAsset에서)
	void AfterSaved();

	// 씬에 실시간 반영 (지원하는 편집기만 — 토글은 DrawLivePreviewToggle)
	virtual bool SupportsLivePreview() const { return false; }
	virtual void PushLivePreview() {}
	// 라이브러리가 지금 주는 이 에셋의 상태 (CaptureState와 같은 형식 — 자동 검증 --sprite-live-preview가 왕복 중 비교)
	virtual std::string CaptureLibraryState() const { return {}; }
	void         DrawLivePreviewToggle();
	bool         IsLivePreviewEnabled() const { return bLivePreview; }
	void         SetLivePreviewEnabled(bool bEnable) { bLivePreview = bEnable; }

	// 자동 검증 왕복
	virtual const wchar_t* GetVerifyRoundTripFlag() const = 0;
	// 프로그램 편집 (편집마다 MarkEdited + CommitPendingEdit(false)). 반환: 편집을 했으면 true
	virtual bool ApplyVerifyEdits(FAssetEditorEnvironment& Env) = 0;

	int32 FrameCounter = 0;

private:
	void RunVerifyRoundTrip(FAssetEditorEnvironment& Env);
	void UpdateLivePreview();
	void EndLivePreview();

	uint32                   KnownSprite2DGeneration = 0;
	bool                     bEditedSinceOpen        = false;
	std::wstring             ScannedExtension;
	std::vector<std::string> ScannedFiles;
	double                   ScanTime = -100.0;
	bool                     bLivePreview        = false;
	bool                     bLivePreviewApplied = false; // 라이브러리에 사본을 넣어 둠 (끄거나 닫을 때 Invalidate)
	std::string              LivePreviewState;            // 마지막으로 넣은 상태 (CaptureState)
};
