#include "Renderer/OcclusionCuller.h"

#include "RHI/ShaderLibrary.h"
#include "Renderer/HzbMath.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/StaticMesh.h"

#include <algorithm>
#include <cstring>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr uint32 CullGroupSize  = 64; // OcclusionCulling.hlsl CullCS numthreads
	constexpr uint32 HzbGroupSize   = 8;
	constexpr uint32 ArgumentStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	static_assert(ArgumentStride == 20);

	enum EHzbRootParameter : uint32
	{
		HzbParam_Constants   = 0, // b0 (4개)
		HzbParam_SourceDepth = 1, // t0
		HzbParam_DestMip     = 2, // u0
		HzbParam_SourceMip   = 3, // u1
	};

	enum ECullRootParameter : uint32
	{
		CullParam_Constants = 0, // b1
		CullParam_Items     = 1, // t1
		CullParam_Hzb       = 2, // t2 (표)
		CullParam_Arguments = 3, // u2
		CullParam_Phase1    = 4, // u3
		CullParam_Phase2    = 5, // u4
		CullParam_Occluded  = 6, // u5
	};

	FShaderCompileDesc MakeDesc(const wchar_t* Entry)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = L"OcclusionCulling.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = EShaderStage::Compute;
		return Desc;
	}
} // namespace

FOcclusionCuller::~FOcclusionCuller()
{
	Shutdown();
}

bool FOcclusionCuller::Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary)
{
	E_CHECKF(Rhi == nullptr, "오클루전 컬러가 이미 초기화되어 있습니다");
	Rhi           = &InRhi;
	ShaderLibrary = &InShaderLibrary;
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	const uint32 HzbConstants = HzbRootSignature.AddConstants(4, 0);
	const uint32 HzbSource    = HzbRootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) });
	const uint32 HzbDest      = HzbRootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0) });
	const uint32 HzbSourceMip = HzbRootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1) });
	E_CHECK(HzbConstants == HzbParam_Constants && HzbSource == HzbParam_SourceDepth && HzbDest == HzbParam_DestMip && HzbSourceMip == HzbParam_SourceMip);
	if (!HzbRootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"HzbRootSignature"))
	{
		return false;
	}

	const uint32 Constants = CullRootSignature.AddConstantBufferView(1);
	const uint32 ItemsSrv  = CullRootSignature.AddShaderResourceView(1);
	const uint32 HzbTable  = CullRootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2) });
	const uint32 Arguments = CullRootSignature.AddUnorderedAccessView(2);
	const uint32 Phase1    = CullRootSignature.AddUnorderedAccessView(3);
	const uint32 Phase2    = CullRootSignature.AddUnorderedAccessView(4);
	const uint32 Flags     = CullRootSignature.AddUnorderedAccessView(5);
	E_CHECK(Constants == CullParam_Constants && ItemsSrv == CullParam_Items && HzbTable == CullParam_Hzb && Arguments == CullParam_Arguments &&
	        Phase1 == CullParam_Phase1 && Phase2 == CullParam_Phase2 && Flags == CullParam_Occluded);
	if (!CullRootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"OcclusionCullRootSignature"))
	{
		return false;
	}

	if (!CreatePipelines(false, HzbFromDepthPipeline, HzbDownsamplePipeline, CullPipeline))
	{
		return false;
	}

	D3D12_INDIRECT_ARGUMENT_DESC Argument{};
	Argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
	D3D12_COMMAND_SIGNATURE_DESC SignatureDesc{};
	SignatureDesc.ByteStride       = ArgumentStride;
	SignatureDesc.NumArgumentDescs = 1;
	SignatureDesc.pArgumentDescs   = &Argument;
	E_D3D_VERIFY(Device->CreateCommandSignature(&SignatureDesc, nullptr, IID_PPV_ARGS(&DrawSignature)));
	DrawSignature->SetName(L"OcclusionDrawSignature");

	// 루트 SRV/UAV가 항상 유효한 주소를 가리키도록 최소 크기로 미리
	EnsureBuffers(1, 1, 1);
	return true;
}

