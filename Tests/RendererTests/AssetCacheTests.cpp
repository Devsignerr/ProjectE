#include "Core/Paths.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Core/Testing/TestFramework.h"
#include "Renderer/AssetCache.h"

#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
	constexpr float Tol = 0.0f; // 바이너리 왕복은 비트 단위로 같아야 한다

	FModelData MakeTestModel()
	{
		FModelData Model;
		Model.Name = "테스트모델";

		FModelImage& Image = Model.Images.emplace_back();
		Image.Name  = "Tex";
		Image.Image = FImage::MakeSolidColor(2, 3, 10, 20, 30, 40);

		FModelMaterial& Material = Model.Materials.emplace_back();
		Material.Name            = "Mat";
		Material.BaseColorFactor = FVector4(0.1f, 0.2f, 0.3f, 1.0f);
		Material.BaseColorImage  = 0;
		Material.RoughnessFactor = 0.25f;

		FModelMesh& Mesh = Model.Meshes.emplace_back();
		Mesh.Name     = "Tri";
		Mesh.Material = 0;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			FVertex Vertex;
			Vertex.Position = FVector3(static_cast<float>(Index), 1.0f, 2.0f);
			Vertex.Normal   = FVector3::UpVector;
			Vertex.UV       = FVector2(0.5f, static_cast<float>(Index));
			Mesh.Data.Vertices.push_back(Vertex);
		}
		Mesh.Data.Indices = { 0, 2, 1 };

		FModelNode& Root = Model.Nodes.emplace_back();
		Root.Name        = "Root";
		Root.Children    = { 1 };
		Root.Translation = FVector3(1.0f, 2.0f, 3.0f);
		FModelNode& Child = Model.Nodes.emplace_back();
		Child.Name     = "Child";
		Child.Parent   = 0;
		Child.Rotation = FQuat::FromEuler(10.0f, 20.0f, 30.0f);
		Child.Scale    = FVector3(2.0f);
		Child.Meshes   = { 0 };
		Model.RootNodes = { 0 };
		return Model;
	}
} // namespace

