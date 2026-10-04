#include "Renderer/RayTracingScene.h"

#include "Core/Math/MathUtils.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Material.h"
#include "Renderer/MaterialRender.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SkinnedMeshData.h"
#include "Renderer/StaticMesh.h"
#include "Renderer/TerrainRenderer.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scene/Terrain.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	enum ESkinningRootParameter : uint32
	{
		SkinParam_Constants = 0, // b0: 정점 수, 첫 본
		SkinParam_Base      = 1, // t0: 기본 정점 (ByteAddressBuffer, FVertex)
		SkinParam_Skin      = 2, // t1: 스킨 스트림 (ByteAddressBuffer, FSkinVertex)
		SkinParam_Palette   = 3, // t15: 프레임 본 팔레트 (SkinnedMesh.hlsli)
		SkinParam_Output    = 4, // u0: 월드 공간 정점 (RWByteAddressBuffer, FVertex)
	};

	constexpr uint32 SkinningGroupSize   = 64;   // RayTracingSkinning.hlsl numthreads
	constexpr uint64 AsAlignment         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT; // 256
	constexpr uint64 SkinnedEvictFrames  = 60;   // 스킨 항목은 정점 버퍼가 커서 더 빨리 해제
	constexpr uint64 SkinVertexPoolInitial = 32ull << 20; // 스킨 정점 풀 첫 크기 (커지면 2배)
	constexpr uint64 SkinVertexPoolMax     = (1ull << 27) * 4; // raw SRV 원소 상한 2^27 × 4바이트
	constexpr uint64 SkinBlasPoolInitial   = 16ull << 20;
	constexpr uint64 ScratchBudget         = 32ull << 20; // 프레임 슬롯 스크래치 목표 (넘으면 배리어 + 재사용)
	constexpr uint64 InvalidRange          = RayTracingMath::FRangeAllocator::InvalidOffset;
	constexpr float  CompactionMinGain   = 0.9f; // 압축 크기가 이보다 크면(10% 미만 절약) 복사하지 않는다

	static_assert(sizeof(FVertex) == 64, "RayTracingCommon.hlsli 정점 읽기는 FVertex 64바이트를 가정한다");
	static_assert(sizeof(FSkinVertex) == 24, "RayTracingSkinning.hlsl 스킨 스트림 읽기는 FSkinVertex 24바이트를 가정한다");

	// 엔진 행벡터 행렬(이동 = M[3][0..2]) → DXR 3x4 행우선 열벡터 변환 (Transform[r][c] = M[c][r])
	void ToInstanceTransform(const FMatrix4x4& World, float (&Out)[3][4])
	{
		for (uint32 Row = 0; Row < 3; ++Row)
		{
			for (uint32 Column = 0; Column < 4; ++Column)
			{
				Out[Row][Column] = World.M[Column][Row];
			}
		}
	}

	float Determinant3x3(const FMatrix4x4& M)
	{
		return M.M[0][0] * (M.M[1][1] * M.M[2][2] - M.M[1][2] * M.M[2][1]) - M.M[0][1] * (M.M[1][0] * M.M[2][2] - M.M[1][2] * M.M[2][0]) +
		       M.M[0][2] * (M.M[1][0] * M.M[2][1] - M.M[1][1] * M.M[2][0]);
	}

	D3D12_RAYTRACING_GEOMETRY_DESC MakeTriangles(D3D12_GPU_VIRTUAL_ADDRESS Vertices, uint32 VertexCount, D3D12_GPU_VIRTUAL_ADDRESS Indices,
	                                             uint32 IndexCount)
	{
		D3D12_RAYTRACING_GEOMETRY_DESC Geometry{};
		Geometry.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		Geometry.Flags                                = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE; // 불투명/Masked는 인스턴스 FORCE_* 플래그가 정한다
		Geometry.Triangles.VertexBuffer.StartAddress  = Vertices;
		Geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(FVertex);
		Geometry.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT; // FVertex::Position (오프셋 0)
		Geometry.Triangles.VertexCount                = VertexCount;
		Geometry.Triangles.IndexBuffer                = Indices;
		Geometry.Triangles.IndexCount                 = IndexCount;
		Geometry.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
		return Geometry;
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS MakeBottomInputs(const D3D12_RAYTRACING_GEOMETRY_DESC* Geometry,
	                                                                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags, uint32 Count = 1)
	{
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs{};
		Inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		Inputs.Flags          = Flags;
		Inputs.NumDescs       = Count;
		Inputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
		Inputs.pGeometryDescs = Geometry;
		return Inputs;
	}
} // namespace

FRayTracingScene::~FRayTracingScene()
{
	Shutdown();
}

bool FRayTracingScene::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary)
{
	Rhi     = &InRhi;
	Library = &InLibrary;
	Device5 = Rhi->GetDevice().GetDevice5();
	if (Device5 == nullptr)
	{
		return true; // DXR 미지원: 레이 트레이싱 효과가 모두 꺼진 채 계속
	}

	const uint32 ConstantsIndex = SkinningRoot.AddConstants(2, 0);
	const uint32 BaseIndex      = SkinningRoot.AddShaderResourceView(0);
	const uint32 SkinIndex      = SkinningRoot.AddShaderResourceView(1);
	const uint32 PaletteIndex   = SkinningRoot.AddShaderResourceView(15);
	const uint32 OutputIndex    = SkinningRoot.AddUnorderedAccessView(0);
	E_CHECK(ConstantsIndex == SkinParam_Constants && BaseIndex == SkinParam_Base && SkinIndex == SkinParam_Skin && PaletteIndex == SkinParam_Palette &&
	        OutputIndex == SkinParam_Output);
	if (!SkinningRoot.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"RayTracingSkinningRoot") ||
	    !CreatePipeline(SkinningPipeline, false))
	{
		E_LOG(LogRenderer, Error, "레이 트레이싱 스키닝 파이프라인 생성 실패 — 레이 트레이싱을 끕니다");
		Device5 = nullptr;
		return true;
	}

	Slots.resize(FD3D12RHI::FrameCount);
	for (uint32 Index = 0; Index < FD3D12RHI::FrameCount; ++Index)
	{
		Slots[Index]                = std::make_unique<FSlot>();
		Slots[Index]->PostbuildInfo = CreateBuffer(MaxPostbuild * sizeof(uint64), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		                                           D3D12_RESOURCE_STATE_COMMON, L"RtPostbuildInfo"); // 버퍼는 COMMON으로 만들어진다 (그래프가 전이)
		Slots[Index]->Readback = CreateBuffer(MaxPostbuild * sizeof(uint64), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
		                                      D3D12_RESOURCE_STATE_COPY_DEST, L"RtPostbuildReadback");
		if (!Slots[Index]->PostbuildInfo || !Slots[Index]->Readback)
		{
			E_LOG(LogRenderer, Error, "레이 트레이싱 압축 크기 버퍼 생성 실패 — 레이 트레이싱을 끕니다");
			Device5 = nullptr;
			return true;
		}
	}
	E_LOG(LogRenderer, Display, "레이 트레이싱 씬 초기화 (인라인 RayQuery, BLAS 캐시 + 프레임 TLAS)");
	return true;
}

bool FRayTracingScene::CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile)
{
	FShaderCompileDesc Desc;
	Desc.FileName   = L"RayTracingSkinning.hlsl";
	Desc.EntryPoint = L"CSMain";
	Desc.Stage      = EShaderStage::Compute;
	if (bForceRecompile && !Library->CookShader(Desc))
	{
		return false;
	}
	const ComPtr<IDxcBlob> Shader = Library->GetShader(Desc);
	return Shader && OutPipeline.InitCompute(Rhi->GetDevice().GetDevice(), SkinningRoot.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()),
	                                         L"RayTracingSkinning");
}

