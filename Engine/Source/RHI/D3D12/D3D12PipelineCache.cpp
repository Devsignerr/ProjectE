#include "RHI/D3D12/D3D12PipelineCache.h"

#include "Core/FileSystem.h"
#include "Core/Jobs/JobQueue.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

namespace
{
	using FClock = std::chrono::steady_clock;

	constexpr uint32 RecipeMaxAgeRuns = 8; // 이만큼의 실행 동안 요청되지 않은 레시피는 사용자 파일에서 뺀다

	double ElapsedMs(FClock::time_point Start)
	{
		return std::chrono::duration<double, std::milli>(FClock::now() - Start).count();
	}

	bool ReadDiskFile(const std::filesystem::path& Path, std::vector<uint8>& Out)
	{
		std::ifstream File(Path, std::ios::binary | std::ios::ate);
		if (!File)
		{
			return false;
		}
		const std::streamsize Size = File.tellg();
		if (Size < 0)
		{
			return false;
		}
		Out.resize(static_cast<size_t>(Size));
		File.seekg(0);
		return Size == 0 || static_cast<bool>(File.read(reinterpret_cast<char*>(Out.data()), Size));
	}

	// 임시 파일에 쓰고 바꿔치기 (중간에 꺼져도 반쯤 쓴 캐시가 남지 않게)
	bool WriteDiskFile(const std::filesystem::path& Path, const void* Header, size_t HeaderSize, const void* Data, size_t Size)
	{
		std::error_code ErrorCode;
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
		const std::filesystem::path Temp = Path.wstring() + L".tmp";
		{
			std::ofstream File(Temp, std::ios::binary | std::ios::trunc);
			if (!File)
			{
				return false;
			}
			if (HeaderSize > 0)
			{
				File.write(static_cast<const char*>(Header), static_cast<std::streamsize>(HeaderSize));
			}
			File.write(static_cast<const char*>(Data), static_cast<std::streamsize>(Size));
			if (!File)
			{
				return false;
			}
		}
		std::filesystem::rename(Temp, Path, ErrorCode);
		return !ErrorCode;
	}

	D3D12_SHADER_BYTECODE BlobBytecode(const PipelineCache::FRecipeFile& File, const PipelineCache::FShaderRef& Ref)
	{
		if (Ref.Size == 0)
		{
			return {};
		}
		const std::vector<uint8>& Bytes = File.Blobs.at(Ref.Hash);
		return { Bytes.data(), Bytes.size() };
	}
} // namespace

FD3D12PipelineCache& FD3D12PipelineCache::Get()
{
	static FD3D12PipelineCache Instance;
	return Instance;
}

FD3D12PipelineCache::FD3D12PipelineCache()  = default;
FD3D12PipelineCache::~FD3D12PipelineCache() = default;

bool FD3D12PipelineCache::Initialize(ID3D12Device* InDevice, IDXGIAdapter* Adapter, const FOptions& InOptions)
{
	E_CHECKF(Device == nullptr, "PSO 캐시가 이미 초기화되어 있습니다");
	Device  = InDevice;
	Options = InOptions;
	Stats   = FStats{};
	Device->QueryInterface(IID_PPV_ARGS(&Device1)); // 없으면 드라이버 캐시 없이 워밍만

	CurrentHeader = PipelineCache::FLibraryHeader{};
	DXGI_ADAPTER_DESC AdapterDesc{};
	if (Adapter != nullptr && SUCCEEDED(Adapter->GetDesc(&AdapterDesc)))
	{
		CurrentHeader.VendorId = AdapterDesc.VendorId;
		CurrentHeader.DeviceId = AdapterDesc.DeviceId;
		CurrentHeader.SubSysId = AdapterDesc.SubSysId;
		CurrentHeader.Revision = AdapterDesc.Revision;
		LARGE_INTEGER UmdVersion{};
		if (SUCCEEDED(Adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &UmdVersion)))
		{
			CurrentHeader.DriverVersion = static_cast<uint64>(UmdVersion.QuadPart);
		}
	}
	std::error_code ErrorCode;
	std::filesystem::create_directories(Options.UserDirectory, ErrorCode);

	const auto Start = FClock::now();
	LoadLibrary();

	// 레시피: 사용자 파일(실행 번호) + 프로젝트 파일(패키지 포함, FFileSystem)
	std::vector<uint8> Bytes;
	if (ReadDiskFile(Options.UserDirectory / L"PipelineRecipes.epso", Bytes) && !UserRecipes.Read(Bytes))
	{
		E_LOG(LogD3D12, Warning, "[PSO 캐시] 사용자 레시피 파일이 손상되어 버립니다");
	}
	CurrentRun = UserRecipes.RunCounter + 1;
	PipelineCache::FRecipeFile WarmSet = UserRecipes;
	uint32 ProjectRecipes = 0;
	if (!Options.ProjectRecipeFile.empty() && FFileSystem::Exists(Options.ProjectRecipeFile))
	{
		PipelineCache::FRecipeFile Project;
		Bytes.clear();
		if (FFileSystem::ReadFile(Options.ProjectRecipeFile, Bytes) && Project.Read(Bytes))
		{
			ProjectRecipes = static_cast<uint32>(Project.Recipes.size());
			WarmSet.Merge(Project);
		}
		else
		{
			E_LOG(LogD3D12, Warning, "[PSO 캐시] 프로젝트 레시피 파일을 읽지 못했습니다: {}", FStringConv::ToUtf8(Options.ProjectRecipeFile.wstring()));
		}
	}
	E_LOG(LogD3D12, Display, "[PSO 캐시] 시작: 드라이버 캐시 {}, 레시피 사용자 {} + 프로젝트 {}, 실행 #{} ({:.1f}ms)",
	      bLibraryFromDisk ? "디스크에서 읽음" : (Library ? "새로 만듦" : "지원 안 됨"),
	      UserRecipes.Recipes.size(), ProjectRecipes, CurrentRun, ElapsedMs(Start));

	if (Options.bWarm && !WarmSet.Recipes.empty())
	{
		WarmSource = std::move(WarmSet);
		StartWarming();
	}
	return true;
}

