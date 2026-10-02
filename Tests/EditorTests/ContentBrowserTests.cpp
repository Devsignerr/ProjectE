#include "Core/Testing/TestFramework.h"
#include "Editor/ContentBrowser/AssetFileOps.h"
#include "Editor/ContentBrowser/AssetReferenceUpdater.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
	namespace fs = std::filesystem;

	void WriteText(const fs::path& Path, const std::string& Text)
	{
		fs::create_directories(Path.parent_path());
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		File << Text;
	}

	std::string ReadText(const fs::path& Path)
	{
		std::ifstream     File(Path, std::ios::binary);
		std::stringstream Buffer;
		Buffer << File.rdbuf();
		return Buffer.str();
	}

	bool Contains(const std::string& Text, const std::string& Needle) { return Text.find(Needle) != std::string::npos; }

	// 임시 콘텐츠: 머티리얼(폴더 기준 텍스처) / 파티클 / 씬(Content 기준) / 프로젝트 파일
	struct FTempContent
	{
		fs::path Root;
		fs::path Content;
		fs::path Project;

		explicit FTempContent(const char* Name)
		{
			Root    = FTestRegistry::GetTempDirectory() / "ProjectE_EditorTests" / Name;
			Content = Root / "Content";
			Project = Root / "Test.eproject";
			std::error_code ErrorCode;
			fs::remove_all(Root, ErrorCode);
			WriteText(Content / "Textures/UV.png", "png");
			WriteText(Content / "Materials/Checker.emat", "{\n  \"Name\": \"Checker\",\n  \"BaseColorTexture\": \"../Textures/UV.png\",\n  \"NormalTexture\": \"\"\n}");
			WriteText(Content / "Particles/Fire.eparticle", "{ \"Name\": \"Fire\", \"Texture\": \"../Textures/UV.png\" }");
			WriteText(Content / "Scenes/Main.escene",
			          "{\"Entities\": [{\"Name\": \"Materials\", \"Components\": {\"StaticMeshComponent\": {\"MeshAsset\": \"primitive:cube\", "
			          "\"MaterialAsset\": \"Materials/Checker.emat\"}, \"ParticleSystemComponent\": {\"Asset\": \"Particles/Fire.eparticle\"}}}]}");
			WriteText(Project, "{ \"Name\": \"Test\", \"DefaultScene\": \"Scenes/Main.escene\" }");
		}
		~FTempContent()
		{
			std::error_code ErrorCode;
			fs::remove_all(Root, ErrorCode);
		}
	};
} // namespace

E_TEST(AssetReference_MoveFolderUpdatesFileRelativeReferences)
{
	FTempContent Temp("MoveFolder");
	fs::create_directories(Temp.Content / "Art");
	fs::path Moved;
	E_EXPECT_TRUE(FAssetFileOps::Move(Temp.Content / "Textures", Temp.Content / "Art", Moved) == FAssetFileOps::EResult::Ok);

	const auto Result = FAssetReferenceUpdater::UpdateAfterMove(Temp.Content, Temp.Project, { { Temp.Content / "Textures", Moved } });
	E_EXPECT_EQ(Result.ChangedFiles.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Contains(ReadText(Temp.Content / "Materials/Checker.emat"), "\"../Art/Textures/UV.png\""));
	E_EXPECT_TRUE(Contains(ReadText(Temp.Content / "Particles/Fire.eparticle"), "\"../Art/Textures/UV.png\""));
	// 원래 서식 유지 (줄바꿈/빈 문자열 키 그대로)
	E_EXPECT_TRUE(Contains(ReadText(Temp.Content / "Materials/Checker.emat"), "{\n  \"Name\": \"Checker\",\n"));
}

E_TEST(AssetReference_MovedFileRecomputesOwnReferences)
{
	FTempContent Temp("MoveFile");
	fs::create_directories(Temp.Content / "Materials/Sub");
	fs::path Moved;
	E_EXPECT_TRUE(FAssetFileOps::Move(Temp.Content / "Materials/Checker.emat", Temp.Content / "Materials/Sub", Moved) == FAssetFileOps::EResult::Ok);

	FAssetReferenceUpdater::UpdateAfterMove(Temp.Content, Temp.Project, { { Temp.Content / "Materials/Checker.emat", Moved } });
	// 옮긴 머티리얼 자신의 텍스처 경로는 새 폴더 기준으로
	E_EXPECT_TRUE(Contains(ReadText(Moved), "\"../../Textures/UV.png\""));
	// 씬은 Content 기준 새 경로, 이름이 폴더와 같은 엔티티("Materials")와 내장 도형은 그대로
	const std::string Scene = ReadText(Temp.Content / "Scenes/Main.escene");
	E_EXPECT_TRUE(Contains(Scene, "\"Materials/Sub/Checker.emat\""));
	E_EXPECT_TRUE(Contains(Scene, "\"Name\": \"Materials\""));
	E_EXPECT_TRUE(Contains(Scene, "\"primitive:cube\""));
}

