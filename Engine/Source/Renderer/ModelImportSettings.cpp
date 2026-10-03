#include "Renderer/ModelImportSettings.h"

#include "Core/FileSystem.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Renderer/GltfLoader.h"
#include "Renderer/LodMath.h"
#include "Renderer/MeshSimplifier.h"

#include <algorithm>

#include <json.hpp>

#include <fstream>
#include <sstream>
#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	void GenerateSmoothNormals(FMeshData& Mesh)
	{
		for (FVertex& Vertex : Mesh.Vertices)
		{
			Vertex.Normal = FVector3::ZeroVector;
		}
		for (size_t Index = 0; Index + 2 < Mesh.Indices.size(); Index += 3)
		{
			FVertex& V0 = Mesh.Vertices[Mesh.Indices[Index + 0]];
			FVertex& V1 = Mesh.Vertices[Mesh.Indices[Index + 1]];
			FVertex& V2 = Mesh.Vertices[Mesh.Indices[Index + 2]];
			const FVector3 FaceNormal = FVector3::Cross(V1.Position - V0.Position, V2.Position - V0.Position);
			V0.Normal += FaceNormal;
			V1.Normal += FaceNormal;
			V2.Normal += FaceNormal;
		}
		for (FVertex& Vertex : Mesh.Vertices)
		{
			Vertex.Normal = Vertex.Normal.GetNormalized();
		}
	}
} // namespace

std::string FModelImportSettings::ToJsonString() const
{
	nlohmann::json Document;
	Document["Scale"]             = Scale;
	Document["YawDegrees"]        = YawDegrees;
	Document["ImportMaterials"]   = bImportMaterials;
	Document["ImportSkin"]        = bImportSkin;
	Document["ImportAnimations"]  = bImportAnimations;
	Document["RecomputeNormals"]  = bRecomputeNormals;
	Document["RecomputeTangents"] = bRecomputeTangents;
	Document["BlendAsMasked"]     = bBlendAsMasked;
	Document["GenerateLods"]      = bGenerateLods;
	Document["LodCount"]          = LodCount;
	Document["MaxTriangles"]      = MaxTriangles;
	Document["AnimationSources"]  = AnimationSources;
	return Document.dump(2);
}

bool FModelImportSettings::FromJsonString(const std::string& Json)
{
	const nlohmann::json Document = nlohmann::json::parse(Json, nullptr, false, true);
	if (Document.is_discarded() || !Document.is_object())
	{
		return false;
	}
	*this              = FModelImportSettings{};
	Scale              = Document.value("Scale", Scale);
	YawDegrees         = Document.value("YawDegrees", YawDegrees);
	bImportMaterials   = Document.value("ImportMaterials", bImportMaterials);
	bImportSkin        = Document.value("ImportSkin", bImportSkin);
	bImportAnimations  = Document.value("ImportAnimations", bImportAnimations);
	bRecomputeNormals  = Document.value("RecomputeNormals", bRecomputeNormals);
	bRecomputeTangents = Document.value("RecomputeTangents", bRecomputeTangents);
	bBlendAsMasked     = Document.value("BlendAsMasked", bBlendAsMasked);
	bGenerateLods      = Document.value("GenerateLods", bGenerateLods);
	LodCount           = std::clamp<uint32>(Document.value("LodCount", LodCount), 1u, LodMath::MaxLods);
	MaxTriangles       = Document.value("MaxTriangles", MaxTriangles);
	if (const auto It = Document.find("AnimationSources"); It != Document.end() && It->is_array())
	{
		for (const nlohmann::json& Item : *It)
		{
			if (Item.is_string())
			{
				AnimationSources.push_back(Item.get<std::string>());
			}
		}
	}
	return true;
}

std::filesystem::path FModelImportSettings::GetSidecarPath(const std::filesystem::path& SourcePath)
{
	std::filesystem::path Path = SourcePath;
	Path += Extension;
	return Path;
}

FModelImportSettings FModelImportSettings::LoadForSource(const std::filesystem::path& SourcePath)
{
	FModelImportSettings Settings;
	std::string          Text;
	if (FFileSystem::ReadTextFile(GetSidecarPath(SourcePath), Text))
	{
		if (!Settings.FromJsonString(Text))
		{
			E_LOG(LogRenderer, Warning, "임포트 설정을 읽지 못해 기본값을 씁니다: {}", FStringConv::ToUtf8(GetSidecarPath(SourcePath).wstring()));
			Settings = FModelImportSettings{};
		}
	}
	return Settings;
}

bool FModelImportSettings::SaveForSource(const std::filesystem::path& SourcePath) const
{
	std::ofstream File(GetSidecarPath(SourcePath), std::ios::binary | std::ios::trunc);
	if (!File)
	{
		return false;
	}
	File << ToJsonString();
	return true;
}

bool FModelImportSettings::IsDefault() const
{
	return ToJsonString() == FModelImportSettings{}.ToJsonString();
}