bool FRayTracingScene::ReloadShaders(bool bForceRecompile)
{
	if (Device5 == nullptr)
	{
		return true;
	}
	FD3D12PipelineState NewPipeline;
	if (!CreatePipeline(NewPipeline, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "레이 트레이싱 스키닝 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	SkinningPipeline.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());
	return true;
}

void FRayTracingScene::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	for (auto& [Hash, Entry] : StaticCache)
	{
		ReleaseStatic(*Entry);
	}
	ReleaseSkinnedPools();
	for (auto& [Data, Terrain] : TerrainCache)
	{
		ReleaseTerrain(*Terrain);
	}
	TerrainCache.clear();
	StaticCache.clear();
	for (std::unique_ptr<FSlot>& Slot : Slots)
	{
		Rhi->DeferRelease(Slot->Tlas);
		Rhi->DeferRelease(Slot->Scratch);
		Rhi->DeferRelease(Slot->PostbuildInfo);
		Rhi->DeferRelease(Slot->Readback);
	}
	Slots.clear();
	SkinningPipeline.Shutdown();
	SkinningRoot.Shutdown();
	Device5 = nullptr;
	Rhi     = nullptr;
}

ComPtr<ID3D12Resource> FRayTracingScene::CreateBuffer(uint64 Size, D3D12_HEAP_TYPE Heap, D3D12_RESOURCE_FLAGS Flags, D3D12_RESOURCE_STATES State,
                                                      const wchar_t* Name) const
{
	D3D12_HEAP_PROPERTIES HeapProperties{};
	HeapProperties.Type = Heap;
	D3D12_RESOURCE_DESC Desc{};
	Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	Desc.Width            = std::max<uint64>(Size, 256);
	Desc.Height           = 1;
	Desc.DepthOrArraySize = 1;
	Desc.MipLevels        = 1;
	Desc.SampleDesc.Count = 1;
	Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	Desc.Flags            = Flags;
	ComPtr<ID3D12Resource> Resource;
	if (FAILED(Rhi->GetDevice().GetDevice()->CreateCommittedResource(&HeapProperties, D3D12_HEAP_FLAG_NONE, &Desc, State, nullptr, IID_PPV_ARGS(&Resource))))
	{
		E_LOG(LogRenderer, Error, "레이 트레이싱 버퍼 생성 실패 ({} 바이트)", Size);
		return nullptr;
	}
	Resource->SetName(Name);
	return Resource;
}

bool FRayTracingScene::EnsureBuffer(ComPtr<ID3D12Resource>& Buffer, uint64& Capacity, uint64 Size, D3D12_RESOURCE_STATES State, const wchar_t* Name)
{
	if (Buffer && Capacity >= Size)
	{
		return true;
	}
	if (Buffer)
	{
		Rhi->DeferRelease(Buffer); // 이전 프레임이 아직 쓸 수 있다
	}
	const uint64 NewCapacity = AlignUp<uint64>(std::max<uint64>(Size + Size / 2, 64 * 1024), AsAlignment); // 1.5배 여유
	Buffer                   = CreateBuffer(NewCapacity, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, State, Name);
	Capacity                 = Buffer ? NewCapacity : 0;
	return Buffer != nullptr;
}

FD3D12DescriptorHandle FRayTracingScene::CreateRawSrv(ID3D12Resource* Resource, uint64 SizeInBytes)
{
	FD3D12DescriptorHandle Handle = Rhi->GetSrvAllocator().Allocate();
	if (!Handle.IsValid())
	{
		return Handle;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC Desc{};
	Desc.Format                  = DXGI_FORMAT_R32_TYPELESS;
	Desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
	Desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	Desc.Buffer.FirstElement     = 0;
	Desc.Buffer.NumElements      = static_cast<UINT>(SizeInBytes / 4);
	Desc.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
	Rhi->GetDevice().GetDevice()->CreateShaderResourceView(Resource, &Desc, Handle.Cpu);
	return Handle;
}

void FRayTracingScene::ReleaseStatic(FStaticBlas& Entry)
{
	Rhi->DeferRelease(Entry.Blas);
	Rhi->DeferRelease(Entry.CompactBlas);
	Entry.Blas.Reset();
	Entry.CompactBlas.Reset();
	if (Entry.VertexSrv.IsValid())
	{
		Rhi->DeferFreeDescriptor(Entry.VertexSrv);
		Entry.VertexSrv = {};
	}
	if (Entry.IndexSrv.IsValid())
	{
		Rhi->DeferFreeDescriptor(Entry.IndexSrv);
		Entry.IndexSrv = {};
	}
}

void FRayTracingScene::ReleaseSkinnedPools()
{
	for (auto& [Resource, Entry] : IndexSrvs)
	{
		Rhi->DeferFreeDescriptor(Entry.Srv);
		Rhi->DeferRelease(Entry.Resource);
	}
	IndexSrvs.clear();
	SkinnedPrimitives.clear();
	SkinnedGroups.clear();
	PendingFrees.clear();
	if (SkinVertexSrv.IsValid())
	{
		Rhi->DeferFreeDescriptor(SkinVertexSrv);
		SkinVertexSrv = {};
	}
	for (FSkinnedPool* Pool : { &SkinVertexPool, &SkinBlasPool })
	{
		if (Pool->Buffer)
		{
			Rhi->DeferRelease(Pool->Buffer);
			Pool->Buffer.Reset();
		}
		Pool->Allocator.Reset(0);
	}
	SkinVertexState = D3D12_RESOURCE_STATE_COMMON;
}

uint64 FRayTracingScene::AllocateSkinned(FSkinnedPool& Pool, uint64 Size, bool bVertexPool)
{
	const uint64 Offset = Pool.Allocator.Allocate(Size, AsAlignment);
	if (Offset != InvalidRange)
	{
		return Offset;
	}
	// 풀 확장 (1.5배): 새 버퍼(기존 위치 유지 — 할당기는 끝에 빈 칸을 늘린다) + 모든 스킨 BLAS 다시 빌드(정점은 다시 스키닝). 이전 버퍼는 지연 해제
	const uint64 OldCapacity = Pool.Allocator.GetCapacity();
	uint64       NewCapacity = std::max({ OldCapacity + OldCapacity / 2, OldCapacity + Size * 2, bVertexPool ? SkinVertexPoolInitial : SkinBlasPoolInitial });
	NewCapacity              = AlignUp<uint64>(NewCapacity, AsAlignment);
	if (bVertexPool)
	{
		NewCapacity = std::min(NewCapacity, SkinVertexPoolMax);
	}
	if (NewCapacity <= OldCapacity)
	{
		return InvalidRange; // 상한
	}
	ComPtr<ID3D12Resource> Buffer =
		CreateBuffer(NewCapacity, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		             bVertexPool ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		             bVertexPool ? L"RtSkinnedVertexPool" : L"RtSkinnedBlasPool");
	if (!Buffer)
	{
		return InvalidRange;
	}
	if (Pool.Buffer)
	{
		Rhi->DeferRelease(Pool.Buffer);
	}
	Pool.Buffer = std::move(Buffer);
	Pool.Allocator.Grow(NewCapacity);
	if (bVertexPool)
	{
		SkinVertexState = D3D12_RESOURCE_STATE_COMMON;
		if (SkinVertexSrv.IsValid())
		{
			Rhi->DeferFreeDescriptor(SkinVertexSrv);
		}
		SkinVertexSrv = CreateRawSrv(Pool.Buffer.Get(), NewCapacity);
	}
	for (auto& [Key, Group] : SkinnedGroups)
	{
		Group->bBuilt = false;
	}
	E_LOG(LogRenderer, Log, "레이 트레이싱 스킨 {} 풀 확장: {:.1f} → {:.1f} MB", bVertexPool ? "정점" : "BLAS", static_cast<double>(OldCapacity) / (1 << 20),
	      static_cast<double>(NewCapacity) / (1 << 20));
	return Pool.Allocator.Allocate(Size, AsAlignment);
}

void FRayTracingScene::FreeSkinned(FSkinnedPool& Pool, uint64& Offset, uint64 Size)
{
	if (Offset != InvalidRange)
	{
		PendingFrees.push_back({ &Pool, Offset, Size, FrameNumber }); // 이전 프레임 GPU 작업이 아직 읽을 수 있다
	}
	Offset = InvalidRange;
}

void FRayTracingScene::ProcessPendingFrees()
{
	std::erase_if(PendingFrees, [this](const FPendingRangeFree& Free) {
		if (FrameNumber < Free.Frame + FD3D12RHI::FrameCount)
		{
			return false;
		}
		Free.Pool->Allocator.Release(Free.Offset, Free.Size);
		return true;
	});
}

uint32 FRayTracingScene::GetIndexSrv(const FStaticMesh& Mesh)
{
	ID3D12Resource* Resource = Mesh.GetIndexBuffer().GetResource();
	FIndexSrv&      Entry    = IndexSrvs[Resource];
	if (!Entry.Srv.IsValid())
	{
		Entry.Resource = Resource;
		Entry.Srv      = CreateRawSrv(Resource, Mesh.GetIndexBuffer().GetSize());
	}
	Entry.LastUsedFrame = FrameNumber;
	return Entry.Srv.IsValid() ? Entry.Srv.Index : 0u;
}

void FRayTracingScene::ProcessCompactionReadback(FSlot& Slot)
{
	// 이 슬롯이 마지막으로 기록한 압축 크기 (같은 슬롯이 돌아왔으므로 그 프레임 GPU 작업은 끝났다 — RHI BeginFrame 대기)
	if (Slot.PostbuildCount == 0)
	{
		return;
	}
	const D3D12_RANGE ReadRange{ 0, Slot.PostbuildCount * sizeof(uint64) };
	void*             Mapped = nullptr;
	if (FAILED(Slot.Readback->Map(0, &ReadRange, &Mapped)))
	{
		return;
	}
	const uint64* Sizes = static_cast<const uint64*>(Mapped);
	for (auto& [Hash, EntryPtr] : StaticCache)
	{
		ProcessCompaction(*EntryPtr, Sizes, Slot.PostbuildCount);
	}
	for (auto& [Data, Terrain] : TerrainCache)
	{
		for (std::unique_ptr<FStaticBlas>& Tile : Terrain->Tiles)
		{
			ProcessCompaction(*Tile, Sizes, Slot.PostbuildCount);
		}
	}
	const D3D12_RANGE WriteRange{ 0, 0 };
	Slot.Readback->Unmap(0, &WriteRange);
	Slot.PostbuildCount = 0;
}

void FRayTracingScene::ProcessCompaction(FStaticBlas& Entry, const uint64* Sizes, uint32 SizeCount)
{
	if (Entry.State != EBlasState::Built || Entry.CompactionSlot != FrameSlot || Entry.BuildFrame >= FrameNumber || Entry.CompactionIndex >= SizeCount ||
	    !Entry.Blas)
	{
		return;
	}
	Entry.State              = EBlasState::Compacted;
	const uint64 CompactSize = AlignUp<uint64>(Sizes[Entry.CompactionIndex], AsAlignment);
	if (CompactSize == 0 || static_cast<float>(CompactSize) > static_cast<float>(Entry.BlasSize) * CompactionMinGain)
	{
		return;
	}
	Entry.CompactBlas = CreateBuffer(CompactSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	                                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"RtBlasCompact");
	if (!Entry.CompactBlas)
	{
		return;
	}
	CompactOps.push_back({ Entry.Blas.Get(), Entry.CompactBlas.Get() });
	FrameWrittenBlas.push_back(Entry.CompactBlas.Get());
	Stats.CompactionSavedBytes += Entry.BlasSize - CompactSize;
	// 이번 프레임 TLAS부터 압축본 (복사 패스가 TLAS 빌드 앞). 원본은 이번 프레임 복사가 끝난 뒤 해제
	Rhi->DeferRelease(Entry.Blas);
	Entry.Blas        = std::move(Entry.CompactBlas);
	Entry.CompactBlas = nullptr;
	Entry.BlasSize    = CompactSize;
	++Stats.CompactedThisFrame;
}

void FRayTracingScene::ReleaseTerrain(FTerrainBlas& Terrain)
{
	for (std::unique_ptr<FStaticBlas>& Tile : Terrain.Tiles)
	{
		ReleaseStatic(*Tile);
		Rhi->DeferRelease(Tile->OwnedVertices);
		Tile->OwnedVertices.Reset();
	}
	Terrain.Tiles.clear();
	Rhi->DeferRelease(Terrain.Indices);
	Terrain.Indices.Reset();
	if (Terrain.IndexSrv.IsValid())
	{
		Rhi->DeferFreeDescriptor(Terrain.IndexSrv);
		Terrain.IndexSrv = {};
	}
}

FRayTracingScene::FTerrainBlas* FRayTracingScene::EnsureTerrain(const FTerrainRayTracingInput& Input, const FRayTracingSceneOptions& Options)
{
	const FTerrainData& Data      = *Input.Data;
	const uint32        Cells     = Data.Resolution - 1;
	const uint32        TileCells = std::max(1u, std::min(Input.ChunkCells, Cells));
	if (Cells % TileCells != 0)
	{
		return nullptr;
	}
	std::unique_ptr<FTerrainBlas>& Entry = TerrainCache[Input.Data];
	const bool bParamsChanged = Entry && (Entry->Resolution != Data.Resolution || Entry->TileCells != TileCells || !(Entry->CellSize == Input.CellSize) ||
	                                      Entry->HeightScale != Input.HeightScale || Entry->Tiling != Input.Tiling || !(Entry->Color == Input.Color) ||
	                                      !(Entry->Origin == Input.Origin));
	if (Entry && bParamsChanged)
	{
		ReleaseTerrain(*Entry); // 크기·배율·UV 기준이 바뀌면 타일 전부 다시
		Entry.reset();
	}
	if (!Entry)
	{
		auto Terrain          = std::make_unique<FTerrainBlas>();
		Terrain->Data         = Input.Data;
		Terrain->Resolution   = Data.Resolution;
		Terrain->TileCells    = TileCells;
		Terrain->Step         = RayTracingMath::SelectTerrainStep(Cells, TileCells, Options.MaxTerrainVertices);
		Terrain->TilesPerSide = Cells / TileCells;
		Terrain->SeenCounter  = Data.ChangeCounter;
		Terrain->CellSize     = Input.CellSize;
		Terrain->HeightScale  = Input.HeightScale;
		Terrain->Tiling       = Input.Tiling;
		Terrain->Color        = Input.Color;
		Terrain->Origin       = Input.Origin;
		// 타일 공용 인덱스 (셀 대각선 (0,0)-(1,1) — 렌더·충돌과 같은 면)
		const uint32        Quads = TileCells / Terrain->Step;
		const uint32        Row   = Quads + 1;
		std::vector<uint32> Indices;
		Indices.reserve(static_cast<size_t>(Quads) * Quads * 6);
		for (uint32 Y = 0; Y < Quads; ++Y)
		{
			for (uint32 X = 0; X < Quads; ++X)
			{
				const uint32 V00 = Y * Row + X;
				const uint32 V10 = V00 + 1;
				const uint32 V01 = V00 + Row;
				const uint32 V11 = V01 + 1;
				Indices.insert(Indices.end(), { V00, V11, V10, V00, V01, V11 });
			}
		}
		Terrain->IndexCount = static_cast<uint32>(Indices.size());
		Terrain->Indices    = CreateBuffer(Indices.size() * sizeof(uint32), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
		                                   D3D12_RESOURCE_STATE_GENERIC_READ, L"RtTerrainIndices");
		void* Mapped = nullptr;
		if (!Terrain->Indices || FAILED(Terrain->Indices->Map(0, nullptr, &Mapped)))
		{
			TerrainCache.erase(Input.Data);
			return nullptr;
		}
		std::memcpy(Mapped, Indices.data(), Indices.size() * sizeof(uint32));
		Terrain->Indices->Unmap(0, nullptr);
		Terrain->IndexSrv = CreateRawSrv(Terrain->Indices.Get(), Indices.size() * sizeof(uint32));
		Terrain->Tiles.resize(static_cast<size_t>(Terrain->TilesPerSide) * Terrain->TilesPerSide);
		for (std::unique_ptr<FStaticBlas>& Tile : Terrain->Tiles)
		{
			Tile             = std::make_unique<FStaticBlas>();
			Tile->IndexCount = Terrain->IndexCount;
		}
		Entry = std::move(Terrain);
	}
	FTerrainBlas& Terrain = *Entry;
	// 편집: 바뀐 영역의 타일만 다시 (카운터가 줄었으면 같은 주소의 다른 데이터 → 전부)
	if (Data.ChangeCounter != Terrain.SeenCounter)
	{
		FTerrainRect Rect;
		const bool   bAll = Data.ChangeCounter < Terrain.SeenCounter || !Data.GetChangesSince(Terrain.SeenCounter, Rect);
		uint32       MinX = 0;
		uint32       MaxX = Terrain.TilesPerSide - 1;
		uint32       MinY = 0;
		uint32       MaxY = Terrain.TilesPerSide - 1;
		if (!bAll && !Rect.IsEmpty())
		{
			RayTracingMath::GetDirtyTileRange(Rect.MinX, Rect.MaxX, Terrain.TileCells, Terrain.TilesPerSide, MinX, MaxX);
			RayTracingMath::GetDirtyTileRange(Rect.MinY, Rect.MaxY, Terrain.TileCells, Terrain.TilesPerSide, MinY, MaxY);
		}
		for (uint32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (uint32 X = MinX; X <= MaxX; ++X)
			{
				Terrain.Tiles[static_cast<size_t>(Y) * Terrain.TilesPerSide + X]->bDirty = true;
			}
		}
		Terrain.SeenCounter = Data.ChangeCounter;
	}
	Terrain.LastUsedFrame = FrameNumber;
	return &Terrain;
}

bool FRayTracingScene::WriteTerrainTile(FTerrainBlas& Terrain, uint32 TileX, uint32 TileY, FStaticBlas& Tile)
{
	// 타일 정점 (FVertex, 지형 원점 기준 위치): 높이 = 16비트 × 배율, 법선 = 원본 해상도 중심 차분 (Terrain.hlsl ComputeTerrainNormal),
	// UV = 월드 XY × 레이어 0 타일링, 탄젠트 = +X (바이탄젠트 부호 -1: 노멀 맵 +Y = -Y 월드, Terrain.hlsl과 같은 틀), 색 = 레이어 0 색
	const FTerrainData& Data  = *Terrain.Data;
	const uint32        Quads = Terrain.TileCells / Terrain.Step;
	const uint32        Row   = Quads + 1;
	const int32         Last  = static_cast<int32>(Data.Resolution) - 1;
	std::vector<FVertex> Vertices(static_cast<size_t>(Row) * Row);
	for (uint32 J = 0; J < Row; ++J)
	{
		for (uint32 I = 0; I < Row; ++I)
		{
			const int32 GX = static_cast<int32>(TileX * Terrain.TileCells + I * Terrain.Step);
			const int32 GY = static_cast<int32>(TileY * Terrain.TileCells + J * Terrain.Step);
			const auto  Height = [&](int32 X, int32 Y) {
                return static_cast<float>(Data.GetHeight(std::clamp(X, 0, Last), std::clamp(Y, 0, Last))) * Terrain.HeightScale;
			};
			FVertex& Vertex = Vertices[static_cast<size_t>(J) * Row + I];
			Vertex.Position = FVector3(static_cast<float>(GX) * Terrain.CellSize.X, static_cast<float>(GY) * Terrain.CellSize.Y, Height(GX, GY));
			const float DzDx = (Height(GX + 1, GY) - Height(GX - 1, GY)) / (2.0f * Terrain.CellSize.X);
			const float DzDy = (Height(GX, GY + 1) - Height(GX, GY - 1)) / (2.0f * Terrain.CellSize.Y);
			Vertex.Normal    = FVector3(-DzDx, -DzDy, 1.0f).GetNormalized();
			Vertex.UV        = FVector2((Terrain.Origin.X + Vertex.Position.X) * Terrain.Tiling, (Terrain.Origin.Y + Vertex.Position.Y) * Terrain.Tiling);
			Vertex.Color     = Terrain.Color;
			Vertex.Tangent   = FVector4(1.0f, 0.0f, 0.0f, -1.0f);
		}
	}
	// 새 버퍼 (이전 프레임이 아직 이전 정점을 바인드리스로 읽을 수 있다 — 이전 것은 지연 해제)
	ReleaseStatic(Tile);
	Rhi->DeferRelease(Tile.OwnedVertices);
	const uint64 Bytes = Vertices.size() * sizeof(FVertex);
	Tile.OwnedVertices = CreateBuffer(Bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"RtTerrainVertices");
	void* Mapped       = nullptr;
	if (!Tile.OwnedVertices || FAILED(Tile.OwnedVertices->Map(0, nullptr, &Mapped)))
	{
		return false;
	}
	std::memcpy(Mapped, Vertices.data(), Bytes);
	Tile.OwnedVertices->Unmap(0, nullptr);
	Tile.VertexCount = static_cast<uint32>(Vertices.size());
	Tile.VertexSrv   = CreateRawSrv(Tile.OwnedVertices.Get(), Bytes);
	Tile.State       = EBlasState::Pending;
	Tile.BuildFrame  = 0;
	return Tile.VertexSrv.IsValid();
}

void FRayTracingScene::EvictUnused()
{
	for (auto It = TerrainCache.begin(); It != TerrainCache.end();)
	{
		if (RayTracingMath::ShouldEvictBlas(It->second->LastUsedFrame, FrameNumber))
		{
			ReleaseTerrain(*It->second);
			It = TerrainCache.erase(It);
		}
		else
		{
			++It;
		}
	}
	for (auto It = StaticCache.begin(); It != StaticCache.end();)
	{
		if (RayTracingMath::ShouldEvictBlas(It->second->LastUsedFrame, FrameNumber))
		{
			ReleaseStatic(*It->second);
			It = StaticCache.erase(It);
		}
		else
		{
			++It;
		}
	}
	for (auto It = SkinnedPrimitives.begin(); It != SkinnedPrimitives.end();)
	{
		It->second->bActive = false;
		if (RayTracingMath::ShouldEvictBlas(It->second->LastUsedFrame, FrameNumber, SkinnedEvictFrames))
		{
			FreeSkinned(SkinVertexPool, It->second->VertexOffset, It->second->VertexBytes);
			It = SkinnedPrimitives.erase(It);
		}
		else
		{
			++It;
		}
	}
	for (auto It = SkinnedGroups.begin(); It != SkinnedGroups.end();)
	{
		if (RayTracingMath::ShouldEvictBlas(It->second->LastUsedFrame, FrameNumber, SkinnedEvictFrames))
		{
			FreeSkinned(SkinBlasPool, It->second->BlasOffset, It->second->BlasSize);
			It = SkinnedGroups.erase(It);
		}
		else
		{
			++It;
		}
	}
	for (auto It = IndexSrvs.begin(); It != IndexSrvs.end();)
	{
		if (RayTracingMath::ShouldEvictBlas(It->second.LastUsedFrame, FrameNumber, SkinnedEvictFrames))
		{
			Rhi->DeferFreeDescriptor(It->second.Srv);
			Rhi->DeferRelease(It->second.Resource);
			It = IndexSrvs.erase(It);
		}
		else
		{
			++It;
		}
	}
}

FRayTracingScene::FStaticBlas* FRayTracingScene::FindOrCreateStatic(const FStaticMesh& Mesh, FMeshHandle Handle, uint32 Lod)
{
	const RayTracingMath::FBlasKey Key{ Handle.Index, Handle.Generation, Lod };
	const uint64                   Hash = RayTracingMath::HashBlasKey(Key);
	const D3D12_GPU_VIRTUAL_ADDRESS VertexAddress = Mesh.GetVertexBuffer().GetGpuAddress();
	if (const auto Found = StaticCache.find(Hash); Found != StaticCache.end())
	{
		FStaticBlas& Entry = *Found->second;
		if (Entry.Key == Key && Entry.Mesh == &Mesh && Entry.SourceVertexAddress == VertexAddress)
		{
			return &Entry;
		}
		// 같은 핸들의 메시가 다시 만들어졌거나 해시 충돌: 버리고 새로
		ReleaseStatic(Entry);
		StaticCache.erase(Found);
	}
	auto Entry                 = std::make_unique<FStaticBlas>();
	Entry->Key                 = Key;
	Entry->Mesh                = &Mesh;
	Entry->SourceVertexAddress = VertexAddress;
	const FStaticMesh::FLodRange& Range = Mesh.GetLod(Lod);
	Entry->FirstIndex = Range.IndexOffset;
	Entry->IndexCount = Range.IndexCount;
	Entry->VertexSrv  = CreateRawSrv(Mesh.GetVertexBuffer().GetResource(), Mesh.GetVertexBuffer().GetSize());
	Entry->IndexSrv   = CreateRawSrv(Mesh.GetIndexBuffer().GetResource(), Mesh.GetIndexBuffer().GetSize());
	if (!Entry->VertexSrv.IsValid() || !Entry->IndexSrv.IsValid() || Entry->IndexCount < 3)
	{
		ReleaseStatic(*Entry);
		return nullptr;
	}
	FStaticBlas* Raw = Entry.get();
	StaticCache.emplace(Hash, std::move(Entry));
	return Raw;
}

FRayTracingScene::FSkinnedPrimitive* FRayTracingScene::FindOrCreateSkinnedPrimitive(FEntity Entity, const FStaticMesh& Mesh, FMeshHandle Handle)
{
	std::unique_ptr<FSkinnedPrimitive>& Slot = SkinnedPrimitives[Entity.ToId()];
	if (Slot && (Slot->MeshHandle != Handle || Slot->Mesh != &Mesh || Slot->VertexCount != Mesh.GetVertexCount() ||
	             Slot->IndexCount != Mesh.GetLod(0).IndexCount))
	{
		FreeSkinned(SkinVertexPool, Slot->VertexOffset, Slot->VertexBytes); // 메시가 바뀌었다
		Slot.reset();
	}
	if (!Slot)
	{
		auto Entry          = std::make_unique<FSkinnedPrimitive>();
		Entry->MeshHandle   = Handle;
		Entry->Mesh         = &Mesh;
		Entry->VertexCount  = Mesh.GetVertexCount();
		Entry->IndexCount   = Mesh.GetLod(0).IndexCount;
		Entry->VertexBytes  = AlignUp<uint64>(static_cast<uint64>(Entry->VertexCount) * sizeof(FVertex), AsAlignment);
		Entry->VertexOffset = Entry->IndexCount >= 3 ? AllocateSkinned(SkinVertexPool, Entry->VertexBytes, true) : InvalidRange;
		if (Entry->VertexOffset == InvalidRange)
		{
			SkinnedPrimitives.erase(Entity.ToId());
			return nullptr;
		}
		// 풀 확장 때 맵이 다시 해시되어도 unique_ptr 대상은 그대로 — Slot 참조 대신 다시 찾는다
		std::unique_ptr<FSkinnedPrimitive>& Stored = SkinnedPrimitives[Entity.ToId()];
		Stored                                     = std::move(Entry);
		return Stored.get();
	}
	return Slot.get();
}

uint32 FRayTracingScene::RegisterMaterial(const FMaterial* Material, const FResourceManager& Resources, bool bGraphMaterials)
{
	if (const auto Found = MaterialIndices.find(Material); Found != MaterialIndices.end())
	{
		return Found->second;
	}
	const FMaterial* Default = Resources.GetMaterial(Resources.GetDefaultMaterial());
	FRayTracingMaterialGpu Gpu;
	const bool bGraph = Material->IsGraphMaterial();
	const FMaterial& Source = bGraph && Default != nullptr ? *Default : *Material;
	Gpu.BaseColorFactor   = Source.Constants.BaseColorFactor;
	Gpu.EmissiveFactor    = Source.Constants.EmissiveFactor;
	Gpu.Metallic          = Source.Constants.Metallic;
	Gpu.Roughness         = Source.Constants.Roughness;
	Gpu.NormalScale       = Source.Constants.NormalScale;
	Gpu.OcclusionStrength = Source.Constants.OcclusionStrength;
	Gpu.AlphaCutoff       = Material->Constants.AlphaCutoff;
	const FD3D12DescriptorHandle& Table = Source.TextureTable.IsValid() || Default == nullptr ? Source.TextureTable : Default->TextureTable;
	Gpu.TextureTable      = Table.IsValid() ? Table.Index : 0;
	const uint32 Index = static_cast<uint32>(MaterialInfos.size());
	if (bGraph)
	{
		// 그래프 머티리얼: 변형이 없을 때의 근사 = 중간 회색 고정 PBR + 기본 텍스처. 변형(E_RT_GRAPH_MATERIALS)은 GraphSlot으로 생성 함수를 부른다
		Gpu.BaseColorFactor = FVector4(0.5f, 0.5f, 0.5f, 1.0f);
		Gpu.EmissiveFactor  = FVector3::ZeroVector;
		Gpu.Metallic        = 0.0f;
		Gpu.Roughness       = 0.5f;
		Gpu.Flags |= FRayTracingMaterialGpu::MaterialFlagGraph;
		if (bGraphMaterials && Material->TextureTable.IsValid())
		{
			// 머리 (FMaterialGraphHeader: 시간, 알파 컷오프) + 파라미터 — MaterialRender::UploadMaterialConstants와 같은 내용
			Gpu.GraphParams       = static_cast<uint32>(GraphParams.size());
			Gpu.GraphTextureTable = Material->TextureTable.Index;
			GraphParams.push_back(FVector4(MaterialRender::GetMaterialTime(), Material->Constants.AlphaCutoff, 0.0f, 0.0f));
			GraphParams.insert(GraphParams.end(), Material->GraphConstants.begin(), Material->GraphConstants.end());
			GraphMaterialShaders.emplace_back(Index, Material->Shader);
		}
	}
	if (Material->BlendMode == EMaterialBlendMode::Masked)
	{
		Gpu.Flags |= FRayTracingMaterialGpu::MaterialFlagMasked;
	}
	MaterialInfos.push_back(Gpu);
	MaterialIndices.emplace(Material, Index);
	return Index;
}

void FRayTracingScene::Prepare(const FMeshInstanceList& Instances, const FResourceManager& Resources, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes,
                               const FRayTracingSceneOptions& Options, const std::vector<FTerrainRayTracingInput>* Terrains, const FScene* Scene)
{
	bPrepared = false;
	if (Device5 == nullptr)
	{
		return;
	}
	const auto CpuStart = std::chrono::steady_clock::now();
	FrameNumber         = Rhi->GetFrameNumber();
	FrameSlot           = Rhi->GetFrameSlot();
	PaletteAddress      = SkinPalettes;
	FSlot& Slot         = *Slots[FrameSlot];

	TlasDescs.clear();
	InstanceInfos.clear();
	MaterialInfos.clear();
	MaterialIndices.clear();
	GraphParams.clear();
	GraphMaterialShaders.clear();
	BuildOps.clear();
	BuildGeometries.clear();
	SkinOps.clear();
	CompactOps.clear();
	FrameWrittenBlas.clear();
	FrameSkinItems.clear();
	FrameGroupOrder.clear();
	bFrameSkinned      = false;
	bFrameSkinnedBuild = false;
	const uint64 SavedBytes = Stats.CompactionSavedBytes;
	Stats                   = FRayTracingSceneStats{};
	Stats.CompactionSavedBytes = SavedBytes;

	ProcessCompactionReadback(Slot);
	EvictUnused();
	ProcessPendingFrees();

	// 스크래치: 프레임 슬롯 버퍼 하나를 나눠 쓰고, ScratchBudget을 넘으면 처음으로 돌아가 재사용한다 (그 빌드 앞에 스크래치 UAV 배리어 —
	//   FBuildOp::bScratchBarrier). 씬 로드 프레임에 스킨 모델 수백 개를 한꺼번에 빌드해도 스크래치가 예산 안에 머문다
	uint64     ScratchCursor = 0;
	uint64     ScratchHigh   = 0;
	bScratchWrapped          = false;
	const auto AllocScratch  = [this, &ScratchCursor, &ScratchHigh](uint64 Size) {
		uint64 Offset = AlignUp<uint64>(ScratchCursor, AsAlignment);
		if (Offset > 0 && Offset + Size > ScratchBudget)
		{
			Offset          = 0;
			bScratchWrapped = true;
		}
		ScratchCursor = Offset + Size;
		ScratchHigh   = std::max(ScratchHigh, ScratchCursor);
		return Offset;
	};

	const std::vector<FMeshInstance>& List = Instances.GetInstances();
	TlasDescs.reserve(List.size());
	InstanceInfos.reserve(List.size());
	uint32 Builds        = 0;
	uint64 BuildTriangles = 0;
	for (const FMeshInstance& Instance : List)
	{
		if (Instance.Mesh == nullptr || Instance.Material == nullptr || Instance.IsTranslucent() || !Instance.Mesh->IsReady())
		{
			continue;
		}
		if (Instance.bFoliage && !Options.bFoliage)
		{
			continue;
		}
		const FStaticMesh& Mesh = *Instance.Mesh;
		FRayTracingInstanceGpu         Info;
		D3D12_RAYTRACING_INSTANCE_DESC Desc{};
		bool                           bMirrored = false;
		if (Instance.IsSkinned())
		{
			// 스킨: 모델별로 묶어 아래 PrepareSkinned에서 (묶음 키 = 스켈레톤 첫 조인트 + 양면·그림자 비트 — 인스턴스 플래그·마스크가 같아야 한다)
			if (!Options.bSkinned || !Mesh.IsSkinned() || SkinPalettes == 0)
			{
				continue;
			}
			uint64 Skeleton = Instance.Entity.ToId();
			if (Scene != nullptr)
			{
				const FSkinComponent* Skin = Scene->GetRegistry().TryGet<FSkinComponent>(Instance.Entity);
				if (Skin != nullptr && !Skin->Joints.empty())
				{
					Skeleton = Skin->Joints.front().ToId();
				}
			}
			const uint64 Key = (Skeleton << 2) | (Instance.bTwoSided ? 1u : 0u) | (Instance.CastsShadow() ? 2u : 0u);
			const uint32 Order = FrameGroupOrder.try_emplace(Key, static_cast<uint32>(FrameGroupOrder.size())).first->second;
			FrameSkinItems.push_back({ &Instance, Key, Order });
			continue;
		}
		{
			const uint32  Lod   = RayTracingMath::ClampLod(Instance.Lod, Mesh.GetLodCount());
			FStaticBlas* Entry = FindOrCreateStatic(Mesh, Instance.MeshHandle, Lod);
			if (Entry == nullptr)
			{
				continue;
			}
			Entry->LastUsedFrame = FrameNumber;
			if (Entry->State == EBlasState::Pending && Entry->BuildFrame != FrameNumber)
			{
				// 새 BLAS: 상한 안에서만 (넘치면 다음 프레임 — 그동안 이 인스턴스는 TLAS에 없다)
				const uint64 Triangles = Entry->IndexCount / 3;
				const bool   bCompact  = Options.bCompaction && Slot.PostbuildCount < MaxPostbuild;
				if (Builds >= Options.MaxBuildsPerFrame || (Builds > 0 && BuildTriangles + Triangles > Options.MaxBuildTriangles))
				{
					++Stats.PendingBuilds;
					continue;
				}
				FBuildOp Op;
				const D3D12_RAYTRACING_GEOMETRY_DESC Geometry =
					MakeTriangles(Mesh.GetVertexBuffer().GetGpuAddress(), Mesh.GetVertexCount(),
				                  Mesh.GetIndexBuffer().GetGpuAddress() + static_cast<uint64>(Entry->FirstIndex) * sizeof(uint32), Entry->IndexCount);
				Op.Flags    = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
				           (bCompact ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION
				                     : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE);
				const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = MakeBottomInputs(&Geometry, Op.Flags);
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO      Info2{};
				Device5->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &Info2);
				Entry->BlasSize    = AlignUp<uint64>(Info2.ResultDataMaxSizeInBytes, AsAlignment);
				Entry->ScratchSize = AlignUp<uint64>(Info2.ScratchDataSizeInBytes, AsAlignment);
				Entry->Blas        = CreateBuffer(Entry->BlasSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
				                                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"RtStaticBlas");
				if (!Entry->Blas)
				{
					continue;
				}
				Op.Dest          = Entry->Blas.Get();
				Op.DestAddress   = Entry->Blas->GetGPUVirtualAddress();
				Op.FirstGeometry = static_cast<uint32>(BuildGeometries.size());
				BuildGeometries.push_back(Geometry);
				Op.ScratchOffset   = AllocScratch(Entry->ScratchSize);
				Op.bScratchBarrier = std::exchange(bScratchWrapped, false);
				if (bCompact)
				{
					Op.PostbuildIndex      = static_cast<int32>(Slot.PostbuildCount);
					Entry->CompactionSlot  = FrameSlot;
					Entry->CompactionIndex = Slot.PostbuildCount++;
				}
				BuildOps.push_back(Op);
				Entry->BuildFrame = FrameNumber;
				Entry->State      = bCompact ? EBlasState::Built : EBlasState::Compacted; // 빌드 패스가 TLAS 빌드보다 앞
				FrameWrittenBlas.push_back(Entry->Blas.Get());
				++Builds;
				BuildTriangles += Triangles;
				++Stats.BuiltThisFrame;
			}
			if (!Entry->Blas)
			{
				continue;
			}
			const float Determinant = Determinant3x3(Instance.World);
			bMirrored               = Determinant < 0.0f;
			Info.VertexBuffer = Entry->VertexSrv.Index;
			Info.IndexBuffer  = Entry->IndexSrv.Index;
			Info.FirstIndex   = Entry->FirstIndex;
			Info.Flags        = bMirrored ? RayTracingMath::InstanceInfoMirrored : 0u;
			ToInstanceTransform(Instance.World, Desc.Transform);
			Desc.AccelerationStructure = Entry->Blas->GetGPUVirtualAddress();
		}

		const bool bMasked = Instance.IsMasked(); // 그래프 Masked도 후보 알파 테스트 (변형이 없으면 기본 텍스처 알파 1 → 불투명과 같음)
		Info.Material = RegisterMaterial(Instance.Material, Resources, Options.bGraphMaterials);
		Info.Flags |= (Instance.bTwoSided ? RayTracingMath::InstanceInfoTwoSided : 0u) | (bMasked ? RayTracingMath::InstanceInfoMasked : 0u);

		Desc.InstanceID   = static_cast<UINT>(InstanceInfos.size());
		Desc.InstanceMask = RayTracingMath::GetInstanceMask(false, Instance.bFoliage, false, Instance.CastsShadow());
		Desc.InstanceContributionToHitGroupIndex = RayTracingMath::ComputeHitGroupOffset(0);
		Desc.Flags = RayTracingMath::ComputeInstanceFlags(bMasked, Instance.bTwoSided, bMirrored);
		TlasDescs.push_back(Desc);
		InstanceInfos.push_back(Info);
	}

	// 스킨: 모델 BLAS (풀 하위 할당, 지오메트리 여러 개)
	PrepareSkinned(Options, Resources, AllocScratch);

	// 지형: 높이장 타일 BLAS (위치 = 지형 원점 기준, TLAS 변환 = 이동). 편집된 타일만 다시 빌드 (빌드 상한 공유)
	if (Options.bTerrain && Terrains != nullptr)
	{
		for (const FTerrainRayTracingInput& Input : *Terrains)
		{
			if (Input.Data == nullptr || !Input.Data->IsValid() || Input.Material == nullptr)
			{
				continue;
			}
			FTerrainBlas* Terrain = EnsureTerrain(Input, Options);
			if (Terrain == nullptr)
			{
				continue;
			}
			const uint32 MaterialIndex = RegisterMaterial(Input.Material, Resources, Options.bGraphMaterials);
			const uint64 TileTriangles = Terrain->IndexCount / 3;
			for (uint32 TileY = 0; TileY < Terrain->TilesPerSide; ++TileY)
			{
				for (uint32 TileX = 0; TileX < Terrain->TilesPerSide; ++TileX)
				{
					FStaticBlas& Tile = *Terrain->Tiles[static_cast<size_t>(TileY) * Terrain->TilesPerSide + TileX];
					Tile.LastUsedFrame = FrameNumber;
					if (Tile.bDirty)
					{
						const bool bCompact = Options.bCompaction && Slot.PostbuildCount < MaxPostbuild;
						if (Builds >= Options.MaxBuildsPerFrame || (Builds > 0 && BuildTriangles + TileTriangles > Options.MaxBuildTriangles))
						{
							++Stats.PendingBuilds; // 편집 중 지난 BLAS는 그대로 쓴다 (다시 만든 정점이 없을 때만 빠짐)
						}
						else if (WriteTerrainTile(*Terrain, TileX, TileY, Tile))
						{
							FBuildOp Op;
							const D3D12_RAYTRACING_GEOMETRY_DESC Geometry = MakeTriangles(
								Tile.OwnedVertices->GetGPUVirtualAddress(), Tile.VertexCount, Terrain->Indices->GetGPUVirtualAddress(), Terrain->IndexCount);
							Op.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
							           (bCompact ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION
							                     : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE);
							const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = MakeBottomInputs(&Geometry, Op.Flags);
							D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO      Prebuild{};
							Device5->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &Prebuild);
							Tile.BlasSize    = AlignUp<uint64>(Prebuild.ResultDataMaxSizeInBytes, AsAlignment);
							Tile.ScratchSize = AlignUp<uint64>(Prebuild.ScratchDataSizeInBytes, AsAlignment);
							Tile.Blas        = CreateBuffer(Tile.BlasSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
							                                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"RtTerrainBlas");
							if (Tile.Blas)
							{
								Op.Dest          = Tile.Blas.Get();
								Op.DestAddress   = Tile.Blas->GetGPUVirtualAddress();
								Op.FirstGeometry = static_cast<uint32>(BuildGeometries.size());
								BuildGeometries.push_back(Geometry);
								Op.ScratchOffset   = AllocScratch(Tile.ScratchSize);
								Op.bScratchBarrier = std::exchange(bScratchWrapped, false);
								if (bCompact)
								{
									Op.PostbuildIndex     = static_cast<int32>(Slot.PostbuildCount);
									Tile.CompactionSlot  = FrameSlot;
									Tile.CompactionIndex = Slot.PostbuildCount++;
								}
								BuildOps.push_back(Op);
								Tile.BuildFrame = FrameNumber;
								Tile.State      = bCompact ? EBlasState::Built : EBlasState::Compacted;
								Tile.bDirty     = false;
								FrameWrittenBlas.push_back(Tile.Blas.Get());
								++Builds;
								BuildTriangles += TileTriangles;
								++Stats.BuiltThisFrame;
							}
						}
					}
					if (!Tile.Blas)
					{
						continue;
					}
					FRayTracingInstanceGpu Info;
					Info.VertexBuffer = Tile.VertexSrv.Index;
					Info.IndexBuffer  = Terrain->IndexSrv.Index;
					Info.Material     = MaterialIndex;
					D3D12_RAYTRACING_INSTANCE_DESC Desc{};
					ToInstanceTransform(FMatrix4x4::Identity, Desc.Transform);
					Desc.Transform[0][3] = Input.Origin.X; // 이동만 (지형은 회전·스케일 무시)
					Desc.Transform[1][3] = Input.Origin.Y;
					Desc.Transform[2][3] = Input.Origin.Z;
					Desc.InstanceID      = static_cast<UINT>(InstanceInfos.size());
					Desc.InstanceMask    = RayTracingMath::GetInstanceMask(false, false, true, Input.bCastShadows);
					Desc.InstanceContributionToHitGroupIndex = RayTracingMath::ComputeHitGroupOffset(0);
					Desc.Flags = RayTracingMath::ComputeInstanceFlags(false, true, false); // 높이장: 컬링 없음 (아래에서 볼 일 없음)
					Desc.AccelerationStructure = Tile.Blas->GetGPUVirtualAddress();
					TlasDescs.push_back(Desc);
					InstanceInfos.push_back(Info);
				}
			}
		}
	}

	BuildGraphVariant(Options);

	// TLAS (빈 TLAS도 만든다 — 추적 셰이더는 항상 유효한 TLAS를 읽는다)
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS TopInputs{};
	TopInputs.Type        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	TopInputs.Flags       = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	TopInputs.NumDescs    = static_cast<UINT>(TlasDescs.size());
	TopInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO TopInfo{};
	Device5->GetRaytracingAccelerationStructurePrebuildInfo(&TopInputs, &TopInfo);
	TlasScratchSize   = AlignUp<uint64>(std::max<uint64>(TopInfo.ScratchDataSizeInBytes, 256), AsAlignment);
	TlasScratchOffset = AllocScratch(TlasScratchSize);
	if (!EnsureBuffer(Slot.Tlas, Slot.TlasCapacity, AlignUp<uint64>(TopInfo.ResultDataMaxSizeInBytes, AsAlignment),
	                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"RtTlas") ||
	    !EnsureBuffer(Slot.Scratch, Slot.ScratchCapacity, ScratchHigh, D3D12_RESOURCE_STATE_COMMON, L"RtScratch"))
	{
		return;
	}

	// 업로드 (동적 업로드 버퍼 — 이번 프레임만 유효)
	FD3D12DynamicUploadBuffer& Dynamic = Rhi->GetDynamicBuffer();
	const auto Upload = [&Dynamic](const void* Data, size_t Size, uint64 Alignment) {
		const FD3D12DynamicAllocation Allocation = Dynamic.Allocate(std::max<uint64>(Size, 64), Alignment);
		if (Size > 0)
		{
			std::memcpy(Allocation.CpuAddress, Data, Size);
		}
		return Allocation.GpuAddress;
	};
	TlasDescAddress       = Upload(TlasDescs.data(), TlasDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT);
	InstanceBufferAddress = Upload(InstanceInfos.data(), InstanceInfos.size() * sizeof(FRayTracingInstanceGpu), 256);
	MaterialBufferAddress = Upload(MaterialInfos.data(), MaterialInfos.size() * sizeof(FRayTracingMaterialGpu), 256);
	GraphParamAddress     = Upload(GraphParams.data(), GraphParams.size() * sizeof(FVector4), 256);
	TlasAddress           = Slot.Tlas->GetGPUVirtualAddress();

	// 통계
	Stats.TlasInstances = static_cast<uint32>(TlasDescs.size());
	Stats.StaticBlas    = static_cast<uint32>(StaticCache.size());
	Stats.SkinnedBlas   = static_cast<uint32>(SkinnedGroups.size());
	for (const auto& [Hash, Entry] : StaticCache)
	{
		Stats.BlasBytes += Entry->Blas ? Entry->BlasSize : 0;
	}
	for (const auto& [Data, Terrain] : TerrainCache)
	{
		for (const std::unique_ptr<FStaticBlas>& Tile : Terrain->Tiles)
		{
			Stats.TerrainTiles += Tile->Blas ? 1u : 0u;
			Stats.BlasBytes += Tile->Blas ? Tile->BlasSize : 0;
			Stats.TerrainVertexBytes += Tile->OwnedVertices ? static_cast<uint64>(Tile->VertexCount) * sizeof(FVertex) : 0;
		}
	}
	Stats.BlasBytes += SkinBlasPool.Allocator.GetCapacity();
	Stats.SkinnedVertexBytes = SkinVertexPool.Allocator.GetCapacity();
	for (const std::unique_ptr<FSlot>& Each : Slots)
	{
		Stats.TlasBytes += Each->TlasCapacity;
		Stats.ScratchBytes += Each->ScratchCapacity;
	}
	Stats.PrepareCpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - CpuStart).count();
	bPrepared          = true;
}

