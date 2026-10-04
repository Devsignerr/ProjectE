#include "Core/Testing/TestFramework.h"
#include "PackageManifest.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
	bool Contains(const std::vector<std::string>& List, std::string_view Value)
	{
		return std::find(List.begin(), List.end(), Value) != List.end();
	}

	// 테스트마다 임시 Content 폴더 (끝나면 지움)
	struct FTempContent
	{
		std::filesystem::path Root;

		FTempContent()
		{
			const auto Stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			Root             = std::filesystem::temp_directory_path() / ("ProjectE_PackageManifest_" + std::to_string(Stamp));
			std::filesystem::create_directories(Root);
		}
		~FTempContent()
		{
			std::error_code ErrorCode;
			std::filesystem::remove_all(Root, ErrorCode);
		}

		void Write(const std::string& Relative, std::string_view Text) const
		{
			const std::filesystem::path Path = Root / std::filesystem::path(Relative);
			std::filesystem::create_directories(Path.parent_path());
			std::ofstream Stream(Path, std::ios::binary);
			Stream.write(Text.data(), static_cast<std::streamsize>(Text.size()));
		}
	};
} // namespace

E_TEST(PackageManifest_ExtractStringsJsonAndEscapedJson)
{
	// 씬 스크립트 PropertyOverrides: 이스케이프된 JSON 문자열 안의 경로도 꺼낸다
	const std::string Text = R"({"Script": "Scripts/Portal.lua", "PropertyOverrides": "{\"TargetScene\": {\"Asset\": \"Scenes/Sub.escene\"}, \"Label\": \"\uD3EC\"}"})";
	const std::vector<std::string> Strings = PackageManifest::ExtractStrings(Text, false);
	E_EXPECT_TRUE(Contains(Strings, "Scripts/Portal.lua"));
	E_EXPECT_TRUE(Contains(Strings, "Scenes/Sub.escene"));
	E_EXPECT_TRUE(Contains(Strings, "PropertyOverrides"));
	E_EXPECT_TRUE(Contains(Strings, "\xED\x8F\xAC")); // \uD3EC = "포" (UTF-8)
}

E_TEST(PackageManifest_ExtractStringsLuaSkipsComments)
{
	const std::string Text = "local A = \"Audio/Hit/\" -- \"Comment/Ignored.wav\"\n"
	                         "--[[ 'Block/Ignored.png' ]]\n"
	                         "local B = 'Data/Items.etable' local C = [[Long/Path.txt]]\n"
	                         "local D = \"it's \\\"quoted\\\"\"\n";
	const std::vector<std::string> Strings = PackageManifest::ExtractStrings(Text, true);
	E_EXPECT_TRUE(Contains(Strings, "Audio/Hit/"));
	E_EXPECT_TRUE(Contains(Strings, "Data/Items.etable"));
	E_EXPECT_TRUE(Contains(Strings, "Long/Path.txt"));
	E_EXPECT_TRUE(Contains(Strings, "it's \"quoted\""));
	E_EXPECT_FALSE(Contains(Strings, "Comment/Ignored.wav"));
	E_EXPECT_FALSE(Contains(Strings, "Block/Ignored.png"));
}

E_TEST(PackageManifest_ReferenceCandidatesAndNormalize)
{
	const std::vector<std::string> Retarget = PackageManifest::SplitReferenceCandidates("Asset/Model.glb:Idle");
	E_EXPECT_TRUE(Contains(Retarget, "Asset/Model.glb"));
	const std::vector<std::string> List = PackageManifest::SplitReferenceCandidates(" A/x.glb ; B\\y.glb ");
	E_EXPECT_TRUE(Contains(List, "A/x.glb"));
	E_EXPECT_TRUE(Contains(List, "B/y.glb"));
	E_EXPECT_TRUE(PackageManifest::SplitReferenceCandidates("C:/Abs/Path.png").empty());
	E_EXPECT_TRUE(PackageManifest::SplitReferenceCandidates("https://example.com/a.png").empty());
	E_EXPECT_TRUE(PackageManifest::SplitReferenceCandidates(std::string(2000, 'a')).empty());

	E_EXPECT_EQ(PackageManifest::NormalizeRelativePath("Materials/../Textures/./a.png"), std::string("Textures/a.png"));
	E_EXPECT_EQ(PackageManifest::NormalizeRelativePath("../Outside.png"), std::string());
}

