#include "Editor/Panels/PostProcessPanel.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Renderer/SceneRenderer.h"

#include <imgui.h>

void FPostProcessPanel::Draw(FEditorContext& Context)
{
	if (!bOpen || Context.Renderer == nullptr)
	{
		return;
	}

	if (ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_WAND_MAGIC_SPARKLES, "포스트 프로세스", "PostProcess").c_str(), &bOpen))
	{
		FPostProcessSettings& Settings = Context.Renderer->PostProcessSettings;

		ImGui::SeparatorText("톤매핑");
		const char* Operators[] = { "없음 (클램프)", "ACES (근사)", "Reinhard" };
		int         Operator    = static_cast<int>(Settings.Tonemapper);
		if (ImGui::Combo("연산자", &Operator, Operators, IM_ARRAYSIZE(Operators)))
		{
			Settings.Tonemapper = static_cast<ETonemapOperator>(Operator);
		}

		ImGui::SeparatorText("노출");
		ImGui::DragFloat(Settings.bAutoExposure ? "노출 보정 (EV)" : "노출 (EV)", &Settings.ExposureEV, 0.05f, -10.0f, 10.0f, "%.2f");
		ImGui::Checkbox("자동 노출", &Settings.bAutoExposure);
		ImGui::BeginDisabled(!Settings.bAutoExposure);
		ImGui::DragFloatRange2("자동 EV 범위", &Settings.AutoExposureMinEV, &Settings.AutoExposureMaxEV, 0.1f, -12.0f, 12.0f, "최소 %.1f", "최대 %.1f");
		ImGui::DragFloat("적응 속도", &Settings.AdaptationSpeed, 0.05f, 0.0f, 20.0f, "%.2f /초");
		ImGui::EndDisabled();

		ImGui::SeparatorText("블룸");
		ImGui::Checkbox("블룸", &Settings.bBloomEnabled);
		ImGui::BeginDisabled(!Settings.bBloomEnabled);
		ImGui::DragFloat("임계값", &Settings.BloomThreshold, 0.02f, 0.0f, 20.0f, "%.2f");
		ImGui::SliderFloat("니 (부드러움)", &Settings.BloomKnee, 0.0f, 1.0f, "%.2f");
		ImGui::DragFloat("강도", &Settings.BloomIntensity, 0.005f, 0.0f, 2.0f, "%.3f");
		ImGui::EndDisabled();

		ImGui::SeparatorText("안티에일리어싱 (TAA)");
		ImGui::Checkbox("TAA", &Settings.bTemporalAA);
		ImGui::SetItemTooltip("프레임마다 화면을 조금씩 흔들어 그린 결과를 누적해 계단 현상을 없앱니다 (픽셀 아트에서는 꺼짐)");
		ImGui::BeginDisabled(!Settings.bTemporalAA);
		ImGui::SliderFloat("현재 프레임 비중", &Settings.TemporalAACurrentWeight, 0.02f, 0.5f, "%.2f");
		ImGui::SetItemTooltip("작을수록 부드럽지만 움직일 때 잔상이 남기 쉽습니다");
		ImGui::SliderFloat("샤프닝", &Settings.TemporalAASharpness, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled();

		ImGui::SeparatorText("주변광 차폐 (SSAO)");
		ImGui::Checkbox("SSAO", &Settings.bAmbientOcclusion);
		ImGui::SetItemTooltip("구석과 틈에 드는 하늘빛·환경광을 줄여 입체감을 살립니다 (직접광에는 영향 없음)");
		ImGui::BeginDisabled(!Settings.bAmbientOcclusion);
		ImGui::DragFloat("AO 세기", &Settings.AmbientOcclusionIntensity, 0.02f, 0.0f, 4.0f, "%.2f");
		ImGui::DragFloat("AO 반경", &Settings.AmbientOcclusionRadius, 1.0f, 5.0f, 500.0f, "%.0f cm");
		ImGui::EndDisabled();

		ImGui::SeparatorText("화면 공간 반사 (SSR)");
		ImGui::Checkbox("SSR", &Settings.bScreenSpaceReflections);
		ImGui::SetItemTooltip("화면에 보이는 물체를 매끈한 표면에 비춥니다 (화면 밖은 반사 캡처 → 하늘)");
		ImGui::BeginDisabled(!Settings.bScreenSpaceReflections);
		ImGui::DragFloat("SSR 세기", &Settings.SsrIntensity, 0.01f, 0.0f, 4.0f, "%.2f");
		ImGui::SliderFloat("SSR 최대 거칠기", &Settings.SsrMaxRoughness, 0.05f, 1.0f, "%.2f");
		ImGui::DragFloat("SSR 최대 거리", &Settings.SsrMaxDistance, 10.0f, 50.0f, 20000.0f, "%.0f cm");
		ImGui::DragFloat("SSR 두께", &Settings.SsrThickness, 1.0f, 1.0f, 500.0f, "%.0f cm");
		ImGui::EndDisabled();

		ImGui::Separator();
		if (ImGui::Button("기본값으로"))
		{
			Settings = FPostProcessSettings{};
		}
	}
	ImGui::End();
}
