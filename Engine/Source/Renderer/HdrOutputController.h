#pragma once

#include "Core/GameUserSettings.h"

class FD3D12RHI;

// HDR 디스플레이 출력 켜고 끄기 (Phase 49). 콘솔 변수 r.HDR.Output(0 끔 / 1 자동 / 2 HDR10 / 3 scRGB), r.HDR.PaperWhite, r.HDR.MaxNits를
// 앱이 프레임마다(BeginFrame 전) Update로 RHI에 반영한다 — 런타임에서 콘솔로 바로 켜고 끈다. 시작 값은 사용자 설정(FGameUserSettings)에서 ApplySettings로
//   자동: 디스플레이(OS)가 HDR이면 HDR10, 아니면 끔. 디스플레이·스왑체인이 지원하지 않으면 RHI가 거절하고 SDR 유지 (다시 시도하지 않음 — 값이 바뀌면 다시)
struct FHdrOutputController
{
	static void ApplySettings(const FGameUserSettings& Settings);
	static void Update(FD3D12RHI& Rhi);
};
