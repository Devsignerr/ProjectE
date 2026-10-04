#pragma once

#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12PipelineRecipe.h"

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class FJobQueue;

// PSO 캐시 (Phase 48): FD3D12PipelineState::InitGraphics/InitCompute 안에서 투명하게 동작한다 (PSO 생성 API는 그대로).
//   키 = 레시피(PSO 설명 + 루트 시그니처 블롭 해시 + 바이트코드 해시) 해시 — D3D12PipelineRecipe.h
//   요청 순서: ① 워밍으로 미리 만든 PSO(같은 루트 시그니처 객체일 때) → ② 드라이버 캐시(ID3D12PipelineLibrary) 불러오기 → ③ 새로 만들기 + 라이브러리에 저장
//   (a) 디스크 캐시 <Saved>/ShaderCache/PipelineLibrary.bin: FLibraryHeader(어댑터 Vendor/Device/SubSys/Revision + UMD 드라이버 버전 + 형식 버전)가
//       다르거나 CreatePipelineLibrary가 거부(드라이버/어댑터 불일치)하면 버린다. 파일 = 머리 + 라이브러리 블롭 + 저장된 키 목록(uint32 개수 + uint64 키들)
//       — 목록에 있는 키만 Load*Pipeline (없는 이름 Load는 디버그 레이어 경고). 종료 때 이번 실행에서 요청된 PSO만으로 새로 써서 낡은 항목이 쌓이지 않는다
//   (b) 워밍: 레시피 파일 = <Saved>/ShaderCache/PipelineRecipes.epso(사용자, 실행 번호로 나이 관리 — 8번 실행 동안 안 쓰면 제거) ∪
//       <프로젝트>/Config/PipelineRecipes.epso(--record-pso로 기록, 패키지에 파일로 포함, FFileSystem으로 읽음). 시작할 때 작업 스레드가
//       루트 시그니처(블롭)·PSO를 미리 만든다 (ID3D12Device는 자유 스레드). 같은 키를 메인 스레드가 요청하면 끝날 때까지 기다린다.
//       워밍은 드라이버 캐시를 읽지 않고 항상 Create한다 — 작업 스레드 라이브러리 Load와 섞으면 화면이 실행마다 달라졌다 (WarmOne 주석)
//   (c) 드라이버 캐시(a)는 기본 끔 (--pso-library로 켬). 2026-10-05 재현(Tests/Tilemap2D, Release ↔ Debug 교대): 드라이버 캐시에서 Load한 PSO가
//       틀리게 그리는 실행이 나왔다 — 릿 스프라이트 PSO(SpritePS*Lit)가 그림자를 읽지 않아 2D 그림자가 통째로 빠짐(스프라이트 그림자 PSO 자체가 아님).
//       ① Load가 다른 스레드의 Create와 겹친 실행은 Load한 PSO가 실행마다 달랐고(20회 중 5회), ② 그렇게(또는 워밍 실행에서 — 드물게, 재현 조건 미상)
//       저장된 캐시 파일은 그 뒤 Load만 하는 실행마다 같은 틀린 화면(Tests/Materials·Decals·Terrain도 화면이 달라짐). 워밍·요청 Create로 만든 PSO는
//       모든 실행에서 맞았다. 드라이버 내부 원인은 미상이고, 워밍이 레시피 전부를 만들어 요청은 대부분 메모리에서 받으므로(Tilemap2D 요청 스레드 PSO
//       시간 Release 0.17s — 드라이버 캐시 Load만이면 0.35s) 드라이버 캐시를 끈다. 켜면 드라이버 PSO 작업을 한 번에 하나로(FDriverScope —
//       Create·Load·워밍 루트 시그니처, 요청 우선) 해 ①을 막는다(②는 남음 — 형식 버전 3으로 이전 파일은 버림)
//   셰이더 핫 리로드·머티리얼 변형: 바이트코드가 바뀌면 키가 바뀌어 새 PSO (옛 항목은 다음 실행 정리에서 빠짐)
//   쓰는 곳: FD3D12Device::Init(Initialize, 프로젝트가 있을 때만 — 테스트·도구는 캐시 없음) / Shutdown(저장, 디바이스 해제 전)
//   끄기 --no-pso-cache, 워밍만 끄기 --no-pso-warm, 통계 로그 "[PSO 캐시]"
class FD3D12PipelineCache
{
public:
	static FD3D12PipelineCache& Get(); // 엔진 DLL 전역 하나

	struct FOptions
	{
		std::filesystem::path UserDirectory;      // <Saved>/ShaderCache
		std::filesystem::path ProjectRecipeFile;  // <프로젝트>/Config/PipelineRecipes.epso (비면 없음)
		bool                  bWarm          = true;
		bool                  bRecordProject = false; // 종료 때 이번 실행 레시피를 ProjectRecipeFile에 합쳐 쓴다 (--record-pso)
		uint32                WarmThreads    = 2;
		bool                  bUseLibrary    = false; // 드라이버 캐시(ID3D12PipelineLibrary) 읽기·저장 (--pso-library, 기본 끔 — 머리 주석 (c))
	};

	struct FStats
	{
		uint32 Requests       = 0; // 캐시를 거친 PSO 요청 (메인/호출 스레드)
		uint32 Bypassed       = 0; // 캐시 불가(등록 안 된 루트 시그니처 등)로 바로 만든 것
		uint32 WarmHits       = 0; // 이미 메모리에 있던 것 (워밍으로 만들었거나 같은 키를 앞서 요청 — 같은 PSO 객체 공유)
		uint32 WarmWaits      = 0; // 워밍 중이라 기다린 것
		uint32 LibraryHits    = 0; // 드라이버 캐시에서 불러온 것
		uint32 Created        = 0; // 새로 컴파일한 것
		double RequestMs      = 0.0; // 요청 스레드가 PSO 요청에 쓴 총 시간
		double CreateMs       = 0.0; // 그중 새로 만들기
		uint32 WarmCreated    = 0;   // 워밍 스레드가 만든 PSO (항상 Create — 라이브러리를 읽지 않는다)
		double WarmMs         = 0.0; // 워밍 스레드 총 시간
	};

