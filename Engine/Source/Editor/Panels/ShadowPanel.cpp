#include "Editor/Panels/ShadowPanel.h"
#include "Editor/EditorContext.h"
#include "Renderer/SceneRenderer.h"
#include <imgui.h>

void FShadowPanel::Draw(FEditorContext& Context)
{
	if (!bOpen || Context.Renderer == nullptr)
	{
		return;
	}
	if (ImGui::Begin("그림자", &bOpen))
	{
		FShadowSettings& Settings = Context.Renderer->ShadowSettings;
		ImGui::Checkbox("그림자 사용", &Settings.bEnabled);
		ImGui::BeginDisabled(!Settings.bEnabled);
		int Count = static_cast<int>(Settings.CascadeCount);
		if (ImGui::SliderInt("캐스케이드 수", &Count, 1, 4))
		{
			Settings.CascadeCount = static_cast<uint32>(Count);
		}
		const char* Resolutions[] = { "512", "1024", "2048", "4096" };
		int ResolutionIndex = Settings.Resolution == 512 ? 0 : Settings.Resolution == 1024 ? 1 : Settings.Resolution == 4096 ? 3 : 2;
		if (ImGui::Combo("해상도", &ResolutionIndex, Resolutions, IM_ARRAYSIZE(Resolutions)))
		{
			Settings.Resolution = 512u << ResolutionIndex;
		}
		ImGui::SliderFloat("최대 거리", &Settings.ShadowDistance, 1.0f, 200.0f);
		ImGui::SliderFloat("로그 분할 비중", &Settings.SplitLambda, 0.0f, 1.0f);
		ImGui::SliderFloat("캐스터 확장 거리", &Settings.CasterExtension, 0.0f, 200.0f);
		ImGui::SliderInt("깊이 바이어스", &Settings.DepthBias, 0, 10000);
		ImGui::SliderFloat("경사 바이어스", &Settings.SlopeBias, 0.0f, 10.0f);
		ImGui::SliderFloat("노멀 오프셋", &Settings.NormalOffset, 0.0f, 5.0f);
		ImGui::Checkbox("캐스케이드 색상 표시", &Settings.bVisualizeCascades);
		ImGui::EndDisabled();
		if (ImGui::Button("기본값으로"))
		{
			Settings = FShadowSettings{};
		}
	}
	ImGui::End();
}