void FRayTracingScene::PrepareSkinned(const FRayTracingSceneOptions& Options, const FResourceManager& Resources,
                                      const std::function<uint64(uint64)>& AllocScratch)
{
	if (FrameSkinItems.empty())
	{
		return;
	}
	// 묶음 첫 등장 순서로 (같은 묶음 안은 인스턴스 목록 순서 = 지오메트리 번호) — 결정적
	std::stable_sort(FrameSkinItems.begin(), FrameSkinItems.end(),
	                 [](const FFrameSkinItem& A, const FFrameSkinItem& B) { return A.GroupOrder < B.GroupOrder; });

	struct FFrameGroup
	{
		FSkinnedGroup* Group = nullptr;
		size_t         Begin = 0; // FrameGeometries 범위
		size_t         End   = 0;
		float          Distance = 0.0f;
		bool           bTwoSided   = false;
		bool           bCastShadow = false;
	};
	struct FFrameGeometry
	{
		const FMeshInstance* Instance  = nullptr;
		FSkinnedPrimitive*   Primitive = nullptr;
	};
	std::vector<FFrameGroup>         Groups;
	std::vector<FFrameGeometry>      Geometries;
	std::vector<FSkinnedGeometryKey> Keys;
	Groups.reserve(FrameGroupOrder.size());
	Geometries.reserve(FrameSkinItems.size());

	// 1) 거리 판정 + 정점 범위 + 서명 (풀 확장이 여기서만 일어난다 → 2)는 최종 버퍼 주소를 쓴다)
	for (size_t Begin = 0; Begin < FrameSkinItems.size();)
	{
		size_t End = Begin + 1;
		while (End < FrameSkinItems.size() && FrameSkinItems[End].GroupOrder == FrameSkinItems[Begin].GroupOrder)
		{
			++End;
		}
		const uint64 Key      = FrameSkinItems[Begin].GroupKey;
		float        Distance = std::numeric_limits<float>::max();
		for (size_t Index = Begin; Index < End; ++Index)
		{
			const FBox& Bounds = FrameSkinItems[Index].Instance->WorldBounds;
			Distance           = std::min(Distance, (Bounds.GetCenter() - Options.CameraPosition).Length() - Bounds.GetExtent().Length());
		}
		const size_t ItemBegin = Begin;
		Begin                  = End;
		if (Distance > Options.SkinnedMaxDistance)
		{
			continue;
		}
		std::unique_ptr<FSkinnedGroup>& GroupSlot = SkinnedGroups[Key];
		if (!GroupSlot)
		{
			GroupSlot        = std::make_unique<FSkinnedGroup>();
			GroupSlot->Phase = RayTracingMath::ComputeRefitPhase(Key);
		}
		FSkinnedGroup* Group = GroupSlot.get(); // 맵이 다시 해시되어도 대상은 그대로
		Group->LastUsedFrame = FrameNumber;

		const size_t GeometryBegin = Geometries.size();
		Keys.clear();
		for (size_t Index = ItemBegin; Index < End; ++Index)
		{
			const FMeshInstance& Instance  = *FrameSkinItems[Index].Instance;
			FSkinnedPrimitive*   Primitive = FindOrCreateSkinnedPrimitive(Instance.Entity, *Instance.Mesh, Instance.MeshHandle);
			if (Primitive == nullptr || Primitive->bActive)
			{
				continue; // 생성 실패(풀 상한) 또는 같은 엔티티 중복
			}
			Primitive->bActive       = true;
			Primitive->LastUsedFrame = FrameNumber;
			Geometries.push_back({ &Instance, Primitive });
			Keys.push_back({ Instance.Entity.ToId(), Instance.Mesh, Primitive->VertexOffset, Instance.Mesh->GetIndexBuffer().GetGpuAddress(),
			                 Primitive->VertexCount, Primitive->IndexCount, !Instance.IsMasked() });
		}
		if (Keys.empty())
		{
			continue;
		}
		if (Keys != Group->Geometries || Group->BlasOffset == InvalidRange)
		{
			// 멤버가 바뀜: 새 크기로 다시 빌드 (이전 범위는 지연 해제)
			FreeSkinned(SkinBlasPool, Group->BlasOffset, Group->BlasSize);
			Group->Geometries = Keys;
			Group->bBuilt     = false;
			std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> Descs;
			Descs.reserve(Keys.size());
			for (const FSkinnedGeometryKey& Geometry : Keys)
			{
				Descs.push_back(MakeTriangles(Geometry.VertexOffset, Geometry.VertexCount, Geometry.IndexAddress, Geometry.IndexCount)); // 크기 계산은 주소를 보지 않는다
			}
			const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs =
				MakeBottomInputs(Descs.data(),
			                     D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD,
			                     static_cast<uint32>(Descs.size()));
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Info{};
			Device5->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &Info);
			Group->BlasSize    = AlignUp<uint64>(Info.ResultDataMaxSizeInBytes, AsAlignment);
			Group->ScratchSize = AlignUp<uint64>(std::max(Info.ScratchDataSizeInBytes, Info.UpdateScratchDataSizeInBytes), AsAlignment);
			Group->BlasOffset  = AllocateSkinned(SkinBlasPool, Group->BlasSize, false);
			if (Group->BlasOffset == InvalidRange)
			{
				Group->Geometries.clear();
				Geometries.resize(GeometryBegin);
				continue;
			}
		}
		const FMeshInstance& First = *FrameSkinItems[ItemBegin].Instance;
		Groups.push_back({ Group, GeometryBegin, Geometries.size(), Distance, First.bTwoSided, First.CastsShadow() });
	}
	if (Groups.empty())
	{
		return;
	}

	// 2) 스키닝 + BLAS 빌드/갱신 (주기) + TLAS 인스턴스 + 지오메트리별 히트 정보
	const D3D12_GPU_VIRTUAL_ADDRESS VertexBase = SkinVertexPool.Buffer->GetGPUVirtualAddress();
	const D3D12_GPU_VIRTUAL_ADDRESS BlasBase   = SkinBlasPool.Buffer->GetGPUVirtualAddress();
	const uint32                    VertexSrv  = SkinVertexSrv.IsValid() ? SkinVertexSrv.Index : 0u;
	for (const FFrameGroup& Frame : Groups)
	{
		FSkinnedGroup& Group = *Frame.Group;
		const uint32   Interval = RayTracingMath::GetSkinnedRefitInterval(Frame.Distance, Options.SkinnedRefitDistance, Options.SkinnedRefitInterval);
		const bool     bUpdate  = !Group.bBuilt || RayTracingMath::ShouldRefitSkinned(FrameNumber, Group.LastRefitFrame, Group.Phase, Interval);
		if (bUpdate)
		{
			FBuildOp Op;
			Op.Dest          = SkinBlasPool.Buffer.Get();
			Op.DestAddress   = BlasBase + Group.BlasOffset;
			Op.FirstGeometry = static_cast<uint32>(BuildGeometries.size());
			Op.GeometryCount = static_cast<uint32>(Frame.End - Frame.Begin);
			Op.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
			if (Group.bBuilt)
			{
				Op.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE; // 위상이 같으니 갱신(refit)
				Op.Source = Op.DestAddress;
				++Stats.RefitThisFrame;
			}
			else
			{
				++Stats.BuiltThisFrame;
			}
			for (size_t Index = Frame.Begin; Index < Frame.End; ++Index)
			{
				const FFrameGeometry&    Geometry = Geometries[Index];
				const FSkinnedPrimitive& Primitive = *Geometry.Primitive;
				const FStaticMesh&       Mesh      = *Geometry.Instance->Mesh;
				SkinOps.push_back({ Primitive.VertexOffset, Mesh.GetVertexBuffer().GetGpuAddress(), Mesh.GetSkinBuffer().GetGpuAddress(), Primitive.VertexCount,
				                    Geometry.Instance->BoneOffset,
				                    Geometry.Instance->bSkinCacheLod0 ? static_cast<uint64>(Geometry.Instance->SkinCacheVertex) * sizeof(FVertex) : ~0ull });
				D3D12_RAYTRACING_GEOMETRY_DESC Desc =
					MakeTriangles(VertexBase + Primitive.VertexOffset, Primitive.VertexCount, Mesh.GetIndexBuffer().GetGpuAddress(), Primitive.IndexCount);
				Desc.Flags = Geometry.Instance->IsMasked() ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
				BuildGeometries.push_back(Desc);
			}
			Op.ScratchOffset   = AllocScratch(Group.ScratchSize);
			Op.bScratchBarrier = std::exchange(bScratchWrapped, false);
			BuildOps.push_back(Op);
			Group.bBuilt         = true;
			Group.LastRefitFrame = FrameNumber;
			bFrameSkinnedBuild   = true;
		}
		else
		{
			++Stats.SkinnedRefitSkipped;
		}

		D3D12_RAYTRACING_INSTANCE_DESC Desc{};
		ToInstanceTransform(FMatrix4x4::Identity, Desc.Transform); // 스키닝 결과가 이미 월드 공간
		Desc.InstanceID   = static_cast<UINT>(InstanceInfos.size());
		Desc.InstanceMask = RayTracingMath::GetInstanceMask(true, false, false, Frame.bCastShadow);
		Desc.InstanceContributionToHitGroupIndex = RayTracingMath::ComputeHitGroupOffset(0);
		Desc.AccelerationStructure               = BlasBase + Group.BlasOffset;
		bool bAnyMasked = false;
		for (size_t Index = Frame.Begin; Index < Frame.End; ++Index)
		{
			const FFrameGeometry& Geometry = Geometries[Index];
			const bool            bMasked  = Geometry.Instance->IsMasked();
			bAnyMasked |= bMasked;
			FRayTracingInstanceGpu Info;
			Info.VertexBuffer = VertexSrv;
			Info.BaseVertex   = static_cast<uint32>(Geometry.Primitive->VertexOffset / sizeof(FVertex));
			Info.IndexBuffer  = GetIndexSrv(*Geometry.Instance->Mesh);
			Info.FirstIndex   = 0;
			Info.Material     = RegisterMaterial(Geometry.Instance->Material, Resources, Options.bGraphMaterials);
			Info.Flags        = RayTracingMath::InstanceInfoSkinned | (Frame.bTwoSided ? RayTracingMath::InstanceInfoTwoSided : 0u) |
			             (bMasked ? RayTracingMath::InstanceInfoMasked : 0u);
			InstanceInfos.push_back(Info);
		}
		Desc.Flags = RayTracingMath::ComputeSkinnedGroupInstanceFlags(bAnyMasked, Frame.bTwoSided);
		TlasDescs.push_back(Desc);
		Stats.SkinnedPrimitives += static_cast<uint32>(Frame.End - Frame.Begin);
	}
	bFrameSkinned = true;
	if (bFrameSkinnedBuild)
	{
		FrameWrittenBlas.push_back(SkinBlasPool.Buffer.Get());
	}
}

