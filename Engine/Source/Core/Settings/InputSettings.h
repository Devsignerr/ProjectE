#pragma once

#include "Core/CoreTypes.h"
#include "Core/InputActions.h"

#include <filesystem>
#include <string>
#include <string_view>

// 프로젝트 설정 "입력" (FProjectSettings::Input) + 플레이어 재지정.
//   프로젝트 매핑 = <프로젝트>/Config/Input.json (FSettingsRegistry 섹션 "Input" — 액션 목록은 리플렉션 값 타입이 아니라
//     섹션의 ReadJson/WriteJson으로 저장하고, 설정 창은 전용 편집 UI를 그린다)
//   사용자 재지정 = <Saved>/Config/InputBindings.json: 액션별 바인딩 목록 교체 (액션 목록/종류는 프로젝트가 정한다).
//     자동 검증 실행은 읽지도 쓰지도 않는다 (SetUserFileEnabled(false) 기본 — 앱이 자동 검증이 아닐 때만 켠다)
//   유효 매핑 = 프로젝트 매핑에 재지정을 덮은 것. FInput::UpdateActions가 이것을 쓴다 (주소 고정 — 바뀌면 같은 객체를 고쳐 쓴다)
class FInputSettings
{
public:
	FInputSettings();

	// 새 프로젝트/기본값으로: Move(WASD + 왼쪽 스틱), Look(마우스 + 오른쪽 스틱), Jump(스페이스 + A)
	static FInputMapping MakeDefaultMapping();

	const FInputMapping& GetProjectMapping() const { return ProjectMapping; }
	void                 SetProjectMapping(FInputMapping Mapping); // 설정 창/로드 → 유효 매핑 다시 만들기
	void                 ResetProjectMapping() { SetProjectMapping(MakeDefaultMapping()); }
	const FInputMapping& GetEffectiveMapping() const { return Effective; }

	// ---- 플레이어 재지정
	void SetUserFileEnabled(bool bEnabled) { bUserFileEnabled = bEnabled; }
	bool IsUserFileEnabled() const { return bUserFileEnabled; }
	bool LoadUserBindings(); // 사용자 파일이 꺼져 있거나 없으면 true (재지정 없음)
	bool SaveUserBindings() const;
	static std::filesystem::path GetUserBindingsPath(); // <Saved>/Config/InputBindings.json

	// Action의 OldSource 바인딩을 NewSource로 바꾼다 (수정자 유지). OldSource가 비면(None) NewSource를 새 바인딩으로 더한다.
	// 실패(액션/바인딩 없음) false. 성공하면 사용자 파일에 저장(켜져 있을 때)
	bool Rebind(std::string_view Action, const FInputSource& OldSource, const FInputSource& NewSource);
	// 한 액션(비우면 전부)의 재지정을 지워 프로젝트 바인딩으로
	void ResetUserBindings(std::string_view Action = {});
	bool HasUserBindings(std::string_view Action) const { return UserBindings.Find(Action) != nullptr; }

	// 테스트/파일 형식 (FInputMapping JSON에서 재지정된 액션만 — Name/Type/Bindings)
	std::string UserBindingsToJson() const { return UserBindings.ToJson(); }
	bool        UserBindingsFromJson(std::string_view Json, std::string* Error = nullptr);

private:
	void Rebuild();
	void SaveUserBindingsIfEnabled() const;

	FInputMapping ProjectMapping;
	FInputMapping UserBindings; // 재지정된 액션만
	FInputMapping Effective;
	bool          bUserFileEnabled = false;
};
