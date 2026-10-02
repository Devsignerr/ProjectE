#pragma once

#include "Core/CoreTypes.h"

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 게임플레이 태그 (언리얼 GameplayTag식, Phase 53 사이드). 태그 = 점으로 구분한 계층 이름 ("State.Stunned", "Ability.Cooldown.Dash").
//   일치 규칙: 태그 T가 질의 Q에 맞는다 = T == Q 이거나 T가 "Q."로 시작한다 (부모 질의는 자식을 포함, 반대는 아님).
//     "State.Stunned.Hard"는 "State.Stunned"·"State"에 맞고, "State.Stunned"는 "State.Stunned.Hard"에 맞지 않는다.
//   이름 규칙: 비어 있지 않고 앞뒤·연속 점 없음, 공백 없음 (FGameplayTags::IsValid). 대소문자 구분.
//   목록 글자: ";" 또는 "," 또는 줄바꿈으로 구분 (앞뒤 공백 무시, 빈 항목 무시)
namespace FGameplayTags
{
	bool IsValid(std::string_view Tag);
	// Tag가 Query에 맞는가 (위 일치 규칙). 둘 중 하나가 비면 false
	bool Matches(std::string_view Tag, std::string_view Query);
	// "A.B.C" → { "A", "A.B", "A.B.C" }
	std::vector<std::string> GetSelfAndParents(std::string_view Tag);
	// 목록 글자 → 태그 배열 (잘못된 이름은 건너뛰고 OutInvalid에 넣는다)
	std::vector<std::string> ParseList(std::string_view Text, std::vector<std::string>* OutInvalid = nullptr);
	std::string              JoinList(const std::vector<std::string>& Tags); // ";" 구분
} // namespace FGameplayTags

// 태그 수 컨테이너: 같은 태그를 여러 원천(효과, 발동 중 능력, 느슨한 태그)이 더하고 뺀다.
// 명시 수(Explicit) = 그 태그 자체로 더한 수, 암시 수 = 자신 + 모든 자식 태그의 명시 수 합 (부모 질의를 O(1)로).
class FGameplayTagCountContainer
{
public:
	// Delta만큼 더한다 (음수 = 빼기, 0 아래로 내려가지 않는다). 잘못된 태그는 무시. 명시 수가 0↔양수로 바뀌었으면 true
	bool  AddTag(std::string_view Tag, int32 Delta = 1);
	bool  RemoveTag(std::string_view Tag, int32 Count = 1) { return AddTag(Tag, -Count); }
	void  Reset();

	bool  HasTag(std::string_view Query) const { return GetCount(Query) > 0; }      // 부모 질의 포함
	bool  HasTagExact(std::string_view Tag) const { return GetExplicitCount(Tag) > 0; }
	int32 GetCount(std::string_view Query) const;                                    // 암시 수
	int32 GetExplicitCount(std::string_view Tag) const;
	bool  HasAny(const std::vector<std::string>& Queries) const; // 빈 목록 = false
	bool  HasAll(const std::vector<std::string>& Queries) const; // 빈 목록 = true

	const std::map<std::string, int32>& GetExplicitTags() const { return Explicit; } // 이름순 (복제 문자열·디버그 표시)
	bool operator==(const FGameplayTagCountContainer& Other) const { return Explicit == Other.Explicit; }

private:
	std::map<std::string, int32>           Explicit;
	std::unordered_map<std::string, int32> Implicit;
};

// 프로젝트에 등록된 태그 목록 (프로젝트 설정 "GameplayTags", Config/GameplayTags.json). 엔진 DLL 전역, 메인 스레드.
// 목록이 비어 있으면 검사하지 않는다(모든 태그 허용). 등록된 태그의 부모도 등록된 것으로 본다 ("A.B" 등록 → "A" 허용).
// 정의 데이터(효과/능력 표)를 읽을 때 등록되지 않은 태그는 "[능력] 등록되지 않은 태그" 경고를 태그마다 한 번 낸다 (오타 잡기)
class FGameplayTagRegistry
{
public:
	static FGameplayTagRegistry& Get();

	void SetTags(const std::vector<std::string>& Tags); // 테스트/설정 변경
	void LoadFromProjectSettings();                     // FProjectSettings::Get().GameplayTags (빈 목록이면 검사 끔)
	bool IsEmpty() const { return Registered.empty(); }
	bool IsRegistered(std::string_view Tag) const;      // 목록이 비면 항상 true
	// 등록 여부를 확인하고 처음 보는 미등록 태그면 경고 (Context = 어디서 나왔는지). 등록됐거나 검사 꺼짐이면 true
	bool Validate(std::string_view Tag, std::string_view Context);
	const std::vector<std::string>& GetTags() const { return Ordered; }

private:
	std::unordered_set<std::string> Registered; // 등록 태그 + 부모
	std::vector<std::string>        Ordered;
	std::unordered_set<std::string> Warned;
};
