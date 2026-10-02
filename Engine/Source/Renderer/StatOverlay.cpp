#include "Renderer/StatOverlay.h"

#include "Core/Console/Console.h"
#include "Renderer/SceneRenderer.h"

#include <algorithm>
#include <format>

namespace
{
	bool IsStatEnabled(const char* Name)
	{
		const FConsoleVariable* Variable = FConsoleManager::Get().FindVariable(Name);
		return Variable != nullptr && Variable->GetBool();
	}
} // namespace

void FStatOverlay::Tick(float DeltaSeconds)
{
	if (DeltaSeconds <= 0.0f)
	{
		return;
	}
	const float FrameMs = DeltaSeconds * 1000.0f;
	SmoothedFrameMs     = SmoothedFrameMs <= 0.0f ? FrameMs : SmoothedFrameMs + (FrameMs - SmoothedFrameMs) * 0.05f;
	SmoothedFps         = SmoothedFrameMs > 0.0f ? 1000.0f / SmoothedFrameMs : 0.0f;

	// 최악 프레임: 1초 창마다 갱신 (끊김 확인용)
	WorstWindowMs = std::max(WorstWindowMs, FrameMs);
	WindowSeconds += DeltaSeconds;
	if (WindowSeconds >= 1.0f)
	{
		WorstFrameMs  = WorstWindowMs;
		WorstWindowMs = 0.0f;
		WindowSeconds = 0.0f;
	}
}

bool FStatOverlay::IsVisible() const
{
	return IsStatEnabled("stat.FPS") || IsStatEnabled("stat.GPU");
}

std::vector<std::string> FStatOverlay::BuildLines(const FSceneRenderStats* Stats) const
{
	std::vector<std::string> Lines;
	if (IsStatEnabled("stat.FPS"))
	{
		Lines.push_back(std::format("{:.1f} FPS  {:.2f} ms (최악 {:.1f} ms)", SmoothedFps, SmoothedFrameMs, WorstFrameMs));
		if (Stats != nullptr)
		{
			Lines.push_back(std::format("드로우 {} (그림자 {}, 사전 {})  삼각형 {}", Stats->DrawCalls, Stats->ShadowDrawCalls, Stats->PrepassDrawCalls,
			                            Stats->Triangles));
		}
	}
	if (IsStatEnabled("stat.GPU") && Stats != nullptr)
	{
		Lines.push_back("구간\tCPU ms\tGPU ms");
		for (uint32 Index = 0; Index < static_cast<uint32>(ERenderTimer::Count); ++Index)
		{
			const float Cpu = Stats->CpuMs[Index];
			const float Gpu = Stats->GpuMs[Index];
			if (Index != 0 && Cpu < 0.005f && Gpu < 0.005f)
			{
				continue; // 이번 프레임 쓰지 않은 구간 (꺼진 효과)
			}
			Lines.push_back(std::format("{}\t{:.3f}\t{:.3f}", GetRenderTimerName(static_cast<ERenderTimer>(Index)), Cpu, Gpu));
		}
	}
	return Lines;
}
