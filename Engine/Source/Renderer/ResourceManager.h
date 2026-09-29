#pragma once

#include "Core/Containers/ResourcePool.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/Image.h"
#include "Renderer/Material.h"
#include "Renderer/StaticMesh.h"
#include "Scene/ResourceHandles.h"

#include <filesystem>
#include <string>
#include <unordered_map>

class FD3D12RHI;

// 렌더 리소스(메시/텍스처/머티리얼) 소유자. 핸들로 접근하며 삭제는 GPU 안전하게 지연 처리된다.
class FResourceManager
{
public:
	bool Init(FD3D12RHI& InRhi);
	void Shutdown();

	// ---- 텍스처
	// 경로 + 색공간으로 캐시. 실패 시 무효 핸들 (Resolve 시 흰색 텍스처로 대체)
	FTextureHandle LoadTexture(const std::filesystem::path& Path, bool bSRGB);
	FTextureHandle CreateTexture(const FImage& Image, bool bSRGB, const std::wstring& DebugName);
	void           DestroyTexture(FTextureHandle Handle);
	FD3D12Texture* GetTexture(FTextureHandle Handle) const { return Textures.Get(Handle); }
	FTextureHandle GetWhiteTexture() const { return WhiteTexture; }
	// 무효 핸들이면 흰색 텍스처
	const FD3D12Texture& ResolveTexture(FTextureHandle Handle) const;

	// ---- 메시
	FMeshHandle  CreateMesh(const FMeshData& MeshData, const std::wstring& DebugName);
	void         DestroyMesh(FMeshHandle Handle);
	FStaticMesh* GetMesh(FMeshHandle Handle) const { return Meshes.Get(Handle); }
	// 내장 도형 메시 ("cube"). 이름별로 한 번만 생성해 공유. 모르는 이름이면 무효 핸들
	FMeshHandle GetOrCreatePrimitiveMesh(std::string_view Name);

	// ---- 머티리얼
	FMaterialHandle CreateMaterial(const FMaterial& Material);
	// .emat 파일 로드 (경로별 캐시). 텍스처는 파일 위치 기준 상대 경로로 로드
	FMaterialHandle LoadMaterial(const std::filesystem::path& Path);
	void            DestroyMaterial(FMaterialHandle Handle);
	FMaterial*      GetMaterial(FMaterialHandle Handle) const { return Materials.Get(Handle); }
	FMaterialHandle GetDefaultMaterial() const { return DefaultMaterial; }
	// 무효 핸들이면 기본 머티리얼
	const FMaterial& ResolveMaterial(FMaterialHandle Handle) const;

	size_t GetTextureCount() const { return Textures.GetCount(); }
	size_t GetMeshCount() const { return Meshes.GetCount(); }
	size_t GetMaterialCount() const { return Materials.GetCount(); }

private:
	FD3D12RHI* Rhi = nullptr;

	TResourcePool<FD3D12Texture, FTextureHandle> Textures;
	TResourcePool<FStaticMesh, FMeshHandle>      Meshes;
	TResourcePool<FMaterial, FMaterialHandle>    Materials;

	std::unordered_map<std::wstring, FTextureHandle>  TextureCache;   // 키: 정규화 경로 + 색공간
	std::unordered_map<std::wstring, FMaterialHandle> MaterialCache;  // 키: 정규화 경로
	std::unordered_map<std::string, FMeshHandle>      PrimitiveMeshes; // 키: 도형 이름

	FTextureHandle  WhiteTexture;
	FMaterialHandle DefaultMaterial;
};
