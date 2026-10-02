#pragma once

#include "Core/Containers/ResourcePool.h"
#include "Core/Jobs/JobQueue.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/GltfLoader.h"
#include "Renderer/Image.h"
#include "Renderer/Material.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/StaticMesh.h"
#include "Renderer/TextureCompression.h"
#include "Scene/ResourceHandles.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class FD3D12RHI;
struct FModelMetadata;
struct FParticleSystemAsset;

// 모델 에셋의 공유 GPU 리소스 (FModelLoader가 채운다). 같은 에셋의 인스턴스는 메시/머티리얼/텍스처를 공유한다.
//   Model: 엔티티 배치용 데이터 (노드/스킨/애니메이션, 메시 인덱스·머티리얼 번호). 이미지·정점 데이터는 GPU 업로드 후 비운다
struct FModelResources
{
	FModelData                   Model;
	std::vector<FMeshHandle>     Meshes;
	std::vector<FMaterialHandle> Materials;
	size_t                       TextureCount = 0;
	// 노티파이/소켓 (.emeta). 편집기가 고치면 이 모델의 모든 인스턴스에 바로 반영된다 (항상 유효)
	std::shared_ptr<FModelMetadata> Metadata;
};

// 리소스 로딩 방식 (r.AsyncLoading, GetLoadMode)
enum class EResourceLoadMode : uint8
{
	Sync,       // 만드는 호출 안에서 디코드·업로드·GPU 완료까지 (테스트/도구/쿠킹, EnableAsyncLoading 안 부른 앱)
	Async,      // 파일 텍스처 디코드/압축은 작업 스레드, 업로드는 복사 큐 — 준비 전에는 대체 텍스처/그리지 않음
	AsyncDrain, // Async와 같은 경로를 쓰되 매 BeginFrame에 전부 끝낸 뒤 그린다 (자동 검증 — 반쯤 로드된 화면을 찍지 않음)
};

// 렌더 리소스(메시/텍스처/머티리얼) 소유자. 핸들로 접근하며 삭제는 GPU 안전하게 지연 처리된다.
// 비동기 로딩(Phase 37): EnableAsyncLoading 뒤에는
//   - LoadTexture: 핸들(자리표시 텍스처)을 바로 돌려주고, 파일 읽기 + 디코드 + BC 압축/쿠킹(FAssetCache)은 작업 스레드,
//     GPU 리소스 생성 + 복사 큐 업로드는 메인 스레드(BeginFrame 콜백 ProcessAsyncLoads)
//   - CreateTexture(쿠킹 텍스처)/CreateMesh/CreateSkinnedMesh: 복사 큐 + 업로드 링으로 CPU 대기 없이 (FD3D12UploadQueue)
//   - 준비 전: ResolveTexture/머티리얼 테이블은 기본 텍스처(흰색/평면 노멀), 메시는 FStaticMesh::IsReady가 false(렌더 수집에서 빠짐).
//     준비되면 그 텍스처를 쓰는 머티리얼 테이블을 다시 만든다
//   - 비압축 이미지(GPU 밉 생성)/원시 텍스처(UI 글꼴 아틀라스)는 바로 쓰이므로 항상 동기
//   - 상태: IsReady(핸들), GetPendingLoadCount, WaitForPendingLoads(전부 끝낼 때까지 CPU 대기)
class FResourceManager
{
public:
	bool Init(FD3D12RHI& InRhi);
	void Shutdown();

	// ---- 비동기 로딩
	// 앱이 Init 뒤에 부른다 (에디터/런타임/Sandbox). bAutomationRun이면 r.AsyncLoading 자동(-1) = AsyncDrain
	void              EnableAsyncLoading(bool bAutomationRun);
	EResourceLoadMode GetLoadMode() const;
	bool              IsReady(FTextureHandle Handle) const;
	bool              IsReady(FMeshHandle Handle) const;
	// 아직 준비되지 않은 로드 수 (작업 스레드 대기·실행 + 업로드 대기)
	uint32            GetPendingLoadCount() const;
	bool              HasPendingLoads() const { return GetPendingLoadCount() > 0; }
	// 진행 중인 로드를 모두 끝낸다 (CPU 대기: 작업 완료 → 업로드 제출 → 복사·전이 완료). 메인 스레드
	void              WaitForPendingLoads();
	// 프레임마다 (RHI BeginFrame 콜백): 끝난 작업 → GPU 업로드, 끝난 업로드 → 준비 + 머티리얼 테이블 갱신
	void              ProcessAsyncLoads();

