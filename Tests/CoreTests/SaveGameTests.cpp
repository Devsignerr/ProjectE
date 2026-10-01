#include "Core/SaveGame.h"
#include "Core/Testing/TestFramework.h"

#include <filesystem>
#include <fstream>

// 슬롯 이름: 경로 탈출/구분자/점/공백/예약 장치 이름/길이 초과 금지, 한글 허용
E_TEST(SaveGame_SlotNameValidation)
{
	E_EXPECT_TRUE(FSaveGame::IsValidSlotName("Slot1"));
	E_EXPECT_TRUE(FSaveGame::IsValidSlotName("auto-save_02"));
	E_EXPECT_TRUE(FSaveGame::IsValidSlotName("슬롯1"));
	for (const char* Bad : { "", "..", "../x", "a/b", "a\\b", "C:x", "a.json", "a b", "CON", "nul", "com1", "LPT9", "a\tb" })
	{
		E_EXPECT_FALSE(FSaveGame::IsValidSlotName(Bad));
	}
	E_EXPECT_TRUE(FSaveGame::IsValidSlotName("CONSOLE")); // 예약 이름과 정확히 같을 때만 금지
	E_EXPECT_FALSE(FSaveGame::IsValidSlotName(std::string(FSaveGame::MaxSlotNameLength + 1, 'a')));
	E_EXPECT_TRUE(FSaveGame::GetSlotPath("../x").empty());
}

// 저장/불러오기/목록/삭제 왕복 (임시 폴더), JSON이 아니면 저장 거부, 깨진 파일은 불러오지 않음, 덮어쓰기
E_TEST(SaveGame_RoundTrip)
{
	const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectESaveGameTests";
	std::filesystem::remove_all(Directory);
	FSaveGame::SetDirectoryOverride(Directory);
	E_EXPECT_TRUE(FSaveGame::GetDirectory() == Directory);
	E_EXPECT_TRUE(FSaveGame::List().empty()); // 폴더가 아직 없음

	E_EXPECT_TRUE(FSaveGame::Save("Slot1", R"({"Level":3,"Name":"용사"})"));
	E_EXPECT_TRUE(std::filesystem::exists(Directory / L"Slot1.json"));
	E_EXPECT_TRUE(FSaveGame::Exists("Slot1"));
	const std::optional<std::string> Loaded = FSaveGame::Load("Slot1");
	E_EXPECT_TRUE(Loaded.has_value() && Loaded->find("\"Level\": 3") != std::string::npos && Loaded->find("용사") != std::string::npos);

	E_EXPECT_TRUE(FSaveGame::Save("Slot1", R"({"Level":4})")); // 덮어쓰기
	E_EXPECT_TRUE(FSaveGame::Load("Slot1")->find("4") != std::string::npos);
	E_EXPECT_FALSE(std::filesystem::exists(Directory / L"Slot1.json.tmp"));

	std::string Error;
	E_EXPECT_FALSE(FSaveGame::Save("Slot2", "{ not json", &Error));
	E_EXPECT_FALSE(Error.empty());
	E_EXPECT_FALSE(FSaveGame::Save("../Escape", "{}"));
	E_EXPECT_FALSE(FSaveGame::Exists("Slot2"));

	{
		std::ofstream File(Directory / L"Broken.json", std::ios::binary);
		File << "{ broken";
	}
	E_EXPECT_FALSE(FSaveGame::Load("Broken").has_value());
	E_EXPECT_TRUE(FSaveGame::Save("슬롯", "[1,2,3]"));

	const std::vector<std::string> Slots = FSaveGame::List();
	E_EXPECT_EQ(Slots.size(), size_t(3)); // Broken, Slot1, 슬롯 (이름순)
	E_EXPECT_TRUE(Slots.size() == 3 && Slots[0] == "Broken" && Slots[1] == "Slot1" && Slots[2] == "슬롯");
	E_EXPECT_TRUE(FSaveGame::Delete("Slot1"));
	E_EXPECT_FALSE(FSaveGame::Delete("Slot1"));
	E_EXPECT_FALSE(FSaveGame::Exists("Slot1"));

	FSaveGame::SetDirectoryOverride({});
	E_EXPECT_TRUE(FSaveGame::GetDirectory().filename() == L"SaveGames");
	std::filesystem::remove_all(Directory);
}
