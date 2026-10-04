#include "Renderer/SkinCache.h"

#include "Core/Jobs/ParallelFor.h"
#include "Core/Log.h"
#include "RHI/D3D12/D3D12DynamicUploadBuffer.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/MeshData.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/SkinnedMeshData.h"
#include "Renderer/StaticMesh.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	enum ESkinCacheRootParameter : uint32
	{
		Param_Constants = 0, // b0: 정점 수, 첫 항목
		Param_Base      = 1, // t0: 기본 정점 (ByteAddressBuffer, FVertex)
		Param_Skin      = 2, // t1: 스킨 스트림 (ByteAddressBuffer, FSkinVertex)
		Param_Items     = 3, // t2: 항목 표 (StructuredBuffer<FSkinCacheItem>)
		Param_Palette   = 4, // t15: 프레임 본 팔레트 (SkinnedMesh.hlsli)
		Param_Output    = 5, // u0: 캐시 (RWByteAddressBuffer)
		Param_List      = 6, // t3: 스키닝할 정점 번호 목록 (StructuredBuffer<uint>, 항등이면 미사용)
	};

	// SkinCache.hlsl FSkinCacheItem과 1:1
	struct FSkinCacheItemGpu
	{
		uint32 FirstVertex    = 0; // 영역 안 첫 정점 (정점 번호 = 첫 정점 + 메시 정점 번호)
		uint32 Capacity       = 0; // 용량 C (영역 시작 = SkinCacheMath::Get*Offset)
		uint32 BoneOffset     = 0;
		uint32 PrevBoneOffset = 0;
		uint32 RtFirstVertex  = 0; // RT 정점 영역 안 첫 정점 (RT 대상 아니면 미사용)
		uint32 Padding[3]     = {};
	};
	static_assert(sizeof(FSkinCacheItemGpu) == 32);
	static_assert(sizeof(FVertex) == SkinCacheMath::RtVertexBytes, "스킨 캐시 RT 정점 영역은 FVertex 그대로 (레이 트레이싱 정점 풀로 복사)");
	static_assert(sizeof(FSkinVertex) == 24, "SkinCache.hlsl 스킨 스트림 읽기는 FSkinVertex 24바이트를 가정한다");
} // namespace

void SkinCacheMath::BuildLayout(const std::vector<FItemInput>& Inputs, FLayout& Out)
{
	const uint32 Count = static_cast<uint32>(Inputs.size());
	Out.ItemOrder.clear();
	Out.Dispatches.clear();
	Out.FirstVertex.assign(Count, 0);
	Out.TotalVertices = 0;

	// 1) 키 → 묶음 번호 (첫 등장 순서): 작은 열린 주소 해시, 부하 1/2을 넘으면 두 배로 다시 (키 = 널 아닌 포인터 × 2 + 변형, 0 = 빈 칸).
	//    조회만 하므로 결정적. 묶음 = 디스패치 단위 키 (메시·LOD 목록·변형) — 수천 입력에 키는 수백 이하
	struct FGroup
	{
		uint32 First = 0; // 첫 입력
		uint32 Count = 0;
		uint32 Start = 0; // ItemOrder 안 시작
	};
	std::vector<FGroup>                    Groups;
	std::vector<uint32>                    GroupOf(Count, ~0u);
	std::vector<std::pair<uint64, uint32>> Table(256, { 0ull, 0u });
	const auto Probe = [&Table](uint64 Key) {
		const uint32 Mask = static_cast<uint32>(Table.size()) - 1;
		uint32       Slot = static_cast<uint32>((Key * 0x9E3779B97F4A7C15ull) >> 32) & Mask;
		while (Table[Slot].first != 0 && Table[Slot].first != Key)
		{
			Slot = (Slot + 1) & Mask;
		}
		return Slot;
	};
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		if (Inputs[Index].VertexCount == 0)
		{
			continue;
		}
		const uint64 Key  = reinterpret_cast<uintptr_t>(Inputs[Index].MeshKey) * 2ull + (Inputs[Index].Variant != 0 ? 1ull : 0ull);
		uint32       Slot = Probe(Key);
		if (Table[Slot].first == 0)
		{
			if ((Groups.size() + 1) * 2 > Table.size())
			{
				std::vector<std::pair<uint64, uint32>> Old = std::move(Table);
				Table.assign(Old.size() * 2, { 0ull, 0u });
				for (const auto& Entry : Old)
				{
					if (Entry.first != 0)
					{
						Table[Probe(Entry.first)] = Entry;
					}
				}
				Slot = Probe(Key);
			}
			Table[Slot] = { Key, static_cast<uint32>(Groups.size()) };
			Groups.push_back({ Index, 0, 0 });
		}
		GroupOf[Index] = Table[Slot].second;
		++Groups[Table[Slot].second].Count;
	}

	// 2) 묶음 시작 (첫 등장 순서) → 3) 입력 순서대로 자리 채우기 (계수 정렬 — 묶음 안은 입력 순서)
	uint32 Total = 0;
	for (FGroup& Group : Groups)
	{
		Group.Start = Total;
		Total += Group.Count;
	}
	Out.ItemOrder.resize(Total);
	std::vector<uint32> Fill(Groups.size(), 0);
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		if (GroupOf[Index] != ~0u)
		{
			const uint32 Group = GroupOf[Index];
			Out.ItemOrder[Groups[Group].Start + Fill[Group]++] = Index;
		}
	}

	// 4) 배치 순서대로 정점 자리 + 디스패치 (같은 묶음 항목이 디스패치 Y 상한을 넘으면 나눔)
	Out.Dispatches.reserve(Groups.size());
	for (const FGroup& Group : Groups)
	{
		const FItemInput& First = Inputs[Group.First];
		for (uint32 Item = 0; Item < Group.Count; ++Item)
		{
			if (Item % MaxDispatchItems == 0)
			{
				FDispatch Dispatch;
				Dispatch.FirstItem   = Group.Start + Item;
				Dispatch.VertexCount = First.VertexCount;
				Dispatch.ThreadCount = First.ThreadCount;
				Dispatch.Variant     = First.Variant;
				Out.Dispatches.push_back(Dispatch);
			}
			const uint32 Index     = Out.ItemOrder[Group.Start + Item];
			Out.FirstVertex[Index] = static_cast<uint32>(Out.TotalVertices);
			Out.TotalVertices += Inputs[Index].VertexCount;
			++Out.Dispatches.back().ItemCount;
		}
	}
}