void FOcclusionCuller::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	// 호출자(씬 렌더러)가 GPU Flush 이후 종료한다
	if (HzbSrv.IsValid())
	{
		Rhi->GetSrvAllocator().Free(HzbSrv);
	}
	for (FD3D12DescriptorHandle& Uav : HzbMipUavs)
	{
		Rhi->GetSrvAllocator().Free(Uav);
	}
	HzbMipUavs.clear();
	Hzb.Reset();
	for (FBuffer* Buffer : { &DrawArguments, &Phase1Indices, &Phase2Indices, &Occluded })
	{
		*Buffer = FBuffer{};
	}
	for (uint32 Slot = 0; Slot < FD3D12RHI::FrameCount; ++Slot)
	{
		Readback[Slot].Reset();
		ReadbackCapacity[Slot] = 0;
		ReadbackTriangles[Slot].clear();
	}
	DrawSignature.Reset();
	HzbFromDepthPipeline.Shutdown();
	HzbDownsamplePipeline.Shutdown();
	CullPipeline.Shutdown();
	HzbRootSignature.Shutdown();
	CullRootSignature.Shutdown();
	bHzbValid     = false;
	HzbWidth      = 0;
	HzbHeight     = 0;
	Rhi           = nullptr;
	ShaderLibrary = nullptr;
}

bool FOcclusionCuller::CreatePipelines(bool bForceRecompile, FD3D12PipelineState& OutFromDepth, FD3D12PipelineState& OutDownsample,
                                       FD3D12PipelineState& OutCull)
{
	const FShaderCompileDesc Descs[] = { MakeDesc(L"HzbFromDepthCS"), MakeDesc(L"HzbDownsampleCS"), MakeDesc(L"CullCS") };
	ComPtr<IDxcBlob>         Blobs[3];
	for (int32 Index = 0; Index < 3; ++Index)
	{
		if (bForceRecompile && !ShaderLibrary->CookShader(Descs[Index]))
		{
			return false;
		}
		Blobs[Index] = ShaderLibrary->GetShader(Descs[Index]);
		if (!Blobs[Index])
		{
			return false;
		}
	}
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	return OutFromDepth.InitCompute(Device, HzbRootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Blobs[0].Get()), L"HzbFromDepthPipeline") &&
	       OutDownsample.InitCompute(Device, HzbRootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Blobs[1].Get()), L"HzbDownsamplePipeline") &&
	       OutCull.InitCompute(Device, CullRootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Blobs[2].Get()), L"OcclusionCullPipeline");
}

bool FOcclusionCuller::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewFromDepth;
	FD3D12PipelineState NewDownsample;
	FD3D12PipelineState NewCull;
	if (!CreatePipelines(bForceRecompile, NewFromDepth, NewDownsample, NewCull))
	{
		E_LOG(LogRenderer, Error, "오클루전 컬링 셰이더 다시 로드 실패: 기존 파이프라인 유지");
		return false;
	}
	HzbFromDepthPipeline.Swap(NewFromDepth);
	Rhi->DeferRelease(NewFromDepth.Detach());
	HzbDownsamplePipeline.Swap(NewDownsample);
	Rhi->DeferRelease(NewDownsample.Detach());
	CullPipeline.Swap(NewCull);
	Rhi->DeferRelease(NewCull.Detach());
	return true;
}

void FOcclusionCuller::Transition(ID3D12Resource* Resource, D3D12_RESOURCE_STATES& State, D3D12_RESOURCE_STATES After)
{
	if (State != After)
	{
		const D3D12_RESOURCE_BARRIER Barrier = MakeTransitionBarrier(Resource, State, After);
		Rhi->GetCommandList()->ResourceBarrier(1, &Barrier);
		State = After;
	}
}

