#include "Renderer/AssetCache.h"

#include "Core/Profiling.h"
#include "Core/FileSystem.h"
#include "Core/Jobs/JobQueue.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Core/StringConv.h"
#include "Renderer/FbxLoader.h"
#include "Renderer/ModelImportSettings.h"
#include "Renderer/TextureStreamingMath.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cwctype>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// 헤더: Magic, Version
	void WriteHeader(FBinaryWriter& Writer, uint32 Magic, uint32 Version)
	{
		Writer.Write(Magic);
		Writer.Write(Version);
	}

	bool ReadHeader(FBinaryReader& Reader, uint32 ExpectedMagic, uint32 ExpectedVersion)
	{
		const uint32 Magic   = Reader.Read<uint32>();
		const uint32 Version = Reader.Read<uint32>();
		return Reader.IsOk() && Magic == ExpectedMagic && Version == ExpectedVersion;
	}

	template <typename TLoadFunc>
	bool LoadCookedFile(const std::filesystem::path& CookedPath, TLoadFunc&& Load)
	{
		std::vector<uint8> Bytes;
		if (!ReadFileBytes(CookedPath, Bytes))
		{
			return false;
		}
		FBinaryReader Reader(Bytes.data(), Bytes.size());
		return Load(Reader) && Reader.IsOk() && Reader.IsAtEnd();
	}

	// 텍스처 본문 (헤더 없음): 형식, sRGB, 밉 수, 밉별 크기/데이터. 밉 0개 = 빈 텍스처 (디코딩 실패 이미지)
	void WriteTexturePayload(FBinaryWriter& Writer, const FCompressedTexture& Texture)
	{
		Writer.Write(static_cast<uint8>(Texture.Format));
		Writer.Write(static_cast<uint8>(Texture.bSRGB ? 1 : 0));
		Writer.Write(static_cast<uint32>(Texture.Mips.size()));
		for (const FTextureMip& Mip : Texture.Mips)
		{
			Writer.Write(Mip.Width);
			Writer.Write(Mip.Height);
			Writer.WriteArray(Mip.Data);
		}
	}

	bool ReadTexturePayload(FBinaryReader& Reader, FCompressedTexture& OutTexture, bool bAllowEmpty)
	{
		OutTexture            = FCompressedTexture{};
		const uint8  Format   = Reader.Read<uint8>();
		const uint8  bSRGB    = Reader.Read<uint8>();
		const uint32 MipCount = Reader.Read<uint32>();
		if (!Reader.IsOk() || Format > static_cast<uint8>(ETextureFormat::BC4) || MipCount > 32 || (MipCount == 0 && !bAllowEmpty))
		{
			return false;
		}
		OutTexture.Format = static_cast<ETextureFormat>(Format);
		OutTexture.bSRGB  = bSRGB != 0;
		OutTexture.Mips.resize(MipCount);
		for (FTextureMip& Mip : OutTexture.Mips)
		{
			Mip.Width  = Reader.Read<uint32>();
			Mip.Height = Reader.Read<uint32>();
			Mip.Data   = Reader.ReadArray<uint8>();
			if (!Reader.IsOk() || Mip.Width == 0 || Mip.Height == 0 ||
			    Mip.Data.size() != TextureCompression::GetMipDataSize(OutTexture.Format, Mip.Width, Mip.Height))
			{
				OutTexture = FCompressedTexture{};
				return false;
			}
		}
		return true;
	}

	std::string ToDisplay(const std::filesystem::path& Path)
	{
		return FStringConv::ToUtf8(Path.filename().wstring());
	}
} // namespace

// ---------------------------------------------------------------- 경로 / 유효성

std::filesystem::path FAssetCache::GetCookedDirectory()
{
	if (!FPaths::IsInitialized() || !FPaths::HasProject())
	{
		return {};
	}
	return FPaths::GetProjectDirectory() / L"Cooked";
}

std::filesystem::path FAssetCache::GetCookedPath(const std::filesystem::path& SourcePath, const wchar_t* CookedExtension)
{
	const std::filesystem::path CookedDirectory = GetCookedDirectory();
	if (CookedDirectory.empty())
	{
		return {};
	}

	std::error_code             ErrorCode;
	const std::filesystem::path Source   = std::filesystem::weakly_canonical(SourcePath, ErrorCode);
	const std::filesystem::path Content  = std::filesystem::weakly_canonical(FPaths::GetProjectContentDirectory(), ErrorCode);
	const std::filesystem::path Relative = std::filesystem::relative(Source, Content, ErrorCode);
	if (ErrorCode || Relative.empty() || Relative.native().rfind(L"..", 0) == 0 || Relative.is_absolute())
	{
		return {}; // Content 밖
	}

	std::filesystem::path Cooked = CookedDirectory / Relative;
	Cooked += CookedExtension;
	return Cooked;
}

