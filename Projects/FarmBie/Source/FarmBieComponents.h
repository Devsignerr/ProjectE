#pragma once

#include "Core/CoreTypes.h"

#include <string>

// FarmBie 게임 컴포넌트 (FarmBieGameModule.cpp에서 리플렉션 등록 → 인스펙터/직렬화/Lua에 자동 노출).
// Lua(FarmBuild.lua)가 설치물 엔티티를 만들고 이 값을 채운다. C++ 디펜스 시스템(F7)은 이 값으로 길을 찾고 피해를 준다.

// 격자 위 설치물 (벽·문·덫·포탑·크리스탈·온실). 칸 번호는 Data/FarmBie/FarmMap.edata 격자 기준
struct FFarmStructureComponent
{
	std::string Kind;              // Buildables.etable 행 이름 (Crystal = 크리스탈)
	float       Hp         = 100.0f;
	float       MaxHp      = 100.0f;
	int32       TX         = 0;
	int32       TY         = 0;
	bool        bBlocks    = true;  // 좀비 길을 막음 (부숴야 지나감) — 덫·지뢰는 false
	bool        bCrystal   = false;
	bool        bDestroyed = false; // Hp가 0이 되면 디펜스 시스템이 켠다 → Lua가 정리(엔티티 삭제·격자 비움)
};
