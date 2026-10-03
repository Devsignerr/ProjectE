#include "Renderer/HdrOutputController.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/RendererConsoleVariables.h"

#include <cmath>

namespace
{
	// 마지막으로 RHI에 요청한 값 (같은 값이면 다시 요청하지 않는다 — 거절된 경우 포함)
	int32 GAppliedMode       = -1;
	float GAppliedPaperWhite = -1.0f;
	float GAppliedMaxNits    = -1.0f;
} // namespace

void FHdrOutputController::ApplySettings(const FGameUserSettings& Settings)
{
	// 명령줄(--hdr-output 등)로 이미 바꾼 값은 그대로 둔다
	if (RendererCVars::HdrOutput->IsDefault())
	{
		RendererCVars::HdrOutput->SetInt(static_cast<int32>(Settings.HdrOutput));
	}
	if (RendererCVars::HdrPaperWhite->IsDefault())
	{
		RendererCVars::HdrPaperWhite->SetFloat(Settings.HdrPaperWhiteNits);
	}
	if (RendererCVars::HdrMaxNits->IsDefault())
	{
		RendererCVars::HdrMaxNits->SetFloat(Settings.HdrMaxNits);
	}
}

void FHdrOutputController::Update(FD3D12RHI& Rhi)
{
	const int32 Mode       = RendererCVars::HdrOutput.Get();
	const float PaperWhite = RendererCVars::HdrPaperWhite.Get();
	const float MaxNits    = RendererCVars::HdrMaxNits.Get();
	if (Mode == GAppliedMode && PaperWhite == GAppliedPaperWhite && MaxNits == GAppliedMaxNits)
	{
		return;
	}
	GAppliedMode       = Mode;
	GAppliedPaperWhite = PaperWhite;
	GAppliedMaxNits    = MaxNits;

	EHdrSwapChainMode Target = EHdrSwapChainMode::Off;
	switch (static_cast<EHdrOutputMode>(Mode))
	{
	case EHdrOutputMode::Auto:  Target = Rhi.QueryHdrDisplay().bHdrEnabled ? EHdrSwapChainMode::Hdr10 : EHdrSwapChainMode::Off; break;
	case EHdrOutputMode::Hdr10: Target = EHdrSwapChainMode::Hdr10; break;
	case EHdrOutputMode::ScRgb: Target = EHdrSwapChainMode::ScRgb; break;
	default:                    Target = EHdrSwapChainMode::Off; break;
	}
	Rhi.SetHdrOutput(Target, PaperWhite, MaxNits);
}
