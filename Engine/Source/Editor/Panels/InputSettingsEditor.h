#pragma once

class FInputSettings;

// 프로젝트 설정 "입력" 섹션의 전용 편집 UI (설정 창이 그린다 — 액션 목록은 리플렉션 값 위젯으로 그릴 수 없다).
// 프로젝트 매핑(Config/Input.json)만 편집한다. 플레이어 재지정(<Saved>/Config/InputBindings.json)은 게임 쪽 API(Lua Input.Rebind)로 바꾼다
class FInputSettingsEditor
{
public:
	// 반환: 이번 프레임에 매핑이 바뀌었는지 (바뀌면 이미 Settings에 반영됨 — 호출자는 저장만)
	static bool Draw(FInputSettings& Settings);
};