bool FAssetCache::IsCookedUpToDate(const std::filesystem::path& SourcePath, const std::filesystem::path& CookedPath)
{
	if (!FFileSystem::Exists(CookedPath))
	{
		return false;
	}
	if (!FFileSystem::Exists(SourcePath))
	{
		return true; // 원본 없이 배포된 패키지: 쿠킹본 신뢰
	}
	const auto SourceTime = FFileSystem::GetLastWriteTime(SourcePath);
	const auto CookedTime = FFileSystem::GetLastWriteTime(CookedPath);
	return SourceTime && CookedTime && *CookedTime >= *SourceTime;
}

// ---------------------------------------------------------------- 텍스처

const wchar_t* FAssetCache::GetTextureExtension(ETextureUsage Usage)
{
	switch (Usage)
	{
	case ETextureUsage::Color:  return L".color.etex";
	case ETextureUsage::Linear: return L".linear.etex";
	case ETextureUsage::Normal: return L".normal.etex";
	case ETextureUsage::Mask:   return L".mask.etex";
	case ETextureUsage::PixelArt: return L".pixel.etex";
	}
	return L".etex";
}

void FAssetCache::WriteTexture(FBinaryWriter& Writer, const FCompressedTexture& Texture)
{
	WriteHeader(Writer, TextureMagic, TextureVersion);
	WriteTexturePayload(Writer, Texture);
}

bool FAssetCache::ReadTexture(FBinaryReader& Reader, FCompressedTexture& OutTexture)
{
	OutTexture = FCompressedTexture{};
	return ReadHeader(Reader, TextureMagic, TextureVersion) && ReadTexturePayload(Reader, OutTexture, false);
}

void FAssetCache::CompressModelImages(FModelData& Model)
{
	// 이미지별 용도: 여러 슬롯이 공유하면 우선순위가 높은 쪽 (ORM처럼 금속·거칠기와 AO가 공유하면 선형 BC7 — R 채널 보존)
	constexpr int32 NotUsed = -1;
	std::vector<int32> Priority(Model.Images.size(), NotUsed);
	std::vector<ETextureUsage> Usages(Model.Images.size(), ETextureUsage::Linear);
	auto Use = [&](int32 ImageIndex, ETextureUsage Usage, int32 UsagePriority) {
		if (ImageIndex >= 0 && ImageIndex < static_cast<int32>(Model.Images.size()) && UsagePriority > Priority[ImageIndex])
		{
			Priority[ImageIndex] = UsagePriority;
			Usages[ImageIndex]   = Usage;
		}
	};
	for (const FModelMaterial& Material : Model.Materials)
	{
		Use(Material.BaseColorImage, ETextureUsage::Color, 3);
		Use(Material.EmissiveImage, ETextureUsage::Color, 3);
		Use(Material.NormalImage, ETextureUsage::Normal, 2);
		Use(Material.MetallicRoughnessImage, ETextureUsage::Linear, 1);
		Use(Material.OcclusionImage, ETextureUsage::Mask, 0);
	}

	const auto StartTime = std::chrono::steady_clock::now();
	size_t     TotalBytes = 0;
	// 이미지끼리 독립이므로 여러 장이면 작업 스레드로 나눠 압축 (결과는 같다 — 순서와 무관)
	std::vector<size_t> ToCompress;
	for (size_t Index = 0; Index < Model.Images.size(); ++Index)
	{
		if (Model.Images[Index].Image.IsValid() && Priority[Index] != NotUsed)
		{
			ToCompress.push_back(Index);
		}
	}
	FJobQueue Workers;
	Workers.Init(ToCompress.size() > 1 ? std::min<uint32>(FJobQueue::GetDefaultWorkerCount(), static_cast<uint32>(ToCompress.size())) : 0,
	             "모델 텍스처 압축");
	for (const size_t Index : ToCompress)
	{
		FModelImage*        Image = &Model.Images[Index];
		const ETextureUsage Usage = Usages[Index];
		Workers.Submit([Image, Usage] { Image->Texture = TextureCompression::Compress(Image->Image, Usage); });
	}
	Workers.WaitIdle();
	Workers.Shutdown();
	for (FModelImage& Image : Model.Images)
	{
		TotalBytes += Image.Texture.IsValid() ? Image.Texture.GetTotalBytes() : 0;
		Image.Image = FImage{};
	}
	E_LOG(LogRenderer, Display, "모델 텍스처 압축: {} — 이미지 {}개 → {} KB ({:.0f} ms)", Model.Name, Model.Images.size(), TotalBytes / 1024,
	      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - StartTime).count());
}

