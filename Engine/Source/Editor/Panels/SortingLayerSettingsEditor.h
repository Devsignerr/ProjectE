#pragma once

class FSortingLayerSettings;

// 프로젝트 설정 "정렬 레이어" 섹션의 전용 편집 UI (설정 창이 그린다 — 순서가 의미인 이름 목록은 리플렉션 값 위젯으로 그릴 수 없다).
// 줄마다 이름 입력 + 위/아래 이동 + 삭제, 마지막 줄은 새 레이어 추가. 0번 Default는 고정
class FSortingLayerSettingsEditor
{
public:
	// 반환: 이번 프레임에 바뀌었는지 (이미 Settings에 반영됨 — 호출자는 저장만)
	static bool Draw(FSortingLayerSettings& Settings);
};
