#include "Renderer/RenderGraph/RenderGraph.h"

#include "Core/Profiling.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

E_DECLARE_LOG_CATEGORY(LogRenderer)

D3D12_RESOURCE_STATES RenderGraphD3D12::ToD3D12(ERGAccess Access)
{
	D3D12_RESOURCE_STATES States = D3D12_RESOURCE_STATE_COMMON;
	const uint32          Bits   = RGAccess::Bits(Access);
	const auto            Add    = [&](ERGAccess Flag, D3D12_RESOURCE_STATES State) {
        if ((Bits & RGAccess::Bits(Flag)) != 0)
        {
            States |= State;
        }
	};
	Add(ERGAccess::RenderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET);
	Add(ERGAccess::DepthWrite, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	Add(ERGAccess::DepthRead, D3D12_RESOURCE_STATE_DEPTH_READ);
	Add(ERGAccess::SrvPixel, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	Add(ERGAccess::SrvNonPixel, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Add(ERGAccess::Uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Add(ERGAccess::CopySource, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Add(ERGAccess::CopyDest, D3D12_RESOURCE_STATE_COPY_DEST);
	Add(ERGAccess::IndirectArgs, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	Add(ERGAccess::AccelStructRead, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	Add(ERGAccess::AccelStructWrite, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	return States; // Present/Common = 0
}

ERGAccess RenderGraphD3D12::FromD3D12(D3D12_RESOURCE_STATES States)
{
	if (States == D3D12_RESOURCE_STATE_COMMON)
	{
		return ERGAccess::Common;
	}
	if (States == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
	{
		return ERGAccess::AccelStructRead; // 가속 구조는 이 상태 하나뿐 (읽기로 본다 — 빌드는 AccelStructWrite 선언)
	}
	ERGAccess  Access = ERGAccess::None;
	const auto Add    = [&](D3D12_RESOURCE_STATES State, ERGAccess Flag) {
        if ((States & State) == State)
        {
            Access |= Flag;
        }
	};
	Add(D3D12_RESOURCE_STATE_RENDER_TARGET, ERGAccess::RenderTarget);
	Add(D3D12_RESOURCE_STATE_DEPTH_WRITE, ERGAccess::DepthWrite);
	Add(D3D12_RESOURCE_STATE_DEPTH_READ, ERGAccess::DepthRead);
	Add(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, ERGAccess::SrvPixel);
	Add(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ERGAccess::SrvNonPixel);
	Add(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, ERGAccess::Uav);
	Add(D3D12_RESOURCE_STATE_COPY_SOURCE, ERGAccess::CopySource);
	Add(D3D12_RESOURCE_STATE_COPY_DEST, ERGAccess::CopyDest);
	Add(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, ERGAccess::IndirectArgs);
	return Access;
}

void FRGContext::UavBarrier(FRGResourceRef Resource) const
{
	if (Graph != nullptr && CommandList != nullptr)
	{
		const D3D12_RESOURCE_BARRIER Barrier = MakeUavBarrier(Graph->GetResource(Resource));
		CommandList->ResourceBarrier(1, &Barrier);
	}
}

// ---------------------------------------------------------------- 풀

bool FRGTextureDesc::operator==(const FRGTextureDesc& Other) const
{
	return Width == Other.Width && Height == Other.Height && DepthOrArraySize == Other.DepthOrArraySize && MipCount == Other.MipCount &&
	       Format == Other.Format && b3D == Other.b3D && bRenderTarget == Other.bRenderTarget && bUnorderedAccess == Other.bUnorderedAccess &&
	       std::memcmp(ClearColor, Other.ClearColor, sizeof(ClearColor)) == 0;
}

uint64 FRGPooledTexture::GetSizeBytes() const
{
	if (!Resource)
	{
		return 0;
	}
	ComPtr<ID3D12Device> Device;
	Resource->GetDevice(IID_PPV_ARGS(&Device));
	const D3D12_RESOURCE_DESC          ResourceDesc = Resource->GetDesc();
	const D3D12_RESOURCE_ALLOCATION_INFO Info       = Device->GetResourceAllocationInfo(0, 1, &ResourceDesc);
	return Info.SizeInBytes;
}

FRGResourcePool::~FRGResourcePool()
{
	Shutdown();
}

void FRGResourcePool::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	for (std::unique_ptr<FRGPooledTexture>& Texture : Textures)
	{
		// 호출자가 GPU Flush 뒤 부른다 (FSceneRenderer::Shutdown) — 지연 해제로 넘겨도 안전
		Rhi->DeferRelease(Texture->Resource);
		Rhi->DeferFreeDescriptor(Texture->Srv);
		for (const FD3D12DescriptorHandle& Uav : Texture->Uavs)
		{
			Rhi->DeferFreeDescriptor(Uav);
		}
	}
	Textures.clear();
	Rhi = nullptr;
}

uint64 FRGResourcePool::GetTotalBytes() const
{
	uint64 Bytes = 0;
	for (const std::unique_ptr<FRGPooledTexture>& Texture : Textures)
	{
		Bytes += Texture->GetSizeBytes();
	}
	return Bytes;
}

FRGPooledTexture* FRGResourcePool::Acquire(const FRGTextureDesc& Desc, const char* Name)
{
	E_CHECKF(Rhi != nullptr, "렌더 그래프 풀이 초기화되지 않았습니다");
	for (std::unique_ptr<FRGPooledTexture>& Texture : Textures)
	{
		if (!Texture->bInUse && Texture->Desc == Desc)
		{
			Texture->bInUse        = true;
			Texture->LastUsedFrame = Rhi->GetFrameNumber();
			return Texture.get();
		}
	}
	auto Texture  = std::make_unique<FRGPooledTexture>();
	Texture->Desc = Desc;
	if (!CreateTexture(*Texture, Name))
	{
		E_LOG(LogRenderer, Fatal, "렌더 그래프 텍스처 생성 실패: {} ({}x{})", Name, Desc.Width, Desc.Height);
	}
	Texture->bInUse        = true;
	Texture->LastUsedFrame = Rhi->GetFrameNumber();
	Textures.push_back(std::move(Texture));
	return Textures.back().get();
}

bool FRGResourcePool::CreateTexture(FRGPooledTexture& Texture, const char* Name)
{
	const FRGTextureDesc& Desc   = Texture.Desc;
	ID3D12Device*         Device = Rhi->GetDevice().GetDevice();

	D3D12_RESOURCE_DESC ResourceDesc{};
	ResourceDesc.Dimension        = Desc.b3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	ResourceDesc.Width            = Desc.Width;
	ResourceDesc.Height           = Desc.Height;
	ResourceDesc.DepthOrArraySize = Desc.DepthOrArraySize;
	ResourceDesc.MipLevels        = Desc.MipCount;
	ResourceDesc.Format           = Desc.Format;
	ResourceDesc.SampleDesc       = { 1, 0 };
	ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	ResourceDesc.Flags            = (Desc.bRenderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE) |
	                     (Desc.bUnorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);

	D3D12_CLEAR_VALUE ClearValue{};
	ClearValue.Format = Desc.Format;
	std::memcpy(ClearValue.Color, Desc.ClearColor, sizeof(ClearValue.Color));
	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	// 시작 상태 = 셰이더 리소스 (풀 기본 상태 — 그래프가 첫 사용 전에 전이)
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &ResourceDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	                                           Desc.bRenderTarget ? &ClearValue : nullptr, IID_PPV_ARGS(&Texture.Resource))))
	{
		return false;
	}
	Texture.Resource->SetName(std::format(L"RG_{}", FStringConv::ToWide(Name)).c_str());
	const uint32 SubresourceCount = static_cast<uint32>(Desc.MipCount) * (Desc.b3D ? 1u : Desc.DepthOrArraySize);
	Texture.States.assign(SubresourceCount, ERGAccess::SrvPixel);

	FD3D12DescriptorAllocator&      Allocator = Rhi->GetSrvAllocator();
	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                  = Desc.Format;
	SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	if (Desc.b3D)
	{
		SrvDesc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE3D;
		SrvDesc.Texture3D.MipLevels = Desc.MipCount;
	}
	else if (Desc.DepthOrArraySize > 1)
	{
		SrvDesc.ViewDimension                  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		SrvDesc.Texture2DArray.MipLevels       = Desc.MipCount;
		SrvDesc.Texture2DArray.ArraySize       = Desc.DepthOrArraySize;
	}
	else
	{
		SrvDesc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
		SrvDesc.Texture2D.MipLevels = Desc.MipCount;
	}
	Texture.Srv = Allocator.Allocate();
	Device->CreateShaderResourceView(Texture.Resource.Get(), &SrvDesc, Texture.Srv.Cpu);

	if (Desc.bUnorderedAccess)
	{
		for (uint32 Mip = 0; Mip < Desc.MipCount; ++Mip)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc{};
			UavDesc.Format = Desc.Format;
			if (Desc.b3D)
			{
				UavDesc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE3D;
				UavDesc.Texture3D.MipSlice = Mip;
				UavDesc.Texture3D.WSize    = static_cast<UINT>(-1);
			}
			else if (Desc.DepthOrArraySize > 1)
			{
				UavDesc.ViewDimension                 = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
				UavDesc.Texture2DArray.MipSlice       = Mip;
				UavDesc.Texture2DArray.ArraySize      = Desc.DepthOrArraySize;
			}
			else
			{
				UavDesc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
				UavDesc.Texture2D.MipSlice = Mip;
			}
			const FD3D12DescriptorHandle Uav = Allocator.Allocate();
			Device->CreateUnorderedAccessView(Texture.Resource.Get(), nullptr, &UavDesc, Uav.Cpu);
			Texture.Uavs.push_back(Uav);
		}
	}
	if (Desc.bRenderTarget)
	{
		if (!Texture.RtvHeap.Init(Device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, L"RenderGraphRtvHeap"))
		{
			return false;
		}
		D3D12_RENDER_TARGET_VIEW_DESC RtvDesc{};
		RtvDesc.Format        = Desc.Format;
		RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		Device->CreateRenderTargetView(Texture.Resource.Get(), &RtvDesc, Texture.RtvHeap.GetCpuHandle(0));
	}
	return true;
}

namespace
{
	uint64 HashKey(const std::vector<uint32>& Key)
	{
		uint64 Hash = 1469598103934665603ull; // FNV-1a
		for (const uint32 Value : Key)
		{
			Hash = (Hash ^ Value) * 1099511628211ull;
		}
		return Hash;
	}
} // namespace

std::shared_ptr<const FRGCompileResult> FRGResourcePool::FindCompiled(const std::vector<uint32>& Key)
{
	const uint64 Hash = HashKey(Key);
	for (size_t Index = 0; Index < CompileCache.size(); ++Index)
	{
		if (CompileCache[Index].Hash == Hash && CompileCache[Index].Key == Key)
		{
			if (Index > 0)
			{
				std::rotate(CompileCache.begin(), CompileCache.begin() + static_cast<std::ptrdiff_t>(Index),
				            CompileCache.begin() + static_cast<std::ptrdiff_t>(Index + 1)); // 앞으로
			}
			return CompileCache.front().Result;
		}
	}
	return nullptr;
}

void FRGResourcePool::StoreCompiled(std::vector<uint32> Key, std::shared_ptr<const FRGCompileResult> Result)
{
	if (CompileCache.size() >= MaxCachedCompiles)
	{
		CompileCache.pop_back();
	}
	FCachedCompile Entry;
	Entry.Hash   = HashKey(Key);
	Entry.Key    = std::move(Key);
	Entry.Result = std::move(Result);
	CompileCache.insert(CompileCache.begin(), std::move(Entry));
}

void FRGResourcePool::ReleaseAll()
{
	for (std::unique_ptr<FRGPooledTexture>& Texture : Textures)
	{
		Texture->bInUse = false;
	}
}

void FRGResourcePool::Trim(uint64 FrameNumber, uint64 UnusedFrames)
{
	std::erase_if(Textures, [&](std::unique_ptr<FRGPooledTexture>& Texture) {
		if (Texture->bInUse || Texture->LastUsedFrame + UnusedFrames >= FrameNumber)
		{
			return false;
		}
		Rhi->DeferRelease(Texture->Resource); // 마지막으로 쓴 프레임이 아직 GPU에 있을 수 있다
		Rhi->DeferFreeDescriptor(Texture->Srv);
		for (const FD3D12DescriptorHandle& Uav : Texture->Uavs)
		{
			Rhi->DeferFreeDescriptor(Uav);
		}
		return true;
	});
}

// ---------------------------------------------------------------- 그래프

FRenderGraph::FRenderGraph(FD3D12RHI& InRhi, FRGResourcePool& InPool, const char* InName) : Rhi(InRhi), Pool(InPool), Name(InName)
{
	Resources.reserve(64);
	Passes.reserve(64);
}

FRenderGraph::~FRenderGraph()
{
	if (bCompiled && !bExecuted)
	{
		Pool.ReleaseAll();
	}
}

FRGResourceRef FRenderGraph::Import(const char* InName, ID3D12Resource* Resource, ERGAccess InitialState, ERGAccess FinalState, uint32 MipCount,
                                    uint32 ArraySize)
{
	E_CHECKF(Resource != nullptr, "렌더 그래프: 빈 리소스를 가져올 수 없습니다 ({})", InName);
	if (const FRGResourceRef Existing = FindImported(Resource); Existing.IsValid())
	{
		return Existing; // 같은 리소스를 두 번 가져오면 상태 추적이 갈라진다
	}
	FResource& Entry = Resources.emplace_back();
	Entry.Name       = InName;
	Entry.Resource   = Resource;
	Entry.bImported  = true;
	Entry.MipCount   = std::max(1u, MipCount);
	Entry.ArraySize  = std::max(1u, ArraySize);
	Entry.InitialStates.assign(static_cast<size_t>(Entry.MipCount) * Entry.ArraySize, InitialState);
	Entry.FinalState = FinalState;
	return { static_cast<uint32>(Resources.size() - 1) };
}

FRGResourceRef FRenderGraph::ImportPerSubresource(const char* InName, ID3D12Resource* Resource, const std::vector<ERGAccess>& InitialStates,
                                                  ERGAccess FinalState, uint32 MipCount, uint32 ArraySize)
{
	const FRGResourceRef Ref = Import(InName, Resource, InitialStates.empty() ? ERGAccess::Common : InitialStates.front(), FinalState, MipCount, ArraySize);
	FResource&           Entry = Resources[Ref.Id];
	for (size_t Index = 0; Index < Entry.InitialStates.size() && Index < InitialStates.size(); ++Index)
	{
		Entry.InitialStates[Index] = InitialStates[Index];
	}
	return Ref;
}

FRGResourceRef FRenderGraph::ImportTracked(const char* InName, ID3D12Resource* Resource, D3D12_RESOURCE_STATES* TrackedState, uint32 MipCount,
                                           uint32 ArraySize)
{
	E_CHECKF(TrackedState != nullptr, "렌더 그래프: 추적 상태 포인터가 없습니다 ({})", InName);
	const FRGResourceRef Ref = Import(InName, Resource, RenderGraphD3D12::FromD3D12(*TrackedState), ERGAccess::None, MipCount, ArraySize);
	Resources[Ref.Id].TrackedState = TrackedState;
	return Ref;
}

FRGResourceRef FRenderGraph::ImportColor(const char* InName, const FD3D12RenderTarget& Target)
{
	return Import(InName, Target.GetColorResource(), ERGAccess::SrvPixel, ERGAccess::SrvPixel);
}

FRGResourceRef FRenderGraph::ImportDepth(const char* InName, const FD3D12RenderTarget& Target)
{
	return Import(InName, Target.GetDepthResource(), ERGAccess::DepthWrite, ERGAccess::DepthWrite);
}

FRGResourceRef FRenderGraph::FindImported(ID3D12Resource* Resource) const
{
	for (uint32 Index = 0; Index < Resources.size(); ++Index)
	{
		if (Resources[Index].bImported && Resources[Index].Resource == Resource)
		{
			return { Index };
		}
	}
	return {};
}

FRGResourceRef FRenderGraph::CreateTexture(const char* InName, const FRGTextureDesc& Desc)
{
	FRGPooledTexture* Texture = Pool.Acquire(Desc, InName);
	FResource&        Entry   = Resources.emplace_back();
	Entry.Name                = InName;
	Entry.Resource            = Texture->Resource.Get();
	Entry.Pooled              = Texture;
	Entry.MipCount            = Desc.MipCount;
	Entry.ArraySize           = Desc.b3D ? 1u : Desc.DepthOrArraySize;
	Entry.InitialStates       = Texture->States;
	return { static_cast<uint32>(Resources.size() - 1) };
}

const FRGPooledTexture* FRenderGraph::GetTexture(FRGResourceRef Resource) const
{
	return Resource.IsValid() && Resource.Id < Resources.size() ? Resources[Resource.Id].Pooled : nullptr;
}

ID3D12Resource* FRenderGraph::GetResource(FRGResourceRef Resource) const
{
	return Resource.IsValid() && Resource.Id < Resources.size() ? Resources[Resource.Id].Resource : nullptr;
}

FRenderGraph::FPassBuilder FRenderGraph::AddPass(const char* InName, ERGQueue Queue)
{
	E_CHECKF(!bCompiled, "렌더 그래프: 컴파일 뒤에는 패스를 추가할 수 없습니다");
	FPass& Pass = Passes.emplace_back();
	Pass.Name   = InName;
	Pass.Queue  = Queue;
	return FPassBuilder(*this, static_cast<uint32>(Passes.size() - 1));
}

FRenderGraph::FPassBuilder& FRenderGraph::FPassBuilder::Read(FRGResourceRef Resource, ERGAccess Access, FRGSubresourceRange Range)
{
	E_CHECKF(Resource.IsValid(), "렌더 그래프: 무효 리소스 읽기 ({})", Graph->Passes[Index].Name);
	Graph->Passes[Index].Accesses.push_back({ Resource.Id, Range, Access, false });
	return *this;
}

FRenderGraph::FPassBuilder& FRenderGraph::FPassBuilder::Write(FRGResourceRef Resource, ERGAccess Access, FRGSubresourceRange Range, bool bOverwrite)
{
	E_CHECKF(Resource.IsValid(), "렌더 그래프: 무효 리소스 쓰기 ({})", Graph->Passes[Index].Name);
	Graph->Passes[Index].Accesses.push_back({ Resource.Id, Range, Access, bOverwrite });
	return *this;
}

FRenderGraph::FPassBuilder& FRenderGraph::FPassBuilder::NeverCull()
{
	Graph->Passes[Index].bNeverCull = true;
	return *this;
}

FRenderGraph::FPassBuilder& FRenderGraph::FPassBuilder::Timer(int32 TimerId)
{
	Graph->Passes[Index].Timer = TimerId;
	return *this;
}

FRenderGraph::FPassBuilder& FRenderGraph::FPassBuilder::Execute(FRGExecuteFn Function)
{
	Graph->Passes[Index].Function = std::move(Function);
	return *this;
}

void FRenderGraph::Compile(const FRGCompileOptions& Options)
{
	E_PROFILE_SCOPE("렌더 그래프 컴파일");
	const auto Start = std::chrono::steady_clock::now();

	// 캐시 키: 컴파일 결과를 정하는 입력 전부 (리소스 서브리소스 수·가져옴·시작/끝 상태, 패스 큐·컬링 금지·접근, 옵션)
	std::vector<uint32> Key;
	Key.reserve(256);
	Key.push_back((Options.bCullPasses ? 1u : 0u) | (Options.bAsyncCompute ? 2u : 0u) | (Options.bMergeAsyncBatches ? 4u : 0u));
	Key.push_back(static_cast<uint32>(Resources.size()));
	for (const FResource& Resource : Resources)
	{
		Key.push_back(Resource.MipCount);
		Key.push_back(Resource.ArraySize);
		Key.push_back((Resource.bImported ? 1u : 0u) | (Resource.TrackedState != nullptr ? 2u : 0u));
		Key.push_back(RGAccess::Bits(Resource.FinalState));
		for (const ERGAccess State : Resource.InitialStates)
		{
			Key.push_back(RGAccess::Bits(State));
		}
	}
	Key.push_back(static_cast<uint32>(Passes.size()));
	for (const FPass& Pass : Passes)
	{
		Key.push_back(static_cast<uint32>(Pass.Queue) | (Pass.bNeverCull ? 0x100u : 0u));
		Key.push_back(static_cast<uint32>(Pass.Accesses.size()));
		for (const FRGCompileAccess& Access : Pass.Accesses)
		{
			Key.push_back(Access.Resource);
			Key.push_back(RGAccess::Bits(Access.Access) | (Access.bOverwrite ? 0x80000000u : 0u));
			Key.push_back(Access.Range.FirstMip);
			Key.push_back(Access.Range.MipCount);
			Key.push_back(Access.Range.FirstSlice);
			Key.push_back(Access.Range.SliceCount);
		}
	}
	CompiledPtr        = CompileCacheEnabled ? Pool.FindCompiled(Key) : nullptr;
	const bool bCached = CompiledPtr != nullptr;
	if (!bCached)
	{
		CompileUncached(Options);
		if (CompileCacheEnabled && CompiledPtr->Errors.empty())
		{
			Pool.StoreCompiled(std::move(Key), CompiledPtr);
		}
	}
	bCompiled                         = true;
	const FRGCompileResult& Compiled  = *CompiledPtr;
	for (const std::string& Error : Compiled.Errors)
	{
		E_LOG(LogRenderer, Error, "렌더 그래프 '{}': {}", Name, Error);
	}

	Stats              = FRGStats{};
	Stats.Passes       = static_cast<uint32>(Passes.size());
	Stats.CulledPasses = Compiled.CulledPassCount;
	Stats.AsyncBatches = static_cast<uint32>(Compiled.Batches.size());
	for (const FRGCompiledPass& Pass : Compiled.Passes)
	{
		Stats.AsyncPasses += Pass.bAsync ? 1u : 0u;
	}
	Stats.Transitions    = Compiled.TransitionCount;
	Stats.UavBarriers    = Compiled.UavBarrierCount;
	Stats.BarrierBatches = Compiled.BarrierBatchCount;
	Stats.Resources      = static_cast<uint32>(Resources.size());
	for (const FResource& Resource : Resources)
	{
		Stats.ImportedResources += Resource.bImported ? 1u : 0u;
	}
	Stats.PooledTextures = Pool.GetTextureCount();
	Stats.bCompileCached = bCached;
	Stats.CompileMs      = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - Start).count();
}

void FRenderGraph::CompileUncached(const FRGCompileOptions& Options)
{
	std::vector<FRGCompileResource> CompileResources(Resources.size());
	for (size_t Index = 0; Index < Resources.size(); ++Index)
	{
		const FResource&    Source = Resources[Index];
		FRGCompileResource& Target = CompileResources[Index];
		Target.Name                = Source.Name.c_str();
		Target.MipCount            = Source.MipCount;
		Target.ArraySize           = Source.ArraySize;
		Target.bImported           = Source.bImported;
		Target.InitialStates       = Source.InitialStates;
		Target.InitialState        = Source.InitialStates.empty() ? ERGAccess::Common : Source.InitialStates.front();
		Target.FinalState          = Source.FinalState;
		Target.bUniformFinal       = Source.TrackedState != nullptr;
	}
	std::vector<FRGCompilePass> CompilePasses(Passes.size());
	for (size_t Index = 0; Index < Passes.size(); ++Index)
	{
		CompilePasses[Index].Name       = Passes[Index].Name.c_str();
		CompilePasses[Index].Queue      = Passes[Index].Queue;
		CompilePasses[Index].bNeverCull = Passes[Index].bNeverCull;
		CompilePasses[Index].Accesses   = Passes[Index].Accesses;
	}
	CompiledPtr = std::make_shared<const FRGCompileResult>(RenderGraphCompiler::Compile(CompileResources, CompilePasses, Options));
}

void FRenderGraph::RecordBarriers(ID3D12GraphicsCommandList* List, const std::vector<FRGBarrier>& Barriers)
{
	if (Barriers.empty())
	{
		return;
	}
	BarrierScratch.clear();
	for (const FRGBarrier& Barrier : Barriers)
	{
		ID3D12Resource* Resource = Resources[Barrier.Resource].Resource;
		if (Barrier.bUav)
		{
			BarrierScratch.push_back(MakeUavBarrier(Resource));
		}
		else
		{
			BarrierScratch.push_back(MakeTransitionBarrier(Resource, RenderGraphD3D12::ToD3D12(Barrier.Before), RenderGraphD3D12::ToD3D12(Barrier.After),
			                                               Barrier.Subresource == FRGBarrier::AllSubresources ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
			                                                                                                 : Barrier.Subresource));
		}
	}
	List->ResourceBarrier(static_cast<UINT>(BarrierScratch.size()), BarrierScratch.data());
}

void FRenderGraph::RunPass(uint32 PassIndex, ID3D12GraphicsCommandList* List, bool bCompute, int32& OpenTimer)
{
	FPass& Pass = Passes[PassIndex];
	if (Pass.Timer != OpenTimer)
	{
		if (OpenTimer >= 0 && OnTimerEnd)
		{
			OnTimerEnd(OpenTimer, List, bCompute);
		}
		OpenTimer = Pass.Timer;
		if (OpenTimer >= 0 && OnTimerBegin)
		{
			OnTimerBegin(OpenTimer, List, bCompute);
		}
	}
	RecordBarriers(List, CompiledPtr->Passes[PassIndex].Barriers);
	if (Pass.Function)
	{
		FRGContext Context;
		Context.CommandList   = List;
		Context.bAsyncCompute = bCompute;
		Context.Graph         = this;
		Pass.Function(Context);
	}
}

void FRenderGraph::Execute(ID3D12GraphicsCommandList* ExternalList)
{
	E_CHECKF(bCompiled && !bExecuted, "렌더 그래프: Compile 뒤 한 번만 Execute");
	const FRGCompileResult& Compiled = *CompiledPtr;
	E_CHECKF(ExternalList == nullptr || Compiled.Batches.empty(), "렌더 그래프 '{}': 외부 명령 목록 실행은 비동기 계산 없이 컴파일해야 합니다", Name);
	E_PROFILE_SCOPE("렌더 그래프 실행");
	bExecuted = true;

	ID3D12GraphicsCommandList* List          = ExternalList != nullptr ? ExternalList : Rhi.GetCommandList();
	int32                      OpenTimer     = -1;
	std::vector<uint64>        BatchFences(Compiled.Batches.size(), 0);
	std::vector<bool>          BatchJoined(Compiled.Batches.size(), false);
	uint32                     SubmittedBatches = 0;

	// 조인: 이 지점 전에 계산 큐를 기다려야 하는 묶음들 (지금까지의 그래픽스 명령을 먼저 제출해 겹쳐 돌게 한다)
	const auto JoinBatches = [&](int32 BeforePass) {
		uint64 WaitFence = 0;
		for (size_t Index = 0; Index < Compiled.Batches.size(); ++Index)
		{
			if (!BatchJoined[Index] && BatchFences[Index] != 0 && Compiled.Batches[Index].JoinBeforePass == BeforePass)
			{
				WaitFence          = std::max(WaitFence, BatchFences[Index]);
				BatchJoined[Index] = true;
			}
		}
		if (WaitFence != 0)
		{
			if (OpenTimer >= 0 && OnTimerEnd)
			{
				OnTimerEnd(OpenTimer, List, false); // 구간이 제출 경계를 넘지 않게
				OpenTimer = -1;
			}
			Rhi.SubmitGraphicsCommands();
			Rhi.WaitForCompute(WaitFence);
		}
	};

	for (const FRGStep& Step : Compiled.Steps)
	{
		if (!Step.bAsyncBatch)
		{
			JoinBatches(static_cast<int32>(Step.Index));
			RunPass(Step.Index, List, false, OpenTimer);
			continue;
		}
		// 포크: 묶음이 쓸 상태로 그래픽스에서 전이 → 지금까지 제출 → 계산 큐가 그 펜스를 기다린 뒤 묶음 실행
		const FRGAsyncBatch& Batch = Compiled.Batches[Step.Index];
		if (OpenTimer >= 0 && OnTimerEnd)
		{
			OnTimerEnd(OpenTimer, List, false);
			OpenTimer = -1;
		}
		RecordBarriers(List, Batch.ForkBarriers);
		const uint64               GraphicsFence = Rhi.SubmitGraphicsCommands();
		ID3D12GraphicsCommandList* ComputeList   = Rhi.BeginComputeCommands(GraphicsFence);
		int32                      ComputeTimer  = -1;
		for (const uint32 PassIndex : Batch.Passes)
		{
			RunPass(PassIndex, ComputeList, true, ComputeTimer);
		}
		if (ComputeTimer >= 0 && OnTimerEnd)
		{
			OnTimerEnd(ComputeTimer, ComputeList, true);
		}
		if (++SubmittedBatches == Compiled.Batches.size() && OnLastComputeBatchEnd)
		{
			OnLastComputeBatchEnd(ComputeList);
		}
		BatchFences[Step.Index] = Rhi.SubmitComputeCommands();
	}
	JoinBatches(-1); // 그래프 끝까지 조인하지 않은 묶음 (프레임 펜스가 계산 작업도 덮게)
	if (OpenTimer >= 0 && OnTimerEnd)
	{
		OnTimerEnd(OpenTimer, List, false);
	}
	RecordBarriers(List, Compiled.FinalBarriers);
	Pool.ReleaseAll();

	// 내부 텍스처의 끝 상태를 풀에 기억 (다음 그래프의 시작 상태), 추적 가져오기는 소유자 상태 갱신
	for (size_t Index = 0; Index < Resources.size(); ++Index)
	{
		if (Resources[Index].Pooled != nullptr)
		{
			Resources[Index].Pooled->States = Compiled.FinalStates[Index];
		}
		if (Resources[Index].TrackedState != nullptr && !Compiled.FinalStates[Index].empty())
		{
			*Resources[Index].TrackedState = RenderGraphD3D12::ToD3D12(Compiled.FinalStates[Index][0]);
		}
	}
}

ERGAccess FRenderGraph::GetFinalState(FRGResourceRef Resource, uint32 Subresource) const
{
	if (!bCompiled || !Resource.IsValid() || Resource.Id >= CompiledPtr->FinalStates.size() || Subresource >= CompiledPtr->FinalStates[Resource.Id].size())
	{
		return ERGAccess::None;
	}
	return CompiledPtr->FinalStates[Resource.Id][Subresource];
}

std::vector<std::string> FRenderGraph::Dump() const
{
	std::vector<std::string> Lines;
	if (!CompiledPtr)
	{
		return Lines;
	}
	const FRGCompileResult& Compiled = *CompiledPtr;
	Lines.push_back(std::format("[렌더 그래프] '{}': 패스 {} (제거 {}, 비동기 계산 {} / 묶음 {}), 리소스 {} (가져옴 {}, 풀 {}), 전이 {}, UAV 배리어 {}, "
	                            "배리어 호출 {}, 컴파일 {:.3f} ms{}",
	                            Name, Stats.Passes, Stats.CulledPasses, Stats.AsyncPasses, Stats.AsyncBatches, Stats.Resources, Stats.ImportedResources,
	                            Stats.PooledTextures, Stats.Transitions, Stats.UavBarriers, Stats.BarrierBatches, Stats.CompileMs,
	                            Stats.bCompileCached ? " (캐시)" : ""));
	uint32 Order = 0;
	for (uint32 StepIndex = 0; StepIndex < Compiled.Steps.size(); ++StepIndex)
	{
		const FRGStep& Step = Compiled.Steps[StepIndex];
		if (Step.bAsyncBatch)
		{
			const FRGAsyncBatch& Batch = Compiled.Batches[Step.Index];
			Lines.push_back(std::format("  ── 포크 (묶음 {}): '{}' 뒤, 포크 전이 {}, 조인 = {}", Step.Index,
			                            Batch.ForkAfterPass >= 0 ? Passes[Batch.ForkAfterPass].Name : std::string("그래프 시작"), Batch.ForkBarriers.size(),
			                            Batch.JoinBeforePass >= 0 ? "'" + Passes[Batch.JoinBeforePass].Name + "' 앞" : std::string("그래프 끝")));
			for (const uint32 PassIndex : Batch.Passes)
			{
				Lines.push_back(std::format("  {:2}. [계산] {} (배리어 {})", Order++, Passes[PassIndex].Name, Compiled.Passes[PassIndex].Barriers.size()));
			}
			continue;
		}
		Lines.push_back(std::format("  {:2}. [그래픽스] {} (배리어 {})", Order++, Passes[Step.Index].Name, Compiled.Passes[Step.Index].Barriers.size()));
	}
	std::string Culled;
	for (uint32 PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
	{
		if (Compiled.Passes[PassIndex].bCulled)
		{
			Culled += (Culled.empty() ? "" : ", ") + Passes[PassIndex].Name;
		}
	}
	Lines.push_back(std::format("  제거된 패스: {}", Culled.empty() ? "없음" : Culled));
	Lines.push_back(std::format("  끝 배리어 {}", Compiled.FinalBarriers.size()));
	for (uint32 Index = 0; Index < Resources.size(); ++Index)
	{
		const FRGLifetime& Lifetime = Compiled.Lifetimes[Index];
		const char*        Kind     = Resources[Index].bImported ? "가져옴" : "풀";
		if (Lifetime.FirstPass < 0)
		{
			Lines.push_back(std::format("  리소스 {} ({}): 사용 없음", Resources[Index].Name, Kind));
			continue;
		}
		Lines.push_back(std::format("  리소스 {} ({}, 서브리소스 {}): 칸 {}~{} ('{}' ~ '{}')", Resources[Index].Name, Kind,
		                            Resources[Index].MipCount * Resources[Index].ArraySize, Lifetime.FirstStep, Lifetime.LastStep,
		                            Passes[Lifetime.FirstPass].Name, Passes[Lifetime.LastPass].Name));
	}
	return Lines;
}
