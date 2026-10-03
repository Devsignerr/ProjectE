#pragma once

#include "Core/Containers/ResourcePool.h"
#include "Core/Jobs/JobQueue.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "Renderer/GltfLoader.h"
#include "Renderer/Image.h"
#include "Renderer/Material.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/ResourceCollector.h"
#include "Renderer/StaticMesh.h"
#include "Renderer/TextureCompression.h"
#include "Renderer/TextureStreaming.h"
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
	// 경로 + 용도로 캐시 (쿠킹: 밉 + BC 압축). 실패 시 무효 핸들 (Resolve 시 흰색 텍스처로 대체).
	// 머티리얼 밖(UI·파티클·썸네일 등)에서 부르는 창구 — 이렇게 얻은 텍스처는 밉 스트리밍하지 않는다(항상 전체 밉, TextureStreaming.h)
	FTextureHandle LoadTexture(const std::filesystem::path& Path, ETextureUsage Usage);
	// 비압축 이미지 (밉은 GPU에서 생성)
	FTextureHandle CreateTexture(const FImage& Image, bool bSRGB, const std::wstring& DebugName);
	// 쿠킹된 텍스처 (전체 밉 체인 업로드)
	FTextureHandle CreateTexture(const FCompressedTexture& Texture, const std::wstring& DebugName);
	// 쿠킹 파일에서 밉 단위로 다시 읽을 수 있는 텍스처 (모델 이미지): 밉 스트리밍 대상 (Source 무효면 CreateTexture와 같다).
	// 비동기 로딩 + r.Streaming이면 꼬리 밉만 올리고 필요에 따라 올린다
	FTextureHandle CreateStreamingTexture(const FCompressedTexture& Texture, const FTextureStreamSource& Source, const std::wstring& DebugName);
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
	// 디스크의 .emat가 바뀜 (에디터 파일 감시 — 핫 리로드): 캐시된 머티리얼과 이 파일을 조상으로 둔 인스턴스를 다시 읽는다.
	// 그래프 컴파일 오류면 이전 셰이더 유지 + 오류 로그. 반환: 다시 읽은 머티리얼이 있으면 true
	bool            ReloadMaterialFile(const std::filesystem::path& Path);
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

	// ---- 수거 / 통계 (Phase 37 — 규칙은 Renderer/ResourceCollector.h 머리 주석, 구현은 ResourceCollector.cpp)
	// 루트 제공자: 수거 때마다 불린다 (씬·편집기·렌더러 캐시가 지금 쓰는 핸들). 반환 ID로 해제 (제공자 수명 안에서)
	uint32 AddRootProvider(FResourceRootProvider Provider);
	void   RemoveRootProvider(uint32 Id);
	// DelayFrames번의 Tick 뒤 수거 (새 씬이 해석·렌더되어 루트가 채워진 뒤). 겹친 요청은 하나로 합친다.
	// bForce = 콘솔/통계 창 요청 (r.ResourceAutoCollect가 꺼져 있어도)
	void   RequestGarbageCollection(std::string_view Reason, uint32 DelayFrames = 2, bool bForce = false);
	// 앱이 프레임마다 한 번 (렌더링 기록 밖): 요청된 수거 + VRAM 예산 초과 경고
	void   Tick();
	// 즉시 수거 (해제는 지연 — GPU 안전). 렌더링 기록 밖에서만 부른다
	FResourceCollectResult CollectGarbage(std::string_view Reason);
	FResourceMemoryStats   GetMemoryStats();
	size_t                 GetParticleSystemCount() const { return ParticleCache.size(); }

	// ---- 텍스처 밉 스트리밍 (Phase 53 — 규칙은 Renderer/TextureStreaming.h 머리 주석, 구현은 TextureStreaming.cpp)
	// 씬 렌더러가 뷰마다 (메시 인스턴스 수집·LOD 선택 뒤, 그래프 실행 전). 자동 검증/동기 로딩이면 부족한 밉을 여기서 바로 채운다
	void                   ReportTextureStreamingView(const FTextureStreamingView& View);
	FTextureStreamingStats GetTextureStreamingStats() const;
	// 스트리밍 항목이면 상주 최상위 밉 (아니면 0 — 전체). 테스트/디버그
	uint32                 GetTextureResidentTopMip(FTextureHandle Handle) const;
	bool                   IsTextureStreamingActive() const { return bAsyncLoadingEnabled; }