void FD3D12PipelineCache::LoadLibrary()
{
	if (!Device1)
	{
		return;
	}
	const std::filesystem::path Path = Options.UserDirectory / L"PipelineLibrary.bin";
	std::vector<uint8>          Bytes;
	if (ReadDiskFile(Path, Bytes) && Bytes.size() > sizeof(PipelineCache::FLibraryHeader))
	{
		PipelineCache::FLibraryHeader Stored;
		std::memcpy(&Stored, Bytes.data(), sizeof(Stored));
		// 머리 + 라이브러리 블롭(DataSize) + 키 개수(uint32) + 키(uint64 × 개수)
		const size_t KeysOffset = sizeof(Stored) + static_cast<size_t>(Stored.DataSize);
		uint32       KeyCount   = 0;
		const bool   bSizeOk    = Bytes.size() >= KeysOffset + sizeof(uint32) &&
		                     (std::memcpy(&KeyCount, Bytes.data() + KeysOffset, sizeof(uint32)), Bytes.size() == KeysOffset + sizeof(uint32) + KeyCount * sizeof(uint64));
		if (PipelineCache::IsLibraryCompatible(Stored, CurrentHeader) && bSizeOk)
		{
			LibraryData.assign(Bytes.begin() + sizeof(Stored), Bytes.begin() + static_cast<std::ptrdiff_t>(KeysOffset));
			const HRESULT Result = Device1->CreatePipelineLibrary(LibraryData.data(), LibraryData.size(), IID_PPV_ARGS(&Library));
			if (SUCCEEDED(Result))
			{
				LibraryKeys.reserve(KeyCount);
				for (uint32 Index = 0; Index < KeyCount; ++Index)
				{
					uint64 Key = 0;
					std::memcpy(&Key, Bytes.data() + KeysOffset + sizeof(uint32) + Index * sizeof(uint64), sizeof(uint64));
					LibraryKeys.insert(Key);
				}
				bLibraryFromDisk = true;
				return;
			}
			E_LOG(LogD3D12, Display, "[PSO 캐시] 드라이버 캐시를 쓸 수 없어 새로 만듭니다 ({})", HResultToString(Result)); // 드라이버/어댑터 불일치
		}
		else
		{
			E_LOG(LogD3D12, Display, "[PSO 캐시] 어댑터·드라이버·형식이 달라 드라이버 캐시를 버립니다");
		}
		bLibraryInvalidated = true;
	}
	LibraryData.clear();
	if (FAILED(Device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&Library))))
	{
		Library.Reset();
		E_LOG(LogD3D12, Warning, "[PSO 캐시] 파이프라인 라이브러리를 지원하지 않습니다 — 워밍만 사용");
	}
}