void SkinCacheMath::BuildLodVertexLists(uint32 VertexCount, const std::vector<uint32>& Indices, const std::vector<std::pair<uint32, uint32>>& LodRanges,
                                        std::vector<std::vector<uint32>>& OutLists)
{
	OutLists.assign(LodRanges.size(), {});
	std::vector<uint8> Used(VertexCount, 0);
	uint32             UsedCount = 0;
	// 거친 LOD부터 거꾸로 쌓는다: 목록[L] = LOD L..마지막의 합집합
	for (size_t Lod = LodRanges.size(); Lod-- > 0;)
	{
		const auto [Offset, Count] = LodRanges[Lod];
		for (uint32 Index = Offset; Index < Offset + Count && Index < Indices.size(); ++Index)
		{
			const uint32 Vertex = Indices[Index];
			if (Vertex < VertexCount && Used[Vertex] == 0)
			{
				Used[Vertex] = 1;
				++UsedCount;
			}
		}
		if (UsedCount == VertexCount)
		{
			continue; // 항등 (빈 목록)
		}
		std::vector<uint32>& List = OutLists[Lod];
		List.reserve(UsedCount);
		for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
		{
			if (Used[Vertex] != 0)
			{
				List.push_back(Vertex);
			}
		}
	}
}

uint64 SkinCacheMath::ComputeCapacity(uint64 Current, uint64 Required)
{
	if (Required <= Current)
	{
		return Current;
	}
	const uint64 Wanted  = std::max(Required, Current + Current / 2);
	const uint64 Rounded = (Wanted + CapacityGranularity - 1) / CapacityGranularity * CapacityGranularity;
	if (Required > MaxCapacity)
	{
		return 0;
	}
	return std::min(Rounded, MaxCapacity);
}

FSkinCache::~FSkinCache()
{
	Shutdown();
}

bool FSkinCache::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary)
{
	Rhi     = &InRhi;
	Library = &InLibrary;
	E_CHECK(RootSignature.AddConstants(4, 0) == Param_Constants);
	E_CHECK(RootSignature.AddShaderResourceView(0) == Param_Base);
	E_CHECK(RootSignature.AddShaderResourceView(1) == Param_Skin);
	E_CHECK(RootSignature.AddShaderResourceView(2) == Param_Items);
	E_CHECK(RootSignature.AddShaderResourceView(15) == Param_Palette);
	E_CHECK(RootSignature.AddUnorderedAccessView(0) == Param_Output);
	E_CHECK(RootSignature.AddShaderResourceView(3) == Param_List);
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"SkinCacheRoot") || !CreatePipeline(Pipeline, false))
	{
		E_LOG(LogRenderer, Error, "스킨 캐시 파이프라인 생성 실패");
		return false;
	}
	return true;
}

