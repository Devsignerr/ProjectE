#pragma once

class FCollisionLayerSettings;

// 프로젝트 설정 "충돌 레이어" 섹션의 전용 편집 UI (설정 창이 그린다 — 레이어 이름 칸 + 레이어 × 레이어 행렬은 리플렉션 값 위젯으로 그릴 수 없다).
// 유니티 Physics 설정처럼 삼각형 체크 행렬. 바꾼 값은 다음 플레이(물리 시작)부터 적용된다
class FCollisionSettingsEditor
{
public:
	// 반환: 이번 프레임에 바뀌었는지 (이미 Settings에 반영됨 — 호출자는 저장만)
	static bool Draw(FCollisionLayerSettings& Settings);
};