E_TEST(PackageManifest_ScannerFollowsReferences)
{
	FTempContent Content;
	// 루트 씬: 모델, 스크립트(이스케이프 JSON 오버라이드), 머티리얼, 파티클, 내장 이름, 대소문자 다른 경로, JSON 키 "Asset"(폴더 이름과 같음)
	Content.Write("Scenes/Main.escene", R"({"Entities": [
		{"Model": {"Asset": "models/ship/SHIP.gltf"}},
		{"Mesh": "primitive:cube", "Foliage": "foliage:grass"},
		{"Script": {"ScriptAsset": "Scripts/Portal.lua", "PropertyOverrides": "{\"TargetScene\": {\"Asset\": \"Scenes/Sub.escene\"}}"}},
		{"Material": "Materials/Wood.emat"},
		{"Particles": "Particles/Fire.eparticle"},
		{"Anim": "Models/Other/rig.glb:Run"}
	]})");
	Content.Write("Scenes/Main.enav", "nav");
	Content.Write("Scenes/Sub.escene", "{}");
	Content.Write("Scenes/Unused.escene", R"({"Asset": "Asset/Big/big.png"})");
	Content.Write("Asset/Big/big.png", "png");

	// glTF: 버퍼·이미지(퍼센트 인코딩)는 모델 내부, 사이드카는 패키지
	Content.Write("Models/Ship/ship.gltf", R"({"buffers": [{"uri": "ship.bin"}], "images": [{"uri": "textures/ship%20diff.png"}, {"uri": "../../Textures/wood_diff.png"}]})");
	Content.Write("Models/Ship/ship.bin", "bin");
	Content.Write("Models/Ship/textures/ship diff.png", "png");
	Content.Write("Models/Ship/ship.gltf.emeta", "{}");
	Content.Write("Models/Ship/ship.gltf.eimport", R"({"Animations": "anims/extra.glb"})");
	Content.Write("Models/Ship/anims/extra.glb", "glb");
	Content.Write("Models/Other/rig.glb", "glb");

	// 머티리얼: 이 파일 폴더 기준 텍스처, 슬롯 용도. wood_diff.png는 glTF도 참조하지만 머티리얼 참조가 있으므로 단독 이미지
	Content.Write("Materials/Wood.emat", R"({"Name": "Wood", "BaseColorTexture": "../Textures/wood_diff.png", "NormalTexture": "../Textures/wood_nor.png"})");
	Content.Write("Textures/wood_diff.png", "png");
	Content.Write("Textures/wood_nor.png", "png");

	// 파티클 텍스처: 파티클 폴더 기준 (색상)
	Content.Write("Particles/Fire.eparticle", R"({"Emitters": [{"Renderers": [{"Texture": "flame.png"}]}]})");
	Content.Write("Particles/flame.png", "png");

	// Lua: 폴더 참조(동적 경로) → 하위 전체, 주석 안 경로 무시, 데이터 표 → 아이콘(색상)
	Content.Write("Scripts/Portal.lua", "local AudioDir = \"Audio/Hit/\"\n-- \"Comment/Ignored.wav\"\nlocal T = 'Data/Items.etable'\n");
	Content.Write("Audio/Hit/a.wav", "wav");
	Content.Write("Audio/Hit/Sub/b.wav", "wav");
	Content.Write("Comment/Ignored.wav", "wav");
	Content.Write("Data/Items.etable", R"({"Rows": [{"Icon": "Icons/sword.png"}]})");
	Content.Write("Icons/sword.png", "png");

	FPackageDependencyScanner Scanner(Content.Root);
	E_EXPECT_TRUE(Scanner.AddRoot("Scenes/Main.escene", true, "테스트"));
	E_EXPECT_FALSE(Scanner.AddRoot("Scenes/Missing.escene", true, "테스트"));
	const FPackageManifest Manifest = Scanner.Run();
	E_EXPECT_TRUE(Manifest.bHasErrors); // 없는 필수 루트

	const auto HasFile = [&](const char* Path) { return Manifest.Files.contains(Path); };
	E_EXPECT_TRUE(HasFile("Scenes/Main.escene"));
	E_EXPECT_TRUE(HasFile("Scenes/Main.enav"));
	E_EXPECT_TRUE(HasFile("Scenes/Sub.escene")); // 이스케이프된 PropertyOverrides
	E_EXPECT_TRUE(HasFile("Models/Ship/ship.gltf")); // 대소문자 무시, 실제 이름으로
	E_EXPECT_TRUE(HasFile("Models/Ship/ship.gltf.emeta"));
	E_EXPECT_TRUE(HasFile("Models/Ship/ship.gltf.eimport"));
	E_EXPECT_TRUE(HasFile("Models/Other/rig.glb")); // "<모델>:<클립>"
	E_EXPECT_TRUE(HasFile("Scripts/Portal.lua"));
	E_EXPECT_TRUE(HasFile("Audio/Hit/a.wav"));
	E_EXPECT_TRUE(HasFile("Audio/Hit/Sub/b.wav"));
	E_EXPECT_TRUE(HasFile("Data/Items.etable"));
	E_EXPECT_TRUE(HasFile("Icons/sword.png"));
	E_EXPECT_TRUE(HasFile("Particles/flame.png"));
	E_EXPECT_TRUE(Manifest.Directories.contains("Audio/Hit"));

	// 참조되지 않거나 주석/내장/JSON 키뿐인 것은 제외
	E_EXPECT_FALSE(HasFile("Scenes/Unused.escene"));
	E_EXPECT_FALSE(HasFile("Asset/Big/big.png"));
	E_EXPECT_FALSE(HasFile("Comment/Ignored.wav"));
	E_EXPECT_FALSE(Manifest.Directories.contains("Asset"));

	// 모델 내부(glTF 버퍼/이미지, .eimport 입력)는 패키지·단독 쿠킹 제외
	E_EXPECT_TRUE(Manifest.Internal.contains("Models/Ship/ship.bin"));
	E_EXPECT_TRUE(Manifest.Internal.contains("Models/Ship/textures/ship diff.png"));
	E_EXPECT_TRUE(Manifest.Internal.contains("Models/Ship/anims/extra.glb"));
	E_EXPECT_FALSE(HasFile("Models/Ship/ship.bin"));
	E_EXPECT_FALSE(Manifest.Images.contains("Models/Ship/textures/ship diff.png"));
	E_EXPECT_FALSE(Manifest.Internal.contains("Textures/wood_diff.png")); // 머티리얼도 참조 → 단독

	E_EXPECT_TRUE(Manifest.Models.contains("Models/Ship/ship.gltf"));
	E_EXPECT_TRUE(Manifest.Models.contains("Models/Other/rig.glb"));
	E_EXPECT_FALSE(Manifest.Models.contains("Models/Ship/anims/extra.glb"));

	// 이미지 용도: 머티리얼 슬롯 그대로, 그 밖은 색상
	const auto UsagesOf = [&](const char* Path) { return Manifest.Images.contains(Path) ? Manifest.Images.at(Path) : std::set<ETextureUsage>{}; };
	E_EXPECT_TRUE(UsagesOf("Textures/wood_diff.png") == std::set<ETextureUsage>{ ETextureUsage::Color });
	E_EXPECT_TRUE(UsagesOf("Textures/wood_nor.png") == std::set<ETextureUsage>{ ETextureUsage::Normal });
	E_EXPECT_TRUE(UsagesOf("Particles/flame.png") == std::set<ETextureUsage>{ ETextureUsage::Color });
	E_EXPECT_TRUE(UsagesOf("Icons/sword.png") == std::set<ETextureUsage>{ ETextureUsage::Color });
}