void FSkinCache::Shutdown()
{
	Buffer.Reset(); // 호출자가 GPU Flush 이후 종료
	Capacity = 0;
	Pipeline.Shutdown();
	RootSignature.Shutdown();
	Rhi = nullptr;
}

bool FSkinCache::CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile)
{
	FShaderCompileDesc Desc;
	Desc.FileName   = L"SkinCache.hlsl";
	Desc.EntryPoint = L"CSMain";
	Desc.Stage      = EShaderStage::Compute;
	if (bForceRecompile && !Library->CookShader(Desc))
	{
		return false;
	}
	const ComPtr<IDxcBlob> Shader = Library->GetShader(Desc);
	return Shader && OutPipeline.InitCompute(Rhi->GetDevice().GetDevice(), RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()), L"SkinCache");
}

bool FSkinCache::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewPipeline;
	if (!CreatePipeline(NewPipeline, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "스킨 캐시 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	Pipeline.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());
	return true;
}

bool FSkinCache::EnsureCapacity(uint64 Required, uint64 RtRequired)
{
	const uint64 NewCapacity   = SkinCacheMath::ComputeCapacity(Capacity, Required);
	const uint64 NewRtCapacity = RtRequired == 0 ? RtCapacity : SkinCacheMath::ComputeCapacity(RtCapacity, RtRequired);
	if (NewCapacity == 0 || (RtRequired > 0 && NewRtCapacity == 0))
	{
		E_LOG(LogRenderer, Error, "스킨 캐시: 정점 {}개는 상한 {}개를 넘습니다", Required, SkinCacheMath::MaxCapacity);
		return false;
	}
	if (NewCapacity == Capacity && NewRtCapacity == RtCapacity && Buffer)
	{
		return true;
	}
	D3D12_HEAP_PROPERTIES HeapProperties{};
	HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC Desc{};
	Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	Desc.Width            = SkinCacheMath::GetBufferBytes(NewCapacity, NewRtCapacity);
	Desc.Height           = 1;
	Desc.DepthOrArraySize = 1;
	Desc.MipLevels        = 1;
	Desc.SampleDesc.Count = 1;
	Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	ComPtr<ID3D12Resource> NewBuffer;
	if (FAILED(Rhi->GetDevice().GetDevice()->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
	                                                                IID_PPV_ARGS(&NewBuffer))))
	{
		E_LOG(LogRenderer, Error, "스킨 캐시 버퍼 생성 실패 ({} MB)", Desc.Width >> 20);
		return false;
	}
	NewBuffer->SetName(L"SkinCache");
	if (Buffer)
	{
		Rhi->DeferRelease(Buffer); // 진행 중인 프레임이 읽고 있을 수 있다
	}
	E_LOG(LogRenderer, Log, "스킨 캐시 버퍼: 정점 {} → {}, RT 정점 {} → {} ({:.1f} MB)", Capacity, NewCapacity, RtCapacity, NewRtCapacity,
	      static_cast<double>(Desc.Width) / (1024.0 * 1024.0));
	Buffer      = NewBuffer;
	RtCapacity  = NewRtCapacity;
	BufferState = D3D12_RESOURCE_STATE_COMMON;
	Capacity    = NewCapacity;
	return true;
}