void FD3D12PipelineCache::StartWarming()
{
	WarmJobs = std::make_unique<FJobQueue>();
	WarmJobs->Init(std::max(Options.WarmThreads, 1u), "PSO 워밍");
	uint32 Submitted = 0;
	{
		std::lock_guard Lock(Mutex);
		for (const auto& [Key, Entry] : WarmSource.Recipes)
		{
			if (!WarmSource.HasBlobs(Entry.Recipe) || Entries.contains(Key))
			{
				continue;
			}
			FEntry& Pending  = Entries[Key];
			Pending.bPending = true;
			++Submitted;
		}
	}
	for (const auto& [Key, Entry] : WarmSource.Recipes)
	{
		if (!WarmSource.HasBlobs(Entry.Recipe))
		{
			continue;
		}
		const uint64 WarmKey = Key;
		WarmJobs->Submit([this, WarmKey]() { WarmOne(WarmKey, WarmSource.Recipes.at(WarmKey).Recipe); });
	}
	E_LOG(LogD3D12, Display, "[PSO 캐시] 워밍 시작: PSO {}개 (작업 스레드 {})", Submitted, WarmJobs->GetWorkerCount());
}

ID3D12RootSignature* FD3D12PipelineCache::GetWarmRootSignature(uint64 Hash)
{
	{
		std::lock_guard Lock(Mutex);
		if (const auto It = WarmRootSignatures.find(Hash); It != WarmRootSignatures.end())
		{
			return It->second.Get();
		}
	}
	const std::vector<uint8>&   Blob = WarmSource.Blobs.at(Hash);
	ComPtr<ID3D12RootSignature> RootSignature;
	if (FAILED(Device->CreateRootSignature(0, Blob.data(), Blob.size(), IID_PPV_ARGS(&RootSignature))))
	{
		return nullptr;
	}
	std::lock_guard Lock(Mutex);
	auto [It, bInserted] = WarmRootSignatures.try_emplace(Hash, RootSignature);
	return It->second.Get();
}

void FD3D12PipelineCache::WarmOne(uint64 Key, const PipelineCache::FRecipe& Recipe)
{
	const auto                  Start = FClock::now();
	ComPtr<ID3D12PipelineState> Pipeline;
	// 워밍은 드라이버 캐시(파이프라인 라이브러리)를 읽지 않고 항상 Create*PipelineState로 만든다 (드라이버 자체 셰이더 캐시는 쓴다).
	// 2026-10-04 측정: 작업 스레드가 블롭으로 만든 루트 시그니처로 라이브러리 Load한 PSO를 쓰면(또는 같은 이름을 나중에 메인 스레드가
	// 다시 Load해도) Demo_Materials/Decals/Terrain 화면이 실행마다 0.1~0.8% 픽셀 달라졌다 (파티클·반투명·조명 미세 차이, MD5가 실행마다 바뀜).
	// 라이브러리만(--no-pso-warm) 또는 워밍만(라이브러리 없음)은 각각 비트 동일 — 드라이버 내부 원인은 미확인, 조합을 피한다
	ID3D12RootSignature*        RootSignature = GetWarmRootSignature(Recipe.RootSignatureHash);
	if (RootSignature != nullptr)
	{
		if (Recipe.Type == PipelineCache::EPipelineType::Graphics)
		{
			std::vector<D3D12_INPUT_ELEMENT_DESC>    Elements;
			const D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc = PipelineCache::ToGraphicsDesc(
				Recipe, RootSignature, BlobBytecode(WarmSource, Recipe.Shaders[0]), BlobBytecode(WarmSource, Recipe.Shaders[1]), Elements);
			if (FAILED(Device->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&Pipeline))))

			{
				Pipeline.Reset();
			}
		}
		else
		{
			const D3D12_COMPUTE_PIPELINE_STATE_DESC Desc = PipelineCache::ToComputeDesc(Recipe, RootSignature, BlobBytecode(WarmSource, Recipe.Shaders[0]));
			if (FAILED(Device->CreateComputePipelineState(&Desc, IID_PPV_ARGS(&Pipeline))))

			{
				Pipeline.Reset();
			}
		}
	}
	{
		std::lock_guard Lock(Mutex);
		FEntry& Entry = Entries[Key];
		if (Entry.bPending) // 그사이 요청 스레드가 직접 만들었으면 그대로 둔다
		{
			Entry.Pipeline      = Pipeline;
			Entry.RootSignature = RootSignature;
			Entry.bPending      = false;
		}
		if (Pipeline)
		{
			++Stats.WarmCreated;
			bLibraryDirty |= !LibraryKeys.contains(Key); // 라이브러리에 없던 PSO면 종료 때 다시 쓴다
		}
		Stats.WarmMs += ElapsedMs(Start);
	}
	PendingDone.notify_all();
}