E_TEST(AssetReference_RenameSceneUpdatesProjectAndFindsReferences)
{
	FTempContent Temp("Rename");
	fs::path Renamed;
	E_EXPECT_TRUE(FAssetFileOps::Rename(Temp.Content / "Scenes/Main.escene", L"Level.escene", Renamed) == FAssetFileOps::EResult::Ok);
	FAssetReferenceUpdater::UpdateAfterMove(Temp.Content, Temp.Project, { { Temp.Content / "Scenes/Main.escene", Renamed } });
	E_EXPECT_TRUE(Contains(ReadText(Temp.Project), "\"Scenes/Level.escene\""));

	// 텍스처를 참조하는 파일: 머티리얼 + 파티클 (씬은 텍스처를 직접 참조하지 않음)
	const auto Referencing = FAssetReferenceUpdater::FindReferencingFiles(Temp.Content, Temp.Project, { Temp.Content / "Textures/UV.png" });
	E_EXPECT_EQ(Referencing.size(), static_cast<size_t>(2));
	// 폴더 전체를 대상으로 해도 같다
	E_EXPECT_EQ(FAssetReferenceUpdater::FindReferencingFiles(Temp.Content, Temp.Project, { Temp.Content / "Textures" }).size(), static_cast<size_t>(2));

	// 열린 씬 문자열 갱신용
	const auto Remapped = FAssetReferenceUpdater::RemapContentPath("Scenes/Main.escene", Temp.Content, { { Temp.Content / "Scenes/Main.escene", Renamed } });
	E_EXPECT_TRUE(Remapped.has_value() && *Remapped == "Scenes/Level.escene");
	E_EXPECT_FALSE(FAssetReferenceUpdater::RemapContentPath("Fox.glb", Temp.Content, { { Temp.Content / "Scenes", Temp.Content / "X" } }).has_value());
}

E_TEST(AssetReference_DataTablesFollowMovedFiles)
{
	// 데이터 구조체/테이블/에셋: Struct 경로, RowRef Table, Asset 값은 Content 기준. 행 이름(확장자 없음)과 필터 문자열은 그대로
	FTempContent Temp("DataTables");
	WriteText(Temp.Content / "Data/Item.estruct",
	          "{\"Version\": 1, \"Fields\": [{\"Name\":\"Icon\",\"Type\":\"Asset\",\"Filter\":\".png;.jpg\",\"Default\":\"\"},"
	          "{\"Name\":\"Next\",\"Type\":\"RowRef\",\"Table\":\"Data/Items.etable\",\"Default\":\"\"}]}");
	WriteText(Temp.Content / "Data/Items.etable",
	          "{\"Version\": 1, \"Struct\": \"Data/Item.estruct\", \"Rows\": [{\"Name\":\"Textures\",\"Values\":{\"Icon\":\"Textures/UV.png\",\"Next\":\"Textures\"}}]}");
	WriteText(Temp.Content / "Data/Config.edata", "{\"Version\": 1, \"Struct\": \"Data/Item.estruct\", \"Values\": {\"Icon\":\"Textures/UV.png\"}}");

	fs::create_directories(Temp.Content / "Art");
	fs::path MovedTextures;
	E_EXPECT_TRUE(FAssetFileOps::Move(Temp.Content / "Textures", Temp.Content / "Art", MovedTextures) == FAssetFileOps::EResult::Ok);
	fs::path MovedTable;
	E_EXPECT_TRUE(FAssetFileOps::Rename(Temp.Content / "Data/Items.etable", L"Loot.etable", MovedTable) == FAssetFileOps::EResult::Ok);
	fs::create_directories(Temp.Content / "Data/Defs");
	fs::path MovedStruct;
	E_EXPECT_TRUE(FAssetFileOps::Move(Temp.Content / "Data/Item.estruct", Temp.Content / "Data/Defs", MovedStruct) == FAssetFileOps::EResult::Ok);
	FAssetReferenceUpdater::UpdateAfterMove(Temp.Content, Temp.Project,
	                                        { { Temp.Content / "Textures", MovedTextures },
	                                          { Temp.Content / "Data/Items.etable", MovedTable },
	                                          { Temp.Content / "Data/Item.estruct", MovedStruct } });

	const std::string Struct = ReadText(MovedStruct);
	E_EXPECT_TRUE(Contains(Struct, "\"Table\":\"Data/Loot.etable\""));
	E_EXPECT_TRUE(Contains(Struct, "\".png;.jpg\""));
	const std::string Table = ReadText(MovedTable);
	E_EXPECT_TRUE(Contains(Table, "\"Struct\": \"Data/Defs/Item.estruct\""));
	E_EXPECT_TRUE(Contains(Table, "\"Icon\":\"Art/Textures/UV.png\""));
	E_EXPECT_TRUE(Contains(Table, "\"Name\":\"Textures\"") && Contains(Table, "\"Next\":\"Textures\"")); // 폴더와 같은 이름의 행은 그대로
	const std::string Asset = ReadText(Temp.Content / "Data/Config.edata");
	E_EXPECT_TRUE(Contains(Asset, "\"Struct\": \"Data/Defs/Item.estruct\"") && Contains(Asset, "\"Art/Textures/UV.png\""));
	// 삭제 확인: 구조체를 참조하는 파일
	E_EXPECT_EQ(FAssetReferenceUpdater::FindReferencingFiles(Temp.Content, Temp.Project, { MovedStruct }).size(), static_cast<size_t>(2));
}

