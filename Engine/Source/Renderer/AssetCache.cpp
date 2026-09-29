#include "Renderer/AssetCache.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Serialization/BinaryArchive.h"
#include "Core/StringConv.h"

#include <chrono>

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
	std::error_code ErrorCode;
	if (!std::filesystem::exists(CookedPath, ErrorCode))
	{
		return false;
	}
	if (!std::filesystem::exists(SourcePath, ErrorCode))
	{
		return true; // 원본 없이 배포된 패키지: 쿠킹본 신뢰
	}
	const auto SourceTime = std::filesystem::last_write_time(SourcePath, ErrorCode);
	if (ErrorCode)
	{
		return false;
	}
	const auto CookedTime = std::filesystem::last_write_time(CookedPath, ErrorCode);
	return !ErrorCode && CookedTime >= SourceTime;
}

// ---------------------------------------------------------------- 이미지

void FAssetCache::WriteImage(FBinaryWriter& Writer, const FImage& Image)
{
	WriteHeader(Writer, ImageMagic, ImageVersion);
	Writer.Write(Image.Width);
	Writer.Write(Image.Height);
	Writer.WriteArray(Image.Pixels);
}

bool FAssetCache::ReadImage(FBinaryReader& Reader, FImage& OutImage)
{
	OutImage = FImage{};
	if (!ReadHeader(Reader, ImageMagic, ImageVersion))
	{
		return false;
	}
	OutImage.Width  = Reader.Read<uint32>();
	OutImage.Height = Reader.Read<uint32>();
	OutImage.Pixels = Reader.ReadArray<uint8>();
	if (!Reader.IsOk() || !OutImage.IsValid())
	{
		OutImage = FImage{};
		return false;
	}
	return true;
}

FAssetCache::ESource FAssetCache::LoadImageAsset(const std::filesystem::path& SourcePath, FImage& OutImage, bool bWriteCooked)
{
	const std::filesystem::path CookedPath = GetCookedPath(SourcePath, ImageExtension);
	if (!CookedPath.empty() && IsCookedUpToDate(SourcePath, CookedPath))
	{
		if (LoadCookedFile(CookedPath, [&](FBinaryReader& Reader) { return ReadImage(Reader, OutImage); }))
		{
			E_LOG(LogRenderer, Verbose, "쿠킹 이미지 사용: {}", ToDisplay(SourcePath));
			return ESource::Cooked;
		}
		E_LOG(LogRenderer, Warning, "쿠킹 이미지가 손상되었거나 형식이 달라 원본을 다시 읽습니다: {}", ToDisplay(CookedPath));
	}

	if (!FImageLoader::LoadFromFile(SourcePath, OutImage))
	{
		return ESource::Failed;
	}

	if (bWriteCooked && !CookedPath.empty())
	{
		FBinaryWriter Writer;
		WriteImage(Writer, OutImage);
		if (!Writer.SaveToFile(CookedPath))
		{
			E_LOG(LogRenderer, Warning, "쿠킹 이미지를 기록하지 못했습니다: {}", FStringConv::ToUtf8(CookedPath.wstring()));
		}
	}
	return ESource::Converted;
}

bool FAssetCache::CookImageAsset(const std::filesystem::path& SourcePath)
{
	const std::filesystem::path CookedPath = GetCookedPath(SourcePath, ImageExtension);
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
	WriteImage(Writer, Image);
	return Writer.SaveToFile(CookedPath);
}

// ---------------------------------------------------------------- 모델

void FAssetCache::WriteModel(FBinaryWriter& Writer, const FModelData& Model)
{
	WriteHeader(Writer, ModelMagic, ModelVersion);
	Writer.WriteString(Model.Name);

	Writer.Write(static_cast<uint32>(Model.Images.size()));
	for (const FModelImage& Image : Model.Images)
	{
		Writer.WriteString(Image.Name);
		Writer.Write(Image.Image.Width);
		Writer.Write(Image.Image.Height);
		Writer.WriteArray(Image.Image.Pixels); // 디코딩 실패 이미지는 빈 배열
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
		Image.Name         = Reader.ReadString();
		Image.Image.Width  = Reader.Read<uint32>();
		Image.Image.Height = Reader.Read<uint32>();
		Image.Image.Pixels = Reader.ReadArray<uint8>();
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
	const std::filesystem::path CookedPath = GetCookedPath(SourcePath, ModelExtension);
	if (!CookedPath.empty() && IsCookedUpToDate(SourcePath, CookedPath))
	{
		const auto StartTime = std::chrono::steady_clock::now();
		if (LoadCookedFile(CookedPath, [&](FBinaryReader& Reader) { return ReadModel(Reader, OutModel); }))
		{
			const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - StartTime).count();
			E_LOG(LogRenderer, Display, "쿠킹 모델 사용: {} ({:.1f} ms)", ToDisplay(SourcePath), Ms);
			return ESource::Cooked;
		}
		E_LOG(LogRenderer, Warning, "쿠킹 모델이 손상되었거나 형식이 달라 원본을 다시 읽습니다: {}", ToDisplay(CookedPath));
	}

	const auto StartTime = std::chrono::steady_clock::now();
	if (!FGltfLoader::Load(SourcePath, OutModel))
	{
		return ESource::Failed;
	}
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - StartTime).count();
	E_LOG(LogRenderer, Display, "원본 모델 변환: {} ({:.1f} ms)", ToDisplay(SourcePath), Ms);

	if (bWriteCooked && !CookedPath.empty())
	{
		FBinaryWriter Writer;
		WriteModel(Writer, OutModel);
		if (!Writer.SaveToFile(CookedPath))
		{
			E_LOG(LogRenderer, Warning, "쿠킹 모델을 기록하지 못했습니다: {}", FStringConv::ToUtf8(CookedPath.wstring()));
		}
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
	if (!FGltfLoader::Load(SourcePath, Model))
	{
		return false;
	}
	FBinaryWriter Writer;
	WriteModel(Writer, Model);
	return Writer.SaveToFile(CookedPath);
}