void FOcclusionCuller::EnsureBuffer(FBuffer& Buffer, uint64 Bytes, const wchar_t* DebugName)
{
	if (Buffer.Resource && Buffer.Capacity >= Bytes)
	{
		return;
	}
	const uint64 Capacity = std::max<uint64>(Bytes + Bytes / 2, 256); // 1.5배로 키워 재생성을 줄인다
	if (Buffer.Resource)
	{
		Rhi->DeferRelease(Buffer.Resource); // 진행 중인 프레임이 참조할 수 있다
	}
	Buffer = FBuffer{};
	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   Desc = MakeBufferDesc(Capacity, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	if (FAILED(Rhi->GetDevice().GetDevice()->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
	                                                                 IID_PPV_ARGS(&Buffer.Resource))))
	{
		E_LOG(LogRenderer, Fatal, "오클루전 컬링 버퍼 생성 실패 ({} bytes)", Capacity);
	}
	Buffer.Resource->SetName(DebugName);
	Buffer.State    = D3D12_RESOURCE_STATE_COMMON;
	Buffer.Capacity = Capacity;
}

void FOcclusionCuller::EnsureBuffers(uint32 SlotCount, uint32 InItemCount, uint32 InBatchCount)
{
	EnsureBuffer(DrawArguments, static_cast<uint64>(std::max(InBatchCount, 1u)) * 2 * ArgumentStride, L"OcclusionDrawArguments");
	EnsureBuffer(Phase1Indices, static_cast<uint64>(std::max(SlotCount, 1u)) * sizeof(uint32), L"OcclusionPhase1Indices");
	EnsureBuffer(Phase2Indices, static_cast<uint64>(std::max(SlotCount, 1u)) * sizeof(uint32), L"OcclusionPhase2Indices");
	EnsureBuffer(Occluded, static_cast<uint64>(std::max(InItemCount, 1u)) * sizeof(uint32), L"OcclusionFlags");
}

void FOcclusionCuller::ReleaseHzb()
{
	if (!Hzb)
	{
		return;
	}
	Rhi->DeferRelease(Hzb);
	Rhi->DeferFreeDescriptor(HzbSrv);
	for (const FD3D12DescriptorHandle& Uav : HzbMipUavs)
	{
		Rhi->DeferFreeDescriptor(Uav);
	}
	HzbMipUavs.clear();
	HzbSrv = FD3D12DescriptorHandle{};
	Hzb.Reset();
	bHzbValid = false;
}

void FOcclusionCuller::EnsureHzb(uint32 Width, uint32 Height)
{
	const uint32 MipWidth  = HzbMath::GetHzbDimension(Width); // 2의 거듭제곱 (D3D12 내림 반감과 맞춘다)
	const uint32 MipHeight = HzbMath::GetHzbDimension(Height);
	if (Hzb && HzbWidth == MipWidth && HzbHeight == MipHeight)
	{
		return;
	}
	ReleaseHzb();
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	HzbWidth  = MipWidth;
	HzbHeight = MipHeight;
	HzbMips   = HzbMath::GetMipCount(MipWidth, MipHeight);

	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   Desc =
		MakeTexture2DDesc(MipWidth, MipHeight, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, static_cast<uint16>(HzbMips));
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
	                                           IID_PPV_ARGS(&Hzb))))
	{
		E_LOG(LogRenderer, Fatal, "HZB 생성 실패 ({}x{})", MipWidth, MipHeight);
	}
	Hzb->SetName(L"HierarchicalZ");
	HzbState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                  = DXGI_FORMAT_R32_FLOAT;
	SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Texture2D.MipLevels     = HzbMips;
	HzbSrv                          = Rhi->GetSrvAllocator().Allocate();
	Device->CreateShaderResourceView(Hzb.Get(), &SrvDesc, HzbSrv.Cpu);
	for (uint32 Mip = 0; Mip < HzbMips; ++Mip)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc{};
		UavDesc.Format             = DXGI_FORMAT_R32_FLOAT;
		UavDesc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
		UavDesc.Texture2D.MipSlice = Mip;
		const FD3D12DescriptorHandle Uav = Rhi->GetSrvAllocator().Allocate();
		Device->CreateUnorderedAccessView(Hzb.Get(), nullptr, &UavDesc, Uav.Cpu);
		HzbMipUavs.push_back(Uav);
	}
	bHzbValid = false; // 새 HZB는 아직 내용이 없다
	E_LOG(LogRenderer, Log, "HZB 생성: {}x{}, 밉 {}", MipWidth, MipHeight, HzbMips);
}