FAssetCache::ESource FAssetCache::LoadTextureAsset(const std::filesystem::path& SourcePath, ETextureUsage Usage,
                                                   FCompressedTexture& OutTexture, bool bWriteCooked)
{
	E_PROFILE_SCOPE("텍스처 에셋 로드");
	const std::filesystem::path CookedPath = GetCookedPath(SourcePath, GetTextureExtension(Usage));
	if (!CookedPath.empty() && IsCookedUpToDate(SourcePath, CookedPath))
	{
		if (LoadCookedFile(CookedPath, [&](FBinaryReader& Reader) { return ReadTexture(Reader, OutTexture); }))
		{
			E_LOG(LogRenderer, Verbose, "쿠킹 텍스처 사용: {}", ToDisplay(SourcePath));
			return ESource::Cooked;
		}
		E_LOG(LogRenderer, Warning, "쿠킹 텍스처가 손상되었거나 형식이 달라 원본을 다시 읽습니다: {}", ToDisplay(CookedPath));
	}

	FImage Image;
	if (!FImageLoader::LoadFromFile(SourcePath, Image))
	{
		return ESource::Failed;
	}
	const auto StartTime = std::chrono::steady_clock::now();
	OutTexture           = TextureCompression::Compress(Image, Usage);
	E_LOG(LogRenderer, Display, "텍스처 압축: {} {}x{} → {} KB ({:.0f} ms)", ToDisplay(SourcePath), Image.Width, Image.Height,
	      OutTexture.GetTotalBytes() / 1024,
	      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - StartTime).count());

	if (bWriteCooked && !CookedPath.empty())
	{
		FBinaryWriter Writer;
		WriteTexture(Writer, OutTexture);
		if (!Writer.SaveToFile(CookedPath))
		{
			E_LOG(LogRenderer, Warning, "쿠킹 텍스처를 기록하지 못했습니다: {}", FStringConv::ToUtf8(CookedPath.wstring()));
		}
	}
	return ESource::Converted;
}

void FAssetCache::WriteEnvironment(FBinaryWriter& Writer, const FEnvironmentImage& Image)
{
	WriteHeader(Writer, EnvironmentMagic, EnvironmentVersion);
	Writer.Write(Image.Width);
	Writer.Write(Image.Height);
	Writer.WriteArray(Image.Pixels);
}

bool FAssetCache::ReadEnvironment(FBinaryReader& Reader, FEnvironmentImage& OutImage)
{
	OutImage = FEnvironmentImage{};
	if (!ReadHeader(Reader, EnvironmentMagic, EnvironmentVersion))
	{
		return false;
	}
	OutImage.Width  = Reader.Read<uint32>();
	OutImage.Height = Reader.Read<uint32>();
	OutImage.Pixels = Reader.ReadArray<uint16>();
	if (!Reader.IsOk() || !OutImage.IsValid())
	{
		OutImage = FEnvironmentImage{};
		return false;
	}
	return true;
}

FAssetCache::ESource FAssetCache::LoadEnvironmentAsset(const std::filesystem::path& SourcePath, FEnvironmentImage& OutImage, bool bWriteCooked)
{
	E_PROFILE_SCOPE("환경맵 에셋 로드");
	const std::filesystem::path CookedPath = GetCookedPath(SourcePath, EnvironmentExtension);
	if (!CookedPath.empty() && IsCookedUpToDate(SourcePath, CookedPath))
	{
		if (LoadCookedFile(CookedPath, [&](FBinaryReader& Reader) { return ReadEnvironment(Reader, OutImage); }))
		{
			E_LOG(LogRenderer, Verbose, "쿠킹 환경맵 사용: {}", ToDisplay(SourcePath));
			return ESource::Cooked;
		}
		E_LOG(LogRenderer, Warning, "쿠킹 환경맵이 손상되었거나 형식이 달라 원본을 다시 읽습니다: {}", ToDisplay(CookedPath));
	}
	if (!FImageLoader::LoadEnvironmentFromFile(SourcePath, OutImage))
	{
		return ESource::Failed;
	}
	if (bWriteCooked && !CookedPath.empty())
	{
		FBinaryWriter Writer;
		WriteEnvironment(Writer, OutImage);
		if (!Writer.SaveToFile(CookedPath))
		{
			E_LOG(LogRenderer, Warning, "쿠킹 환경맵을 기록하지 못했습니다: {}", FStringConv::ToUtf8(CookedPath.wstring()));
		}
	}
	return ESource::Converted;
}

