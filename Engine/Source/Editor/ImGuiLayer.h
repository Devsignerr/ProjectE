#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <utility>
#include <vector>

struct ID3D12GraphicsCommandList;
class FD3D12RHI;
class FWindow;

// Dear ImGui 통합 (Win32 + D3D12 백엔드, 도킹).
// 프레임 흐름: BeginFrame() → ImGui 위젯 코드 → (RHI가 UNORM 백버퍼 뷰 바인딩) → EndFrame(CommandList)
class FImGuiLayer
{
public:
	~FImGuiLayer();

	bool Init(FWindow& Window, FD3D12RHI& InRhi, const std::filesystem::path& IniFilePath);
	void Shutdown();

	// 새 UI 프레임 시작 + 전체 창 도크스페이스
	void BeginFrame();
	// 드로우 데이터 생성 및 기록
	void EndFrame(ID3D12GraphicsCommandList* CommandList);

	// ImGui가 입력을 소비 중이면 게임/카메라 입력을 무시해야 한다
	bool WantCaptureMouse() const;
	bool WantCaptureKeyboard() const;

	// 윈도우 탐색기에서 창에 끌어 놓은 파일 (꺼내면 비워진다)
	std::vector<std::filesystem::path> ConsumeDroppedFiles() { return std::exchange(DroppedFiles, {}); }

	float GetDpiScale() const { return DpiScale; }
	// 메인 도크스페이스 ID (BeginFrame 이후 유효 — 기본 레이아웃 구성용)
	uint32 GetDockSpaceId() const { return DockSpaceId; }

private:
	FD3D12RHI*  Rhi          = nullptr;
	FWindow*    Window       = nullptr;
	std::string IniFilePath; // ImGui가 포인터를 보관하므로 수명 유지
	float       DpiScale     = 1.0f;
	uint32      DockSpaceId  = 0;
	std::vector<std::filesystem::path> DroppedFiles;
	bool        bInitialized = false;
	bool        bFrameBegun  = false;
};