E_TEST(AssetFileOps_GuardsAndUniqueNames)
{
	FTempContent Temp("FileOps");
	fs::path Out;
	// 폴더를 자기 안으로, 같은 위치, 이름 충돌, 잘못된 이름
	fs::create_directories(Temp.Content / "Textures/Inner");
	E_EXPECT_TRUE(FAssetFileOps::Move(Temp.Content / "Textures", Temp.Content / "Textures/Inner", Out) == FAssetFileOps::EResult::IntoOwnSubtree);
	E_EXPECT_TRUE(FAssetFileOps::Move(Temp.Content / "Textures/UV.png", Temp.Content / "Textures", Out) == FAssetFileOps::EResult::SameLocation);
	WriteText(Temp.Content / "Particles/UV.png", "other");
	E_EXPECT_TRUE(FAssetFileOps::Move(Temp.Content / "Textures/UV.png", Temp.Content / "Particles", Out) == FAssetFileOps::EResult::AlreadyExists);
	E_EXPECT_TRUE(FAssetFileOps::Rename(Temp.Content / "Textures/UV.png", L"a:b.png", Out) == FAssetFileOps::EResult::InvalidName);
	E_EXPECT_TRUE(fs::exists(Temp.Content / "Textures/UV.png"));

	// 복제: 번호가 붙은 새 이름, 폴더는 통째로
	E_EXPECT_TRUE(FAssetFileOps::Duplicate(Temp.Content / "Materials/Checker.emat", Out) == FAssetFileOps::EResult::Ok);
	E_EXPECT_TRUE(Out.filename() == L"Checker1.emat");
	E_EXPECT_TRUE(FAssetFileOps::Duplicate(Temp.Content / "Materials/Checker.emat", Out) == FAssetFileOps::EResult::Ok);
	E_EXPECT_TRUE(Out.filename() == L"Checker2.emat");
	E_EXPECT_TRUE(FAssetFileOps::Duplicate(Temp.Content / "Textures", Out) == FAssetFileOps::EResult::Ok);
	E_EXPECT_TRUE(fs::exists(Out / "UV.png"));

	E_EXPECT_TRUE(FAssetFileOps::CreateFolder(Temp.Content, L"New Folder", Out) == FAssetFileOps::EResult::Ok);
	E_EXPECT_TRUE(FAssetFileOps::CreateFolder(Temp.Content, L"New Folder", Out) == FAssetFileOps::EResult::Ok);
	E_EXPECT_TRUE(Out.filename() == L"New Folder1");
	E_EXPECT_TRUE(FAssetFileOps::IsSameOrUnder(Temp.Content / "A/B", Temp.Content / "a"));
	E_EXPECT_FALSE(FAssetFileOps::IsSameOrUnder(Temp.Content / "AB", Temp.Content / "A"));
}