void FD3D12PipelineCache::RegisterRootSignature(ID3D12RootSignature* RootSignature, const void* Blob, size_t Size)
{
	if (Device == nullptr || RootSignature == nullptr)
	{
		return;
	}
	const uint64    Hash = PipelineCache::HashBytes(Blob, Size);
	std::lock_guard Lock(Mutex);
	RootSignatureHashes[RootSignature] = Hash;
	SessionRecipes.Blobs.try_emplace(Hash, static_cast<const uint8*>(Blob), static_cast<const uint8*>(Blob) + Size);
}

void FD3D12PipelineCache::UnregisterRootSignature(ID3D12RootSignature* RootSignature)
{
	std::lock_guard Lock(Mutex);
	RootSignatureHashes.erase(RootSignature);
}

HRESULT FD3D12PipelineCache::CreateGraphics(ID3D12Device* InDevice, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc, ComPtr<ID3D12PipelineState>& Out)
{
	uint64 RootHash = 0;
	bool   bKnownRoot = false;
	if (Device != nullptr && InDevice == Device && PipelineCache::IsCacheable(Desc))
	{
		std::lock_guard Lock(Mutex);
		if (const auto It = RootSignatureHashes.find(Desc.pRootSignature); It != RootSignatureHashes.end())
		{
			RootHash   = It->second;
			bKnownRoot = true;
		}
	}
	if (!bKnownRoot)
	{
		if (Device != nullptr)
		{
			std::lock_guard Lock(Mutex);
			++Stats.Bypassed;
		}
		return InDevice->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(&Out));
	}
	return Request(InDevice, PipelineCache::FromDesc(Desc, RootHash), Desc.pRootSignature, &Desc, Out);
}

HRESULT FD3D12PipelineCache::CreateCompute(ID3D12Device* InDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc, ComPtr<ID3D12PipelineState>& Out)
{
	uint64 RootHash   = 0;
	bool   bKnownRoot = false;
	if (Device != nullptr && InDevice == Device && PipelineCache::IsCacheable(Desc))
	{
		std::lock_guard Lock(Mutex);
		if (const auto It = RootSignatureHashes.find(Desc.pRootSignature); It != RootSignatureHashes.end())
		{
			RootHash   = It->second;
			bKnownRoot = true;
		}
	}
	if (!bKnownRoot)
	{
		if (Device != nullptr)
		{
			std::lock_guard Lock(Mutex);
			++Stats.Bypassed;
		}
		return InDevice->CreateComputePipelineState(&Desc, IID_PPV_ARGS(&Out));
	}
	return Request(InDevice, PipelineCache::FromDesc(Desc, RootHash), Desc.pRootSignature, &Desc, Out);
}

HRESULT FD3D12PipelineCache::CreateDirect(ID3D12Device* InDevice, const PipelineCache::FRecipe& Recipe, const void* Desc, ComPtr<ID3D12PipelineState>& Out)
{
	return Recipe.Type == PipelineCache::EPipelineType::Graphics
		       ? InDevice->CreateGraphicsPipelineState(static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(Desc), IID_PPV_ARGS(&Out))
		       : InDevice->CreateComputePipelineState(static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(Desc), IID_PPV_ARGS(&Out));
}

void FD3D12PipelineCache::RecordBlobs(const PipelineCache::FRecipe& Recipe, const void* Desc)
{
	// 호출자가 Mutex를 잡고 있다. 루트 시그니처 블롭은 등록 때 SessionRecipes.Blobs에 들어가 있다
	const D3D12_SHADER_BYTECODE Codes[2] = {
		Recipe.Type == PipelineCache::EPipelineType::Graphics ? static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(Desc)->VS
		                                                      : static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(Desc)->CS,
		Recipe.Type == PipelineCache::EPipelineType::Graphics ? static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(Desc)->PS
		                                                      : D3D12_SHADER_BYTECODE{},
	};
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		if (Recipe.Shaders[Index].Size > 0)
		{
			const uint8* Bytes = static_cast<const uint8*>(Codes[Index].pShaderBytecode);
			SessionRecipes.Blobs.try_emplace(Recipe.Shaders[Index].Hash, Bytes, Bytes + Codes[Index].BytecodeLength);
		}
	}
}