void FOcclusionCuller::ReadStats()
{
	const uint32 Slot = Rhi->GetFrameSlot();
	if (!Readback[Slot] || ReadbackTriangles[Slot].empty())
	{
		return;
	}
	const uint32      Batches = static_cast<uint32>(ReadbackTriangles[Slot].size());
	const D3D12_RANGE Range{ 0, static_cast<SIZE_T>(Batches) * 2 * ArgumentStride };
	void*             Mapped = nullptr;
	if (FAILED(Readback[Slot]->Map(0, &Range, &Mapped)))
	{
		return;
	}
	const uint8* Data = static_cast<const uint8*>(Mapped);
	StatPhase1    = 0;
	StatPhase2    = 0;
	StatTriangles = 0;
	for (uint32 Batch = 0; Batch < Batches; ++Batch)
	{
		D3D12_DRAW_INDEXED_ARGUMENTS Phase1{};
		D3D12_DRAW_INDEXED_ARGUMENTS Phase2{};
		std::memcpy(&Phase1, Data + (Batch * 2) * ArgumentStride, ArgumentStride);
		std::memcpy(&Phase2, Data + (Batch * 2 + 1) * ArgumentStride, ArgumentStride);
		StatPhase1 += Phase1.InstanceCount;
		StatPhase2 += Phase2.InstanceCount;
		StatTriangles += static_cast<uint64>(Phase1.InstanceCount + Phase2.InstanceCount) * ReadbackTriangles[Slot][Batch];
	}
	const D3D12_RANGE NoWrite{ 0, 0 };
	Readback[Slot]->Unmap(0, &NoWrite);
	StatTested = ReadbackTested[Slot];
}

