#include "Core/Paths.h"
#include "Core/Project.h"
#include "Core/Reflection/ReflectionJson.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/Settings/SettingsRegistry.h"
#include "Core/Testing/TestFramework.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
	namespace fs = std::filesystem;

	enum class ETestQuality : int32
	{
		Low,
		Medium,
		High,
	};

	struct FTestSettings
	{
		bool         bEnabled = true;
		int32        Count    = 3;
		uint32       Port     = 7777;
		float        Scale    = 1.0f;
		std::string  Name     = "기본";
		FVector3     Gravity  = FVector3(0.0f, 0.0f, -980.0f);
		ETestQuality Quality  = ETestQuality::Medium;
	};

	FTypeInfo MakeTestType()
	{
		FTypeInfo Type;
		Type.Name = "TestSettings";
		TTypeBuilder<FTestSettings>(Type)
			.Property(&FTestSettings::bEnabled, "Enabled", "켜기")
			.Property(&FTestSettings::Count, "Count", "개수").Range(0.0f, 10.0f)
			.Property(&FTestSettings::Port, "Port", "포트")
			.Property(&FTestSettings::Scale, "Scale", "배율").Tooltip("설명")
			.Property(&FTestSettings::Name, "Name", "이름")
			.Property(&FTestSettings::Gravity, "Gravity", "중력")
			.Property(&FTestSettings::Quality, "Quality", "품질").Enum({ { "Low", "낮음" }, { "Medium", "중간" }, { "High", "높음" } });
		return Type;
	}

	void WriteText(const fs::path& Path, const std::string& Text)
	{
		std::error_code ErrorCode;
		fs::create_directories(Path.parent_path(), ErrorCode);
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		File << Text;
	}
} // namespace

E_TEST(ReflectionJson_RoundtripAndEnum)
{
	const FTypeInfo Type = MakeTestType();
	E_EXPECT_TRUE(Type.FindProperty("Quality")->IsEnum());
	E_EXPECT_TRUE(Type.FindProperty("Quality")->Type == EPropertyType::Int32);
	E_EXPECT_TRUE(Type.FindProperty("Scale")->Tooltip == "설명");
	E_EXPECT_EQ(Type.FindProperty("Quality")->FindEnumValue("high"), 2);
	E_EXPECT_EQ(Type.FindProperty("Quality")->FindEnumValue("Ultra"), -1);

	FTestSettings Source;
	Source.bEnabled = false;
	Source.Count    = 7;
	Source.Name     = "바뀐 이름";
	Source.Gravity  = FVector3(1.0f, 2.0f, 3.0f);
	Source.Quality  = ETestQuality::High;
	const std::string Json = FReflectionJson::Write(Type, &Source);
	E_EXPECT_TRUE(Json.find("\"Quality\": \"High\"") != std::string::npos); // enum은 이름으로

	FTestSettings Loaded;
	E_EXPECT_TRUE(FReflectionJson::Apply(Type, &Loaded, Json));
	E_EXPECT_FALSE(Loaded.bEnabled);
	E_EXPECT_EQ(Loaded.Count, 7);
	E_EXPECT_TRUE(Loaded.Name == "바뀐 이름");
	E_EXPECT_TRUE(Loaded.Gravity == FVector3(1.0f, 2.0f, 3.0f));
	E_EXPECT_TRUE(Loaded.Quality == ETestQuality::High);

	// 없는 키는 유지, 모르는 키 무시, 타입이 틀리면 건너뜀, 범위는 잘라 넣음, enum은 숫자도 받음
	FTestSettings Partial;
	E_EXPECT_TRUE(FReflectionJson::Apply(Type, &Partial, R"({ "Count": 99, "Unknown": 1, "Scale": "큼", "Quality": 0, "Port": 8000 })"));
	E_EXPECT_EQ(Partial.Count, 10);
	E_EXPECT_NEAR(Partial.Scale, 1.0f, 1.0e-6f);
	E_EXPECT_TRUE(Partial.Quality == ETestQuality::Low);
	E_EXPECT_EQ(Partial.Port, 8000u);
	E_EXPECT_TRUE(Partial.Name == "기본");
	// 알 수 없는 enum 이름/범위 밖 숫자는 무시
	E_EXPECT_TRUE(FReflectionJson::Apply(Type, &Partial, R"({ "Quality": "Ultra" })") && Partial.Quality == ETestQuality::Low);
	E_EXPECT_TRUE(FReflectionJson::Apply(Type, &Partial, R"({ "Quality": 5 })") && Partial.Quality == ETestQuality::Low);
	// JSON 오류는 실패
	std::string Error;
	E_EXPECT_FALSE(FReflectionJson::Apply(Type, &Partial, "[1, 2]", &Error));
	E_EXPECT_FALSE(Error.empty());
}