bool FAssetCache::CookEnvironmentAsset(const std::filesystem::path& SourcePath)
{
	const std::filesystem::path CookedPath = GetCookedPath(SourcePath, EnvironmentExtension);
	FEnvironmentImage           Image;
	if (CookedPath.empty() || !FImageLoader::LoadEnvironmentFromFile(SourcePath, Image))
	{
		return false;
	}
	FBinaryWriter Writer;
	WriteEnvironment(Writer, Image);
	return Writer.SaveToFile(CookedPath);
}

bool FAssetCache::CookTextureAsset(const std::filesystem::path& SourcePath, ETextureUsage Usage)
{
	const std::filesystem::path CookedPath = GetCookedPath(SourcePath, GetTextureExtension(Usage));
	if (CookedPath.empty())
	{
		E_LOG(LogRenderer, Warning, "프로젝트 Content 밖의 이미지는 쿠킹하지 않습니다: {}", FStringConv::ToUtf8(SourcePath.wstring()));
		return false;
	}
	FImage Image;
	if (!FImageLoader::LoadFromFile(SourcePath, Image))
	{
		return false;
	}
	FBinaryWriter Writer;
	WriteTexture(Writer, TextureCompression::Compress(Image, Usage));
	return Writer.SaveToFile(CookedPath);
}

// ---------------------------------------------------------------- 모델

void FAssetCache::ComputeModelUvDensities(FModelData& Model)
{
	for (FModelMesh& Mesh : Model.Meshes)
	{
		Mesh.Data.UvDensity = TextureStreamingMath::ComputeUvDensity(Mesh.Data.Vertices, Mesh.Data.Indices);
	}
}

void FAssetCache::WriteModel(FBinaryWriter& Writer, const FModelData& Model, std::vector<uint64>* OutImagePayloadOffsets)
{
	WriteHeader(Writer, ModelMagic, ModelVersion);
	Writer.WriteString(Model.Name);

	Writer.Write(static_cast<uint32>(Model.Images.size()));
	if (OutImagePayloadOffsets != nullptr)
	{
		OutImagePayloadOffsets->clear();
	}
	for (const FModelImage& Image : Model.Images)
	{
		Writer.WriteString(Image.Name);
		if (OutImagePayloadOffsets != nullptr)
		{
			OutImagePayloadOffsets->push_back(Writer.GetBuffer().size());
		}
		WriteTexturePayload(Writer, Image.Texture); // 디코딩 실패/미사용 이미지는 밉 0개
	}

	Writer.Write(static_cast<uint32>(Model.Materials.size()));
	for (const FModelMaterial& Material : Model.Materials)
	{
		Writer.WriteString(Material.Name);
		Writer.Write(Material.BaseColorFactor);
		Writer.Write(Material.EmissiveFactor);
		Writer.Write(Material.MetallicFactor);
		Writer.Write(Material.RoughnessFactor);
		Writer.Write(Material.NormalScale);
		Writer.Write(Material.OcclusionStrength);
		Writer.Write(static_cast<uint32>(Material.BlendMode));
		Writer.Write(Material.AlphaCutoff);
		Writer.Write(static_cast<uint32>(Material.bTwoSided ? 1u : 0u));
		Writer.Write(Material.BaseColorImage);
		Writer.Write(Material.MetallicRoughnessImage);
		Writer.Write(Material.NormalImage);
		Writer.Write(Material.OcclusionImage);
		Writer.Write(Material.EmissiveImage);
	}

	Writer.Write(static_cast<uint32>(Model.Meshes.size()));
	for (const FModelMesh& Mesh : Model.Meshes)
	{
		Writer.WriteString(Mesh.Name);
		Writer.Write(Mesh.Material);
		Writer.WriteArray(Mesh.Data.Vertices);
		Writer.WriteArray(Mesh.Data.Indices);
		Writer.WriteArray(Mesh.SkinVertices);
		Writer.Write(Mesh.Data.UvDensity);
		Writer.Write(static_cast<uint32>(Mesh.Data.Lods.size()));
		for (const FMeshLod& Lod : Mesh.Data.Lods)
		{
			Writer.WriteArray(Lod.Indices);
			Writer.Write(Lod.ScreenSize);
			Writer.Write(Lod.Error);
		}
	}

	Writer.Write(static_cast<uint32>(Model.Nodes.size()));
	for (const FModelNode& Node : Model.Nodes)
	{
		Writer.WriteString(Node.Name);
		Writer.Write(Node.Parent);
		Writer.WriteArray(Node.Children);
		Writer.Write(Node.Translation);
		Writer.Write(Node.Rotation);
		Writer.Write(Node.Scale);
		Writer.WriteArray(Node.Meshes);
		Writer.Write(Node.Skin);
	}

	Writer.WriteArray(Model.RootNodes);

	Writer.Write(static_cast<uint32>(Model.Skins.size()));
	for (const FModelSkin& Skin : Model.Skins)
	{
		Writer.WriteString(Skin.Name);
		Writer.WriteArray(Skin.Joints);
		Writer.WriteArray(Skin.InverseBindMatrices);
	}

	Writer.Write(static_cast<uint32>(Model.Animations.size()));
	for (const FAnimationClip& Clip : Model.Animations)
	{
		Writer.WriteString(Clip.Name);
		Writer.Write(Clip.Duration);
		Writer.Write(static_cast<uint32>(Clip.Channels.size()));
		for (const FAnimationChannel& Channel : Clip.Channels)
		{
			Writer.Write(Channel.Node);
			Writer.Write(static_cast<uint8>(Channel.Path));
			Writer.Write(static_cast<uint8>(Channel.Interpolation));
			Writer.WriteArray(Channel.Times);
			Writer.WriteArray(Channel.Values);
		}
	}
}

