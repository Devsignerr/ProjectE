#include "Renderer/RayTracingScene.h"

#include "Core/Math/MathUtils.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Material.h"
#include "Renderer/MeshInstancing.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/SkinnedMeshData.h"
#include "Renderer/StaticMesh.h"

#include <algorithm>
#include <chrono>
#include <cstring>

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
	                                                                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS Flags)
	{
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs{};
		Inputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		Inputs.Flags          = Flags;
		Inputs.NumDescs       = 1;
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
	for (auto& [Id, Entry] : SkinnedCache)
	{
		ReleaseSkinned(*Entry);
	}
	StaticCache.clear();
	SkinnedCache.clear();
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

void FRayTracingScene::ReleaseSkinned(FSkinnedBlas& Entry)
{
	Rhi->DeferRelease(Entry.Blas);
	Rhi->DeferRelease(Entry.Vertices);
	Entry.Blas.Reset();
	Entry.Vertices.Reset();
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
		FStaticBlas& Entry = *EntryPtr;
		if (Entry.State != EBlasState::Built || Entry.CompactionSlot != FrameSlot || Entry.BuildFrame >= FrameNumber ||
		    Entry.CompactionIndex >= Slot.PostbuildCount)
		{
			continue;
		}
		Entry.State              = EBlasState::Compacted;
		const uint64 CompactSize = AlignUp<uint64>(Sizes[Entry.CompactionIndex], AsAlignment);
		if (CompactSize == 0 || static_cast<float>(CompactSize) > static_cast<float>(Entry.BlasSize) * CompactionMinGain)
		{
			continue;
		}
		Entry.CompactBlas = CreateBuffer(CompactSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		                                 D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"RtBlasCompact");
		if (!Entry.CompactBlas)
		{
			continue;
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
	const D3D12_RANGE WriteRange{ 0, 0 };
	Slot.Readback->Unmap(0, &WriteRange);
	Slot.PostbuildCount = 0;
}

void FRayTracingScene::EvictUnused()
{
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
	for (auto It = SkinnedCache.begin(); It != SkinnedCache.end();)
	{
		It->second->bActive = false;
		if (RayTracingMath::ShouldEvictBlas(It->second->LastUsedFrame, FrameNumber, SkinnedEvictFrames))
		{
			ReleaseSkinned(*It->second);
			It = SkinnedCache.erase(It);
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

FRayTracingScene::FSkinnedBlas* FRayTracingScene::FindOrCreateSkinned(FEntity Entity, const FStaticMesh& Mesh, FMeshHandle Handle)
{
	if (const auto Found = SkinnedCache.find(Entity.ToId()); Found != SkinnedCache.end())
	{
		FSkinnedBlas& Entry = *Found->second;
		if (Entry.MeshHandle == Handle && Entry.Mesh == &Mesh && Entry.VertexCount == Mesh.GetVertexCount())
		{
			return &Entry;
		}
		ReleaseSkinned(Entry);
		SkinnedCache.erase(Found);
	}
	auto Entry         = std::make_unique<FSkinnedBlas>();
	Entry->Entity      = Entity;
	Entry->MeshHandle  = Handle;
	Entry->Mesh        = &Mesh;
	Entry->VertexCount = Mesh.GetVertexCount();
	Entry->IndexCount  = Mesh.GetLod(0).IndexCount;
	const uint64 VertexBytes = static_cast<uint64>(Entry->VertexCount) * sizeof(FVertex);
	Entry->Vertices    = CreateBuffer(VertexBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,
	                                  L"RtSkinnedVertices");
	Entry->VertexState = D3D12_RESOURCE_STATE_COMMON;
	if (!Entry->Vertices)
	{
		return nullptr;
	}
	const D3D12_RAYTRACING_GEOMETRY_DESC Geometry =
		MakeTriangles(Entry->Vertices->GetGPUVirtualAddress(), Entry->VertexCount, Mesh.GetIndexBuffer().GetGpuAddress(), Entry->IndexCount);
	const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = MakeBottomInputs(
		&Geometry, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD);
	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO Info{};
	Device5->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &Info);
	Entry->BlasSize    = AlignUp<uint64>(Info.ResultDataMaxSizeInBytes, AsAlignment);
	Entry->ScratchSize = AlignUp<uint64>(std::max(Info.ScratchDataSizeInBytes, Info.UpdateScratchDataSizeInBytes), AsAlignment);
	Entry->Blas        = CreateBuffer(Entry->BlasSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
	                                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"RtSkinnedBlas");
	Entry->VertexSrv   = CreateRawSrv(Entry->Vertices.Get(), VertexBytes);
	Entry->IndexSrv    = CreateRawSrv(Mesh.GetIndexBuffer().GetResource(), Mesh.GetIndexBuffer().GetSize());
	if (!Entry->Blas || !Entry->VertexSrv.IsValid() || !Entry->IndexSrv.IsValid())
	{
		ReleaseSkinned(*Entry);
		return nullptr;
	}
	FSkinnedBlas* Raw = Entry.get();
	SkinnedCache.emplace(Entity.ToId(), std::move(Entry));
	return Raw;
}

uint32 FRayTracingScene::RegisterMaterial(const FMaterial* Material, const FResourceManager& Resources)
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
	if (bGraph)
	{
		// 그래프 머티리얼: 히트에서 생성 셰이더를 평가할 수 없다(인라인 RayQuery 하나의 셰이더) → 중간 회색 고정 PBR 근사
		Gpu.BaseColorFactor = FVector4(0.5f, 0.5f, 0.5f, 1.0f);
		Gpu.EmissiveFactor  = FVector3::ZeroVector;
		Gpu.Metallic        = 0.0f;
		Gpu.Roughness       = 0.5f;
		Gpu.Flags |= FRayTracingMaterialGpu::MaterialFlagGraph;
	}
	if (Material->BlendMode == EMaterialBlendMode::Masked && !bGraph)
	{
		Gpu.Flags |= FRayTracingMaterialGpu::MaterialFlagMasked;
	}
	const uint32 Index = static_cast<uint32>(MaterialInfos.size());
	MaterialInfos.push_back(Gpu);
	MaterialIndices.emplace(Material, Index);
	return Index;
}

void FRayTracingScene::Prepare(const FMeshInstanceList& Instances, const FResourceManager& Resources, D3D12_GPU_VIRTUAL_ADDRESS SkinPalettes,
                               const FRayTracingSceneOptions& Options)
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
	BuildOps.clear();
	SkinOps.clear();
	CompactOps.clear();
	FrameWrittenBlas.clear();
	FrameSkinned.clear();
	const uint64 SavedBytes = Stats.CompactionSavedBytes;
	Stats                   = FRayTracingSceneStats{};
	Stats.CompactionSavedBytes = SavedBytes;

	ProcessCompactionReadback(Slot);
	EvictUnused();

	uint64     ScratchCursor = 0;
	const auto AllocScratch  = [&ScratchCursor](uint64 Size) {
        const uint64 Offset = AlignUp<uint64>(ScratchCursor, AsAlignment);
        ScratchCursor       = Offset + Size;
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
			if (!Options.bSkinned || !Mesh.IsSkinned() || SkinPalettes == 0)
			{
				continue;
			}
			const float Distance = (Instance.WorldBounds.GetCenter() - Options.CameraPosition).Length() - Instance.WorldBounds.GetExtent().Length();
			if (Distance > Options.SkinnedMaxDistance)
			{
				continue;
			}
			FSkinnedBlas* Entry = FindOrCreateSkinned(Instance.Entity, Mesh, Instance.MeshHandle);
			if (Entry == nullptr || Entry->bActive)
			{
				continue; // 생성 실패 또는 같은 엔티티 중복
			}
			Entry->bActive       = true;
			Entry->BoneOffset    = Instance.BoneOffset;
			Entry->LastUsedFrame = FrameNumber;
			SkinOps.push_back({ Entry->Vertices.Get(), &Entry->VertexState, Mesh.GetVertexBuffer().GetGpuAddress(), Mesh.GetSkinBuffer().GetGpuAddress(),
			                    Entry->VertexCount, Instance.BoneOffset });
			FBuildOp Op;
			Op.Dest     = Entry->Blas.Get();
			Op.Geometry = MakeTriangles(Entry->Vertices->GetGPUVirtualAddress(), Entry->VertexCount, Mesh.GetIndexBuffer().GetGpuAddress(), Entry->IndexCount);
			Op.Flags    = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
			if (Entry->bBuilt)
			{
				Op.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE; // 위상이 같으니 갱신(refit)
				Op.Source = Entry->Blas->GetGPUVirtualAddress();
				++Stats.RefitThisFrame;
			}
			else
			{
				++Stats.BuiltThisFrame;
			}
			Op.ScratchOffset = AllocScratch(Entry->ScratchSize);
			BuildOps.push_back(Op);
			Entry->bBuilt = true;
			FrameWrittenBlas.push_back(Entry->Blas.Get());
			FrameSkinned.push_back(Entry);

			Info.VertexBuffer = Entry->VertexSrv.Index;
			Info.IndexBuffer  = Entry->IndexSrv.Index;
			Info.FirstIndex   = 0;
			Info.Flags        = RayTracingMath::InstanceInfoSkinned;
			ToInstanceTransform(FMatrix4x4::Identity, Desc.Transform); // 스키닝 결과가 이미 월드 공간
			Desc.AccelerationStructure = Entry->Blas->GetGPUVirtualAddress();
		}
		else
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
				Op.Geometry = MakeTriangles(Mesh.GetVertexBuffer().GetGpuAddress(), Mesh.GetVertexCount(),
				                            Mesh.GetIndexBuffer().GetGpuAddress() + static_cast<uint64>(Entry->FirstIndex) * sizeof(uint32), Entry->IndexCount);
				Op.Flags    = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
				           (bCompact ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION
				                     : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE);
				const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = MakeBottomInputs(&Op.Geometry, Op.Flags);
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
				Op.ScratchOffset = AllocScratch(Entry->ScratchSize);
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

		const bool bMasked = Instance.IsMasked() && !Instance.Material->IsGraphMaterial();
		Info.Material = RegisterMaterial(Instance.Material, Resources);
		Info.Flags |= (Instance.bTwoSided ? RayTracingMath::InstanceInfoTwoSided : 0u) | (bMasked ? RayTracingMath::InstanceInfoMasked : 0u);

		Desc.InstanceID   = static_cast<UINT>(InstanceInfos.size());
		Desc.InstanceMask = RayTracingMath::GetInstanceMask(Instance.IsSkinned(), Instance.bFoliage, false, Instance.CastsShadow());
		Desc.InstanceContributionToHitGroupIndex = RayTracingMath::ComputeHitGroupOffset(0);
		Desc.Flags = RayTracingMath::ComputeInstanceFlags(bMasked, Instance.bTwoSided, bMirrored);
		TlasDescs.push_back(Desc);
		InstanceInfos.push_back(Info);
	}

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
	    !EnsureBuffer(Slot.Scratch, Slot.ScratchCapacity, ScratchCursor, D3D12_RESOURCE_STATE_COMMON, L"RtScratch"))
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
	TlasAddress           = Slot.Tlas->GetGPUVirtualAddress();

	// 통계
	Stats.TlasInstances = static_cast<uint32>(TlasDescs.size());
	Stats.StaticBlas    = static_cast<uint32>(StaticCache.size());
	Stats.SkinnedBlas   = static_cast<uint32>(SkinnedCache.size());
	for (const auto& [Hash, Entry] : StaticCache)
	{
		Stats.BlasBytes += Entry->Blas ? Entry->BlasSize : 0;
	}
	for (const auto& [Id, Entry] : SkinnedCache)
	{
		Stats.BlasBytes += Entry->BlasSize;
		Stats.SkinnedVertexBytes += static_cast<uint64>(Entry->VertexCount) * sizeof(FVertex);
	}
	for (const std::unique_ptr<FSlot>& Each : Slots)
	{
		Stats.TlasBytes += Each->TlasCapacity;
		Stats.ScratchBytes += Each->ScratchCapacity;
	}
	Stats.PrepareCpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - CpuStart).count();
	bPrepared          = true;
}

