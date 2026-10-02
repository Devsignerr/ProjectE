#pragma once

#include "Scene/Ability/AbilityTypes.h"

#include <memory>
#include <string>
#include <unordered_map>

// 능력 정의 캐시 (엔진 DLL 전역, 메인 스레드). 표 경로 3개 묶음마다 FAbilitySet 하나를 만들어 공유한다.
// 표는 FDataLibrary로 읽으므로 핫 리로드(FDataLibrary::Invalidate)로 세대가 바뀌면 다음 Load에서 다시 만든다 — 그때 컴포넌트는
// 정의만 새로 묶고(속성 값·활성 효과 유지, 없어진 효과는 제거) 이미 받은 FAbilitySet 객체는 바뀌지 않는다(불변).
// 경고(필드 타입, 없는 효과/속성, 미등록 태그)는 정의마다 한 번 "[능력]" 로그
class FAbilityLibrary
{
public:
	static FAbilityLibrary& Get();

	// 비어 있는 경로는 그 표 없음. 모두 비면 nullptr
	std::shared_ptr<const FAbilitySet> Load(const std::string& AttributeTable, const std::string& EffectTable, const std::string& AbilityTable);
	uint32                             GetGeneration() const; // FDataLibrary 세대
	void                               Invalidate() { Cache.clear(); }

	static std::string MakeKey(const std::string& AttributeTable, const std::string& EffectTable, const std::string& AbilityTable);

private:
	struct FEntry
	{
		uint32                             Generation = 0;
		std::shared_ptr<const FAbilitySet> Set;
	};
	std::unordered_map<std::string, FEntry> Cache;
};