HRESULT FD3D12PipelineCache::Request(ID3D12Device* InDevice, PipelineCache::FRecipe&& Recipe, ID3D12RootSignature* RootSignature, const void* Desc,
                                     ComPtr<ID3D12PipelineState>& Out)
{
	const auto   Start = FClock::now();
	const uint64 Key   = PipelineCache::ComputeKey(Recipe);
	{
		std::unique_lock Lock(Mutex);
		++Stats.Requests;
		auto It = Entries.find(Key);
		if (It != Entries.end() && It->second.bPending)
		{
			++Stats.WarmWaits;
			PendingDone.wait(Lock, [&]() { return !Entries[Key].bPending; });
			It = Entries.find(Key);
		}
		// 워밍 PSO는 같은 루트 시그니처 객체로 만들었을 때만 (D3D12 런타임은 같은 블롭이면 같은 객체를 돌려주지만 보장하지 않는다)
		if (It != Entries.end() && It->second.Pipeline && It->second.RootSignature == RootSignature)
		{
			Out = It->second.Pipeline;
			++Stats.WarmHits;
			It->second.bRequested = true;
			SessionRecipes.Recipes[Key] = { std::move(Recipe), CurrentRun };
			RecordBlobs(SessionRecipes.Recipes[Key].Recipe, Desc);
			Stats.RequestMs += ElapsedMs(Start);
			return S_OK;
		}
	}

	// 드라이버 캐시 → 새로 만들기
	HRESULT      Result       = E_FAIL;
	bool         bFromLibrary = false;
	const std::wstring Name   = PipelineCache::MakeLibraryName(Key);
	if (Library && LibraryKeys.contains(Key))
	{
		std::lock_guard Lock(LibraryMutex);
		Result = Recipe.Type == PipelineCache::EPipelineType::Graphics
			         ? Library->LoadGraphicsPipeline(Name.c_str(), static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(Desc), IID_PPV_ARGS(&Out))
			         : Library->LoadComputePipeline(Name.c_str(), static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(Desc), IID_PPV_ARGS(&Out));
		bFromLibrary = SUCCEEDED(Result);
	}
	double CreateMs = 0.0;
	if (!bFromLibrary)
	{
		const auto CreateStart = FClock::now();
		Result                 = CreateDirect(InDevice, Recipe, Desc, Out);
		CreateMs               = ElapsedMs(CreateStart);
		if (FAILED(Result))
		{
			return Result;
		}
	}

	std::lock_guard Lock(Mutex);
	FEntry& Entry       = Entries[Key];
	Entry.Pipeline      = Out;
	Entry.RootSignature = RootSignature;
	Entry.bPending      = false;
	Entry.bRequested    = true;
	if (bFromLibrary)
	{
		++Stats.LibraryHits;
	}
	else
	{
		++Stats.Created;
		Stats.CreateMs += CreateMs;
		bLibraryDirty = true;
	}
	SessionRecipes.Recipes[Key] = { std::move(Recipe), CurrentRun };
	RecordBlobs(SessionRecipes.Recipes[Key].Recipe, Desc);
	Stats.RequestMs += ElapsedMs(Start);
	return S_OK;
}

void FD3D12PipelineCache::SaveLibrary()
{
	if (!Device1 || !(bLibraryDirty || bLibraryInvalidated))
	{
		return; // 바뀐 것 없음 (디스크 캐시 그대로)
	}
	// 이번 실행에 있는 PSO(요청 + 워밍)만으로 새 라이브러리 → 낡은 항목(옛 바이트코드)이 쌓이지 않는다
	ComPtr<ID3D12PipelineLibrary> Fresh;
	if (FAILED(Device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&Fresh))))
	{
		return;
	}
	uint32              Stored = 0;
	std::vector<uint64> StoredKeys;
	StoredKeys.reserve(Entries.size());
	for (const auto& [Key, Entry] : Entries)
	{
		if (Entry.Pipeline && SUCCEEDED(Fresh->StorePipeline(PipelineCache::MakeLibraryName(Key).c_str(), Entry.Pipeline.Get())))
		{
			++Stored;
			StoredKeys.push_back(Key);
		}
	}
	const SIZE_T       Size = Fresh->GetSerializedSize();
	std::vector<uint8> Data(Size);
	if (Size == 0 || FAILED(Fresh->Serialize(Data.data(), Size)))
	{
		return;
	}
	// 블롭 뒤에 저장된 키 목록 (불러올 때 있는 이름만 Load)
	const uint32 KeyCount = static_cast<uint32>(StoredKeys.size());
	const size_t KeysAt   = Data.size();
	Data.resize(KeysAt + sizeof(uint32) + StoredKeys.size() * sizeof(uint64));
	std::memcpy(Data.data() + KeysAt, &KeyCount, sizeof(uint32));
	if (!StoredKeys.empty())
	{
		std::memcpy(Data.data() + KeysAt + sizeof(uint32), StoredKeys.data(), StoredKeys.size() * sizeof(uint64));
	}
	PipelineCache::FLibraryHeader Header = CurrentHeader;
	Header.DataSize                      = Size;
	if (WriteDiskFile(Options.UserDirectory / L"PipelineLibrary.bin", &Header, sizeof(Header), Data.data(), Data.size()))
	{
		E_LOG(LogD3D12, Display, "[PSO 캐시] 드라이버 캐시 저장: PSO {}개, {:.1f} KB", Stored, static_cast<double>(Size) / 1024.0);
	}
}

