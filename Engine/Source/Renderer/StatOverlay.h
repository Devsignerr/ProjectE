#pragma once

#include "Core/CoreTypes.h"

#include <string>
#include <vector>

class FResourceManager;
struct FSceneRenderStats;

// 화면 통계 (콘솔 "stat fps" / "stat gpu" / "stat memory" / "stat streaming" → 변수 stat.FPS / stat.GPU / stat.Memory / stat.Streaming).
// 문자열만 만들고 그리기는 앱이 한다
// (에디터 = 뷰포트 ImGui 그리기 목록, 런타임 = 게임 UI 그리기 목록 FUIDebugDraw).
class FStatOverlay
{
public:
	// 프레임마다 (평활 FPS, stat memory면 0.5초마다 리소스 통계 갱신 — 렌더링 기록 밖에서)
	void Tick(float DeltaSeconds);
	// stat memory가 읽을 리소스 관리자 (비소유, 앱 수명)
	void SetResources(FResourceManager* InResources) { Resources = InResources; }

	bool IsVisible() const; // 켜진 통계가 하나라도 있음
	// 켜진 통계 줄 (없으면 빈 목록). 표 줄은 열을 '\t'로 나눈다 (그리는 쪽이 열 위치를 맞춤). Stats가 없으면(렌더러 없음) 구간 표는 생략
	std::vector<std::string> BuildLines(const FSceneRenderStats* Stats) const;

	float GetSmoothedFps() const { return SmoothedFps; }

private:
	float SmoothedFps     = 0.0f;
	float SmoothedFrameMs = 0.0f;
	float WorstFrameMs    = 0.0f; // 최근 1초 최대
	float WorstWindowMs   = 0.0f;
	float WindowSeconds   = 0.0f;

	FResourceManager*        Resources = nullptr;
	std::vector<std::string> MemoryLines;
	float                    MemorySeconds = 0.0f; // 다음 갱신까지 남은 시간
	std::vector<std::string> StreamingLines;      // stat streaming (텍스처 밉 스트리밍)
	float                    StreamingSeconds = 0.0f;
};
