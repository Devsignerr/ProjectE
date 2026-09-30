#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/Project.h"
#include "Core/Testing/TestFramework.h"

#include <filesystem>
#include <fstream>

E_TEST(Paths_EngineDirectoryDetected)
{
	// TestMain에서 FPaths::Initialize()가 호출된 상태
	E_EXPECT_TRUE(FPaths::IsInitialized());

	const std::filesystem::path Engine = FPaths::GetEngineDirectory();
	E_EXPECT_TRUE(std::filesystem::exists(Engine / L"Engine" / L"Shaders" / L"Common.hlsli"));
	E_EXPECT_TRUE(std::filesystem::exists(FPaths::GetEngineShaderDirectory() / L"Mesh.hlsl"));
	E_EXPECT_TRUE(std::filesystem::exists(FPaths::GetExecutableDirectory()));

	// 마커가 없는 디렉터리에서는 찾지 못한다
	const std::filesystem::path NoMarker = FTestRegistry::GetTempDirectory() / L"ProjectE_NoEngineMarker";
	std::error_code             ErrorCode;
	std::filesystem::create_directories(NoMarker, ErrorCode);
	E_EXPECT_TRUE(FPaths::FindEngineDirectory(NoMarker).empty());
	std::filesystem::remove(NoMarker, ErrorCode);
}

E_TEST(Paths_DefaultSampleProject)
{
	E_EXPECT_TRUE(FPaths::HasProject());
	E_EXPECT_TRUE(FPaths::GetProjectName() == "Sample");
	E_EXPECT_TRUE(FPaths::GetProjectFile().filename() == L"Sample.eproject");
	E_EXPECT_TRUE(std::filesystem::exists(FPaths::GetProjectContentDirectory() / L"UVChecker.png"));
	E_EXPECT_TRUE(FPaths::GetProjectConfigDirectory().filename() == L"Config");
	// Saved는 없으면 생성된다
	E_EXPECT_TRUE(std::filesystem::is_directory(FPaths::GetProjectSavedDirectory()));
	E_EXPECT_TRUE(FPaths::GetSavedDirectory() == FPaths::GetProjectSavedDirectory());

	// 폴더로 지정해도 .eproject를 찾는다
	E_EXPECT_TRUE(FPaths::ResolveProjectFile(FPaths::GetProjectDirectory()) == FPaths::GetProjectFile() ||
	              FPaths::ResolveProjectFile(FPaths::GetProjectDirectory()).filename() == L"Sample.eproject");
	E_EXPECT_TRUE(FPaths::ResolveProjectFile(FTestRegistry::GetTempDirectory() / L"없는_폴더_ProjectE").empty());
}

E_TEST(Project_DescriptorRoundtrip)
{
	const std::filesystem::path Directory = FTestRegistry::GetTempDirectory() / L"ProjectE_ProjectTest";
	const std::filesystem::path File      = Directory / L"테스트.eproject";

	FProjectDescriptor Saved;
	Saved.Name          = "테스트 프로젝트";
	Saved.EngineVersion = "0.1.0";
	Saved.DefaultScene  = "Scenes/Main.escene";
	E_EXPECT_TRUE(Saved.SaveToFile(File));

	FProjectDescriptor Loaded;
	E_EXPECT_TRUE(Loaded.LoadFromFile(File));
	E_EXPECT_TRUE(Loaded.Name == Saved.Name);
	E_EXPECT_TRUE(Loaded.EngineVersion == Saved.EngineVersion);
	E_EXPECT_TRUE(Loaded.DefaultScene == Saved.DefaultScene);

	// 잘못된 JSON은 실패
	{
		std::ofstream Broken(File, std::ios::binary | std::ios::trunc);
		Broken << "{ \"Name\": ";
	}
	FProjectDescriptor Invalid;
	E_EXPECT_FALSE(Invalid.LoadFromFile(File));
	E_EXPECT_FALSE(Invalid.LoadFromFile(Directory / L"없는파일.eproject"));

	std::error_code ErrorCode;
	std::filesystem::remove_all(Directory, ErrorCode);
}

E_TEST(CommandLine_Parse)
{
	const FCommandLine CommandLine = FCommandLine::Parse(L"--project \"C:\\My Dir\\Game.eproject\" --flag --key=value --last");

	E_EXPECT_EQ(CommandLine.GetArguments().size(), static_cast<size_t>(5));
	E_EXPECT_TRUE(CommandLine.GetValue(L"--project") == L"C:\\My Dir\\Game.eproject");
	E_EXPECT_TRUE(CommandLine.HasFlag(L"--flag"));
	E_EXPECT_TRUE(CommandLine.HasFlag(L"--key"));
	E_EXPECT_TRUE(CommandLine.GetValue(L"--key") == L"value");
	E_EXPECT_FALSE(CommandLine.HasFlag(L"--missing"));
	E_EXPECT_TRUE(CommandLine.GetValue(L"--missing").empty());
	// 뒤에 다른 옵션이 오면 값이 없다
	E_EXPECT_TRUE(CommandLine.GetValue(L"--flag").empty());
	E_EXPECT_TRUE(CommandLine.GetValue(L"--last").empty());

	E_EXPECT_TRUE(FCommandLine::Parse(L"").GetArguments().empty());
}