E_TEST(AssetCache_ModelSerializationRoundTrip)
{
	FModelData Source = MakeTestModel();
	FAssetCache::CompressModelImages(Source); // 2x3 → 블록 크기가 아니므로 RGBA8 + 밉
	E_EXPECT_FALSE(Source.Images[0].Image.IsValid());
	E_EXPECT_TRUE(Source.Images[0].Texture.bSRGB); // 베이스 컬러 → 색상 용도

	FBinaryWriter Writer;
	FAssetCache::WriteModel(Writer, Source);
	FBinaryReader Reader(Writer.GetBuffer().data(), Writer.GetBuffer().size());
	FModelData    Loaded;
	E_EXPECT_TRUE(FAssetCache::ReadModel(Reader, Loaded));
	E_EXPECT_TRUE(Reader.IsAtEnd());

	E_EXPECT_TRUE(Loaded.Name == Source.Name);
	E_EXPECT_EQ(Loaded.Images.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(Loaded.Images[0].Texture.Mips[0].Data == FImage::MakeSolidColor(2, 3, 10, 20, 30, 40).Pixels);
	E_EXPECT_EQ(Loaded.Images[0].Texture.GetHeight(), 3u);
	E_EXPECT_EQ(Loaded.Images[0].Texture.Mips.size(), Source.Images[0].Texture.Mips.size());
	E_EXPECT_EQUALS(Loaded.Materials[0].BaseColorFactor, Source.Materials[0].BaseColorFactor, Tol);
	E_EXPECT_NEAR(Loaded.Materials[0].RoughnessFactor, 0.25f, Tol);
	E_EXPECT_EQ(Loaded.Meshes[0].Data.Vertices.size(), static_cast<size_t>(3));
	E_EXPECT_EQUALS(Loaded.Meshes[0].Data.Vertices[2].Position, FVector3(2.0f, 1.0f, 2.0f), Tol);
	E_EXPECT_TRUE(Loaded.Meshes[0].Data.Indices == Source.Meshes[0].Data.Indices);
	E_EXPECT_EQ(Loaded.Nodes.size(), static_cast<size_t>(2));
	E_EXPECT_EQUALS(Loaded.Nodes[1].Rotation, Source.Nodes[1].Rotation, Tol);
	E_EXPECT_TRUE(Loaded.Nodes[0].Children == std::vector<int32>{ 1 });
	E_EXPECT_TRUE(Loaded.RootNodes == Source.RootNodes);
}

E_TEST(AssetCache_RejectsCorruptOrMismatchedData)
{
	FBinaryWriter Writer;
	FAssetCache::WriteModel(Writer, MakeTestModel());
	std::vector<uint8> Bytes = Writer.GetBuffer();

	// 버전 불일치
	std::vector<uint8> WrongVersion = Bytes;
	WrongVersion[4] ^= 0xFF;
	FBinaryReader VersionReader(WrongVersion.data(), WrongVersion.size());
	FModelData    Out;
	E_EXPECT_FALSE(FAssetCache::ReadModel(VersionReader, Out));

	// 잘린 파일
	FBinaryReader TruncatedReader(Bytes.data(), Bytes.size() / 2);
	E_EXPECT_FALSE(FAssetCache::ReadModel(TruncatedReader, Out));

	// 범위 밖 인덱스를 가진 모델은 쓰기는 되지만 읽기에서 거부된다
	FModelData Broken = MakeTestModel();
	Broken.Meshes[0].Data.Indices = { 0, 1, 99 };
	FBinaryWriter BrokenWriter;
	FAssetCache::WriteModel(BrokenWriter, Broken);
	FBinaryReader BrokenReader(BrokenWriter.GetBuffer().data(), BrokenWriter.GetBuffer().size());
	E_EXPECT_FALSE(FAssetCache::ReadModel(BrokenReader, Out));

	// 텍스처 형식 불일치 (모델 데이터를 텍스처로 읽기)
	FBinaryReader      TextureReader(Bytes.data(), Bytes.size());
	FCompressedTexture Texture;
	E_EXPECT_FALSE(FAssetCache::ReadTexture(TextureReader, Texture));
}

E_TEST(AssetCache_TextureRoundTripAndPaths)
{
	const FCompressedTexture Source = TextureCompression::Compress(FImage::MakeSolidColor(8, 4, 1, 2, 3, 4), ETextureUsage::Color);
	FBinaryWriter            Writer;
	FAssetCache::WriteTexture(Writer, Source);
	FBinaryReader      Reader(Writer.GetBuffer().data(), Writer.GetBuffer().size());
	FCompressedTexture Loaded;
	E_EXPECT_TRUE(FAssetCache::ReadTexture(Reader, Loaded));
	E_EXPECT_TRUE(Loaded.Format == ETextureFormat::BC7 && Loaded.bSRGB);
	E_EXPECT_EQ(Loaded.Mips.size(), Source.Mips.size());
	E_EXPECT_TRUE(Loaded.GetWidth() == 8 && Loaded.GetHeight() == 4 && Loaded.Mips.back().Data == Source.Mips.back().Data);

	// 밉 데이터 크기가 형식과 맞지 않으면 거부
	FCompressedTexture Broken = Source;
	Broken.Mips[0].Data.pop_back();
	FBinaryWriter BrokenWriter;
	FAssetCache::WriteTexture(BrokenWriter, Broken);
	FBinaryReader BrokenReader(BrokenWriter.GetBuffer().data(), BrokenWriter.GetBuffer().size());
	E_EXPECT_FALSE(FAssetCache::ReadTexture(BrokenReader, Loaded));

	// 경로 규칙: Content 안 → Cooked/<상대 경로><용도 확장자>, 밖 → 빈 경로
	if (FPaths::HasProject())
	{
		const std::filesystem::path Cooked = FAssetCache::GetCookedPath(FPaths::GetProjectContentDirectory() / L"Sub" / L"A.png",
		                                                                FAssetCache::GetTextureExtension(ETextureUsage::Normal));
		E_EXPECT_TRUE(Cooked == FPaths::GetProjectDirectory() / L"Cooked" / L"Sub" / L"A.png.normal.etex");
		E_EXPECT_TRUE(FAssetCache::GetCookedPath(FTestRegistry::GetTempDirectory() / L"X.png",
		                                         FAssetCache::GetTextureExtension(ETextureUsage::Color)).empty());
	}
}

E_TEST(AssetCache_UpToDateCheck)
{
	const std::filesystem::path Dir    = FTestRegistry::GetTempDirectory() / L"ProjectE_테스트_Cache";
	const std::filesystem::path Source = Dir / L"Source.txt";
	const std::filesystem::path Cooked = Dir / L"Source.txt.etex";
	std::filesystem::create_directories(Dir);

	std::ofstream(Source) << "a";
	E_EXPECT_FALSE(FAssetCache::IsCookedUpToDate(Source, Cooked)); // 쿠킹본 없음

	std::ofstream(Cooked) << "b";
	std::filesystem::last_write_time(Cooked, std::filesystem::last_write_time(Source) + std::chrono::seconds(1));
	E_EXPECT_TRUE(FAssetCache::IsCookedUpToDate(Source, Cooked));

	// 원본이 더 새로워지면 무효
	std::filesystem::last_write_time(Source, std::filesystem::last_write_time(Cooked) + std::chrono::seconds(1));
	E_EXPECT_FALSE(FAssetCache::IsCookedUpToDate(Source, Cooked));

	// 원본이 없으면(패키지) 쿠킹본 신뢰
	std::filesystem::remove(Source);
	E_EXPECT_TRUE(FAssetCache::IsCookedUpToDate(Source, Cooked));

	std::filesystem::remove_all(Dir);
}

// 실제 샘플 모델: 첫 로드는 변환, 두 번째는 쿠킹본 사용이며 내용이 같아야 한다
E_TEST(AssetCache_DamagedHelmetCookedMatchesSource)
{
	if (!FPaths::HasProject())
	{
		return;
	}
	const std::filesystem::path Source = FPaths::GetProjectContentDirectory() / L"DamagedHelmet.glb";
	if (!std::filesystem::exists(Source))
	{
		return;
	}
	const std::filesystem::path CookedPath = FAssetCache::GetCookedPath(Source, FAssetCache::ModelExtension);
	std::error_code             ErrorCode;
	std::filesystem::remove(CookedPath, ErrorCode);

	FModelData First;
	E_EXPECT_TRUE(FAssetCache::LoadModelAsset(Source, First) == FAssetCache::ESource::Converted);
	E_EXPECT_TRUE(std::filesystem::exists(CookedPath));

	FModelData Second;
	E_EXPECT_TRUE(FAssetCache::LoadModelAsset(Source, Second) == FAssetCache::ESource::Cooked);

	E_EXPECT_EQ(Second.Meshes.size(), First.Meshes.size());
	E_EXPECT_EQ(Second.Images.size(), First.Images.size());
	if (!First.Meshes.empty() && Second.Meshes.size() == First.Meshes.size())
	{
		E_EXPECT_EQ(Second.Meshes[0].Data.Vertices.size(), First.Meshes[0].Data.Vertices.size());
		E_EXPECT_TRUE(Second.Meshes[0].Data.Indices == First.Meshes[0].Data.Indices);
		E_EXPECT_EQUALS(Second.Meshes[0].Data.Vertices.back().Position, First.Meshes[0].Data.Vertices.back().Position, 0.0f);
	}
	if (!First.Images.empty() && Second.Images.size() == First.Images.size())
	{
		E_EXPECT_TRUE(Second.Images[0].Texture.Format == First.Images[0].Texture.Format);
		E_EXPECT_TRUE(Second.Images[0].Texture.Mips.back().Data == First.Images[0].Texture.Mips.back().Data);
	}
}