	// ---- 텍스처
	// 경로 + 용도로 캐시 (쿠킹: 밉 + BC 압축). 실패 시 무효 핸들 (Resolve 시 흰색 텍스처로 대체)
	FTextureHandle LoadTexture(const std::filesystem::path& Path, ETextureUsage Usage);
	// 비압축 이미지 (밉은 GPU에서 생성)
	FTextureHandle CreateTexture(const FImage& Image, bool bSRGB, const std::wstring& DebugName);
	// 쿠킹된 텍스처 (전체 밉 체인 업로드)
	FTextureHandle CreateTexture(const FCompressedTexture& Texture, const std::wstring& DebugName);
	// 밉 1개 원시 텍스처 (UI 글꼴 아틀라스 R8 등). Pixels는 행 단위로 빈틈없이 (Width * BytesPerPixel)
	FTextureHandle CreateTexture(uint32 Width, uint32 Height, DXGI_FORMAT Format, const void* Pixels, uint32 BytesPerPixel, const std::wstring& DebugName);
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
	// .emat 파일 로드 (경로별 캐시). 텍스처는 파일 위치 기준 상대 경로로 로드. 인스턴스(Parent)는 부모 체인을 해석해 채운다
	FMaterialHandle LoadMaterial(const std::filesystem::path& Path);
	// .emat 내용을 기존 머티리얼에 반영 (에디터 실시간 편집/되돌리기). 텍스처 경로는 BaseDirectory 기준.
	// 내용은 그 경로의 "편집 중 원본"으로 기억되어 이 머티리얼을 부모로 둔 인스턴스 해석에도 쓰이고, 캐시된 자식 인스턴스는 바로 다시 해석된다.
	// 텍스처 핸들이 바뀐 경우에만 디스크립터 테이블을 다시 만든다 (상수 드래그 중 매 프레임 호출 가능)
	void            ApplyMaterialAsset(FMaterialHandle Handle, const FMaterialAsset& Asset, const std::filesystem::path& BaseDirectory);
	// 인스턴스 해석 (편집 중 원본 → 디스크 순으로 부모를 읽는다). AssetPath = 그 .emat 경로. 실패(순환/부모 없음)면 false + 오류 로그
	bool            ResolveMaterialAsset(const FMaterialAsset& Asset, const std::filesystem::path& AssetPath, FMaterialAsset& OutResolved,
	                                     std::vector<std::filesystem::path>* OutChain = nullptr, std::string* OutError = nullptr) const;
	void            DestroyMaterial(FMaterialHandle Handle);
	FMaterial*      GetMaterial(FMaterialHandle Handle) const { return Materials.Get(Handle); }
	FMaterialHandle GetDefaultMaterial() const { return DefaultMaterial; }
	// 무효 핸들이면 기본 머티리얼
	const FMaterial& ResolveMaterial(FMaterialHandle Handle) const;

	// ---- 파티클 시스템 (.eparticle, 경로별 캐시). 반환된 에셋을 고치면(파티클 편집기) 같은 에셋의 모든 컴포넌트에 즉시 반영된다
	std::shared_ptr<FParticleSystemAsset> LoadParticleSystem(const std::filesystem::path& Path);
	// 모든 렌더러의 TexturePath/MeshAsset으로 핸들을 채운다 (BaseDirectory = .eparticle 폴더)
	void ResolveParticleResources(FParticleSystemAsset& System, const std::filesystem::path& BaseDirectory);

	// ---- 모델 (키: 정규화 경로). 캐시된 핸들 중 하나라도 삭제됐으면 무효로 보고 항목을 버린다
	const FModelResources* FindModelResources(const std::wstring& Key);
	const FModelResources& AddModelResources(const std::wstring& Key, FModelResources Resources);
	size_t                 GetModelCount() const { return ModelCache.size(); }
	// 다시 가져오기: 캐시에서 모델 리소스를 꺼낸다 (씬 인스턴스를 새 리소스로 다시 만든 뒤 DestroyModelResources로 해제)
	std::unique_ptr<FModelResources> TakeModelResources(const std::filesystem::path& Path);
	// 여러 모델 파일 읽기/파싱(FAssetCache::LoadModelAsset)을 작업 스레드에서 병렬로 미리 해 두고 끝날 때까지 기다린다
	// (씬/맵 전환 해석 앞에서 — 엔티티 배치와 GPU 업로드는 그 뒤 메인 스레드). 비동기 로딩이 꺼져 있으면 아무것도 하지 않는다.
	// 키 = FindModelResources와 같은 정규화 경로. 이미 캐시된 모델은 건너뛴다
	void PrefetchModels(const std::vector<std::filesystem::path>& Paths);
	// PrefetchModels 결과를 꺼낸다 (없으면 false — 호출자가 직접 읽는다)
	bool TakePrefetchedModel(const std::wstring& Key, FModelData& OutModel);
	// 모델이 만든 메시/머티리얼/텍스처를 지연 해제 (기본 텍스처/머티리얼은 제외)
	void DestroyModelResources(const FModelResources& Model);