E_TEST(PackageManifest_FollowsSprite2DReferences)
{
	// 씬 → 플립북(Content 기준) → 스프라이트(플립북 폴더 기준) → 텍스처(스프라이트 폴더 기준), 타일셋 → 텍스처. 이미지 용도는 색상
	FTempContent Content;
	Content.Write("Scenes/Level.escene", R"({"Entities": [
		{"FlipbookComponent": {"Flipbook": "Sprites/Hero/Run.eflipbook"}},
		{"TilemapComponent": {"Tileset": "Tiles/Ground.etileset", "TileData": "AQEAAAA="}}
	]})");
	Content.Write("Sprites/Hero/Run.eflipbook", R"({"Version": 1, "Sprite": "Atlas/Hero.esprite", "Frames": [{"Slice": "Run_0"}]})");
	Content.Write("Sprites/Hero/Atlas/Hero.esprite", R"({"Version": 1, "Texture": "../Hero.png", "Slices": []})");
	Content.Write("Sprites/Hero/Hero.png", "png");
	Content.Write("Tiles/Ground.etileset", R"({"Version": 1, "Texture": "../Textures/Ground.png", "Tiles": []})");
	Content.Write("Textures/Ground.png", "png");
	Content.Write("Textures/Unused.png", "png");

	FPackageDependencyScanner Scanner(Content.Root);
	E_EXPECT_TRUE(Scanner.AddRoot("Scenes/Level.escene", true, "테스트"));
	const FPackageManifest Manifest = Scanner.Run();
	E_EXPECT_FALSE(Manifest.bHasErrors);
	E_EXPECT_TRUE(Manifest.Files.contains("Sprites/Hero/Run.eflipbook"));
	E_EXPECT_TRUE(Manifest.Files.contains("Sprites/Hero/Atlas/Hero.esprite"));
	E_EXPECT_TRUE(Manifest.Files.contains("Sprites/Hero/Hero.png"));
	E_EXPECT_TRUE(Manifest.Files.contains("Tiles/Ground.etileset"));
	E_EXPECT_TRUE(Manifest.Files.contains("Textures/Ground.png"));
	E_EXPECT_FALSE(Manifest.Files.contains("Textures/Unused.png"));
	const auto UsagesOf = [&](const char* Path) { return Manifest.Images.contains(Path) ? Manifest.Images.at(Path) : std::set<ETextureUsage>{}; };
	E_EXPECT_TRUE(UsagesOf("Sprites/Hero/Hero.png") == std::set<ETextureUsage>{ ETextureUsage::Color });
	E_EXPECT_TRUE(UsagesOf("Textures/Ground.png") == std::set<ETextureUsage>{ ETextureUsage::Color });
}

E_TEST(PackageManifest_TextRoundTrip)
{
	FPackageManifest Manifest;
	Manifest.Files       = { "A/b.escene", "T/n.png" };
	Manifest.Models      = { "M/m.glb" };
	Manifest.Images      = { { "T/n.png", { ETextureUsage::Normal, ETextureUsage::Linear } } };
	Manifest.Internal    = { "M/m.bin" };
	Manifest.Directories = { "Audio/RPG" };
	Manifest.Warnings    = { "경고" };

	FPackageManifest Parsed;
	E_EXPECT_TRUE(FPackageManifest::ParseText(Manifest.ToText(), Parsed));
	E_EXPECT_TRUE(Parsed.Files == Manifest.Files);
	E_EXPECT_TRUE(Parsed.Models == Manifest.Models);
	E_EXPECT_TRUE(Parsed.Images == Manifest.Images);
	E_EXPECT_TRUE(Parsed.Internal == Manifest.Internal);
	E_EXPECT_TRUE(Parsed.Directories == Manifest.Directories);
	E_EXPECT_EQ(Parsed.Warnings.size(), size_t(1));
}