FRGResourceRef FRayTracingScene::AddBuildPasses(FRenderGraph& Graph, int32 Timer)
{
	FrameSkinVertexRefs.clear();
	if (!bPrepared)
	{
		return {};
	}
	FSlot&         Slot       = *Slots[FrameSlot];
	ID3D12Resource* Scratch    = Slot.Scratch.Get();
	const FRGResourceRef ScratchRef = Graph.Import("RtScratch", Scratch, ERGAccess::Common, ERGAccess::Common); // 버퍼는 COMMON에서 시작·끝 (ECL 끝 감쇠와 같은 상태)
	const FRGResourceRef TlasRef    = Graph.ImportAccelerationStructure("RtTlas", Slot.Tlas.Get());

	// 1) 스키닝 (계산): 기본 정점 + 스킨 스트림 + 프레임 팔레트 → 월드 공간 정점 (BLAS 갱신·히트 보간 공용)
	for (FSkinnedBlas* Entry : FrameSkinned)
	{
		FrameSkinVertexRefs.push_back(Graph.ImportTracked("RtSkinnedVertices", Entry->Vertices.Get(), &Entry->VertexState));
	}
	if (!SkinOps.empty())
	{
		FRenderGraph::FPassBuilder Pass = Graph.AddPass("RT 스키닝");
		for (const FRGResourceRef& Ref : FrameSkinVertexRefs)
		{
			Pass.Write(Ref, ERGAccess::Uav, FRGSubresourceRange::All(), true);
		}
		Pass.Timer(Timer).Execute([this, Ops = SkinOps, Palette = PaletteAddress](FRGContext& Context) {
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
				CommandList->SetComputeRootUnorderedAccessView(SkinParam_Output, Op.Output->GetGPUVirtualAddress());
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
		for (const FBuildOp& Op : BuildOps)
		{
			Pass.Write(Graph.FindImported(Op.Dest), ERGAccess::AccelStructWrite);
		}
		for (const FRGResourceRef& Ref : FrameSkinVertexRefs)
		{
			Pass.Read(Ref, ERGAccess::SrvNonPixel);
		}
		Pass.Write(ScratchRef, ERGAccess::Uav);
		if (bPostbuild)
		{
			Pass.Write(PostbuildRef, ERGAccess::Uav);
		}
		Pass.Timer(Timer).Execute([Ops = BuildOps, Scratch, Postbuild = Slot.PostbuildInfo.Get()](FRGContext& Context) {
			ComPtr<ID3D12GraphicsCommandList4> List4;
			if (FAILED(Context.CommandList->QueryInterface(IID_PPV_ARGS(&List4))))
			{
				return;
			}
			const D3D12_GPU_VIRTUAL_ADDRESS ScratchBase = Scratch->GetGPUVirtualAddress();
			for (const FBuildOp& Op : Ops)
			{
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Desc{};
				Desc.Inputs                           = MakeBottomInputs(&Op.Geometry, Op.Flags);
				Desc.DestAccelerationStructureData    = Op.Dest->GetGPUVirtualAddress();
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
	for (const FRGResourceRef& Ref : FrameSkinVertexRefs)
	{
		Pass.Read(Ref, ERGAccess::SrvPixel);
	}
}