void FModelImportSettings::Apply(FModelData& Model) const
{
	if (!bImportMaterials)
	{
		Model.Materials.clear();
		Model.Images.clear();
		for (FModelMesh& Mesh : Model.Meshes)
		{
			Mesh.Material = -1;
		}
	}
	if (!bImportSkin)
	{
		Model.Skins.clear();
		for (FModelMesh& Mesh : Model.Meshes)
		{
			Mesh.SkinVertices.clear();
		}
		for (FModelNode& Node : Model.Nodes)
		{
			Node.Skin = -1;
		}
	}
	if (!bImportAnimations)
	{
		Model.Animations.clear();
	}
	if (bBlendAsMasked)
	{
		for (FModelMaterial& Material : Model.Materials)
		{
			if (Material.BlendMode == EMaterialBlendMode::Translucent)
			{
				Material.BlendMode = EMaterialBlendMode::Masked; // 컷오프는 glTF alphaCutoff(기본 0.5)
			}
		}
	}
	size_t StaticTriangles = 0;
	for (const FModelMesh& Mesh : Model.Meshes)
	{
		StaticTriangles += Mesh.SkinVertices.empty() ? Mesh.Data.Indices.size() / 3 : 0;
	}
	for (FModelMesh& Mesh : Model.Meshes)
	{
		if (bRecomputeNormals)
		{
			GenerateSmoothNormals(Mesh.Data);
		}
		if (bRecomputeNormals || bRecomputeTangents)
		{
			Mesh.Data.ComputeTangents();
		}
		// 잎 메시 = 마스크 머티리얼 + 떨어진 작은 조각이 많은 메시 → 상한·LOD 모두 솎아내기 (QEM은 조각 사이를 줄이지 못한다)
		const bool bStatic  = Mesh.SkinVertices.empty();
		const bool bMasked  = Mesh.Material >= 0 && static_cast<size_t>(Mesh.Material) < Model.Materials.size() &&
		                      Model.Materials[static_cast<size_t>(Mesh.Material)].BlendMode == EMaterialBlendMode::Masked;
		const bool bIslands = bStatic && bMasked && (MaxTriangles > 0 || bGenerateLods) && MeshSimplifier::IsIslandMesh(Mesh.Data);
		if (bStatic && MaxTriangles > 0 && StaticTriangles > MaxTriangles)
		{
			const double Share  = static_cast<double>(Mesh.Data.Indices.size() / 3) / static_cast<double>(StaticTriangles);
			const uint32 Target = std::max<uint32>(static_cast<uint32>(Share * MaxTriangles), MeshSimplifier::MinTriangles);
			if (bIslands)
			{
				MeshSimplifier::ThinBase(Mesh.Data, Target);
			}
			else
			{
				MeshSimplifier::SimplifyBase(Mesh.Data, Target);
			}
		}
		// LOD: 정적 메시만 (스킨 메시는 항상 LOD0으로 그린다)
		if (bGenerateLods && bStatic)
		{
			if (bIslands)
			{
				MeshSimplifier::GenerateIslandLods(Mesh.Data, LodCount);
			}
			else
			{
				MeshSimplifier::GenerateLods(Mesh.Data, LodCount);
			}
		}
		else
		{
			Mesh.Data.Lods.clear();
		}
	}

	// 크기/방향: 기존 루트들을 새 루트 아래로 (루트 노드에 애니메이션이 있어도 덮어쓰지 않는다)
	if (!FMath::IsNearlyEqual(Scale, 1.0f) || !FMath::IsNearlyZero(YawDegrees))
	{
		FModelNode Root;
		Root.Name     = "ImportRoot";
		Root.Rotation = FQuat::FromAxisAngle(FVector3::UpVector, FMath::DegreesToRadians(YawDegrees));
		Root.Scale    = FVector3(Scale);
		const int32 RootIndex = static_cast<int32>(Model.Nodes.size());
		for (const int32 OldRoot : Model.RootNodes)
		{
			Model.Nodes[static_cast<size_t>(OldRoot)].Parent = RootIndex;
			Root.Children.push_back(OldRoot);
		}
		Model.Nodes.push_back(std::move(Root));
		Model.RootNodes = { RootIndex };
	}
}

uint32 FModelImportSettings::MergeAnimations(FModelData& Target, const FModelData& Source, const std::string& ClipPrefix)
{
	std::unordered_map<std::string, int32> TargetNodes;
	for (size_t Index = 0; Index < Target.Nodes.size(); ++Index)
	{
		TargetNodes.emplace(Target.Nodes[Index].Name, static_cast<int32>(Index));
	}
	uint32 Merged = 0;
	for (const FAnimationClip& Clip : Source.Animations)
	{
		FAnimationClip Copy;
		Copy.Name     = ClipPrefix.empty() ? Clip.Name : ClipPrefix + (Source.Animations.size() > 1 ? "_" + Clip.Name : std::string());
		Copy.Duration = Clip.Duration;
		for (const FAnimationChannel& Channel : Clip.Channels)
		{
			if (Channel.Node < 0 || Channel.Node >= static_cast<int32>(Source.Nodes.size()))
			{
				continue;
			}
			const auto Found = TargetNodes.find(Source.Nodes[static_cast<size_t>(Channel.Node)].Name);
			if (Found == TargetNodes.end())
			{
				continue; // 이 모델에 없는 뼈 (이름이 다름)
			}
			FAnimationChannel Mapped = Channel;
			Mapped.Node              = Found->second;
			Copy.Channels.push_back(std::move(Mapped));
		}
		if (!Copy.Channels.empty())
		{
			Target.Animations.push_back(std::move(Copy));
			++Merged;
		}
	}
	return Merged;
}
