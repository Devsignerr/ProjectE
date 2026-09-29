#include "Renderer/GltfLoader.h"

#include "Core/CoreMinimal.h"
#include "Core/StringConv.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#pragma warning(push, 0)
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#pragma warning(pop)

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// glTF → 엔진 축 변환 행렬 C (행벡터: v_e = v_gl * C). 직교 행렬이며 det = -1 (반사).
	const FMatrix4x4 GAxisConversion(FVector4(0.0f, 1.0f, 0.0f, 0.0f),  // glTF X(오른쪽) → 엔진 +Y
	                                 FVector4(0.0f, 0.0f, 1.0f, 0.0f),  // glTF Y(위)     → 엔진 +Z
	                                 FVector4(-1.0f, 0.0f, 0.0f, 0.0f), // glTF Z(뒤)     → 엔진 -X
	                                 FVector4(0.0f, 0.0f, 0.0f, 1.0f));

	// cgltf 파일 콜백: UTF-8 경로를 유니코드로 열어 한글 경로 지원
	cgltf_result ReadFile(const cgltf_memory_options* MemoryOptions, const cgltf_file_options* /*FileOptions*/,
	                      const char* Path, cgltf_size* Size, void** Data)
	{
		FILE* File = nullptr;
		if (_wfopen_s(&File, FStringConv::ToWide(Path).c_str(), L"rb") != 0 || File == nullptr)
		{
			return cgltf_result_file_not_found;
		}

		std::fseek(File, 0, SEEK_END);
		const long FileSize = std::ftell(File);
		std::fseek(File, 0, SEEK_SET);
		if (FileSize < 0)
		{
			std::fclose(File);
			return cgltf_result_io_error;
		}

		void* (*Alloc)(void*, cgltf_size) = MemoryOptions->alloc_func ? MemoryOptions->alloc_func : &cgltf_default_alloc;
		void* Buffer = Alloc(MemoryOptions->user_data, static_cast<cgltf_size>(FileSize));
		if (Buffer == nullptr)
		{
			std::fclose(File);
			return cgltf_result_out_of_memory;
		}

		const size_t Read = std::fread(Buffer, 1, static_cast<size_t>(FileSize), File);
		std::fclose(File);
		if (Read != static_cast<size_t>(FileSize))
		{
			void (*Free)(void*, void*) = MemoryOptions->free_func ? MemoryOptions->free_func : &cgltf_default_free;
			Free(MemoryOptions->user_data, Buffer);
			return cgltf_result_io_error;
		}

		*Size = static_cast<cgltf_size>(FileSize);
		*Data = Buffer;
		return cgltf_result_success;
	}

	void ReleaseFile(const cgltf_memory_options* MemoryOptions, const cgltf_file_options* /*FileOptions*/, void* Data, cgltf_size /*Size*/)
	{
		void (*Free)(void*, void*) = MemoryOptions->free_func ? MemoryOptions->free_func : &cgltf_default_free;
		Free(MemoryOptions->user_data, Data);
	}

	const char* ResultToString(cgltf_result Result)
	{
		switch (Result)
		{
		case cgltf_result_success:         return "성공";
		case cgltf_result_data_too_short:  return "데이터가 너무 짧음";
		case cgltf_result_unknown_format:  return "알 수 없는 포맷";
		case cgltf_result_invalid_json:    return "잘못된 JSON";
		case cgltf_result_invalid_gltf:    return "잘못된 glTF";
		case cgltf_result_invalid_options: return "잘못된 옵션";
		case cgltf_result_file_not_found:  return "파일 없음";
		case cgltf_result_io_error:        return "입출력 오류";
		case cgltf_result_out_of_memory:   return "메모리 부족";
		case cgltf_result_legacy_gltf:     return "구버전 glTF (1.0)";
		default:                           return "알 수 없는 오류";
		}
	}

	std::string SafeName(const char* Name, const char* Fallback, size_t Index)
	{
		return (Name != nullptr && Name[0] != '\0') ? std::string(Name) : std::format("{}_{}", Fallback, Index);
	}

	const cgltf_accessor* FindAttribute(const cgltf_primitive& Primitive, cgltf_attribute_type Type, int32 SetIndex = 0)
	{
		for (cgltf_size Index = 0; Index < Primitive.attributes_count; ++Index)
		{
			const cgltf_attribute& Attribute = Primitive.attributes[Index];
			if (Attribute.type == Type && Attribute.index == SetIndex)
			{
				return Attribute.data;
			}
		}
		return nullptr;
	}

	// 접근자의 모든 요소를 float 벡터로 (요소당 ComponentCount개, 부족하면 0 채움)
	std::vector<float> UnpackFloats(const cgltf_accessor* Accessor, cgltf_size ComponentCount)
	{
		std::vector<float> Result(Accessor->count * ComponentCount, 0.0f);
		const cgltf_size   SourceComponents = cgltf_num_components(Accessor->type);
		if (SourceComponents == ComponentCount)
		{
			cgltf_accessor_unpack_floats(Accessor, Result.data(), Result.size());
		}
		else
		{
			std::vector<float> Temp(SourceComponents, 0.0f);
			for (cgltf_size Index = 0; Index < Accessor->count; ++Index)
			{
				cgltf_accessor_read_float(Accessor, Index, Temp.data(), SourceComponents);
				const cgltf_size Copy = FMath::Min(SourceComponents, ComponentCount);
				std::memcpy(&Result[Index * ComponentCount], Temp.data(), Copy * sizeof(float));
			}
		}
		return Result;
	}

	// 법선이 없을 때: 면 법선을 정점에 누적해 부드러운 법선 생성
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
			// 엔진 규약(CW 앞면): Cross(E1, E2)가 바깥 방향
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

	bool LoadPrimitive(const cgltf_data& Data, const cgltf_mesh& GltfMesh, const cgltf_primitive& Primitive, size_t PrimitiveIndex,
	                   FModelMesh& OutMesh)
	{
		if (Primitive.type != cgltf_primitive_type_triangles)
		{
			E_LOG(LogRenderer, Warning, "삼각형 리스트가 아닌 primitive는 건너뜁니다: {}", SafeName(GltfMesh.name, "Mesh", 0));
			return false;
		}

		const cgltf_accessor* PositionAccessor = FindAttribute(Primitive, cgltf_attribute_type_position);
		if (PositionAccessor == nullptr)
		{
			E_LOG(LogRenderer, Warning, "POSITION 속성이 없는 primitive는 건너뜁니다: {}", SafeName(GltfMesh.name, "Mesh", 0));
			return false;
		}
		const cgltf_accessor* NormalAccessor   = FindAttribute(Primitive, cgltf_attribute_type_normal);
		const cgltf_accessor* TexCoordAccessor = FindAttribute(Primitive, cgltf_attribute_type_texcoord, 0);
		const cgltf_accessor* ColorAccessor    = FindAttribute(Primitive, cgltf_attribute_type_color, 0);
		const cgltf_accessor* TangentAccessor  = FindAttribute(Primitive, cgltf_attribute_type_tangent);

		const cgltf_size         VertexCount = PositionAccessor->count;
		const std::vector<float> Positions   = UnpackFloats(PositionAccessor, 3);
		const std::vector<float> Normals     = NormalAccessor ? UnpackFloats(NormalAccessor, 3) : std::vector<float>();
		const std::vector<float> TexCoords   = TexCoordAccessor ? UnpackFloats(TexCoordAccessor, 2) : std::vector<float>();
		const std::vector<float> Tangents    = TangentAccessor ? UnpackFloats(TangentAccessor, 4) : std::vector<float>();
		std::vector<float>       Colors;
		if (ColorAccessor)
		{
			Colors = UnpackFloats(ColorAccessor, 4);
			if (cgltf_num_components(ColorAccessor->type) == 3)
			{
				for (cgltf_size Index = 0; Index < VertexCount; ++Index)
				{
					Colors[Index * 4 + 3] = 1.0f;
				}
			}
		}

		OutMesh.Name = SafeName(GltfMesh.name, "Mesh", 0) + std::format("_{}", PrimitiveIndex);
		OutMesh.Data.Vertices.resize(VertexCount);
		for (cgltf_size Index = 0; Index < VertexCount; ++Index)
		{
			FVertex& Vertex = OutMesh.Data.Vertices[Index];
			Vertex.Position = FGltfLoader::ConvertPosition({ Positions[Index * 3], Positions[Index * 3 + 1], Positions[Index * 3 + 2] });
			if (NormalAccessor)
			{
				Vertex.Normal = FGltfLoader::ConvertPosition({ Normals[Index * 3], Normals[Index * 3 + 1], Normals[Index * 3 + 2] }).GetNormalized();
			}
			if (TexCoordAccessor)
			{
				Vertex.UV = { TexCoords[Index * 2], TexCoords[Index * 2 + 1] }; // glTF UV 원점도 왼쪽 위
			}
			if (ColorAccessor)
			{
				Vertex.Color = { Colors[Index * 4], Colors[Index * 4 + 1], Colors[Index * 4 + 2], Colors[Index * 4 + 3] };
			}
			if (TangentAccessor)
			{
				Vertex.Tangent = FGltfLoader::ConvertTangent({ Tangents[Index * 4], Tangents[Index * 4 + 1], Tangents[Index * 4 + 2], Tangents[Index * 4 + 3] });
			}
		}

		if (Primitive.indices != nullptr)
		{
			OutMesh.Data.Indices.resize(Primitive.indices->count);
			for (cgltf_size Index = 0; Index < Primitive.indices->count; ++Index)
			{
				OutMesh.Data.Indices[Index] = static_cast<uint32>(cgltf_accessor_read_index(Primitive.indices, Index));
			}
		}
		else
		{
			OutMesh.Data.Indices.resize(VertexCount);
			for (cgltf_size Index = 0; Index < VertexCount; ++Index)
			{
				OutMesh.Data.Indices[Index] = static_cast<uint32>(Index);
			}
		}

		// 와인딩 반전: glTF는 CCW 앞면. 축 변환(반사)과 엔진 카메라 규약(오른쪽=+Y, 역시 반사)이 상쇄되어
		// 화면상 방향이 그대로 유지되므로, 엔진의 CW 앞면 규약에 맞추려면 삼각형마다 뒤 두 인덱스를 교환한다.
		for (size_t Index = 0; Index + 2 < OutMesh.Data.Indices.size(); Index += 3)
		{
			std::swap(OutMesh.Data.Indices[Index + 1], OutMesh.Data.Indices[Index + 2]);
		}

		if (NormalAccessor == nullptr)
		{
			GenerateSmoothNormals(OutMesh.Data);
		}
		// 탄젠트가 없으면 UV로 계산 (UV도 없으면 법선에 수직인 임의 방향). 계산식은 위치/UV만 쓰므로 와인딩 교환과 무관
		if (TangentAccessor == nullptr)
		{
			OutMesh.Data.ComputeTangents();
		}

		OutMesh.Material = Primitive.material ? static_cast<int32>(Primitive.material - Data.materials) : -1;
		return true;
	}

	void LoadImages(const cgltf_data& Data, const std::filesystem::path& BaseDirectory, FModelData& OutModel)
	{
		OutModel.Images.resize(Data.images_count);
		for (cgltf_size Index = 0; Index < Data.images_count; ++Index)
		{
			const cgltf_image& GltfImage = Data.images[Index];
			FModelImage&       ModelImage = OutModel.Images[Index];
			ModelImage.Name = SafeName(GltfImage.name, "Image", Index);

			if (GltfImage.buffer_view != nullptr)
			{
				const cgltf_buffer_view& View = *GltfImage.buffer_view;
				const uint8* Bytes = static_cast<const uint8*>(View.buffer->data) + View.offset;
				FImageLoader::LoadFromMemory(Bytes, View.size, ModelImage.Image, ModelImage.Name.c_str());
			}
			else if (GltfImage.uri != nullptr)
			{
				if (std::strncmp(GltfImage.uri, "data:", 5) == 0)
				{
					E_LOG(LogRenderer, Warning, "data URI 이미지는 아직 지원하지 않습니다: {}", ModelImage.Name);
					continue;
				}
				std::string DecodedUri(GltfImage.uri);
				cgltf_decode_uri(DecodedUri.data());
				DecodedUri.resize(std::strlen(DecodedUri.c_str()));
				FImageLoader::LoadFromFile(BaseDirectory / FStringConv::ToWide(DecodedUri), ModelImage.Image);
			}
		}
	}

	void LoadMaterials(const cgltf_data& Data, FModelData& OutModel)
	{
		OutModel.Materials.resize(Data.materials_count);
		for (cgltf_size Index = 0; Index < Data.materials_count; ++Index)
		{
			const cgltf_material& GltfMaterial = Data.materials[Index];
			FModelMaterial&       Material     = OutModel.Materials[Index];
			Material.Name = SafeName(GltfMaterial.name, "Material", Index);

			const auto ImageIndexOf = [&](const cgltf_texture_view& View) -> int32 {
				return (View.texture != nullptr && View.texture->image != nullptr) ? static_cast<int32>(View.texture->image - Data.images) : -1;
			};

			if (GltfMaterial.has_pbr_metallic_roughness)
			{
				const cgltf_pbr_metallic_roughness& Pbr = GltfMaterial.pbr_metallic_roughness;
				Material.BaseColorFactor        = { Pbr.base_color_factor[0], Pbr.base_color_factor[1], Pbr.base_color_factor[2], Pbr.base_color_factor[3] };
				Material.MetallicFactor         = Pbr.metallic_factor;
				Material.RoughnessFactor        = Pbr.roughness_factor;
				Material.BaseColorImage         = ImageIndexOf(Pbr.base_color_texture);
				Material.MetallicRoughnessImage = ImageIndexOf(Pbr.metallic_roughness_texture);
			}

			Material.NormalImage       = ImageIndexOf(GltfMaterial.normal_texture);
			Material.NormalScale       = GltfMaterial.normal_texture.texture ? GltfMaterial.normal_texture.scale : 1.0f;
			Material.OcclusionImage    = ImageIndexOf(GltfMaterial.occlusion_texture);
			Material.OcclusionStrength = GltfMaterial.occlusion_texture.texture ? GltfMaterial.occlusion_texture.scale : 1.0f;
			Material.EmissiveImage     = ImageIndexOf(GltfMaterial.emissive_texture);

			const float EmissiveStrength = GltfMaterial.has_emissive_strength ? GltfMaterial.emissive_strength.emissive_strength : 1.0f;
			Material.EmissiveFactor = FVector3(GltfMaterial.emissive_factor[0], GltfMaterial.emissive_factor[1], GltfMaterial.emissive_factor[2]) * EmissiveStrength;
		}
	}

	void LoadNodes(const cgltf_data& Data, const std::vector<std::vector<int32>>& MeshPrimitiveLists, FModelData& OutModel)
	{
		OutModel.Nodes.resize(Data.nodes_count);
		for (cgltf_size Index = 0; Index < Data.nodes_count; ++Index)
		{
			const cgltf_node& GltfNode = Data.nodes[Index];
			FModelNode&       Node     = OutModel.Nodes[Index];
			Node.Name   = SafeName(GltfNode.name, "Node", Index);
			Node.Parent = GltfNode.parent ? static_cast<int32>(GltfNode.parent - Data.nodes) : -1;
			for (cgltf_size Child = 0; Child < GltfNode.children_count; ++Child)
			{
				Node.Children.push_back(static_cast<int32>(GltfNode.children[Child] - Data.nodes));
			}

			if (GltfNode.has_matrix)
			{
				// 열우선 float[16]을 행우선으로 읽으면 행벡터 규약 행렬이 된다
				FMatrix4x4 Local;
				std::memcpy(Local.M, GltfNode.matrix, sizeof(float) * 16);
				Local = FGltfLoader::ConvertMatrix(Local);

				Node.Translation = Local.GetOrigin();
				Node.Scale       = { Local.GetAxisX().Length(), Local.GetAxisY().Length(), Local.GetAxisZ().Length() };
				FMatrix4x4 RotationOnly;
				for (int32 Row = 0; Row < 3; ++Row)
				{
					const float InvScale = Node.Scale[Row] > FMath::SmallNumber ? 1.0f / Node.Scale[Row] : 0.0f;
					for (int32 Col = 0; Col < 3; ++Col)
					{
						RotationOnly.M[Row][Col] = Local.M[Row][Col] * InvScale;
					}
				}
				Node.Rotation = FQuat::FromRotationMatrix(RotationOnly.M);
			}
			else
			{
				if (GltfNode.has_translation)
				{
					Node.Translation = FGltfLoader::ConvertPosition({ GltfNode.translation[0], GltfNode.translation[1], GltfNode.translation[2] });
				}
				if (GltfNode.has_rotation)
				{
					Node.Rotation = FGltfLoader::ConvertRotation({ GltfNode.rotation[0], GltfNode.rotation[1], GltfNode.rotation[2], GltfNode.rotation[3] });
				}
				if (GltfNode.has_scale)
				{
					Node.Scale = FGltfLoader::ConvertScale({ GltfNode.scale[0], GltfNode.scale[1], GltfNode.scale[2] });
				}
			}

			if (GltfNode.mesh != nullptr)
			{
				Node.Meshes = MeshPrimitiveLists[static_cast<size_t>(GltfNode.mesh - Data.meshes)];
			}
		}

		// 루트: 기본 씬의 노드 목록, 없으면 부모 없는 노드 전체
		const cgltf_scene* Scene = Data.scene ? Data.scene : (Data.scenes_count > 0 ? &Data.scenes[0] : nullptr);
		if (Scene != nullptr)
		{
			for (cgltf_size Index = 0; Index < Scene->nodes_count; ++Index)
			{
				OutModel.RootNodes.push_back(static_cast<int32>(Scene->nodes[Index] - Data.nodes));
			}
		}
		else
		{
			for (size_t Index = 0; Index < OutModel.Nodes.size(); ++Index)
			{
				if (OutModel.Nodes[Index].Parent < 0)
				{
					OutModel.RootNodes.push_back(static_cast<int32>(Index));
				}
			}
		}
	}
} // namespace

