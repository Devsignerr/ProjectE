#pragma once

#include "Core/ECS/Entity.h"
#include "Core/Math/Math.h"

#include <vector>

class FScene;

// 2D 콜라이더 모양 → 월드 선 (뷰포트 외곽선·클릭 판정 공용, Phase 56-5a).
// 물리(FPhysics2DSystem::BuildDesc)와 같은 규칙: 바디 자세 = 월드 행렬 분해(위치 + 2D 각 Physics2DMath::AngleFromRotation, 평면 밖 회전 무시),
// 모양은 바디 공간에서 스케일 X/Z를 곱한 값(상자 크기·오프셋·다각형 점 = 축별, 원 반지름 = max(|X|, |Z|), 캡슐 반지름 = |X|·높이 = |Z|),
// 상자 Angle은 모양 추가 회전. 원·캡슐 반원은 다각형 근사(원 32칸). 2D 캐릭터 이동기 캡슐은 스케일·회전 없이 엔티티 위치가 가운데.
namespace Collider2DShapes
{
	enum class EKind : uint8
	{
		Collider,  // 2D 콜라이더
		Character, // 2D 캐릭터 이동기 캡슐
	};

	struct FOutline
	{
		std::vector<FVector3> Points; // 월드 (깊이 Y = 엔티티 월드 Y)
		bool                  bClosed  = true;
		bool                  bTrigger = false;
		bool                  bOneWay  = false;
		EKind                 Kind     = EKind::Collider;
	};

	// 엔티티에 2D 콜라이더 또는 2D 캐릭터 이동기가 있는가
	bool HasShapes(const FScene& Scene, FEntity Entity);
	// 엔티티의 모든 2D 콜라이더(+ 이동기 캡슐) 외곽선을 Out 뒤에 덧붙인다
	void Collect(const FScene& Scene, FEntity Entity, std::vector<FOutline>& Out);
	// 평면 점(월드 X, Z)이 외곽선 안(닫힌 것) 또는 Tolerance(cm) 안(열린 선)인가
	bool Contains(const FOutline& Outline, const FVector2& PlanePoint, float Tolerance);
} // namespace Collider2DShapes