bool FAssetCache::ReadModel(FBinaryReader& Reader, FModelData& OutModel)
{
	OutModel = FModelData{};
	if (!ReadHeader(Reader, ModelMagic, ModelVersion))
	{
		return false;
	}
	OutModel.Name = Reader.ReadString();

	constexpr uint32 MaxElements = 1u << 20; // 손상 파일 방어

	const uint32 ImageCount = Reader.Read<uint32>();
	if (ImageCount > MaxElements)
	{
		return false;
	}
	OutModel.Images.resize(ImageCount);
	for (FModelImage& Image : OutModel.Images)
	{
		Image.Name                = Reader.ReadString();
		Image.CookedPayloadOffset = Reader.GetOffset(); // 파일 전체를 읽으므로 파일 안 위치와 같다
		if (!ReadTexturePayload(Reader, Image.Texture, true))
		{
			return false;
		}
	}

	const uint32 MaterialCount = Reader.Read<uint32>();
	if (MaterialCount > MaxElements)
	{
		return false;
	}
	OutModel.Materials.resize(MaterialCount);
	for (FModelMaterial& Material : OutModel.Materials)
	{
		Material.Name                   = Reader.ReadString();
		Material.BaseColorFactor        = Reader.Read<FVector4>();
		Material.EmissiveFactor         = Reader.Read<FVector3>();
		Material.MetallicFactor         = Reader.Read<float>();
		Material.RoughnessFactor        = Reader.Read<float>();
		Material.NormalScale            = Reader.Read<float>();
		Material.OcclusionStrength      = Reader.Read<float>();
		const uint32 BlendMode          = Reader.Read<uint32>();
		Material.BlendMode              = BlendMode < static_cast<uint32>(EMaterialBlendMode::Count) ? static_cast<EMaterialBlendMode>(BlendMode)
		                                                                                              : EMaterialBlendMode::Opaque;
		Material.AlphaCutoff            = Reader.Read<float>();
		Material.bTwoSided              = Reader.Read<uint32>() != 0;
		Material.BaseColorImage         = Reader.Read<int32>();
		Material.MetallicRoughnessImage = Reader.Read<int32>();
		Material.NormalImage            = Reader.Read<int32>();
		Material.OcclusionImage         = Reader.Read<int32>();
		Material.EmissiveImage          = Reader.Read<int32>();
	}

	const uint32 MeshCount = Reader.Read<uint32>();
	if (MeshCount > MaxElements)
	{
		return false;
	}
	OutModel.Meshes.resize(MeshCount);
	for (FModelMesh& Mesh : OutModel.Meshes)
	{
		Mesh.Name          = Reader.ReadString();
		Mesh.Material      = Reader.Read<int32>();
		Mesh.Data.Vertices = Reader.ReadArray<FVertex>();
		Mesh.Data.Indices  = Reader.ReadArray<uint32>();
		Mesh.SkinVertices  = Reader.ReadArray<FSkinVertex>();
		Mesh.Data.UvDensity = Reader.Read<float>();
		const uint32 LodCount = Reader.Read<uint32>();
		if (LodCount > 16)
		{
			return false;
		}
		Mesh.Data.Lods.resize(LodCount);
		for (FMeshLod& Lod : Mesh.Data.Lods)
		{
			Lod.Indices    = Reader.ReadArray<uint32>();
			Lod.ScreenSize = Reader.Read<float>();
			Lod.Error      = Reader.Read<float>();
		}
	}

	const uint32 NodeCount = Reader.Read<uint32>();
	if (NodeCount > MaxElements)
	{
		return false;
	}
	OutModel.Nodes.resize(NodeCount);
	for (FModelNode& Node : OutModel.Nodes)
	{
		Node.Name        = Reader.ReadString();
		Node.Parent      = Reader.Read<int32>();
		Node.Children    = Reader.ReadArray<int32>();
		Node.Translation = Reader.Read<FVector3>();
		Node.Rotation    = Reader.Read<FQuat>();
		Node.Scale       = Reader.Read<FVector3>();
		Node.Meshes      = Reader.ReadArray<int32>();
		Node.Skin        = Reader.Read<int32>();
	}

	OutModel.RootNodes = Reader.ReadArray<int32>();

	const uint32 SkinCount = Reader.Read<uint32>();
	if (SkinCount > MaxElements)
	{
		return false;
	}
	OutModel.Skins.resize(SkinCount);
	for (FModelSkin& Skin : OutModel.Skins)
	{
		Skin.Name                = Reader.ReadString();
		Skin.Joints              = Reader.ReadArray<int32>(MaxSkinJoints);
		Skin.InverseBindMatrices = Reader.ReadArray<FMatrix4x4>(MaxSkinJoints);
	}

	const uint32 AnimationCount = Reader.Read<uint32>();
	if (AnimationCount > MaxElements)
	{
		return false;
	}
	OutModel.Animations.resize(AnimationCount);
	for (FAnimationClip& Clip : OutModel.Animations)
	{
		Clip.Name     = Reader.ReadString();
		Clip.Duration = Reader.Read<float>();
		const uint32 ChannelCount = Reader.Read<uint32>();
		if (ChannelCount > MaxElements || !Reader.IsOk())
		{
			return false;
		}
		Clip.Channels.resize(ChannelCount);
		for (FAnimationChannel& Channel : Clip.Channels)
		{
			Channel.Node          = Reader.Read<int32>();
			const uint8 Path      = Reader.Read<uint8>();
			const uint8 Interp    = Reader.Read<uint8>();
			if (Path > static_cast<uint8>(EAnimationPath::Scale) || Interp > static_cast<uint8>(EAnimationInterpolation::Step))
			{
				return false;
			}
			Channel.Path          = static_cast<EAnimationPath>(Path);
			Channel.Interpolation = static_cast<EAnimationInterpolation>(Interp);
			Channel.Times         = Reader.ReadArray<float>();
			Channel.Values        = Reader.ReadArray<FVector4>();
		}
	}
	if (!Reader.IsOk())
	{
		OutModel = FModelData{};
		return false;
	}

	// 인덱스 범위 검증 (손상된 쿠킹본이 인스턴스화 단계에서 범위 밖 접근을 일으키지 않도록)
	const auto InRange = [](int32 Index, size_t Count) { return Index >= -1 && Index < static_cast<int32>(Count); };
	for (const FModelMaterial& Material : OutModel.Materials)
	{
		for (const int32 ImageIndex : { Material.BaseColorImage, Material.MetallicRoughnessImage, Material.NormalImage,
		                                Material.OcclusionImage, Material.EmissiveImage })
		{
			if (!InRange(ImageIndex, OutModel.Images.size())) return false;
		}
	}
	for (const FModelMesh& Mesh : OutModel.Meshes)
	{
		if (!InRange(Mesh.Material, OutModel.Materials.size())) return false;
		for (uint32 Index : Mesh.Data.Indices)
		{
			if (Index >= Mesh.Data.Vertices.size()) return false;
		}
		for (const FMeshLod& Lod : Mesh.Data.Lods)
		{
			if (Lod.Indices.size() % 3 != 0) return false;
			for (uint32 Index : Lod.Indices)
			{
				if (Index >= Mesh.Data.Vertices.size()) return false;
			}
		}
	}
	for (const FModelNode& Node : OutModel.Nodes)
	{
		if (!InRange(Node.Parent, OutModel.Nodes.size())) return false;
		for (int32 Child : Node.Children)
		{
			if (Child < 0 || Child >= static_cast<int32>(OutModel.Nodes.size())) return false;
		}
		for (int32 MeshIndex : Node.Meshes)
		{
			if (MeshIndex < 0 || MeshIndex >= static_cast<int32>(OutModel.Meshes.size())) return false;
		}
	}
	for (int32 Root : OutModel.RootNodes)
	{
		if (Root < 0 || Root >= static_cast<int32>(OutModel.Nodes.size())) return false;
	}
	for (const FModelMesh& Mesh : OutModel.Meshes)
	{
		if (!Mesh.SkinVertices.empty() && Mesh.SkinVertices.size() != Mesh.Data.Vertices.size()) return false;
	}
	for (const FModelNode& Node : OutModel.Nodes)
	{
		if (!InRange(Node.Skin, OutModel.Skins.size())) return false;
	}
	for (const FModelSkin& Skin : OutModel.Skins)
	{
		if (Skin.Joints.size() != Skin.InverseBindMatrices.size()) return false;
		for (int32 Joint : Skin.Joints)
		{
			if (Joint < 0 || Joint >= static_cast<int32>(OutModel.Nodes.size())) return false;
		}
	}
	for (const FAnimationClip& Clip : OutModel.Animations)
	{
		for (const FAnimationChannel& Channel : Clip.Channels)
		{
			if (Channel.Node < 0 || Channel.Node >= static_cast<int32>(OutModel.Nodes.size())) return false;
			if (Channel.Times.size() != Channel.Values.size()) return false;
		}
	}
	return true;
}