void FOcclusionCuller::CullPhase1(const FMeshInstanceList& Instances, const FMeshPassBatches& Batches, uint32 Width, uint32 Height)
{
	E_CHECKF(Rhi != nullptr, "오클루전 컬러가 초기화되지 않았습니다");
	ReadStats();

	// ---- 항목: 정적 묶음의 인스턴스마다 (스킨 묶음은 간접 인자 0으로 두고 호출자가 직접 그린다)
	const std::vector<FInstanceBatch>& BatchList = Batches.GetBatches();
	const std::vector<uint32>&         Indices   = Batches.GetIndices();
	Items.clear();
	TrianglesPerInstance.assign(BatchList.size(), 0);
	std::vector<D3D12_DRAW_INDEXED_ARGUMENTS> Arguments(BatchList.size() * 2, D3D12_DRAW_INDEXED_ARGUMENTS{});
	for (uint32 Batch = 0; Batch < static_cast<uint32>(BatchList.size()); ++Batch)
	{
		const FInstanceBatch& Info           = BatchList[Batch];
		const FMeshInstance&  Representative = Instances[Info.Instance];
		if (Representative.IsSkinned())
		{
			continue;
		}
		const FStaticMesh::FLodRange& Range = Representative.Mesh->GetLod(Representative.Lod);
		TrianglesPerInstance[Batch]         = Range.IndexCount / 3;
		for (uint32 Phase = 0; Phase < 2; ++Phase)
		{
			D3D12_DRAW_INDEXED_ARGUMENTS& Argument = Arguments[Batch * 2 + Phase];
			Argument.IndexCountPerInstance        = Range.IndexCount;
			Argument.StartIndexLocation           = Range.IndexOffset;
		}
		for (uint32 Slot = Info.First; Slot < Info.First + Info.Count; ++Slot)
		{
			const FMeshInstance& Instance = Instances[Indices[Slot]];
			FOcclusionItem&      Item     = Items.emplace_back();
			Item.BoundsMin                = Instance.WorldBounds.Min;
			Item.BoundsMax                = Instance.WorldBounds.Max;
			Item.Batch                    = Batch;
			Item.Instance                 = Indices[Slot];
			Item.BatchFirst               = Info.First;
		}
	}
	ItemCount  = static_cast<uint32>(Items.size());
	BatchCount = static_cast<uint32>(BatchList.size());
	EnsureBuffers(static_cast<uint32>(Indices.size()), ItemCount, BatchCount);
	EnsureHzb(Width, Height);
	if (HzbDepthWidth != Width || HzbDepthHeight != Height)
	{
		bHzbValid = false; // 다른 크기의 깊이로 만든 HZB
	}

	ID3D12GraphicsCommandList* CommandList   = Rhi->GetCommandList();
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();

	// 간접 인자 초기화 (InstanceCount = 0): 업로드 → 복사
	if (!Arguments.empty())
	{
		const size_t                  Bytes  = Arguments.size() * ArgumentStride;
		const FD3D12DynamicAllocation Upload = DynamicBuffer.Allocate(Bytes, 16);
		std::memcpy(Upload.CpuAddress, Arguments.data(), Bytes);
		Transition(DrawArguments.Resource.Get(), DrawArguments.State, D3D12_RESOURCE_STATE_COPY_DEST);
		CommandList->CopyBufferRegion(DrawArguments.Resource.Get(), 0, DynamicBuffer.GetResource(), DynamicBuffer.GetOffset(Upload), Bytes);
	}
	const size_t                  ItemBytes = std::max<size_t>(Items.size(), 1) * sizeof(FOcclusionItem);
	const FD3D12DynamicAllocation ItemAlloc = DynamicBuffer.Allocate(ItemBytes, 16);
	std::memset(ItemAlloc.CpuAddress, 0, sizeof(FOcclusionItem));
	if (!Items.empty())
	{
		std::memcpy(ItemAlloc.CpuAddress, Items.data(), Items.size() * sizeof(FOcclusionItem));
	}
	ItemsAddress = ItemAlloc.GpuAddress;

	Transition(DrawArguments.Resource.Get(), DrawArguments.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(Phase1Indices.Resource.Get(), Phase1Indices.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(Phase2Indices.Resource.Get(), Phase2Indices.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(Occluded.Resource.Get(), Occluded.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(Hzb.Get(), HzbState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	if (ItemCount > 0)
	{
		FOcclusionCullConstants Constants;
		Constants.ViewProjection = HzbViewProjection;
		Constants.DepthSize      = FVector2(static_cast<float>(HzbDepthWidth), static_cast<float>(HzbDepthHeight));
		Constants.HzbMipCount    = HzbMips;
		Constants.ItemCount      = ItemCount;
		Constants.Phase          = 1;
		Constants.bHzbValid      = bHzbValid ? 1u : 0u;

		CommandList->SetComputeRootSignature(CullRootSignature.Get());
		CommandList->SetPipelineState(CullPipeline.Get());
		CommandList->SetComputeRootConstantBufferView(CullParam_Constants, DynamicBuffer.AllocateConstants(Constants).GpuAddress);
		CommandList->SetComputeRootShaderResourceView(CullParam_Items, ItemsAddress);
		CommandList->SetComputeRootDescriptorTable(CullParam_Hzb, HzbSrv.Gpu);
		CommandList->SetComputeRootUnorderedAccessView(CullParam_Arguments, DrawArguments.Resource->GetGPUVirtualAddress());
		CommandList->SetComputeRootUnorderedAccessView(CullParam_Phase1, Phase1Indices.Resource->GetGPUVirtualAddress());
		CommandList->SetComputeRootUnorderedAccessView(CullParam_Phase2, Phase2Indices.Resource->GetGPUVirtualAddress());
		CommandList->SetComputeRootUnorderedAccessView(CullParam_Occluded, Occluded.Resource->GetGPUVirtualAddress());
		CommandList->Dispatch((ItemCount + CullGroupSize - 1) / CullGroupSize, 1, 1);
	}

	Transition(DrawArguments.Resource.Get(), DrawArguments.State, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	Transition(Phase1Indices.Resource.Get(), Phase1Indices.State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void FOcclusionCuller::BuildHzbAndCullPhase2(FD3D12RenderTarget& SceneColor, const FMatrix4x4& ViewProjection)
{
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	ID3D12Resource*            Depth       = SceneColor.GetDepthResource();
	const uint32               Width       = SceneColor.GetWidth();
	const uint32               Height      = SceneColor.GetHeight();

	// ---- HZB: 깊이 → 밉 0 → 밉 k
	D3D12_RESOURCE_STATES DepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	Transition(Depth, DepthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(Hzb.Get(), HzbState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	CommandList->SetComputeRootSignature(HzbRootSignature.Get());
	CommandList->SetPipelineState(HzbFromDepthPipeline.Get());
	CommandList->SetComputeRootDescriptorTable(HzbParam_SourceDepth, SceneColor.GetDepthSrv().Gpu);
	uint32 SourceWidth  = Width;
	uint32 SourceHeight = Height;
	uint32 DestWidth    = HzbWidth;
	uint32 DestHeight   = HzbHeight;
	for (uint32 Mip = 0; Mip < HzbMips; ++Mip)
	{
		if (Mip == 1)
		{
			CommandList->SetPipelineState(HzbDownsamplePipeline.Get());
		}
		if (Mip > 0)
		{
			const D3D12_RESOURCE_BARRIER Barrier = MakeUavBarrier(Hzb.Get()); // 위 밉 쓰기 완료
			CommandList->ResourceBarrier(1, &Barrier);
		}
		const FHzbBuildConstants Constants{ SourceWidth, SourceHeight, DestWidth, DestHeight };
		CommandList->SetComputeRoot32BitConstants(HzbParam_Constants, 4, &Constants, 0);
		CommandList->SetComputeRootDescriptorTable(HzbParam_DestMip, HzbMipUavs[Mip].Gpu);
		CommandList->SetComputeRootDescriptorTable(HzbParam_SourceMip, HzbMipUavs[Mip > 0 ? Mip - 1 : 0].Gpu);
		CommandList->Dispatch((DestWidth + HzbGroupSize - 1) / HzbGroupSize, (DestHeight + HzbGroupSize - 1) / HzbGroupSize, 1);
		SourceWidth  = DestWidth;
		SourceHeight = DestHeight;
		DestWidth    = std::max(DestWidth / 2, 1u);
		DestHeight   = std::max(DestHeight / 2, 1u);
	}
	Transition(Hzb.Get(), HzbState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(Depth, DepthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	HzbViewProjection = ViewProjection;
	HzbDepthWidth     = Width;
	HzbDepthHeight    = Height;
	bHzbValid         = true;

	// ---- 2단계: 1단계에서 가려진 항목을 이번 프레임 HZB로
	if (ItemCount > 0)
	{
		Transition(DrawArguments.Resource.Get(), DrawArguments.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		const D3D12_RESOURCE_BARRIER FlagsBarrier = MakeUavBarrier(Occluded.Resource.Get()); // 1단계 플래그
		CommandList->ResourceBarrier(1, &FlagsBarrier);

		FOcclusionCullConstants Constants;
		Constants.ViewProjection = ViewProjection;
		Constants.DepthSize      = FVector2(static_cast<float>(Width), static_cast<float>(Height));
		Constants.HzbMipCount    = HzbMips;
		Constants.ItemCount      = ItemCount;
		Constants.Phase          = 2;
		Constants.bHzbValid      = 1;

		CommandList->SetComputeRootSignature(CullRootSignature.Get());
		CommandList->SetPipelineState(CullPipeline.Get());
		CommandList->SetComputeRootConstantBufferView(CullParam_Constants, Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress);
		CommandList->SetComputeRootShaderResourceView(CullParam_Items, ItemsAddress);
		CommandList->SetComputeRootDescriptorTable(CullParam_Hzb, HzbSrv.Gpu);
		CommandList->SetComputeRootUnorderedAccessView(CullParam_Arguments, DrawArguments.Resource->GetGPUVirtualAddress());
		// 1단계 목록은 이미 정점 셰이더 읽기 상태 → 2단계에서 쓰지 않으므로 같은 칸에 2단계 목록을 묶는다
		CommandList->SetComputeRootUnorderedAccessView(CullParam_Phase1, Phase2Indices.Resource->GetGPUVirtualAddress());
		CommandList->SetComputeRootUnorderedAccessView(CullParam_Phase2, Phase2Indices.Resource->GetGPUVirtualAddress());
		CommandList->SetComputeRootUnorderedAccessView(CullParam_Occluded, Occluded.Resource->GetGPUVirtualAddress());
		CommandList->Dispatch((ItemCount + CullGroupSize - 1) / CullGroupSize, 1, 1);
		Transition(DrawArguments.Resource.Get(), DrawArguments.State, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
	}
	Transition(Phase2Indices.Resource.Get(), Phase2Indices.State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

D3D12_GPU_VIRTUAL_ADDRESS FOcclusionCuller::GetIndices(uint32 Phase) const
{
	return (Phase == 1 ? Phase1Indices : Phase2Indices).Resource->GetGPUVirtualAddress();
}

void FOcclusionCuller::DrawIndirect(ID3D12GraphicsCommandList* CommandList, uint32 Batch, uint32 Phase) const
{
	CommandList->ExecuteIndirect(DrawSignature.Get(), 1, DrawArguments.Resource.Get(), static_cast<uint64>(Batch * 2 + (Phase - 1)) * ArgumentStride,
	                             nullptr, 0);
}

void FOcclusionCuller::FinishFrame()
{
	const uint32 Slot  = Rhi->GetFrameSlot();
	const uint64 Bytes = static_cast<uint64>(BatchCount) * 2 * ArgumentStride;
	ReadbackTriangles[Slot].clear();
	if (Bytes == 0 || ItemCount == 0)
	{
		return;
	}
	if (!Readback[Slot] || ReadbackCapacity[Slot] < Bytes)
	{
		if (Readback[Slot])
		{
			Rhi->DeferRelease(Readback[Slot]);
		}
		const uint64                Capacity = Bytes + Bytes / 2;
		const D3D12_HEAP_PROPERTIES Heap     = MakeHeapProperties(D3D12_HEAP_TYPE_READBACK);
		const D3D12_RESOURCE_DESC   Desc     = MakeBufferDesc(Capacity);
		if (FAILED(Rhi->GetDevice().GetDevice()->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		                                                                 IID_PPV_ARGS(&Readback[Slot]))))
		{
			Readback[Slot].Reset();
			return;
		}
		Readback[Slot]->SetName(L"OcclusionStatsReadback");
		ReadbackCapacity[Slot] = Capacity;
	}
	Transition(DrawArguments.Resource.Get(), DrawArguments.State, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Rhi->GetCommandList()->CopyBufferRegion(Readback[Slot].Get(), 0, DrawArguments.Resource.Get(), 0, Bytes);
	ReadbackTriangles[Slot] = TrianglesPerInstance;
	ReadbackTested[Slot]    = ItemCount;
}