	bool Initialize(ID3D12Device* Device, IDXGIAdapter* Adapter, const FOptions& Options);
	void Shutdown(); // 워밍 대기 → 저장 → 해제 (디바이스 해제 전)
	bool IsEnabled() const { return Device != nullptr; }

	// FD3D12RootSignature: 직렬화 블롭 등록/해제 (등록된 루트 시그니처의 PSO만 캐시한다)
	void RegisterRootSignature(ID3D12RootSignature* RootSignature, const void* Blob, size_t Size);
	void UnregisterRootSignature(ID3D12RootSignature* RootSignature);

	// FD3D12PipelineState가 부른다. 캐시가 꺼져 있으면 바로 Create*PipelineState
	HRESULT CreateGraphics(ID3D12Device* InDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc, ComPtr<ID3D12PipelineState>& Out);
	HRESULT CreateCompute(ID3D12Device* InDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc, ComPtr<ID3D12PipelineState>& Out);

	FStats GetStats() const;
	void   LogStats(const char* When) const;

	FD3D12PipelineCache();
	~FD3D12PipelineCache();

private:
	struct FEntry
	{
		ComPtr<ID3D12PipelineState> Pipeline;
		ID3D12RootSignature*        RootSignature = nullptr; // 만들 때 쓴 객체 (워밍은 블롭으로 만든 것)
		bool                        bPending      = false;
		bool                        bRequested    = false;   // 이번 실행 엔진이 요청함 (저장 대상)
	};

	HRESULT Request(ID3D12Device* InDevice, PipelineCache::FRecipe&& Recipe, ID3D12RootSignature* RootSignature, const void* Desc,
	                ComPtr<ID3D12PipelineState>& Out);
	HRESULT CreateDirect(ID3D12Device* InDevice, const PipelineCache::FRecipe& Recipe, const void* Desc, ComPtr<ID3D12PipelineState>& Out);
	void    StartWarming();
	void    WarmOne(uint64 Key, const PipelineCache::FRecipe& Recipe);
	ID3D12RootSignature* GetWarmRootSignature(uint64 Hash);
	void    LoadLibrary();
	void    SaveLibrary();
	void    SaveRecipes();
	void    RecordBlobs(const PipelineCache::FRecipe& Recipe, const void* Desc);

	ID3D12Device*         Device = nullptr;
	ComPtr<ID3D12Device1> Device1;
	FOptions              Options;
	PipelineCache::FLibraryHeader CurrentHeader;

	mutable std::mutex      Mutex;
	std::condition_variable PendingDone;
	std::unordered_map<uint64, FEntry>                 Entries;            // 키 → PSO
	std::unordered_map<ID3D12RootSignature*, uint64>   RootSignatureHashes; // 엔진 루트 시그니처 → 블롭 해시
	std::unordered_map<uint64, ComPtr<ID3D12RootSignature>> WarmRootSignatures; // 블롭 해시 → 워밍용 객체
	ComPtr<ID3D12PipelineLibrary> Library;
	std::vector<uint8>            LibraryData; // 라이브러리가 참조하는 직렬화 바이트 (라이브러리보다 오래 살아야 함)
	std::unordered_set<uint64>    LibraryKeys; // 디스크 라이브러리에 저장된 키 (없는 이름을 Load하면 디버그 레이어 경고 — 있을 때만 불러온다, 읽은 뒤 읽기 전용)
	// 드라이버 캐시 Load(배타) ↔ PSO·루트 시그니처 Create(공유, 워밍·요청 스레드): 라이브러리 Load가 다른 스레드의 Create와 겹치면
	// 화면이 실행마다 달라지고 PSO가 통째로 그리지 않는 일이 있었다 (2026-10-05 재현 — 2D 그림자가 빠짐, 머리 주석 참고)
	// 드라이버 캐시를 켰을 때만(Library): 드라이버 PSO 작업(Create*PipelineState·Load*Pipeline·워밍 루트 시그니처)은 프로세스 안에서 한 번에 하나
	// (머리 주석 (c)). 요청(비워밍) 우선: 요청이 기다리는 동안 워밍은 새 작업을 시작하지 않는다
	std::mutex              GateMutex;
	std::condition_variable GateChanged;
	uint32                  PendingRequests = 0;
	bool                    bDriverBusy     = false;
	struct FDriverScope
	{
		FDriverScope(FD3D12PipelineCache& InCache, bool bWarm);
		~FDriverScope();
		FD3D12PipelineCache& Cache;
		bool                 bActive = false;
	};
	PipelineCache::FRecipeFile    UserRecipes;   // 읽은 사용자 레시피 (+ 이번 실행 기록)
	PipelineCache::FRecipeFile    SessionRecipes; // 이번 실행 요청 레시피 (+ 블롭)
	PipelineCache::FRecipeFile    WarmSource;     // 워밍 레시피 (사용자 ∪ 프로젝트, 워밍 중 읽기 전용)
	bool                          bLibraryDirty       = false; // 새로 컴파일한 PSO가 있다 → 종료 때 드라이버 캐시 다시 쓰기
	bool                          bLibraryInvalidated = false; // 디스크 캐시를 버렸다 (드라이버/어댑터 변경)
	bool                          bLibraryFromDisk    = false;
	uint32                        CurrentRun = 0;
	std::unique_ptr<FJobQueue>    WarmJobs;
	FStats                        Stats;
};
