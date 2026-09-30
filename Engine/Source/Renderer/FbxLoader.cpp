#include "Renderer/FbxLoader.h"

#include "Core/Log.h"
#include "Core/StringConv.h"

#pragma warning(push, 0)
#include <ufbx.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	std::string ToString(const ufbx_string& String, const char* Fallback, size_t Index)
	{
		return String.length > 0 ? std::string(String.data, String.length) : std::format("{}_{}", Fallback, Index);
	}

	FVector3 ToVector(const ufbx_vec3& V) { return FVector3(static_cast<float>(V.x), static_cast<float>(V.y), static_cast<float>(V.z)); }

	// ufbx 3x4 열 행렬(열 = X/Y/Z 축, 이동) → 행벡터 규약 FMatrix4x4 (행 = 축). glTF 열우선 배열을 행으로 읽은 것과 같은 의미
	FMatrix4x4 ToMatrix(const ufbx_matrix& M)
	{
		FMatrix4x4 Result;
		for (int32 Column = 0; Column < 4; ++Column)
		{
			Result.M[Column][0] = static_cast<float>(M.cols[Column].x);
			Result.M[Column][1] = static_cast<float>(M.cols[Column].y);
			Result.M[Column][2] = static_cast<float>(M.cols[Column].z);
			Result.M[Column][3] = Column == 3 ? 1.0f : 0.0f;
		}
		return Result;
	}

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

	// 정점 합치기 키: 제어점 + 법선 + UV + 색 (같으면 같은 정점)
	struct FVertexKey
	{
		uint32                Point = 0;
		std::array<float, 9>  Values{};
		bool operator==(const FVertexKey& Other) const { return Point == Other.Point && Values == Other.Values; }
	};
	struct FVertexKeyHash
	{
		size_t operator()(const FVertexKey& Key) const
		{
			size_t Hash = Key.Point * 0x9E3779B97F4A7C15ull;
			for (const float Value : Key.Values)
			{
				uint32 Bits = 0;
				std::memcpy(&Bits, &Value, sizeof(Bits));
				Hash ^= Bits + 0x9E3779B97F4A7C15ull + (Hash << 6) + (Hash >> 2);
			}
			return Hash;
		}
	};

	struct FLoadContext
	{
		const ufbx_scene*                          Scene = nullptr;
		std::filesystem::path                      BaseDirectory;
		FModelData*                                Model = nullptr;
		std::unordered_map<const ufbx_texture*, int32> ImageIndices;
		std::unordered_map<const ufbx_node*, int32>    NodeIndices;
	};

	int32 GetImageIndex(FLoadContext& Context, const ufbx_texture* Texture)
	{
		if (Texture == nullptr)
		{
			return -1;
		}
		if (const auto Found = Context.ImageIndices.find(Texture); Found != Context.ImageIndices.end())
		{
			return Found->second;
		}
		FModelImage Image;
		Image.Name = ToString(Texture->name, "Texture", Context.Model->Images.size());
		bool bLoaded = false;
		if (Texture->content.size > 0)
		{
			bLoaded = FImageLoader::LoadFromMemory(static_cast<const uint8*>(Texture->content.data), Texture->content.size, Image.Image, Image.Name.c_str());
		}
		// 외부 파일: FBX 기준 상대 경로 → 절대 경로 → 파일 이름만 (FBX 옆)
		const ufbx_string Candidates[] = { Texture->relative_filename, Texture->absolute_filename, Texture->filename };
		for (const ufbx_string& Candidate : Candidates)
		{
			if (bLoaded || Candidate.length == 0)
			{
				continue;
			}
			const std::filesystem::path Path = FStringConv::ToWide(std::string(Candidate.data, Candidate.length));
			for (const std::filesystem::path& Try : { Path.is_absolute() ? Path : Context.BaseDirectory / Path, Context.BaseDirectory / Path.filename() })
			{
				std::error_code ErrorCode;
				if (!bLoaded && std::filesystem::exists(Try, ErrorCode))
				{
					bLoaded = FImageLoader::LoadFromFile(Try, Image.Image);
				}
			}
		}
		if (!bLoaded)
		{
			E_LOG(LogRenderer, Warning, "FBX 텍스처를 찾지 못했습니다: {}", Image.Name);
			Context.ImageIndices[Texture] = -1;
			return -1;
		}
		const int32 Index             = static_cast<int32>(Context.Model->Images.size());
		Context.ImageIndices[Texture] = Index;
		Context.Model->Images.push_back(std::move(Image));
		return Index;
	}

	void LoadMaterials(FLoadContext& Context)
	{
		const ufbx_scene& Scene = *Context.Scene;
		for (size_t Index = 0; Index < Scene.materials.count; ++Index)
		{
			const ufbx_material& Source = *Scene.materials.data[Index];
			FModelMaterial       Material;
			Material.Name = ToString(Source.name, "Material", Index);

			const ufbx_material_pbr_maps& Pbr = Source.pbr;
			const float BaseFactor = Pbr.base_factor.has_value ? static_cast<float>(Pbr.base_factor.value_real) : 1.0f;
			if (Pbr.base_color.has_value)
			{
				const ufbx_vec4& Color   = Pbr.base_color.value_vec4;
				Material.BaseColorFactor = FVector4(static_cast<float>(Color.x) * BaseFactor, static_cast<float>(Color.y) * BaseFactor,
				                                    static_cast<float>(Color.z) * BaseFactor, 1.0f);
			}
			Material.BaseColorImage = GetImageIndex(Context, Pbr.base_color.texture);
			if (Material.BaseColorImage >= 0)
			{
				Material.BaseColorFactor = FVector4(BaseFactor, BaseFactor, BaseFactor, 1.0f); // 텍스처가 있으면 색은 텍스처 그대로
			}
			Material.MetallicFactor  = Pbr.metalness.has_value ? static_cast<float>(Pbr.metalness.value_real) : 0.0f;
			Material.RoughnessFactor = Pbr.roughness.has_value ? static_cast<float>(Pbr.roughness.value_real) : 0.8f;
			Material.NormalImage     = GetImageIndex(Context, Pbr.normal_map.texture);
			Material.OcclusionImage  = GetImageIndex(Context, Pbr.ambient_occlusion.texture);
			Material.EmissiveImage   = GetImageIndex(Context, Pbr.emission_color.texture);
			if (Pbr.emission_color.has_value)
			{
				const float EmissionFactor = Pbr.emission_factor.has_value ? static_cast<float>(Pbr.emission_factor.value_real) : 1.0f;
				Material.EmissiveFactor    = ToVector(Pbr.emission_color.value_vec3) * EmissionFactor;
			}
			if (Material.EmissiveImage >= 0 && Material.EmissiveFactor.LengthSquared() <= 0.0f)
			{
				Material.EmissiveFactor = FVector3::OneVector;
			}
			Context.Model->Materials.push_back(Material);
		}
	}

	// 노드 메시 하나 → 머티리얼 부분마다 FModelMesh
	std::vector<int32> LoadMesh(FLoadContext& Context, const ufbx_node& Node, const ufbx_mesh& Mesh, int32 SkinIndex)
	{
		std::vector<int32>          Result;
		const ufbx_skin_deformer*   Skin = Mesh.skin_deformers.count > 0 ? Mesh.skin_deformers.data[0] : nullptr;
		std::vector<uint32>         Triangle(Mesh.max_face_triangles * 3);

		// 머티리얼 부분이 없으면 전체를 한 부분으로
		std::vector<std::pair<uint32, std::vector<uint32>>> Parts;
		if (Mesh.material_parts.count > 0)
		{
			for (size_t PartIndex = 0; PartIndex < Mesh.material_parts.count; ++PartIndex)
			{
				const ufbx_mesh_part& Part = Mesh.material_parts.data[PartIndex];
				if (Part.num_triangles == 0)
				{
					continue;
				}
				Parts.emplace_back(Part.index, std::vector<uint32>(Part.face_indices.data, Part.face_indices.data + Part.face_indices.count));
			}
		}
		else
		{
			std::vector<uint32> All(Mesh.num_faces);
			for (uint32 Face = 0; Face < Mesh.num_faces; ++Face)
			{
				All[Face] = Face;
			}
			Parts.emplace_back(UINT32_MAX, std::move(All));
		}

		for (const auto& [MaterialSlot, Faces] : Parts)
		{
			FModelMesh Out;
			Out.Name = ToString(Node.name, "Mesh", Context.Model->Meshes.size()) + std::format("_{}", Result.size());
			std::unordered_map<FVertexKey, uint32, FVertexKeyHash> Unique;

			for (const uint32 FaceIndex : Faces)
			{
				const ufbx_face Face     = Mesh.faces.data[FaceIndex];
				const uint32    TriCount = ufbx_triangulate_face(Triangle.data(), Triangle.size(), &Mesh, Face);
				for (uint32 Corner = 0; Corner < TriCount * 3; ++Corner)
				{
					const uint32 Index = Triangle[Corner];
					FVertexKey   Key;
					Key.Point            = Mesh.vertex_indices.data[Index];
					const ufbx_vec3 N    = Mesh.vertex_normal.exists ? ufbx_get_vertex_vec3(&Mesh.vertex_normal, Index) : ufbx_vec3{};
					const ufbx_vec2 UV   = Mesh.vertex_uv.exists ? ufbx_get_vertex_vec2(&Mesh.vertex_uv, Index) : ufbx_vec2{};
					const ufbx_vec4 C    = Mesh.vertex_color.exists ? ufbx_get_vertex_vec4(&Mesh.vertex_color, Index) : ufbx_vec4{ { 1, 1, 1, 1 } };
					Key.Values           = { static_cast<float>(N.x), static_cast<float>(N.y), static_cast<float>(N.z), static_cast<float>(UV.x),
					                         static_cast<float>(UV.y), static_cast<float>(C.x), static_cast<float>(C.y), static_cast<float>(C.z),
					                         static_cast<float>(C.w) };

					auto [It, bInserted] = Unique.try_emplace(Key, static_cast<uint32>(Out.Data.Vertices.size()));
					if (bInserted)
					{
						FVertex Vertex;
						Vertex.Position = FGltfLoader::ConvertPosition(ToVector(ufbx_get_vertex_vec3(&Mesh.vertex_position, Index))) * FGltfLoader::ImportScale;
						Vertex.Normal   = FGltfLoader::ConvertPosition(FVector3(Key.Values[0], Key.Values[1], Key.Values[2])).GetNormalized();
						Vertex.UV       = { Key.Values[3], 1.0f - Key.Values[4] }; // FBX UV 원점은 왼쪽 아래 → 엔진(왼쪽 위)
						Vertex.Color    = { Key.Values[5], Key.Values[6], Key.Values[7], Key.Values[8] };
						Out.Data.Vertices.push_back(Vertex);

						if (Skin != nullptr)
						{
							// 영향이 큰 조인트 4개, 합이 1이 되도록
							std::vector<std::pair<float, uint32>> Influences;
							if (Key.Point < Skin->vertices.count)
							{
								const ufbx_skin_vertex& SkinVertex = Skin->vertices.data[Key.Point];
								for (uint32 Weight = 0; Weight < SkinVertex.num_weights; ++Weight)
								{
									const ufbx_skin_weight& W = Skin->weights.data[SkinVertex.weight_begin + Weight];
									Influences.emplace_back(static_cast<float>(W.weight), W.cluster_index);
								}
							}
							std::sort(Influences.begin(), Influences.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
							FSkinVertex SkinOut;
							float       Sum = 0.0f;
							float       Weights[4] = {};
							for (size_t Slot = 0; Slot < 4 && Slot < Influences.size(); ++Slot)
							{
								SkinOut.Joints[Slot] = static_cast<uint16>(FMath::Min<uint32>(Influences[Slot].second, MaxSkinJoints - 1));
								Weights[Slot]        = Influences[Slot].first;
								Sum += Weights[Slot];
							}
							SkinOut.Weights = Sum > FMath::SmallNumber ? FVector4(Weights[0], Weights[1], Weights[2], Weights[3]) / Sum : FVector4(1.0f, 0.0f, 0.0f, 0.0f);
							Out.SkinVertices.push_back(SkinOut);
						}
					}
					Out.Data.Indices.push_back(It->second);
				}
			}
			if (Out.Data.Indices.empty())
			{
				continue;
			}

			// 와인딩: glTF와 같은 축으로 받았으므로 glTF 로더와 같이 CCW → 엔진 CW
			for (size_t Index = 0; Index + 2 < Out.Data.Indices.size(); Index += 3)
			{
				std::swap(Out.Data.Indices[Index + 1], Out.Data.Indices[Index + 2]);
			}
			if (!Mesh.vertex_normal.exists)
			{
				GenerateSmoothNormals(Out.Data);
			}
			Out.Data.ComputeTangents();

			// 머티리얼: 노드 인스턴스 머티리얼 우선, 없으면 메시 머티리얼
			const ufbx_material* Material = nullptr;
			if (MaterialSlot != UINT32_MAX)
			{
				Material = MaterialSlot < Node.materials.count ? Node.materials.data[MaterialSlot]
				                                                : (MaterialSlot < Mesh.materials.count ? Mesh.materials.data[MaterialSlot] : nullptr);
			}
			Out.Material = Material != nullptr ? static_cast<int32>(Material->typed_id) : -1;
			(void)SkinIndex;

			Result.push_back(static_cast<int32>(Context.Model->Meshes.size()));
			Context.Model->Meshes.push_back(std::move(Out));
		}
		return Result;
	}

	void LoadNodesAndMeshes(FLoadContext& Context)
	{
		const ufbx_scene& Scene = *Context.Scene;
		FModelData&       Model = *Context.Model;

		// 노드 (장면 루트는 제외 — 그 자식들이 모델 루트)
		for (size_t Index = 0; Index < Scene.nodes.count; ++Index)
		{
			const ufbx_node* Node = Scene.nodes.data[Index];
			if (!Node->is_root)
			{
				Context.NodeIndices[Node] = static_cast<int32>(Model.Nodes.size());
				Model.Nodes.emplace_back();
			}
		}
		for (size_t Index = 0; Index < Scene.nodes.count; ++Index)
		{
			const ufbx_node* Source = Scene.nodes.data[Index];
			if (Source->is_root)
			{
				continue;
			}
			FModelNode& Node = Model.Nodes[static_cast<size_t>(Context.NodeIndices[Source])];
			Node.Name        = ToString(Source->name, "Node", Index);
			Node.Parent      = (Source->parent != nullptr && !Source->parent->is_root) ? Context.NodeIndices[Source->parent] : -1;
			for (size_t Child = 0; Child < Source->children.count; ++Child)
			{
				Node.Children.push_back(Context.NodeIndices[Source->children.data[Child]]);
			}
			const ufbx_transform& Local = Source->local_transform;
			Node.Translation = FGltfLoader::ConvertPosition(ToVector(Local.translation)) * FGltfLoader::ImportScale;
			Node.Rotation    = FGltfLoader::ConvertRotation(FQuat(static_cast<float>(Local.rotation.x), static_cast<float>(Local.rotation.y),
			                                                      static_cast<float>(Local.rotation.z), static_cast<float>(Local.rotation.w)));
			Node.Scale       = FGltfLoader::ConvertScale(ToVector(Local.scale));
			if (Node.Parent < 0)
			{
				Model.RootNodes.push_back(Context.NodeIndices[Source]);
			}
		}

		// 메시 + 스킨
		for (size_t Index = 0; Index < Scene.nodes.count; ++Index)
		{
			const ufbx_node* Source = Scene.nodes.data[Index];
			if (Source->is_root || Source->mesh == nullptr)
			{
				continue;
			}
			const ufbx_mesh& Mesh      = *Source->mesh;
			int32            SkinIndex = -1;
			if (Mesh.skin_deformers.count > 0)
			{
				const ufbx_skin_deformer& Deformer = *Mesh.skin_deformers.data[0];
				FModelSkin                Skin;
				Skin.Name                = ToString(Deformer.name, "Skin", Model.Skins.size());
				const size_t JointCount  = FMath::Min<size_t>(Deformer.clusters.count, MaxSkinJoints);
				if (Deformer.clusters.count > MaxSkinJoints)
				{
					E_LOG(LogRenderer, Warning, "FBX 스킨 '{}' 조인트 {}개 중 {}개만 사용합니다", Skin.Name, Deformer.clusters.count, MaxSkinJoints);
				}
				for (size_t Cluster = 0; Cluster < JointCount; ++Cluster)
				{
					const ufbx_skin_cluster& Source0 = *Deformer.clusters.data[Cluster];
					const auto               Found   = Source0.bone_node ? Context.NodeIndices.find(Source0.bone_node) : Context.NodeIndices.end();
					Skin.Joints.push_back(Found != Context.NodeIndices.end() ? Found->second : 0);
					// 역바인드 = geometry_to_bone. 축 변환 후 이동부만 cm로 (glTF 로더와 같음)
					FMatrix4x4 Matrix = FGltfLoader::ConvertMatrix(ToMatrix(Source0.geometry_to_bone));
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						Matrix.M[3][Axis] *= FGltfLoader::ImportScale;
					}
					Skin.InverseBindMatrices.push_back(Matrix);
				}
				SkinIndex = static_cast<int32>(Model.Skins.size());
				Model.Skins.push_back(std::move(Skin));
			}
			FModelNode& Node = Model.Nodes[static_cast<size_t>(Context.NodeIndices[Source])];
			Node.Meshes      = LoadMesh(Context, *Source, Mesh, SkinIndex);
			Node.Skin        = SkinIndex;
		}
	}

	void LoadAnimations(FLoadContext& Context)
	{
		const ufbx_scene& Scene = *Context.Scene;
		for (size_t StackIndex = 0; StackIndex < Scene.anim_stacks.count; ++StackIndex)
		{
			const ufbx_anim_stack& Stack = *Scene.anim_stacks.data[StackIndex];
			ufbx_bake_opts         Options{};
			ufbx_error             Error{};
			ufbx_baked_anim*       Baked = ufbx_bake_anim(&Scene, Stack.anim, &Options, &Error);
			if (Baked == nullptr)
			{
				E_LOG(LogRenderer, Warning, "FBX 애니메이션 굽기 실패: {}", std::string(Error.description.data, Error.description.length));
				continue;
			}
			FAnimationClip Clip;
			Clip.Name         = ToString(Stack.name, "Animation", StackIndex);
			const double Base = Baked->playback_time_begin;
			for (size_t NodeIndex = 0; NodeIndex < Baked->nodes.count; ++NodeIndex)
			{
				const ufbx_baked_node& BakedNode = Baked->nodes.data[NodeIndex];
				const ufbx_node*       Target    = BakedNode.typed_id < Scene.nodes.count ? Scene.nodes.data[BakedNode.typed_id] : nullptr;
				const auto             Found     = Target ? Context.NodeIndices.find(Target) : Context.NodeIndices.end();
				if (Found == Context.NodeIndices.end())
				{
					continue;
				}
				const auto AddChannel = [&](EAnimationPath Path, size_t Count, auto&& GetTime, auto&& GetValue) {
					if (Count == 0)
					{
						return;
					}
					FAnimationChannel Channel;
					Channel.Node = Found->second;
					Channel.Path = Path;
					for (size_t Key = 0; Key < Count; ++Key)
					{
						Channel.Times.push_back(static_cast<float>(FMath::Max(0.0, GetTime(Key) - Base)));
						Channel.Values.push_back(GetValue(Key));
					}
					Clip.Duration = FMath::Max(Clip.Duration, Channel.Times.back());
					Clip.Channels.push_back(std::move(Channel));
				};
				AddChannel(
					EAnimationPath::Translation, BakedNode.translation_keys.count, [&](size_t Key) { return BakedNode.translation_keys.data[Key].time; },
					[&](size_t Key) { return FVector4(FGltfLoader::ConvertPosition(ToVector(BakedNode.translation_keys.data[Key].value)) * FGltfLoader::ImportScale, 0.0f); });
				AddChannel(
					EAnimationPath::Rotation, BakedNode.rotation_keys.count, [&](size_t Key) { return BakedNode.rotation_keys.data[Key].time; },
					[&](size_t Key) {
						const ufbx_quat& Q = BakedNode.rotation_keys.data[Key].value;
						const FQuat      R = FGltfLoader::ConvertRotation(FQuat(static_cast<float>(Q.x), static_cast<float>(Q.y), static_cast<float>(Q.z), static_cast<float>(Q.w)));
						return FVector4(R.X, R.Y, R.Z, R.W);
					});
				AddChannel(
					EAnimationPath::Scale, BakedNode.scale_keys.count, [&](size_t Key) { return BakedNode.scale_keys.data[Key].time; },
					[&](size_t Key) { return FVector4(FGltfLoader::ConvertScale(ToVector(BakedNode.scale_keys.data[Key].value)), 0.0f); });
			}
			ufbx_free_baked_anim(Baked);
			if (!Clip.Channels.empty())
			{
				Context.Model->Animations.push_back(std::move(Clip));
			}
		}
	}
} // namespace

