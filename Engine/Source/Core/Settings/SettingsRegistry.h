#pragma once

#include "Core/CoreTypes.h"
#include "Core/Reflection/TypeInfo.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// 설정 저장 범위
enum class ESettingsScope : uint8
{
	Project,    // <프로젝트>/Config/<Id>.json — 저장소에 커밋 (언리얼 Config/Default*.ini)
	EditorUser, // %LOCALAPPDATA%/ProjectE/EditorPreferences/<Id>.json — 개인, 모든 프로젝트 공통
	ProjectUser, // <Saved>/Config/<Id>.json — 개인, 프로젝트별 (마지막으로 연 씬 등 상태)
};

// 설정 섹션 하나 = 리플렉션으로 기술한 구조체 객체 + 파일. 설정 창이 카테고리별로 나열하고 속성을 그린다
struct FSettingsSection
{
	std::string    Id;          // 파일 이름 (예 "Maps")
	std::string    DisplayName; // 창 표시 이름 (예 "맵 & 모드")
	std::string    Category;    // 창 왼쪽 묶음 (예 "프로젝트", "엔진", "에디터")
	std::string    Description;
	ESettingsScope Scope            = ESettingsScope::Project;
	bool           bRequiresRestart = false; // 바꾼 뒤 다시 시작해야 적용 (창에 표시)
	bool           bHidden          = false; // 설정 창에 표시하지 않음 (저장만 — 상태 섹션)

	void*                      Object = nullptr; // 비소유 — 설정 객체는 프로세스 수명 (FProjectSettings 등)
	std::unique_ptr<FTypeInfo> Type;             // 섹션 전용 타입 정보 (전역 FTypeRegistry에 넣지 않는다 — 컴포넌트/스크립트 목록과 섞이지 않게)
	std::function<void()>      ResetToDefaults;  // 기본값으로 되돌리기
	std::function<void()>      OnChanged;        // 창에서 값이 바뀐 뒤 (적용이 필요한 섹션만)

	// 리플렉션 값 타입으로 기술할 수 없는 섹션 (입력 액션 목록 등): 있으면 Type 대신 이 함수로 파일을 읽고 쓴다.
	// 설정 창은 이런 섹션에 Id별 전용 편집 UI를 그린다 (FSettingsWindow)
	std::function<bool(std::string_view Json, std::string* Error)> ReadJson;
	std::function<std::string()>                                    WriteJson;
	bool IsCustom() const { return static_cast<bool>(ReadJson); }

	std::filesystem::path GetFilePath() const;
	bool Load() const; // 파일이 없으면 true (값 유지), JSON 오류면 경고 + false
	bool Save() const;
	bool LoadFrom(const std::filesystem::path& Path) const;
	bool SaveTo(const std::filesystem::path& Path) const;
};

// 프로세스 전역 설정 섹션 목록 (엔진 DLL 하나)
class FSettingsRegistry
{
public:
	static FSettingsRegistry& Get();

	// 섹션 등록 (같은 Id가 있으면 교체). 반환한 빌더로 프로퍼티를 기술한다:
	//   FSettingsRegistry::Get().Register(Maps, { "Maps", "맵 & 모드", "프로젝트" }).Property(&FMapsSettings::GameDefaultMap, ...);
	struct FDesc
	{
		std::string    Id;
		std::string    DisplayName;
		std::string    Category;
		std::string    Description;
		ESettingsScope Scope            = ESettingsScope::Project;
		bool           bRequiresRestart = false;
		bool           bHidden          = false;
	};
	template <typename T>
	TTypeBuilder<T> Register(T& Object, FDesc Desc)
	{
		FSettingsSection& Section = AddSection(std::move(Desc));
		Section.Object            = &Object;
		Section.Type->Size        = sizeof(T);
		Section.Type->Alignment   = alignof(T);
		Section.ResetToDefaults   = [&Object]() { Object = T{}; };
		return TTypeBuilder<T>(*Section.Type);
	}

	// 사용자 정의 JSON 섹션 등록 (ReadJson/WriteJson/ResetToDefaults를 채운다)
	FSettingsSection& RegisterCustom(void* Object, FDesc Desc, std::function<bool(std::string_view, std::string*)> ReadJson,
	                                 std::function<std::string()> WriteJson, std::function<void()> ResetToDefaults);

	FSettingsSection*                                     Find(std::string_view Id);
	const std::vector<std::unique_ptr<FSettingsSection>>& GetSections() const { return Sections; }
	void                                                  LoadAll(ESettingsScope Scope);

	// %LOCALAPPDATA%/ProjectE/EditorPreferences (없으면 <엔진>/Saved/EditorPreferences)
	static std::filesystem::path GetEditorUserDirectory();

private:
	FSettingsSection& AddSection(FDesc Desc);

	std::vector<std::unique_ptr<FSettingsSection>> Sections;
};