void FRayTracingScene::BuildGraphVariant(const FRayTracingSceneOptions& Options)
{
	// 슬롯 = 이번 프레임 그래프 셰이더를 해시 순으로 (인스턴스 순서와 무관 — 같은 집합이면 같은 생성 소스·같은 변형 키)
	std::vector<std::shared_ptr<const FMaterialShader>> Shaders;
	for (const auto& [MaterialIndex, Shader] : GraphMaterialShaders)
	{
		if (std::none_of(Shaders.begin(), Shaders.end(), [&Shader](const auto& Existing) { return Existing->Hash == Shader->Hash; }))
		{
			Shaders.push_back(Shader);
		}
	}
	std::sort(Shaders.begin(), Shaders.end(), [](const auto& A, const auto& B) { return A->Hash < B->Hash; });
	if (Shaders.size() > Options.MaxGraphSlots)
	{
		Shaders.resize(Options.MaxGraphSlots); // 나머지는 GraphSlot 0 = 회색 근사
	}
	for (const auto& [MaterialIndex, Shader] : GraphMaterialShaders)
	{
		const auto Found = std::find_if(Shaders.begin(), Shaders.end(), [&Shader](const auto& Existing) { return Existing->Hash == Shader->Hash; });
		MaterialInfos[MaterialIndex].GraphSlot = Found != Shaders.end() ? static_cast<uint32>(Found - Shaders.begin()) + 1u : 0u;
	}

	uint64 Key = 0;
	if (!Shaders.empty())
	{
		Key = 14695981039346656037ull; // FNV-1a 64 (해시 목록)
		for (const auto& Shader : Shaders)
		{
			for (uint32 Byte = 0; Byte < 8; ++Byte)
			{
				Key ^= (Shader->Hash >> (Byte * 8)) & 0xFFu;
				Key *= 1099511628211ull;
			}
		}
		Key = Key == 0 ? 1 : Key;
	}
	if (Key == GraphVariant.Key)
	{
		return; // 같은 집합 — 생성 소스 재사용
	}
	GraphVariant           = FRayTracingGraphVariant{};
	GraphVariant.Key       = Key;
	GraphVariant.SlotCount = static_cast<uint32>(Shaders.size());
	if (Key == 0)
	{
		return;
	}
	// 생성 함수마다 EvaluateMaterial을 슬롯 이름으로 바꿔 이어 붙이고 슬롯 분기 함수를 만든다 (RayTracingCommon.hlsli EvaluateHitMaterial)
	std::string& Source = GraphVariant.Source;
	Source = "// RT 그래프 머티리얼 변형 (FRayTracingScene::BuildGraphVariant 생성 — 손으로 고치지 않는다). 슬롯 = 셰이더 해시 순\n";
	for (uint32 Slot = 1; Slot <= Shaders.size(); ++Slot)
	{
		Source += std::format("#undef E_MATERIAL_GRAPH_CONSTANT_REGISTERS\n#undef E_MATERIAL_GRAPH_TEXTURE_COUNT\n#define EvaluateMaterial RtGraphMaterial{}\n", Slot);
		Source += Shaders[Slot - 1]->Hlsl;
		Source += "\n#undef EvaluateMaterial\n";
	}
	Source += "#undef E_MATERIAL_GRAPH_CONSTANT_REGISTERS\n#undef E_MATERIAL_GRAPH_TEXTURE_COUNT\n\n";
	Source += "void EvaluateGraphMaterial(uint Slot, in FMaterialPixelInputs In, out FMaterialSurface Out)\n{\n\tswitch (Slot)\n\t{\n";
	for (uint32 Slot = 1; Slot <= Shaders.size(); ++Slot)
	{
		Source += std::format("\tcase {0}:\n\t\tRtGraphMaterial{0}(In, Out);\n\t\treturn;\n", Slot);
	}
	Source += "\tdefault:\n\t\tEvaluateMaterial(In, Out);\n\t\treturn;\n\t}\n}\n";
	E_LOG(LogRenderer, Log, "레이 트레이싱 그래프 머티리얼 변형: 셰이더 {}개 (키 {:016x})", Shaders.size(), Key);
}

