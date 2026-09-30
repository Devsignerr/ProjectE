#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Units.h"
#include "Core/Math/Vector3.h"

// 엔진 ↔ Recast/Detour 좌표 변환 (FNavMesh 경계에서만 사용한다).
//
// 엔진:   왼손 Z-up (+X 앞, +Y 오른쪽, +Z 위), 1 = 1cm
// Recast: Y-up, 1 = 1m
//
// 축 매핑: recast (x, y, z) = (X, Z, Y) × UnitsToMeters
//          엔진  (X, Y, Z) = (x, z, y) × MetersToUnits
//
// 와인딩: Y와 Z를 맞바꾸는 매핑은 반사(행렬식 -1)라서 같은 인덱스 순서의 외적 방향이 뒤집힌다.
//   - 엔진 규약: Cross(P1 - P0, P2 - P0)가 앞면 노멀 (CLAUDE.md의 glTF 와인딩 검증식과 같다)
//   - Recast:    Cross(v1 - v0, v2 - v0).y > cos(최대 경사)인 면만 걸을 수 있는 면으로 표시한다
//   → 엔진 삼각형 (I0, I1, I2)는 Recast에 (I0, I2, I1)로 넘긴다. 그래야 +Z를 향한 엔진 바닥이 +y를 향한다.
//     Recast/Detour에서 엔진으로 돌려주는 삼각형(디버그 표시)도 같은 규칙으로 다시 뒤집는다.
//   NavMeshTests의 NavCoordinates_* / NavMesh_UpFacingFloorIsWalkable / NavMesh_DownFacingFloorIsNotWalkable가
//   이 규칙을 고정한다.
struct FNavCoordinates
{
	// 엔진 점(cm) → Recast 점(m). Out은 float[3]
	static void ToRecast(const FVector3& Engine, float* Out)
	{
		Out[0] = Engine.X * FUnits::UnitsToMeters;
		Out[1] = Engine.Z * FUnits::UnitsToMeters;
		Out[2] = Engine.Y * FUnits::UnitsToMeters;
	}

	// Recast 점(m) → 엔진 점(cm)
	static FVector3 FromRecast(const float* Recast)
	{
		return FVector3(Recast[0] * FUnits::MetersToUnits,
		                Recast[2] * FUnits::MetersToUnits,
		                Recast[1] * FUnits::MetersToUnits);
	}

	static constexpr float ToMeters(float Centimeters) { return Centimeters * FUnits::UnitsToMeters; }
	static constexpr float ToUnits(float Meters) { return Meters * FUnits::MetersToUnits; }

	// 삼각형 인덱스 와인딩 변환 (I0, I1, I2) → (I0, I2, I1). 반사를 보정하며 자기 역함수라 양방향에 같이 쓴다
	template <typename TIndex>
	static void FlipWinding(TIndex& I0, TIndex& I1, TIndex& I2)
	{
		(void)I0;
		const TIndex Temp = I1;
		I1                = I2;
		I2                = Temp;
	}
};