E_TEST(Settings_SectionLoadSave)
{
	FTestSettings Object;
	FSettingsRegistry::Get()
		.Register(Object, { "TestSection", "테스트", "테스트", "", ESettingsScope::EditorUser })
		.Property(&FTestSettings::Count, "Count", "개수")
		.Property(&FTestSettings::Name, "Name", "이름");
	FSettingsSection* Section = FSettingsRegistry::Get().Find("TestSection");
	E_EXPECT_TRUE(Section != nullptr);
	if (Section == nullptr)
	{
		return;
	}
	E_EXPECT_TRUE(Section->GetFilePath().filename() == L"TestSection.json");
	E_EXPECT_TRUE(Section->GetFilePath().parent_path() == FSettingsRegistry::GetEditorUserDirectory());

	const fs::path Path = FTestRegistry::GetTempDirectory() / L"ProjectE_Settings" / L"TestSection.json";
	Object.Count = 5;
	Object.Name  = "저장";
	E_EXPECT_TRUE(Section->SaveTo(Path));
	Section->ResetToDefaults();
	E_EXPECT_EQ(Object.Count, 3);
	E_EXPECT_TRUE(Section->LoadFrom(Path));
	E_EXPECT_EQ(Object.Count, 5);
	E_EXPECT_TRUE(Object.Name == "저장");
	// 파일이 없으면 성공 + 값 유지, 깨진 파일은 실패
	E_EXPECT_TRUE(Section->LoadFrom(Path.parent_path() / L"없음.json"));
	E_EXPECT_EQ(Object.Count, 5);
	WriteText(Path, "{ \"Count\": ");
	E_EXPECT_FALSE(Section->LoadFrom(Path));

	// 같은 Id로 다시 등록하면 교체 (중복 없음)
	FTestSettings Other;
	FSettingsRegistry::Get().Register(Other, { "TestSection", "테스트2", "테스트" }).Property(&FTestSettings::Count, "Count", "개수");
	size_t Count = 0;
	for (const auto& Each : FSettingsRegistry::Get().GetSections())
	{
		Count += Each->Id == "TestSection" ? 1 : 0;
	}
	E_EXPECT_EQ(Count, static_cast<size_t>(1));
	E_EXPECT_TRUE(FSettingsRegistry::Get().Find("TestSection")->DisplayName == "테스트2");

	std::error_code ErrorCode;
	fs::remove_all(Path.parent_path(), ErrorCode);
}

E_TEST(ProjectSettings_MigrationAndConfigOverride)
{
	const fs::path OriginalProject = FPaths::GetProjectFile();
	const fs::path Directory       = FTestRegistry::GetTempDirectory() / L"ProjectE_SettingsProject";
	std::error_code ErrorCode;
	fs::remove_all(Directory, ErrorCode);

	// 예전 .eproject 필드 → 프로젝트 설정 (Config 파일 없음)
	WriteText(Directory / L"Old.eproject",
	          R"({ "Name": "Old", "DefaultScene": "Scenes/A.escene", "PlayerPrefab": "P.eprefab", "DisplayName": "옛 게임", "Version": "2.0.0", "SteamAppId": 480 })");
	E_EXPECT_TRUE(FPaths::SetProject(Directory));
	const FProjectSettings& Settings = FProjectSettings::Get();
	E_EXPECT_TRUE(Settings.Maps.GameDefaultMap == "Scenes/A.escene");
	E_EXPECT_TRUE(Settings.GetEditorStartupMap() == "Scenes/A.escene"); // 비면 게임 기본 맵
	E_EXPECT_TRUE(Settings.GetServerDefaultMap() == "Scenes/A.escene");
	E_EXPECT_TRUE(Settings.Maps.PlayerPrefab == "P.eprefab");
	E_EXPECT_TRUE(Settings.GetDisplayName() == "옛 게임");
	E_EXPECT_TRUE(Settings.GetExecutableName() == "Old");
	E_EXPECT_TRUE(Settings.Info.Version == "2.0.0");
	E_EXPECT_EQ(Settings.Info.SteamAppId, 480u);
	E_EXPECT_EQ(Settings.GetSteamDepotId(), 481u);
	E_EXPECT_NEAR(Settings.Physics.FixedStepHz, 60.0f, 1.0e-6f);

	// Config 파일이 이전 필드보다 우선 + 예전 DefaultGameUserSettings.json은 Display.json이 없을 때만
	WriteText(Directory / L"Config" / L"Maps.json", R"({ "GameDefaultMap": "Scenes/B.escene", "EditorStartupMap": "Scenes/Edit.escene" })");
	WriteText(Directory / L"Config" / L"Network.json", R"({ "DefaultPort": 9000, "MaxPlayers": 4 })");
	WriteText(Directory / L"Config" / L"DefaultGameUserSettings.json", R"({ "WindowMode": "BorderlessFullscreen" })");
	E_EXPECT_TRUE(FPaths::SetProject(Directory));
	E_EXPECT_TRUE(Settings.Maps.GameDefaultMap == "Scenes/B.escene");
	E_EXPECT_TRUE(Settings.GetEditorStartupMap() == "Scenes/Edit.escene");
	E_EXPECT_EQ(Settings.Network.DefaultPort, 9000u);
	E_EXPECT_EQ(Settings.Network.MaxPlayers, 4u);
	E_EXPECT_TRUE(Settings.Display.WindowMode == EWindowMode::BorderlessFullscreen);

	// 저장: 모든 섹션이 Config/에 생기고 다시 읽으면 같은 값
	E_EXPECT_TRUE(FProjectSettings::Get().SaveAll());
	for (const wchar_t* Name : { L"Project.json", L"Maps.json", L"Packaging.json", L"Physics.json", L"Network.json", L"Display.json" })
	{
		E_EXPECT_TRUE(fs::exists(Directory / L"Config" / Name));
	}
	WriteText(Directory / L"Config" / L"DefaultGameUserSettings.json", R"({ "WindowMode": "Windowed" })"); // Display.json이 있으므로 무시된다
	E_EXPECT_TRUE(FPaths::SetProject(Directory));
	E_EXPECT_TRUE(Settings.Display.WindowMode == EWindowMode::BorderlessFullscreen);
	E_EXPECT_TRUE(Settings.Packaging.Configuration == EPackageConfiguration::Release);

	// 다른 테스트를 위해 원래 프로젝트로
	E_EXPECT_TRUE(FPaths::SetProject(OriginalProject));
	fs::remove_all(Directory, ErrorCode);
}