FRGResourceRef FRayTracingScene::AddBuildPasses(FRenderGraph& Graph, int32 Timer, ID3D12Resource* SkinCache, FRGResourceRef SkinCacheRef)
{
	FrameSkinVertexRef = {};
	if (!bPrepared)
	{
		return {};
	}
	FSlot&         Slot       = *Slots[FrameSlot];
	ID3D12Resource* Scratch    = Slot.Scratch.Get();
	const FRGResourceRef ScratchRef = Graph.Import("RtScratch", Scratch, ERGAccess::Common, ERGAccess::Common); // 버퍼는 COMMON에서 시작·끝 (ECL 끝 감쇠와 같은 상태)
	const FRGResourceRef TlasRef    = Graph.ImportAccelerationStructure("RtTlas", Slot.Tlas.Get());

	// 1) 스키닝 (계산): 기본 정점 + 스킨 스트림 + 프레임 팔레트 → 월드 공간 정점 (BLAS 갱신·히트 보간 공용)
	if (bFrameSkinned)
	{
		FrameSkinVertexRef = Graph.ImportTracked("RtSkinnedVertexPool", SkinVertexPool.Buffer.Get(), &SkinVertexState);
	}
	// 스킨 캐시가 이번 프레임 같은 식으로 LOD0 정점까지 스키닝한 인스턴스는 FVertex를 그대로 복사 (계산 스키닝 한 번으로 래스터·RT 공용),
	// 나머지(먼 LOD로 그려 캐시가 일부 정점만 스키닝)는 아래에서 직접 스키닝
	std::vector<FSkinOp> CopyOps;
	std::vector<FSkinOp> DispatchOps;
	for (const FSkinOp& Op : SkinOps)
	{
		(SkinCache != nullptr && SkinCacheRef.IsValid() && Op.CacheOffset != ~0ull ? CopyOps : DispatchOps).push_back(Op);
	}
	if (!CopyOps.empty())
	{
		FRenderGraph::FPassBuilder Pass = Graph.AddPass("RT 스킨 정점 복사");
		Pass.Read(SkinCacheRef, ERGAccess::CopySource).Write(FrameSkinVertexRef, ERGAccess::CopyDest);
		Pass.Timer(Timer).Execute([Ops = std::move(CopyOps), Source = SkinCache, Dest = SkinVertexPool.Buffer.Get()](FRGContext& Context) {
			for (const FSkinOp& Op : Ops)
			{
				Context.CommandList->CopyBufferRegion(Dest, Op.OutputOffset, Source, Op.CacheOffset, static_cast<uint64>(Op.VertexCount) * sizeof(FVertex));
			}
		});
	}
	if (!DispatchOps.empty())
	{
		// 정점 풀의 일부 범위만 쓴다 (갱신을 건너뛴 모델의 정점은 그대로) — 덮어쓰기 아님
		FRenderGraph::FPassBuilder Pass = Graph.AddPass("RT 스키닝");
		Pass.Write(FrameSkinVertexRef, ERGAccess::Uav);
		Pass.Timer(Timer).Execute([this, Ops = std::move(DispatchOps), Palette = PaletteAddress, Output = SkinVertexPool.Buffer->GetGPUVirtualAddress()](FRGContext& Context) {
			ID3D12GraphicsCommandList* CommandList = Context.CommandList;
			CommandList->SetComputeRootSignature(SkinningRoot.Get());
			CommandList->SetPipelineState(SkinningPipeline.Get());
			CommandList->SetComputeRootShaderResourceView(SkinParam_Palette, Palette);
			for (const FSkinOp& Op : Ops)
			{
				const uint32 Constants[2] = { Op.VertexCount, Op.BoneOffset };
				CommandList->SetComputeRoot32BitConstants(SkinParam_Constants, 2, Constants, 0);
				CommandList->SetComputeRootShaderResourceView(SkinParam_Base, Op.BaseVertices);
				CommandList->SetComputeRootShaderResourceView(SkinParam_Skin, Op.SkinVertices);
				CommandList->SetComputeRootUnorderedAccessView(SkinParam_Output, Output + Op.OutputOffset);
				CommandList->Dispatch((Op.VertexCount + SkinningGroupSize - 1) / SkinningGroupSize, 1, 1);
			}
		});
	}

	// 2) BLAS 빌드/갱신 (+ 압축 크기 기록)
	std::vector<FRGResourceRef> WrittenRefs;
	WrittenRefs.reserve(FrameWrittenBlas.size());
	for (ID3D12Resource* Blas : FrameWrittenBlas)
	{
		WrittenRefs.push_back(Graph.ImportAccelerationStructure("RtBlas", Blas));
	}
	const bool           bPostbuild   = Slot.PostbuildCount > 0;
	const FRGResourceRef PostbuildRef = Graph.Import("RtPostbuildInfo", Slot.PostbuildInfo.Get(), ERGAccess::Common, ERGAccess::Common);
	if (!BuildOps.empty())
	{
		FRenderGraph::FPassBuilder Pass = Graph.AddPass("BLAS 빌드");
		ID3D12Resource* const SkinBlas = SkinBlasPool.Buffer.Get();
		for (const FBuildOp& Op : BuildOps)
		{
			if (Op.Dest != SkinBlas) // 스킨 풀은 아래에서 한 번
			{
				Pass.Write(Graph.FindImported(Op.Dest), ERGAccess::AccelStructWrite);
			}
		}
		if (bFrameSkinnedBuild)
		{
			Pass.Write(Graph.FindImported(SkinBlas), ERGAccess::AccelStructWrite);
			Pass.Read(FrameSkinVertexRef, ERGAccess::SrvNonPixel);
		}
		Pass.Write(ScratchRef, ERGAccess::Uav);
		if (bPostbuild)
		{
			Pass.Write(PostbuildRef, ERGAccess::Uav);
		}
		Pass.Timer(Timer).Execute([Ops = BuildOps, Geometries = BuildGeometries, Scratch, ScratchRef, Postbuild = Slot.PostbuildInfo.Get()](FRGContext& Context) {
			ComPtr<ID3D12GraphicsCommandList4> List4;
			if (FAILED(Context.CommandList->QueryInterface(IID_PPV_ARGS(&List4))))
			{
				return;
			}
			const D3D12_GPU_VIRTUAL_ADDRESS ScratchBase = Scratch->GetGPUVirtualAddress();
			for (const FBuildOp& Op : Ops)
			{
				if (Op.bScratchBarrier)
				{
					Context.UavBarrier(ScratchRef); // 스크래치 예산을 넘어 앞 빌드의 스크래치 영역을 다시 쓴다
				}
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Desc{};
				Desc.Inputs                           = MakeBottomInputs(&Geometries[Op.FirstGeometry], Op.Flags, Op.GeometryCount);
				Desc.DestAccelerationStructureData    = Op.DestAddress;
				Desc.SourceAccelerationStructureData  = Op.Source;
				Desc.ScratchAccelerationStructureData = ScratchBase + Op.ScratchOffset;
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC PostbuildDesc{};
				PostbuildDesc.InfoType        = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
				PostbuildDesc.DestBuffer      = Postbuild->GetGPUVirtualAddress() + static_cast<uint64>(std::max(Op.PostbuildIndex, 0)) * sizeof(uint64);
				List4->BuildRaytracingAccelerationStructure(&Desc, Op.PostbuildIndex >= 0 ? 1u : 0u, Op.PostbuildIndex >= 0 ? &PostbuildDesc : nullptr);
			}
		});
	}

	// 3) 압축 복사 (몇 프레임 전 빌드의 압축 크기를 읽은 것)
	if (!CompactOps.empty())
	{
		FRenderGraph::FPassBuilder Pass = Graph.AddPass("BLAS 압축");
		for (const FCompactOp& Op : CompactOps)
		{
			Pass.Read(Graph.ImportAccelerationStructure("RtBlasSource", Op.Source), ERGAccess::AccelStructRead);
			Pass.Write(Graph.FindImported(Op.Dest), ERGAccess::AccelStructWrite, FRGSubresourceRange::All(), true);
		}
		Pass.Timer(Timer).Execute([Ops = CompactOps](FRGContext& Context) {
			ComPtr<ID3D12GraphicsCommandList4> List4;
			if (FAILED(Context.CommandList->QueryInterface(IID_PPV_ARGS(&List4))))
			{
				return;
			}
			for (const FCompactOp& Op : Ops)
			{
				List4->CopyRaytracingAccelerationStructure(Op.Dest->GetGPUVirtualAddress(), Op.Source->GetGPUVirtualAddress(),
				                                           D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
			}
		});
	}

	// 4) 압축 크기 리드백 (같은 슬롯이 돌아오면 Prepare가 읽는다)
	if (bPostbuild)
	{
		const uint64 Bytes = Slot.PostbuildCount * sizeof(uint64);
		Graph.AddPass("BLAS 압축 크기 리드백")
			.Read(PostbuildRef, ERGAccess::CopySource)
			.NeverCull()
			.Execute([Readback = Slot.Readback.Get(), Postbuild = Slot.PostbuildInfo.Get(), Bytes](FRGContext& Context) {
				Context.CommandList->CopyBufferRegion(Readback, 0, Postbuild, 0, Bytes);
			});
	}

	// 5) TLAS 빌드 (이번 프레임 쓰인 BLAS를 읽는다 → UAV 배리어)
	{
		FRenderGraph::FPassBuilder Pass = Graph.AddPass("TLAS 빌드");
		for (const FRGResourceRef& Ref : WrittenRefs)
		{
			Pass.Read(Ref, ERGAccess::AccelStructRead);
		}
		Pass.Write(TlasRef, ERGAccess::AccelStructWrite, FRGSubresourceRange::All(), true).Write(ScratchRef, ERGAccess::Uav);
		const UINT                      Count       = static_cast<UINT>(TlasDescs.size());
		const D3D12_GPU_VIRTUAL_ADDRESS Descs       = TlasDescAddress;
		const D3D12_GPU_VIRTUAL_ADDRESS Dest        = Slot.Tlas->GetGPUVirtualAddress();
		const D3D12_GPU_VIRTUAL_ADDRESS ScratchAddr = Scratch->GetGPUVirtualAddress() + TlasScratchOffset;
		Pass.Timer(Timer).Execute([Count, Descs, Dest, ScratchAddr](FRGContext& Context) {
			ComPtr<ID3D12GraphicsCommandList4> List4;
			if (FAILED(Context.CommandList->QueryInterface(IID_PPV_ARGS(&List4))))
			{
				return;
			}
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Desc{};
			Desc.Inputs.Type                      = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			Desc.Inputs.Flags                     = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			Desc.Inputs.NumDescs                  = Count;
			Desc.Inputs.DescsLayout               = D3D12_ELEMENTS_LAYOUT_ARRAY;
			Desc.Inputs.InstanceDescs             = Descs;
			Desc.DestAccelerationStructureData    = Dest;
			Desc.ScratchAccelerationStructureData = ScratchAddr;
			List4->BuildRaytracingAccelerationStructure(&Desc, 0, nullptr);
		});
	}
	return TlasRef;
}

void FRayTracingScene::DeclareTraceReads(FRenderGraph::FPassBuilder& Pass, FRGResourceRef Tlas) const
{
	Pass.Read(Tlas, ERGAccess::AccelStructRead);
	if (FrameSkinVertexRef.IsValid())
	{
		Pass.Read(FrameSkinVertexRef, ERGAccess::SrvPixel);
	}
}