FAssetCache::ESource FAssetCache::LoadModelAsset(const std::filesystem::path& SourcePath, FModelData& OutModel, bool bWriteCooked)
{
	E_PROFILE_SCOPE("모델 에셋 로드");
	const std::filesystem::path CookedPath = GetCookedPath(SourcePath, ModelExtension);
	if (!CookedPath.empty() && IsCookedUpToDate(SourcePath, CookedPath) && IsCookedNewerThanImportInputs(SourcePath, CookedPath) &&
	    IsCookedWithCurrentImportSettings(SourcePath, CookedPath))
	{
		const auto StartTime = std::chrono::steady_clock::now();
		if (LoadCookedFile(CookedPath, [&](FBinaryReader& Reader) { return ReadModel(Reader, OutModel); }))
		{
			const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - StartTime).count();
			E_LOG(LogRenderer, Display, "쿠킹 모델 사용: {} ({:.1f} ms)", ToDisplay(SourcePath), Ms);
			OutModel.CookedPath = CookedPath;
			return ESource::Cooked;
		}
		E_LOG(LogRenderer, Warning, "쿠킹 모델이 손상되었거나 형식이 달라 원본을 다시 읽습니다: {}", ToDisplay(CookedPath));
	}

	const auto StartTime = std::chrono::steady_clock::now();
	if (!LoadModelSource(SourcePath, OutModel))
	{
		return ESource::Failed;
	}
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - StartTime).count();
	E_LOG(LogRenderer, Display, "원본 모델 변환: {} ({:.1f} ms)", ToDisplay(SourcePath), Ms);
	CompressModelImages(OutModel);
	ComputeModelUvDensities(OutModel);

	if (bWriteCooked && !CookedPath.empty())
	{
		FBinaryWriter       Writer;
		std::vector<uint64> ImageOffsets;
		WriteModel(Writer, OutModel, &ImageOffsets);
		if (!Writer.SaveToFile(CookedPath))
		{
			E_LOG(LogRenderer, Warning, "쿠킹 모델을 기록하지 못했습니다: {}", FStringConv::ToUtf8(CookedPath.wstring()));
		}
		else
		{
			// 방금 기록한 파일에서 밉 스트리밍이 다시 읽을 수 있다
			OutModel.CookedPath = CookedPath;
			for (size_t Index = 0; Index < OutModel.Images.size() && Index < ImageOffsets.size(); ++Index)
			{
				OutModel.Images[Index].CookedPayloadOffset = ImageOffsets[Index];
			}
		}
		WriteImportSettingsRecord(SourcePath, CookedPath);
	}
	return ESource::Converted;
}

