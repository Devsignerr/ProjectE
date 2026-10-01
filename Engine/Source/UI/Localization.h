#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// 문자열 표 (.estrings, JSON): 키 → 언어별 문자열. 한 파일에 모든 언어를 담는다 (편집 창 하나로 번역을 나란히 본다).
// { "Version": 1, "Languages": ["ko", "en"], "Strings": { "Menu.Play": { "ko": "게임 시작", "en": "Play" } } }
// 언어 코드는 소문자 BCP 47 앞부분을 권장 ("ko", "en", "ja", "zh-cn"). 값 안의 {0}/{이름}은 FLocalization::Format 인자 자리
struct FStringTable
{
	static constexpr const wchar_t* Extension = L".estrings";
	static constexpr int32          Version   = 1;

	std::vector<std::string>                                      Languages; // 표시 순서 (편집 창 열)
	std::map<std::string, std::map<std::string, std::string>, std::less<>> Strings; // 키 → (언어 → 값). 키 순서로 저장 (diff 안정)

	bool        FromJsonString(const std::string& JsonText, std::string* OutError = nullptr);
	std::string ToJsonString() const;
	bool        LoadFromFile(const std::filesystem::path& Path); // FFileSystem (pak 포함)
	bool        SaveToFile(const std::filesystem::path& Path) const;

	// 언어 목록에 없으면 끝에 추가. 반환: 추가됨
	bool AddLanguage(std::string_view Language);
};

// 형식 인자: 위치 인자 {0} {1} ... + 이름 인자 {Name}. "{{" / "}}"는 중괄호 자체
struct FLocFormatArgs
{
	std::vector<std::string>                         Positional;
	std::vector<std::pair<std::string, std::string>> Named;
};

// 다국어 (엔진 DLL 전역 하나, 게임 스레드 전용). 게임 모듈 C++ API: FLocalization::Get().Get("키") / SetLanguage("en")
//   표: 프로젝트 설정 "Localization"의 StringTables(";" 구분, Content 기준) — 비면 디스크의 Content/Localization/*.estrings
//   현재 언어(처음 쓸 때 정한다): 명령줄 --language > 사용자 설정(<Saved>/Config/Language.json) > 시스템 언어(설정 켰을 때) > 기본 언어 > 표 첫 언어
//   찾기: 현재 언어 → 기본 언어 → 키 문자열 그대로 (없는 키는 키마다 경고 한 번)
// 언어/표가 바뀌면 GetRevision이 오른다 → UI 텍스트는 그릴 때마다 키로 다시 찾으므로 즉시 갱신된다.
class FLocalization
{
public:
	static FLocalization& Get();

	// ---- 표
	// 프로젝트 설정의 표를 다시 읽는다 (편집 창 저장 후). 현재 언어는 유지 (새 표에 없으면 다시 정함)
	void Reload();
	// 테스트/도구: 표를 비우고 처음 쓸 때 프로젝트에서 읽지 않게 한다
	void ResetForTests();
	void AddTable(const FStringTable& Table);

	// ---- 언어
	const std::string&       GetLanguage();
	const std::string&       GetDefaultLanguage();
	std::vector<std::string> GetLanguages(); // 읽은 표들의 언어 합집합 (처음 나온 순서)
	// 표에 없는 언어면 false (그대로). bSaveUserSetting: 사용자 설정 파일에 기록 (게임 옵션 메뉴 — 다음 실행에도 유지)
	bool                     SetLanguage(std::string_view Language, bool bSaveUserSetting = false);
	// 테스트: 기본 언어 지정 (보통은 프로젝트 설정)
	void                     SetDefaultLanguage(std::string_view Language);

	// ---- 찾기
	// 현재 언어 → 기본 언어 → 키 (경고 한 번). 반환 참조는 다음 표/언어 변경 전까지 유효
	const std::string& Lookup(std::string_view Key);
	std::string        Get(std::string_view Key, const FLocFormatArgs& Args);
	bool               Has(std::string_view Key);
	// 한 언어의 값만 (대체 없음). 없으면 nullptr
	const std::string* Find(std::string_view Key, std::string_view Language);
	// 모든 키 (정렬, 디자이너 키 고르기)
	std::vector<std::string> GetKeys();

	// 순수 함수: Pattern의 {0}/{이름}을 인자로. 없는 인자 자리는 그대로 둔다
	static std::string Format(std::string_view Pattern, const FLocFormatArgs& Args);
	// "ko-KR" → "ko-kr" 같은 정규화 (소문자, '_' → '-')
	static std::string NormalizeLanguage(std::string_view Language);
	// 사용 가능한 언어 중 Wanted와 같거나 앞부분("ko-kr" → "ko")이 같은 것. 없으면 빈 문자열
	static std::string MatchLanguage(std::string_view Wanted, const std::vector<std::string>& Available);
	// 화면 표시용 언어 이름 ("ko" → "한국어"). 모르면 코드 그대로
	static std::string GetLanguageDisplayName(std::string_view Language);

	uint32 GetRevision() const { return Revision; }
	static std::filesystem::path GetUserSettingsPath(); // <Saved>/Config/Language.json

	FLocalization(const FLocalization&)            = delete;
	FLocalization& operator=(const FLocalization&) = delete;

private:
	FLocalization() = default;
	void EnsureInitialized();
	void LoadProjectTables();
	void ChooseInitialLanguage();

	struct FStringHash
	{
		using is_transparent = void;
		size_t operator()(std::string_view Text) const { return std::hash<std::string_view>{}(Text); }
	};
	using FLanguageValues = std::unordered_map<std::string, std::string, FStringHash, std::equal_to<>>;

	std::unordered_map<std::string, FLanguageValues, FStringHash, std::equal_to<>> Strings; // 키 → (언어 → 값), 표를 합친 결과 (뒤 표가 우선)
	std::vector<std::string>                                                      Languages;
	std::unordered_set<std::string, FStringHash, std::equal_to<>>                 MissingKeys; // 경고한 키 (Lookup이 키 자체를 돌려줄 때 참조 대상)
	std::string                                                                   Language;
	std::string                                                                   DefaultLanguage;
	uint32                                                                        Revision     = 1;
	bool                                                                          bInitialized = false;
};
