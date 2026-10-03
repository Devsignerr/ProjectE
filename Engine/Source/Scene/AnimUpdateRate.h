#pragma once

#include "Core/CoreTypes.h"

#include <algorithm>
#include <cmath>

// 애니메이션 갱신 빈도 LOD (URO, 언리얼 Update Rate Optimization식). FAnimationSystem::Update가 쓴다.
//   - 화면 크기 = 반경 / (거리 × tan(세로 FOV / 2)) (직교: 반경 / (OrthoHeight / 2)) — 화면 세로 절반 대비 경계 구 반경
//   - 간격: 화면 크기 >= FullRateScreenSize → 매 프레임, >= HalfRateScreenSize → 2프레임마다, 그 밖 → MaxInterval프레임마다
//   - 평가 프레임 = (런타임 틱 + 엔티티 번호) % 간격 == 0 (결정적 엇갈림 — 같은 씬·같은 프레임 수면 항상 같은 결과). 처음 갱신은 항상 평가
//   - 건너뛴 프레임은 포즈를 쓰지 않고(노드 로컬 트랜스폼 유지) 시간만 모은다. 다음 평가가 모은 시간 + 이번 시간을 한 번에 진행하므로
//     노티파이(AnimNotifyMath::Collect는 구간 판정)·몽타주 끝 이벤트는 잃지 않고 그 평가 프레임에 나온다 (스테이트 Tick은 평가마다 한 번, 모은 시간)
//   - 대상 제외 (항상 매 프레임): 카메라 없음, 일시정지(스크럽 즉시 반영), 루트 모션 추출(이동이 끊기지 않게), 래그돌
namespace AnimUpdateRateMath
{
	struct FSettings
	{
		float  FullRateScreenSize = 0.2f;
		float  HalfRateScreenSize = 0.08f;
		uint32 MaxInterval        = 4;
	};

	inline float ComputeScreenSize(float Radius, float Distance, float FovYRadians, bool bOrthographic, float OrthoHeight)
	{
		if (bOrthographic)
		{
			return OrthoHeight > 0.0f ? Radius / (OrthoHeight * 0.5f) : 1.0e9f;
		}
		const float HalfHeight = std::max(Distance, 1.0e-3f) * std::tan(std::max(FovYRadians, 1.0e-3f) * 0.5f);
		return Radius / HalfHeight;
	}

	inline uint32 SelectInterval(float ScreenSize, const FSettings& Settings)
	{
		if (ScreenSize >= Settings.FullRateScreenSize)
		{
			return 1;
		}
		if (ScreenSize >= Settings.HalfRateScreenSize)
		{
			return std::min<uint32>(2, std::max<uint32>(Settings.MaxInterval, 1));
		}
		return std::max<uint32>(Settings.MaxInterval, 1);
	}

	// Tick = 이 런타임의 갱신 호출 횟수(0부터), Phase = 엇갈림 번호(엔티티 번호), bFirst = 아직 한 번도 평가하지 않음
	inline bool ShouldEvaluate(uint32 Tick, uint32 Phase, uint32 Interval, bool bFirst)
	{
		return bFirst || Interval <= 1 || (Tick + Phase) % Interval == 0;
	}
} // namespace AnimUpdateRateMath