bool FAssetCache::CookModelAsset(const std::filesystem::path& SourcePath)
{
	const std::filesystem::path CookedPath = GetCookedPath(SourcePath, ModelExtension);
	if (CookedPath.empty())
	{
		E_LOG(LogRenderer, Warning, "프로젝트 Content 밖의 모델은 쿠킹하지 않습니다: {}", FStringConv::ToUtf8(SourcePath.wstring()));
		return false;
	}
	FModelData Model;
	if (!LoadModelSource(SourcePath, Model))
	{
		return false;
	}
	CompressModelImages(Model);
	ComputeModelUvDensities(Model);
	FBinaryWriter Writer;
	WriteModel(Writer, Model);
	if (!Writer.SaveToFile(CookedPath))
	{
		return false;
	}
	WriteImportSettingsRecord(SourcePath, CookedPath);
	return true;
}

// ---------------------------------------------------------------- 모델 원본 (+ 임포트 설정)

bool FAssetCache::LoadModelFile(const std::filesystem::path& SourcePath, FModelData& OutModel)
{
	std::wstring Extension = SourcePath.extension().wstring();
	std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
	return Extension == L".fbx" ? FFbxLoader::Load(SourcePath, OutModel) : FGltfLoader::Load(SourcePath, OutModel);
}