	// 에셋 파일/폴더 이동 후 경로 캐시 키를 새 경로로 옮긴다 (같은 에셋을 새 경로로 다시 로드해 GPU 리소스가 중복되지 않게). 경로는 절대 경로
	void OnAssetMoved(const std::filesystem::path& From, const std::filesystem::path& To);

	size_t GetTextureCount() const { return Textures.GetCount(); }
	size_t GetMeshCount() const { return Meshes.GetCount(); }
	size_t GetMaterialCount() const { return Materials.GetCount(); }

private:
	struct FPendingUpload
	{
		FTextureHandle Texture;
		FMeshHandle    Mesh;
		uint64         Fence = 0;
	};
	bool IsAsyncUpload() const { return GetLoadMode() != EResourceLoadMode::Sync; }
	// 쿠킹 텍스처를 Target에 만든다 (비동기면 복사 큐 + 대기 목록, 아니면 동기)
	bool UploadCompressedTexture(FD3D12Texture& Target, FTextureHandle Handle, const FCompressedTexture& Texture, const std::wstring& DebugName);
	void FinishTextureLoad(FTextureHandle Handle, const FCompressedTexture& Texture, bool bLoaded, const std::wstring& DebugName);
	// 펜스 <= FinalizedFence인 대기 업로드를 준비 상태로 + 그 텍스처를 쓰는 머티리얼 테이블 재작성
	void CompletePendingUploads(uint64 FinalizedFence);
	void NoteAsyncRequest(); // 로딩 묶음 시간 측정 (ProcessAsyncLoads가 모두 끝날 때 로그)

	// 슬롯별 해석된 텍스처로 새 디스크립터 테이블 작성 (이전 테이블은 지연 해제)
	void BuildMaterialTable(FMaterial& Material);
	// Asset은 해석된(평탄한) 내용. 텍스처 핸들이 바뀌었으면 true
	bool FillMaterialFromAsset(FMaterial& Material, const FMaterialAsset& Asset, const std::filesystem::path& BaseDirectory);
	// 경로의 .emat를 해석해 머티리얼에 채운다 (ParentChain 갱신, 텍스처가 바뀌면 테이블 재작성)
	void ResolveAndFillMaterial(FMaterial& Material, const FMaterialAsset& Asset, const std::filesystem::path& AssetPath, bool bBuildTable);
	const FD3D12Texture& ResolveSlotTexture(const FMaterial& Material, uint32 Slot) const;

	FD3D12RHI* Rhi = nullptr;

	TResourcePool<FD3D12Texture, FTextureHandle> Textures;
	TResourcePool<FStaticMesh, FMeshHandle>      Meshes;
	TResourcePool<FMaterial, FMaterialHandle>    Materials;

	std::unordered_map<std::wstring, FTextureHandle>  TextureCache;   // 키: 정규화 경로 + 색공간
	std::unordered_map<std::wstring, FMaterialHandle> MaterialCache;  // 키: 정규화 경로
	std::unordered_map<std::wstring, FMaterialAsset>  EditedMaterialSources; // 키: FMaterialAsset::MakePathKey — ApplyMaterialAsset 내용 (인스턴스 해석이 디스크보다 먼저 읽는다)
	std::unordered_map<std::string, FMeshHandle>      PrimitiveMeshes; // 키: 도형 이름
	std::unordered_map<std::wstring, std::unique_ptr<FModelResources>> ModelCache; // 키: 정규화 경로 (주소 고정)
	std::unordered_map<std::wstring, std::shared_ptr<FParticleSystemAsset>> ParticleCache; // 키: 정규화 경로
	std::unordered_map<std::wstring, FModelData>      PrefetchedModels; // PrefetchModels 결과 (LoadModelResources가 꺼내 씀)

	FJobQueue                   LoadJobs;            // 파일 텍스처 읽기/디코드/압축 (EnableAsyncLoading이 시작)
	std::vector<FPendingUpload> PendingUploads;      // 복사 큐 업로드 완료 대기
	bool                        bAsyncLoadingEnabled = false;
	bool                        bAutomationRun       = false;
	uint32                      BeginFrameCallbackId = 0;
	bool                                  bLoadBurstActive  = false;
	uint32                                LoadBurstRequests = 0;
	std::chrono::steady_clock::time_point LoadBurstStart;

	FTextureHandle  WhiteTexture;
	FTextureHandle  FlatNormalTexture;
	FMaterialHandle DefaultMaterial;
};