private:
	struct FPendingUpload
	{
		FTextureHandle Texture;
		FMeshHandle    Mesh;
		uint64         Fence = 0;
	};
	bool IsAsyncUpload() const { return GetLoadMode() != EResourceLoadMode::Sync; }
	// bStreamable: 머티리얼이 부름 (밉 스트리밍 대상). false면 고정(전체 밉) — 이미 스트리밍 중이면 고정으로 바꾼다
	FTextureHandle LoadTextureInternal(const std::filesystem::path& Path, ETextureUsage Usage, bool bStreamable);
	// 스트리밍 (TextureStreaming.cpp)
	bool IsStreamingEnabled() const;                 // 비동기 로딩 사용 + r.Streaming
	bool IsStreamingDeterministic() const;           // Sync/AsyncDrain: 처음 전체 + 보고 안에서 바로 채움
	void RegisterStreamingTexture(FTextureHandle Handle, const FTextureStreamSource& Source, const TextureStreamingMath::FPayloadLayout& Layout,
	                              uint32 ResidentTop, bool bPinned, const std::wstring& DebugName);
	void UnregisterStreamingTexture(FTextureHandle Handle);
	void PinStreamingTexture(FTextureHandle Handle);
	void ProcessTextureStreaming();                  // BeginFrame (ProcessAsyncLoads)
	// bFinal = BeginFrame (지난 프레임 보고 확정: 히스테리시스 진행 + 보고 비움), false = 보고 중간 (더 세밀해진 것만)
	void UpdateStreamingTargets(float DeltaSeconds, bool bFinal);
	void IssueStreamingRequests(bool bAllowDrops, bool bUnlimited);
	void IssueStreamingRequest(FTextureStreamingState::FEntry& Entry, uint32 NewTop);
	void CompleteStreamUploads(uint64 FinalizedFence);
	void FinishStreamingNow();                       // 결정적 모드: 진행 중 요청을 CPU 대기로 모두 끝낸다
	uint32 GetStreamingPendingCount() const;
	uint64 GetStreamingPoolBytes() const;
	void InitStreamingConsole();
	void ShutdownStreaming();
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
	// 그래프 머티리얼 (컴파일 성공한 셰이더): 상수·텍스처 칸을 레이아웃대로. 반환: 테이블을 다시 만들어야 하면 true
	bool FillGraphMaterial(FMaterial& Material, const FMaterialAsset& Asset, std::shared_ptr<const FMaterialShader> Shader,
	                       const std::filesystem::path& BaseDirectory);
	// 경로의 .emat를 해석해 머티리얼에 채운다 (ParentChain 갱신, 텍스처가 바뀌면 테이블 재작성)
	void ResolveAndFillMaterial(FMaterial& Material, const FMaterialAsset& Asset, const std::filesystem::path& AssetPath, bool bBuildTable);
	const FD3D12Texture& ResolveSlotTexture(const FMaterial& Material, uint32 Slot) const;
	// 수거 (ResourceCollector.cpp): Init/Shutdown에서 콘솔 명령 등록/해제
	void InitCollector();
	void ShutdownCollector();

	FD3D12RHI* Rhi = nullptr;

	TResourcePool<FD3D12Texture, FTextureHandle> Textures;
	TResourcePool<FStaticMesh, FMeshHandle>      Meshes;
	TResourcePool<FMaterial, FMaterialHandle>    Materials;

	std::unordered_map<std::wstring, FTextureHandle>  TextureCache;   // 키: 정규화 경로 + 색공간
	std::unordered_map<std::wstring, FMaterialHandle> MaterialCache;  // 키: 정규화 경로
	std::unordered_map<uint64, std::weak_ptr<const FMaterialShader>> GraphShaders; // 그래프 셰이더 공유 (키: HLSL 해시)
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

	// 수거 (Phase 37)
	struct FRootProviderEntry
	{
		uint32                Id = 0;
		FResourceRootProvider Provider;
	};
	std::vector<FRootProviderEntry> RootProviders;
	uint32                          NextRootProviderId  = 1;
	int32                           PendingCollectTicks = -1; // < 0 = 요청 없음
	std::string                     PendingCollectReason;
	uint64                          TickCount            = 0;
	bool                            bWarnedOverBudget    = false;
	bool                            bOwnsConsoleCommands = false;

	// 텍스처 밉 스트리밍 (Phase 53)
	FTextureStreamingState Streaming;
};