FQuat FGltfLoader::ConvertRotation(const FQuat& Gltf)
{
	// 반사 변환으로 켤레하면 축은 변환되고 회전 방향은 반대가 된다 → 벡터부 부호 반전
	const FVector3 Axis = ConvertPosition({ Gltf.X, Gltf.Y, Gltf.Z });
	return FQuat(-Axis.X, -Axis.Y, -Axis.Z, Gltf.W).GetNormalized();
}

FVector4 FGltfLoader::ConvertTangent(const FVector4& Gltf)
{
	// 반사 C에 대해 Cross(C·N, C·T) = -C·Cross(N, T). 같은 바이탄젠트를 얻으려면 w 부호를 뒤집어야 한다
	const FVector3 Tangent = ConvertPosition(FVector3(Gltf.X, Gltf.Y, Gltf.Z)).GetNormalized();
	return FVector4(Tangent, Gltf.W < 0.0f ? 1.0f : -1.0f);
}

FMatrix4x4 FGltfLoader::ConvertMatrix(const FMatrix4x4& GltfRowMajor)
{
	// M_e = C^T * M * C
	return GAxisConversion.GetTransposed() * GltfRowMajor * GAxisConversion;
}

bool FGltfLoader::Load(const std::filesystem::path& Path, FModelData& OutModel)
{
	OutModel = FModelData{};
	OutModel.Name = FStringConv::ToUtf8(Path.stem().wstring());

	const std::string PathUtf8 = FStringConv::ToUtf8(Path.wstring());

	cgltf_options Options{};
	Options.file.read    = &ReadFile;
	Options.file.release = &ReleaseFile;

	cgltf_data*  Data   = nullptr;
	cgltf_result Result = cgltf_parse_file(&Options, PathUtf8.c_str(), &Data);
	if (Result != cgltf_result_success)
	{
		E_LOG(LogRenderer, Error, "glTF 파싱 실패: {} ({})", PathUtf8, ResultToString(Result));
		return false;
	}

	Result = cgltf_load_buffers(&Options, Data, PathUtf8.c_str());
	if (Result != cgltf_result_success)
	{
		E_LOG(LogRenderer, Error, "glTF 버퍼 로드 실패: {} ({})", PathUtf8, ResultToString(Result));
		cgltf_free(Data);
		return false;
	}

	Result = cgltf_validate(Data);
	if (Result != cgltf_result_success)
	{
		E_LOG(LogRenderer, Error, "glTF 검증 실패: {} ({})", PathUtf8, ResultToString(Result));
		cgltf_free(Data);
		return false;
	}

	LoadImages(*Data, Path.parent_path(), OutModel);
	LoadMaterials(*Data, OutModel);

	// 메시: glTF 메시 인덱스 → 생성된 FModelMesh 인덱스 목록
	std::vector<std::vector<int32>> MeshPrimitiveLists(Data->meshes_count);
	for (cgltf_size MeshIndex = 0; MeshIndex < Data->meshes_count; ++MeshIndex)
	{
		const cgltf_mesh& GltfMesh = Data->meshes[MeshIndex];
		for (cgltf_size PrimitiveIndex = 0; PrimitiveIndex < GltfMesh.primitives_count; ++PrimitiveIndex)
		{
			FModelMesh Mesh;
			if (LoadPrimitive(*Data, GltfMesh, GltfMesh.primitives[PrimitiveIndex], PrimitiveIndex, Mesh))
			{
				MeshPrimitiveLists[MeshIndex].push_back(static_cast<int32>(OutModel.Meshes.size()));
				OutModel.Meshes.push_back(std::move(Mesh));
			}
		}
	}

	LoadNodes(*Data, MeshPrimitiveLists, OutModel);
	cgltf_free(Data);

	size_t TotalVertices = 0;
	size_t TotalIndices  = 0;
	for (const FModelMesh& Mesh : OutModel.Meshes)
	{
		TotalVertices += Mesh.Data.Vertices.size();
		TotalIndices += Mesh.Data.Indices.size();
	}
	E_LOG(LogRenderer, Display, "glTF 로드: {} (노드 {}, 메시 {}, 머티리얼 {}, 이미지 {}, 정점 {}, 삼각형 {})", OutModel.Name,
	      OutModel.Nodes.size(), OutModel.Meshes.size(), OutModel.Materials.size(), OutModel.Images.size(), TotalVertices,
	      TotalIndices / 3);
	return true;
}
