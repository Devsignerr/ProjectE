#pragma once

#include "Core/CoreTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 세이브 게임 (언리얼 SaveGame 슬롯): 슬롯 이름 → <Saved>/SaveGames/<슬롯>.json (UTF-8 JSON 텍스트).
//   C++: FSaveGame::Save("Slot1", R"({"Level":3})") / Load("Slot1") → JSON 텍스트. 저장할 때 JSON인지 검사하고 보기 좋게 다시 쓴다
//   Lua: SaveGame.Save(slot, table) / Load(slot) / Exists / Delete / List (Scripting/ScriptGameplayBindings.cpp)
// 슬롯 이름: 1~64바이트, 영문/숫자/'_'/'-'/한글 등 UTF-8 문자만 (경로 구분자·'.'·공백·Windows 예약 이름 금지 — 경로 탈출 방지)
// 쓰기는 임시 파일에 쓴 뒤 바꿔치기 (도중에 꺼져도 이전 세이브가 남는다). 세이브는 콘텐츠가 아니므로 pak이 아니라 디스크에서 읽는다.
// 자동 검증 실행(--exit-after)은 FApplication이 프로세스 전용 임시 폴더로 돌려 사용자 세이브를 읽거나 쓰지 않는다
class FSaveGame
{
public:
	static constexpr size_t MaxSlotNameLength = 64;

	static bool IsValidSlotName(std::string_view Slot);

	// 세이브 폴더 (재정의가 없으면 <Saved>/SaveGames). 만들지는 않는다 (Save가 만든다)
	static std::filesystem::path GetDirectory();
	// 테스트/자동 검증용 폴더 재정의 (빈 경로 = 기본으로 되돌림)
	static void                  SetDirectoryOverride(const std::filesystem::path& Directory);
	// 잘못된 슬롯 이름이면 빈 경로
	static std::filesystem::path GetSlotPath(std::string_view Slot);

	// JsonText가 JSON이 아니거나 이름이 잘못됐거나 쓰기에 실패하면 false + OutError
	static bool                       Save(std::string_view Slot, std::string_view JsonText, std::string* OutError = nullptr);
	static std::optional<std::string> Load(std::string_view Slot); // 없거나 JSON이 깨졌으면 nullopt
	static bool                       Exists(std::string_view Slot);
	static bool                       Delete(std::string_view Slot); // 지웠으면 true
	static std::vector<std::string>   List();                        // 슬롯 이름 (이름순)
};