bool FSkinCache::Prepare(FMeshInstanceList& Instances, FD3D12DynamicUploadBuffer& DynamicBuffer, const FSkinCacheRtOptions& RtOptions)
{
	bPrepared     = false;
	FrameVertices = 0;
	FrameDispatches.clear();
	Inputs.clear();
	InputInstances.clear();
	InputBones.clear();

	std::vector<FMeshInstance>& List  = Instances.GetInstances();
	const uint32                Count = Instances.GetGatheredCount();
	// 1) 인스턴스마다 입력 (병렬 — 인스턴스 칸별로 쓰고 아래에서 순서대로 모은다: 결정적)
	PerInstance.resize(Count);
	FParallel::ParallelFor(Count, 512, [&](uint32 Begin, uint32 End) {
		for (uint32 Index = Begin; Index < End; ++Index)
		{
			FMeshInstance& Instance = List[Index];
			FInstanceInput& Slot    = PerInstance[Index];
			Slot.bSkinned           = Instance.IsSkinned();
			if (!Slot.bSkinned)
			{
				continue;
			}
			// RT 복사용 완전한 FVertex: LOD0(목록이 LOD0 정점을 덮음) + RT 스킨 거리 안 (FRayTracingScene과 같은 식 — 아니면 RT가 직접 스키닝)
			Instance.bSkinCacheLod0 = RtOptions.bEnabled && Instance.Lod == 0 &&
			                          (Instance.WorldBounds.GetCenter() - RtOptions.CameraPosition).Length() - Instance.WorldBounds.GetExtent().Length() <=
			                              RtOptions.MaxDistance;
			// 이 인스턴스가 쓰는 LOD 이상의 정점만 스키닝 (키 = 메시의 그 LOD 목록 — 메시·LOD마다 디스패치 하나)
			const std::vector<uint32>& LodList     = Instance.Mesh->GetLodVertexList(Instance.Lod);
			const uint32               VertexCount = Instance.Mesh->GetVertexCount();
			Slot.Input = { &LodList, VertexCount, LodList.empty() ? VertexCount : static_cast<uint32>(LodList.size()), Instance.bSkinCacheLod0 ? 1u : 0u };
			Slot.Bones = { Instance.BoneOffset, Instance.PrevBoneOffset }; // 항목 표는 배치 순서 — 인스턴스를 다시 무작위로 읽지 않게
		}
	});
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		if (PerInstance[Index].bSkinned)
		{
			Inputs.push_back(PerInstance[Index].Input);
			InputInstances.push_back(Index);
			InputBones.push_back(PerInstance[Index].Bones);
		}
	}
	if (Inputs.empty())
	{
		return false;
	}
	// 배치는 입력(키·정점 수·변형 순서)이 지난 Prepare와 같으면 그대로 (보통 LOD 전환이 없는 프레임) — 결과는 입력만의 함수
	bool bSameInputs = LayoutInputs.size() == Inputs.size();
	for (size_t Index = 0; bSameInputs && Index < Inputs.size(); ++Index)
	{
		const SkinCacheMath::FItemInput& A = Inputs[Index];
		const SkinCacheMath::FItemInput& B = LayoutInputs[Index];
		bSameInputs = A.MeshKey == B.MeshKey && A.VertexCount == B.VertexCount && A.ThreadCount == B.ThreadCount && A.Variant == B.Variant;
	}
	if (!bSameInputs)
	{
		SkinCacheMath::BuildLayout(Inputs, Layout);
		LayoutInputs = Inputs;
	}
	// RT 정점 영역: RT 대상 인스턴스만 배치 순서대로 따로 (래스터 영역 크기와 무관)
	uint64 RtVertices = 0;
	for (const uint32 Input : Layout.ItemOrder)
	{
		RtVertices += Inputs[Input].Variant != 0 ? Inputs[Input].VertexCount : 0;
	}
	if (Layout.TotalVertices == 0 || !EnsureCapacity(Layout.TotalVertices, RtVertices))
	{
		return false;
	}

	const uint32                  ItemCount  = static_cast<uint32>(Layout.ItemOrder.size());
	const FD3D12DynamicAllocation Allocation = DynamicBuffer.Allocate(sizeof(FSkinCacheItemGpu) * ItemCount, 16);
	FSkinCacheItemGpu*            Items      = static_cast<FSkinCacheItemGpu*>(Allocation.CpuAddress);
	// RT 정점 영역 자리 (배치 순서 누적 — 순차, 가볍다)
	InputRtFirst.assign(Inputs.size(), 0);
	uint32 RtNext = 0;
	for (const uint32 Input : Layout.ItemOrder)
	{
		if (Inputs[Input].Variant != 0)
		{
			InputRtFirst[Input] = RtNext;
			RtNext += Inputs[Input].VertexCount;
		}
	}
	// 2) 항목 표 (배치 순서, 병렬 — 업로드 힙은 구간마다 순차 쓰기만)
	const uint32 CapacityU32 = static_cast<uint32>(Capacity);
	FParallel::ParallelFor(ItemCount, 1024, [&](uint32 Begin, uint32 End) {
		for (uint32 Slot = Begin; Slot < End; ++Slot)
		{
			const uint32      Input = Layout.ItemOrder[Slot];
			FSkinCacheItemGpu Item;
			Item.FirstVertex    = Layout.FirstVertex[Input];
			Item.Capacity       = CapacityU32;
			Item.BoneOffset     = InputBones[Input].first;
			Item.PrevBoneOffset = InputBones[Input].second;
			Item.RtFirstVertex  = InputRtFirst[Input];
			std::memcpy(Items + Slot, &Item, sizeof(Item));
		}
	});
	ItemTable = Allocation.GpuAddress;
	// 3) 인스턴스에 자리 기록 (병렬, 입력 = 인스턴스 오름차순)
	const uint64 RtBase = SkinCacheMath::GetRtOffset(Capacity);
	FParallel::ParallelFor(static_cast<uint32>(Inputs.size()), 1024, [&](uint32 Begin, uint32 End) {
		for (uint32 Input = Begin; Input < End; ++Input)
		{
			FMeshInstance& Instance    = List[InputInstances[Input]];
			Instance.SkinCacheVertex   = Layout.FirstVertex[Input];
			Instance.SkinCacheCapacity = CapacityU32;
			Instance.SkinCacheRtOffset = RtBase + static_cast<uint64>(InputRtFirst[Input]) * SkinCacheMath::RtVertexBytes;
		}
	});

	// 정점 목록은 프레임마다 동적 업로드 (메시·LOD 조합 수만큼 — 작다. 같은 목록은 한 번)
	FrameDispatches.reserve(Layout.Dispatches.size());
	std::unordered_map<const void*, D3D12_GPU_VIRTUAL_ADDRESS> ListAddresses;
	for (const SkinCacheMath::FDispatch& Dispatch : Layout.Dispatches)
	{
		const uint32               Input    = Layout.ItemOrder[Dispatch.FirstItem];
		const FStaticMesh&         Mesh     = *List[InputInstances[Input]].Mesh;
		const std::vector<uint32>& Vertices = *static_cast<const std::vector<uint32>*>(Inputs[Input].MeshKey);
		D3D12_GPU_VIRTUAL_ADDRESS  ListAddress = 0;
		if (!Vertices.empty())
		{
			const auto [It, bNew] = ListAddresses.try_emplace(&Vertices, 0);
			if (bNew)
			{
				const FD3D12DynamicAllocation ListAllocation = DynamicBuffer.Allocate(Vertices.size() * sizeof(uint32), 16);
				std::memcpy(ListAllocation.CpuAddress, Vertices.data(), Vertices.size() * sizeof(uint32));
				It->second = ListAllocation.GpuAddress;
			}
			ListAddress = It->second;
		}
		FrameDispatches.push_back({ Mesh.GetVertexBuffer().GetGpuAddress(), Mesh.GetSkinBuffer().GetGpuAddress(), ListAddress, Dispatch.ThreadCount,
		                            Dispatch.FirstItem, Dispatch.ItemCount, Dispatch.Variant });
		FrameVertices += Dispatch.ThreadCount * static_cast<uint64>(Dispatch.ItemCount);
	}
	bPrepared = true;
	return true;
}