bool FAssetCache::LoadModelSource(const std::filesystem::path& SourcePath, FModelData& OutModel)
{
	if (!LoadModelFile(SourcePath, OutModel))
	{
		return false;
	}
	const FModelImportSettings Settings = FModelImportSettings::LoadForSource(SourcePath);
	// 추가 애니메이션 파일: 노드 이름으로 채널을 맞춰 붙인다 (클립 이름 = 파일 이름)
	for (const std::string& Relative : Settings.AnimationSources)
	{
		const std::filesystem::path AnimationPath = SourcePath.parent_path() / FStringConv::ToWide(Relative);
		FModelData                  AnimationModel;
		if (!LoadModelFile(AnimationPath, AnimationModel))
		{
			E_LOG(LogRenderer, Warning, "추가 애니메이션 파일을 읽지 못했습니다: {}", Relative);
			continue;
		}
		const uint32 Merged = FModelImportSettings::MergeAnimations(OutModel, AnimationModel, FStringConv::ToUtf8(AnimationPath.stem().wstring()));
		E_LOG(LogRenderer, Display, "추가 애니메이션 {}: 클립 {}개", Relative, Merged);
	}
	Settings.Apply(OutModel);
	return true;
}

bool FAssetCache::IsCookedNewerThanImportInputs(const std::filesystem::path& SourcePath, const std::filesystem::path& CookedPath)
{
	// 임포트 설정 파일과 추가 애니메이션 파일도 원본의 일부로 본다
	const auto CookedTime = FFileSystem::GetLastWriteTime(CookedPath);
	if (!CookedTime)
	{
		return false;
	}
	std::vector<std::filesystem::path> Inputs = { FModelImportSettings::GetSidecarPath(SourcePath) };
	for (const std::string& Relative : FModelImportSettings::LoadForSource(SourcePath).AnimationSources)
	{
		Inputs.push_back(SourcePath.parent_path() / FStringConv::ToWide(Relative));
	}
	for (const std::filesystem::path& Input : Inputs)
	{
		const auto InputTime = FFileSystem::GetLastWriteTime(Input);
		if (InputTime && *InputTime > *CookedTime)
		{
			return false;
		}
	}
	return true;
}

// 쿠킹에 쓴 임포트 설정을 "<쿠킹 파일>.import"에 남긴다 (설정 파일을 지우거나 되돌린 경우도 알아채기 위해 — 시각 비교만으로는 못 잡음)
namespace
{
	std::filesystem::path GetImportRecordPath(const std::filesystem::path& CookedPath)
	{
		std::filesystem::path Path = CookedPath;
		Path += L".import";
		return Path;
	}
} // namespace

void FAssetCache::WriteImportSettingsRecord(const std::filesystem::path& SourcePath, const std::filesystem::path& CookedPath)
{
	std::ofstream File(GetImportRecordPath(CookedPath), std::ios::binary | std::ios::trunc);
	File << FModelImportSettings::LoadForSource(SourcePath).ToJsonString();
}

bool FAssetCache::IsCookedWithCurrentImportSettings(const std::filesystem::path& SourcePath, const std::filesystem::path& CookedPath)
{
	if (!FFileSystem::Exists(SourcePath))
	{
		return true; // 원본 없는 패키지
	}
	std::string Record;
	if (!FFileSystem::ReadTextFile(GetImportRecordPath(CookedPath), Record))
	{
		return FModelImportSettings::LoadForSource(SourcePath).IsDefault(); // 기록이 없던 옛 쿠킹본: 기본 설정일 때만 유효
	}
	return Record == FModelImportSettings::LoadForSource(SourcePath).ToJsonString();
}