void FD3D12PipelineCache::SaveRecipes()
{
	UserRecipes.Merge(SessionRecipes);
	// 워밍만 되고 요청되지 않은 레시피는 나이가 늘어난다 (LastUsedRun 그대로)
	UserRecipes.RunCounter = CurrentRun;
	const uint32 Pruned    = UserRecipes.Prune(CurrentRun, RecipeMaxAgeRuns);
	const std::vector<uint8> Bytes = UserRecipes.Write();
	WriteDiskFile(Options.UserDirectory / L"PipelineRecipes.epso", nullptr, 0, Bytes.data(), Bytes.size());
	if (Pruned > 0)
	{
		E_LOG(LogD3D12, Display, "[PSO 캐시] 오래 안 쓴 레시피 {}개 정리", Pruned);
	}

	if (Options.bRecordProject && !Options.ProjectRecipeFile.empty())
	{
		// 프로젝트 파일: 이번 실행 레시피를 합친다 (나이 관리 없음 — 다시 만들려면 파일을 지우고 기록)
		PipelineCache::FRecipeFile Project;
		std::vector<uint8>         Existing;
		if (ReadDiskFile(Options.ProjectRecipeFile, Existing))
		{
			Project.Read(Existing);
		}
		Project.Merge(SessionRecipes);
		for (auto& [Key, Entry] : Project.Recipes)
		{
			Entry.LastUsedRun = 0;
		}
		Project.RunCounter = 0;
		Project.Prune(0, ~0u); // 쓰이지 않는 블롭만 제거
		const std::vector<uint8> ProjectBytes = Project.Write();
		if (WriteDiskFile(Options.ProjectRecipeFile, nullptr, 0, ProjectBytes.data(), ProjectBytes.size()))
		{
			E_LOG(LogD3D12, Display, "[PSO 캐시] 프로젝트 레시피 기록: {}개 → {}", Project.Recipes.size(), FStringConv::ToUtf8(Options.ProjectRecipeFile.wstring()));
		}
	}
}

void FD3D12PipelineCache::Shutdown()
{
	if (Device == nullptr)
	{
		return;
	}
	if (WarmJobs)
	{
		WarmJobs->WaitIdle();
		WarmJobs->Shutdown();
		WarmJobs.reset();
	}
	LogStats("종료");
	SaveLibrary();
	SaveRecipes();

	std::lock_guard Lock(Mutex);
	Entries.clear();
	RootSignatureHashes.clear();
	WarmRootSignatures.clear();
	Library.Reset();
	LibraryData.clear();
	UserRecipes    = {};
	SessionRecipes = {};
	WarmSource     = {};
	Device1.Reset();
	Device              = nullptr;
	bLibraryDirty       = false;
	bLibraryInvalidated = false;
	bLibraryFromDisk    = false;
}

FD3D12PipelineCache::FStats FD3D12PipelineCache::GetStats() const
{
	std::lock_guard Lock(Mutex);
	return Stats;
}

void FD3D12PipelineCache::LogStats(const char* When) const
{
	const FStats S = GetStats();
	E_LOG(LogD3D12, Display,
	      "[PSO 캐시] {}: 요청 {} (메모리 {} — 워밍 대기 {}, 드라이버 캐시 {}, 새로 컴파일 {} {:.1f}ms), 우회 {}, 요청 스레드 총 {:.1f}ms / 워밍 스레드 PSO {} {:.1f}ms",
	      When, S.Requests, S.WarmHits, S.WarmWaits, S.LibraryHits, S.Created, S.CreateMs, S.Bypassed, S.RequestMs, S.WarmCreated,
	      S.WarmMs);
}
