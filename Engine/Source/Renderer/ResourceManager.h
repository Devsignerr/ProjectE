#pragma once

#include "Core/Containers/ResourcePool.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/GltfLoader.h"
#include "Renderer/Image.h"
#include "Renderer/Material.h"
#include "Renderer/StaticMesh.h"
#include "Renderer/TextureCompression.h"
#include "Scene/ResourceHandles.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class FD3D12RHI;

// 모델 에셋의 공유 GPU 리소스 (FModelLoader가 채운다). 같은 에셋의 인스턴스는 메시/머티리얼/텍스처를 공유한다.
//   Model: 엔티티 배치용 데이터 (노드/스킨/애니메이션, 메시 인덱스·머티리얼 번호). 이미지·정점 데이터는 GPU 업로드 후 비운다
struct FModelResources
{
	FModelData                   Model;
	std::vector<FMeshHandle>     Meshes;
	std::vector<FMaterialHandle> Materials;
	size_t                       TextureCount = 0;
};

// 렌더 리소스(메시/텍스처/머티리얼) 소유자. 핸들로 접근하며 삭제는 GPU 안전하게 지연 처리된다.
class FResourceManager
{
public:
	bool Init(FD3D12RHI& InRhi);
	void Shutdown();

	// ---- 텍스처
	// 경로 + 용도로 캐시 (쿠킹: 밉 + BC 압축). 실패 시 무효 핸들 (Resolve 시 흰색 텍스처로 대체)
	FTextureHandle LoadTexture(const std::filesystem::path& Path, ETextureUsage Usage);
	// 비압축 이미지 (밉은 GPU에서 생성)
	FTextureHandle CreateTexture(const FImage& Image, bool bSRGB, const std::wstring& DebugName);
	// 쿠킹된 텍스처 (전체 밉 체인 업로드)
	FTextureHandle CreateTexture(const FCompressedTexture& Texture, const std::wstring& DebugName);
	void           DestroyTexture(FTextureHandle Handle);
	FD3D12Texture* GetTexture(FTextureHandle Handle) const { return Textures.Get(Handle); }
	FTextureHandle GetWhiteTexture() const { return WhiteTexture; }
	FTextureHandle GetFlatNormalTexture() const { return FlatNormalTexture; } // (0.5, 0.5, 1) 선형
	// 무효 핸들이면 흰색 텍스처
	const FD3D12Texture& ResolveTexture(FTextureHandle Handle) const;

	// ---- 메시
	FMeshHandle  CreateMesh(const FMeshData& MeshData, const std::wstring& DebugName);
	// 스킨 정점 스트림을 가진 메시 (SkinVertices.size() == 정점 수). GPU 스키닝으로 그려진다
	FMeshHandle  CreateSkinnedMesh(const FMeshData& MeshData, const std::vector<FSkinVertex>& SkinVertices, const std::wstring& DebugName);
	void         DestroyMesh(FMeshHandle Handle);
	FStaticMesh* GetMesh(FMeshHandle Handle) const { return Meshes.Get(Handle); }
	// 내장 도형 메시 ("cube"). 이름별로 한 번만 생성해 공유. 모르는 이름이면 무효 핸들
	FMeshHandle GetOrCreatePrimitiveMesh(std::string_view Name);

	// ---- 머티리얼
	// TextureTable은 무시되고 새로 만들어진다
	FMaterialHandle CreateMaterial(const FMaterial& Material);
	// 머티리얼의 텍스처 핸들을 바꾼 뒤 호출: 디스크립터 테이블을 새로 만들고 이전 것은 지연 해제
	void            RefreshMaterialTextures(FMaterialHandle Handle);
	// .emat 파일 로드 (경로별 캐시). 텍스처는 파일 위치 기준 상대 경로로 로드
	FMaterialHandle LoadMaterial(const std::filesystem::path& Path);
	void            DestroyMaterial(FMaterialHandle Handle);
	FMaterial*      GetMaterial(FMaterialHandle Handle) const { return Materials.Get(Handle); }
	FMaterialHandle GetDefaultMaterial() const { return DefaultMaterial; }
	// 무효 핸들이면 기본 머티리얼
	const FMaterial& ResolveMaterial(FMaterialHandle Handle) const;

	// ---- 모델 (키: 정규화 경로). 캐시된 핸들 중 하나라도 삭제됐으면 무효로 보고 항목을 버린다
	const FModelResources* FindModelResources(const std::wstring& Key);
	const FModelResources& AddModelResources(const std::wstring& Key, FModelResources Resources);
	size_t                 GetModelCount() const { return ModelCache.size(); }

	size_t GetTextureCount() const { return Textures.GetCount(); }
	size_t GetMeshCount() const { return Meshes.GetCount(); }
	size_t GetMaterialCount() const { return Materials.GetCount(); }

private:
	// 슬롯별 해석된 텍스처로 새 디스크립터 테이블 작성 (이전 테이블은 지연 해제)
	void BuildMaterialTable(FMaterial& Material);
	const FD3D12Texture& ResolveSlotTexture(const FMaterial& Material, uint32 Slot) const;

	FD3D12RHI* Rhi = nullptr;

	TResourcePool<FD3D12Texture, FTextureHandle> Textures;
	TResourcePool<FStaticMesh, FMeshHandle>      Meshes;
	TResourcePool<FMaterial, FMaterialHandle>    Materials;

	std::unordered_map<std::wstring, FTextureHandle>  TextureCache;   // 키: 정규화 경로 + 색공간
	std::unordered_map<std::wstring, FMaterialHandle> MaterialCache;  // 키: 정규화 경로
	std::unordered_map<std::string, FMeshHandle>      PrimitiveMeshes; // 키: 도형 이름
	std::unordered_map<std::wstring, std::unique_ptr<FModelResources>> ModelCache; // 키: 정규화 경로 (주소 고정)

	FTextureHandle  WhiteTexture;
	FTextureHandle  FlatNormalTexture;
	FMaterialHandle DefaultMaterial;
};