bool FFbxLoader::Load(const std::filesystem::path& Path, FModelData& OutModel)
{
	OutModel      = FModelData{};
	OutModel.Name = FStringConv::ToUtf8(Path.stem().wstring());

	ufbx_load_opts Options{};
	Options.target_axes                 = ufbx_axes_right_handed_y_up; // glTF와 같은 축
	Options.target_unit_meters          = 1.0f;
	Options.space_conversion            = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
	Options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
	Options.generate_missing_normals    = true;
	Options.load_external_files         = false;

	const std::string PathUtf8 = FStringConv::ToUtf8(Path.wstring());
	ufbx_error        Error{};
	ufbx_scene*       Scene = ufbx_load_file(PathUtf8.c_str(), &Options, &Error);
	if (Scene == nullptr)
	{
		E_LOG(LogRenderer, Error, "FBX 로드 실패: {} ({})", PathUtf8, std::string(Error.description.data, Error.description.length));
		return false;
	}

	FLoadContext Context;
	Context.Scene         = Scene;
	Context.BaseDirectory = Path.parent_path();
	Context.Model         = &OutModel;
	LoadMaterials(Context);
	LoadNodesAndMeshes(Context);
	LoadAnimations(Context);
	ufbx_free_scene(Scene);

	size_t Vertices = 0;
	size_t Indices  = 0;
	for (const FModelMesh& Mesh : OutModel.Meshes)
	{
		Vertices += Mesh.Data.Vertices.size();
		Indices += Mesh.Data.Indices.size();
	}
	E_LOG(LogRenderer, Display, "FBX 로드: {} (노드 {}, 메시 {}, 머티리얼 {}, 이미지 {}, 정점 {}, 삼각형 {}, 스킨 {}, 애니메이션 {})", OutModel.Name,
	      OutModel.Nodes.size(), OutModel.Meshes.size(), OutModel.Materials.size(), OutModel.Images.size(), Vertices, Indices / 3,
	      OutModel.Skins.size(), OutModel.Animations.size());
	return true;
}