FRGResourceRef FSkinCache::AddPass(FRenderGraph& Graph, D3D12_GPU_VIRTUAL_ADDRESS Palette, int32 Timer)
{
	if (!bPrepared)
	{
		return {};
	}
	const FRGResourceRef Ref = Graph.ImportTracked("SkinCache", Buffer.Get(), &BufferState);
	// 이번 프레임 쓰는 범위만 (나머지 칸은 아무도 읽지 않는다)
	Graph.AddPass("스킨 캐시")
		.Write(Ref, ERGAccess::Uav)
		.Timer(Timer)
		.Execute([this, Dispatches = FrameDispatches, Items = ItemTable, Palette, Output = Buffer->GetGPUVirtualAddress()](FRGContext& Context) {
			ID3D12GraphicsCommandList* CommandList = Context.CommandList;
			CommandList->SetComputeRootSignature(RootSignature.Get());
			CommandList->SetPipelineState(Pipeline.Get());
			CommandList->SetComputeRootShaderResourceView(Param_Items, Items);
			CommandList->SetComputeRootShaderResourceView(Param_Palette, Palette);
			CommandList->SetComputeRootUnorderedAccessView(Param_Output, Output);
			for (const FFrameDispatch& Dispatch : Dispatches)
			{
				const uint32 Constants[4] = { Dispatch.ThreadCount, Dispatch.FirstItem, Dispatch.VertexList != 0 ? 1u : 0u, Dispatch.bAttributes };
				CommandList->SetComputeRoot32BitConstants(Param_Constants, 4, Constants, 0);
				CommandList->SetComputeRootShaderResourceView(Param_Base, Dispatch.BaseVertices);
				CommandList->SetComputeRootShaderResourceView(Param_Skin, Dispatch.SkinVertices);
				CommandList->SetComputeRootShaderResourceView(Param_List, Dispatch.VertexList != 0 ? Dispatch.VertexList : Items); // 항등이면 읽지 않음
				CommandList->Dispatch((Dispatch.ThreadCount + SkinCacheMath::GroupSize - 1) / SkinCacheMath::GroupSize, Dispatch.ItemCount, 1);
			}
		});
	return Ref;
}
