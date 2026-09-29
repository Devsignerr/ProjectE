#pragma once

// 엔진 단위 규약: 1 단위 = 1 센티미터 (언리얼과 동일). 질량은 kg, 시간은 초.
// 외부 형식(glTF = 미터)이나 미터 기반 라이브러리(Jolt 물리)와의 경계에서만 변환한다.
struct FUnits
{
	static constexpr float MetersToUnits = 100.0f;
	static constexpr float UnitsToMeters = 1.0f / MetersToUnits;

	// 표준 중력 가속도 (cm/s², -Z 방향)
	static constexpr float StandardGravity = 980.665f;
};
