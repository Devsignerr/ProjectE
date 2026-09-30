#pragma once

#include "Core/CoreTypes.h"
#include "Editor/AssetEditors/OrbitCamera.h"
#include "Renderer/Camera.h"
#include "Renderer/SceneRenderer.h"
#include "Scene/Scene.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

class FD3D12RenderTarget;
struct FEditorContext;

// 콘텐츠 브라우저 썸네일: 에셋을 전용 씬에 올려 고정 크기 오프스크린 타깃에 그려 두고 ImGui 이미지로 쓴다.
//   모델=모델, 머티리얼=구, 파티클=1.5초 진행한 이미터, 씬=씬 전체, 이미지=발광 사각형(색 그대로), 그 밖=없음(아이콘).
//   전용 씬 렌더러 하나를 고정 크기로만 써서 버퍼 재생성이 없다. 프레임마다 몇 개씩만 그리고 파일 수정 시각이 바뀌면 다시 그린다.
class FThumbnailCache
{
public:
	static constexpr uint32 Size = 256;

	FThumbnailCache();
	~FThumbnailCache();

	void Shutdown(FEditorContext& Context);

	// 썸네일을 만들 수 있는 확장자인지
	static bool Supports(const std::filesystem::path& Path);
	// UI 중: 준비됐으면 ImGui 텍스처 ID, 아니면 0 (요청을 대기열에 넣는다)
	uint64 Request(const std::filesystem::path& Path);
	// Rhi BeginFrame 이후: 대기열에서 Budget개까지 그린다
	void RenderPending(FEditorContext& Context, uint32 Budget);
	void Invalidate(const std::filesystem::path& Path);
	bool ReloadShaders(const std::vector<std::filesystem::path>* ChangedFiles);

private:
	struct FEntry
	{
		std::unique_ptr<FD3D12RenderTarget> Target;
		std::filesystem::file_time_type     WriteTime{};
		bool                                bQueued = false;
		bool                                bFailed = false;
	};

	bool EnsureRenderer(FEditorContext& Context);
	bool BuildScene(FEditorContext& Context, const std::filesystem::path& Path, bool& bOutImage);

	std::unordered_map<std::wstring, FEntry> Entries; // 키: 정규화 경로 (소문자)
	std::deque<std::filesystem::path>        Queue;
	FSceneRenderer                           Renderer;
	FScene                                   Scene;
	FCamera                                  Camera;
	FOrbitCamera                             Orbit;
	FMaterialHandle                          ImageMaterial; // 이미지 썸네일용 (발광 슬롯에 이미지)
	bool                                     bReady  = false;
	bool                                     bFailed = false;
};
