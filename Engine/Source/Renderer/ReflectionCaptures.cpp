#include "Renderer/ReflectionCaptures.h"

#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/IblMath.h"
#include "Renderer/ReflectionMath.h"
#include "Scene/Scene.h"

#include <cstring>
#include <fstream>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr DXGI_FORMAT CaptureFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	constexpr uint32      FaceMipCount  = 6 * FReflectionMath::CaptureMipCount; // 칸 하나의 하위 리소스 수
	const char            CaptureMagic[4] = { 'E', 'C', 'A', 'P' };

	// Ibl.hlsl BakeConstants와 1:1
	struct FBakeConstants
	{
		uint32 Size        = 0;
		uint32 SampleCount = IblMath::IntegrationSampleCount;
		float  Roughness   = 0.0f;
		uint32 Padding     = 0;
	};

	std::filesystem::path ResolveCapturePath(const std::string& AssetPath)
	{
		const std::filesystem::path Path = FStringConv::ToWide(AssetPath);
		if (Path.is_absolute())
		{
			return Path;
		}
		return (FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory()) / Path;
	}

	// 칸 Slot의 하위 리소스 범위 시작 (면 순서 = 배열 조각, 밉이 안쪽)
	uint32 FirstSubresource(uint32 Slot)
	{
		return Slot * FaceMipCount;
	}
} // namespace

// ---------------------------------------------------------------- 파일

size_t FReflectionCaptureFile::GetExpectedBytes() const
{
	size_t Bytes = 0;
	for (uint32 Mip = 0; Mip < MipCount; ++Mip)
	{
		Bytes += GetFaceMipBytes(Size, Mip);
	}
	return Bytes * 6;
}

bool FReflectionCaptureFile::Save(const std::filesystem::path& Path) const
{
	std::error_code Error;
	std::filesystem::create_directories(Path.parent_path(), Error);
	std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
	if (!Stream || Data.size() != GetExpectedBytes())
	{
		return false;
	}
	const uint32 Header[4] = { Version, Size, MipCount, static_cast<uint32>(CaptureFormat) };
	Stream.write(CaptureMagic, 4);
	Stream.write(reinterpret_cast<const char*>(Header), sizeof(Header));
	Stream.write(reinterpret_cast<const char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
	return static_cast<bool>(Stream);
}

bool FReflectionCaptureFile::Load(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path, std::ios::binary);
	if (!Stream)
	{
		return false;
	}
	char   Magic[4] = {};
	uint32 Header[4] = {};
	Stream.read(Magic, 4);
	Stream.read(reinterpret_cast<char*>(Header), sizeof(Header));
	if (!Stream || std::memcmp(Magic, CaptureMagic, 4) != 0 || Header[0] != Version || Header[3] != static_cast<uint32>(CaptureFormat))
	{
		return false;
	}
	Size     = Header[1];
	MipCount = Header[2];
	Data.resize(GetExpectedBytes());
	Stream.read(reinterpret_cast<char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
	return static_cast<bool>(Stream);
}

// ---------------------------------------------------------------- 관리자

FReflectionCaptures::~FReflectionCaptures()
{
	Shutdown();
}

bool FReflectionCaptures::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary)
{
	Rhi     = &InRhi;
	Library = &InLibrary;
	ID3D12Device*               Device = Rhi->GetDevice().GetDevice();
	const D3D12_HEAP_PROPERTIES Heap   = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);

	D3D12_RESOURCE_DESC AtlasDesc = MakeTexture2DDesc(FReflectionMath::CaptureSize, FReflectionMath::CaptureSize, CaptureFormat,
	                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, static_cast<uint16>(FReflectionMath::CaptureMipCount));
	AtlasDesc.DepthOrArraySize = static_cast<UINT16>(6 * FReflectionMath::MaxCaptures);
	E_D3D_VERIFY(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &AtlasDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
	                                             IID_PPV_ARGS(&Atlas)));
	Atlas->SetName(L"ReflectionCaptureAtlas");
	D3D12_SHADER_RESOURCE_VIEW_DESC AtlasView{};
	AtlasView.Format                        = CaptureFormat;
	AtlasView.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
	AtlasView.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	AtlasView.TextureCubeArray.MipLevels    = FReflectionMath::CaptureMipCount;
	AtlasView.TextureCubeArray.NumCubes     = FReflectionMath::MaxCaptures;
	AtlasSrv                                = Rhi->GetSrvAllocator().Allocate();
	Device->CreateShaderResourceView(Atlas.Get(), &AtlasView, AtlasSrv.Cpu);

	D3D12_RESOURCE_DESC RawDesc = MakeTexture2DDesc(FReflectionMath::CaptureSize, FReflectionMath::CaptureSize, CaptureFormat, D3D12_RESOURCE_FLAG_NONE, 1);
	RawDesc.DepthOrArraySize    = 6;
	E_D3D_VERIFY(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &RawDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&RawCube)));
	RawCube->SetName(L"ReflectionCaptureRaw");
	RawCubeState = D3D12_RESOURCE_STATE_COPY_DEST;
	D3D12_SHADER_RESOURCE_VIEW_DESC RawView{};
	RawView.Format                  = CaptureFormat;
	RawView.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURECUBE;
	RawView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	RawView.TextureCube.MipLevels   = 1;
	RawCubeSrv                      = Rhi->GetSrvAllocator().Allocate();
	Device->CreateShaderResourceView(RawCube.Get(), &RawView, RawCubeSrv.Cpu);

	const auto Table = [](D3D12_DESCRIPTOR_RANGE_TYPE Type, uint32 Register) {
		return std::vector<D3D12_DESCRIPTOR_RANGE1>{ FD3D12RootSignature::MakeRange(Type, 1, Register, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) };
	};
	PrefilterRoot.AddConstants(4, 0);
	PrefilterRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0));
	PrefilterRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0));
	PrefilterRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1));
	PrefilterRoot.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	if (!PrefilterRoot.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ReflectionCapturePrefilterRoot"))
	{
		return false;
	}
	Slots.resize(FReflectionMath::MaxCaptures);
	return CreatePipeline(PrefilterPipeline, false);
}

void FReflectionCaptures::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	Rhi->GetGraphicsQueue().Flush();
	ProcessPendingSaves(true); // GPU가 끝났으므로 남은 굽기를 저장
	for (FPendingSave& Pending : PendingSaves)
	{
		E_LOG(LogRenderer, Warning, "반사 캡처 저장 못 함 (종료): {}", Pending.AssetPath);
	}
	PendingSaves.clear();
	FD3D12DescriptorAllocator& Allocator = Rhi->GetSrvAllocator();
	Allocator.Free(AtlasSrv);
	Allocator.Free(RawCubeSrv);
	Atlas.Reset();
	RawCube.Reset();
	PrefilterPipeline.Shutdown();
	PrefilterRoot.Shutdown();
	Slots.clear();
	Rhi = nullptr;
}

bool FReflectionCaptures::CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile)
{
	FShaderCompileDesc Desc;
	Desc.FileName   = L"Ibl.hlsl";
	Desc.EntryPoint = L"PrefilterCS";
	Desc.Stage      = EShaderStage::Compute;
	if (bForceRecompile && !Library->CookShader(Desc))
	{
		return false;
	}
	const ComPtr<IDxcBlob> Shader = Library->GetShader(Desc);
	return Shader && OutPipeline.InitCompute(Rhi->GetDevice().GetDevice(), PrefilterRoot.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()),
	                                         L"ReflectionCapturePrefilter");
}

bool FReflectionCaptures::ReloadShaders(bool bForceRecompile)
{
	FD3D12PipelineState NewPipeline;
	if (!CreatePipeline(NewPipeline, bForceRecompile))
	{
		return false;
	}
	PrefilterPipeline.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());
	return true;
}

int32 FReflectionCaptures::FindOrAssignSlot(const std::string& AssetPath)
{
	for (uint32 Index = 0; Index < Slots.size(); ++Index)
	{
		if (Slots[Index].AssetPath == AssetPath)
		{
			return static_cast<int32>(Index);
		}
	}
	// 빈 칸 → 없으면 이번 프레임에 쓰지 않은 가장 오래된 칸
	int32  Best      = -1;
	uint64 BestFrame = ~0ull;
	for (uint32 Index = 0; Index < Slots.size(); ++Index)
	{
		const FSlot& Slot = Slots[Index];
		if (Slot.AssetPath.empty())
		{
			Best = static_cast<int32>(Index);
			break;
		}
		if (Slot.LastUsedFrame < GatherCount && Slot.LastUsedFrame < BestFrame)
		{
			Best      = static_cast<int32>(Index);
			BestFrame = Slot.LastUsedFrame;
		}
	}
	if (Best >= 0)
	{
		Slots[Best]           = FSlot{};
		Slots[Best].AssetPath = AssetPath;
	}
	return Best;
}

bool FReflectionCaptures::UploadSlot(uint32 Slot, const FReflectionCaptureFile& File)
{
	if (File.Size != FReflectionMath::CaptureSize || File.MipCount != FReflectionMath::CaptureMipCount)
	{
		return false;
	}
	ID3D12Device*             Device    = Rhi->GetDevice().GetDevice();
	const D3D12_RESOURCE_DESC AtlasDesc = Atlas->GetDesc();
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints(FaceMipCount);
	std::vector<UINT>                               Rows(FaceMipCount);
	std::vector<UINT64>                             RowBytes(FaceMipCount);
	UINT64                                          Total = 0;
	Device->GetCopyableFootprints(&AtlasDesc, FirstSubresource(Slot), FaceMipCount, 0, Footprints.data(), Rows.data(), RowBytes.data(), &Total);

	const D3D12_HEAP_PROPERTIES UploadHeap = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
	const D3D12_RESOURCE_DESC   BufferDesc = MakeBufferDesc(Total);
	ComPtr<ID3D12Resource>      Upload;
	E_D3D_VERIFY(Device->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &BufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
	                                             IID_PPV_ARGS(&Upload)));
	uint8* Mapped = nullptr;
	E_D3D_VERIFY(Upload->Map(0, nullptr, reinterpret_cast<void**>(&Mapped)));
	const uint8* Source = File.Data.data();
	for (uint32 Index = 0; Index < FaceMipCount; ++Index) // 면 바깥, 밉 안쪽 = 하위 리소스 순서
	{
		for (UINT Row = 0; Row < Rows[Index]; ++Row)
		{
			std::memcpy(Mapped + Footprints[Index].Offset + static_cast<UINT64>(Row) * Footprints[Index].Footprint.RowPitch, Source, RowBytes[Index]);
			Source += RowBytes[Index];
		}
	}
	Upload->Unmap(0, nullptr);

	return Rhi->GetGraphicsQueue().ExecuteImmediate(Device, [&](ID3D12GraphicsCommandList* List) {
		const D3D12_RESOURCE_BARRIER ToCopy = MakeTransitionBarrier(Atlas.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
		List->ResourceBarrier(1, &ToCopy);
		for (uint32 Index = 0; Index < FaceMipCount; ++Index)
		{
			D3D12_TEXTURE_COPY_LOCATION Dest{};
			Dest.pResource        = Atlas.Get();
			Dest.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			Dest.SubresourceIndex = FirstSubresource(Slot) + Index;
			D3D12_TEXTURE_COPY_LOCATION Src{};
			Src.pResource       = Upload.Get();
			Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			Src.PlacedFootprint = Footprints[Index];
			List->CopyTextureRegion(&Dest, 0, 0, 0, &Src, nullptr);
		}
		const D3D12_RESOURCE_BARRIER ToRead = MakeTransitionBarrier(Atlas.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		List->ResourceBarrier(1, &ToRead);
	});
}

uint32 FReflectionCaptures::Gather(FScene& Scene)
{
	++GatherCount;
	FrameCaptures.clear();
	struct FSorted
	{
		FReflectionCaptureGpuData Gpu;
		int32                     Priority = 0;
		float                     Volume   = 0.0f;
	};
	std::vector<FSorted> Sorted;
	FRegistry&           Registry = Scene.GetRegistry();
	Registry.View<FTransformComponent, FReflectionCaptureComponent>().Each([&](FEntity, FTransformComponent& Transform, FReflectionCaptureComponent& Capture) {
		if (Capture.CaptureAsset.empty() || Capture.Intensity <= 0.0f)
		{
			return;
		}
		const int32 SlotIndex = FindOrAssignSlot(Capture.CaptureAsset);
		if (SlotIndex < 0)
		{
			return; // 이번 프레임 칸이 모자람 (8개 초과)
		}
		FSlot& Slot = Slots[SlotIndex];
		if (!Slot.bLoaded && Slot.LastUsedFrame == 0)
		{
			// 처음 보는 경로: 구운 파일을 한 번 올린다 (없으면 굽기 전까지 쓰지 않음)
			FReflectionCaptureFile File;
			if (File.Load(ResolveCapturePath(Capture.CaptureAsset)) && UploadSlot(static_cast<uint32>(SlotIndex), File))
			{
				Slot.bLoaded = true;
			}
		}
		Slot.LastUsedFrame = GatherCount;
		if (!Slot.bLoaded)
		{
			return;
		}
		FSorted Item;
		Item.Gpu.Position     = Transform.GetWorldPosition();
		Item.Gpu.Shape        = Capture.Shape == 0 ? 0u : 1u;
		Item.Gpu.BoxExtent    = FVector3(FMath::Max(Capture.BoxExtent.X, 1.0f), FMath::Max(Capture.BoxExtent.Y, 1.0f), FMath::Max(Capture.BoxExtent.Z, 1.0f));
		Item.Gpu.Radius       = FMath::Max(Capture.Radius, 1.0f);
		Item.Gpu.FadeDistance = FMath::Max(Capture.FadeDistance, 1.0f);
		Item.Gpu.Intensity    = Capture.Intensity;
		Item.Gpu.Slot         = static_cast<uint32>(SlotIndex);
		Item.Priority         = Capture.Priority;
		Item.Volume           = Item.Gpu.Shape == 0 ? Item.Gpu.Radius * Item.Gpu.Radius * Item.Gpu.Radius
		                                            : Item.Gpu.BoxExtent.X * Item.Gpu.BoxExtent.Y * Item.Gpu.BoxExtent.Z;
		Sorted.push_back(Item);
	});
	// 우선순위 큰 것 → 작은 영역 먼저 (셰이더가 앞에서부터 남은 비중을 채운다)
	std::sort(Sorted.begin(), Sorted.end(), [](const FSorted& A, const FSorted& B) {
		return A.Priority != B.Priority ? A.Priority > B.Priority : A.Volume < B.Volume;
	});
	for (const FSorted& Item : Sorted)
	{
		FrameCaptures.push_back(Item.Gpu);
	}

	const size_t                  Count      = std::max<size_t>(FrameCaptures.size(), 1);
	const FD3D12DynamicAllocation Allocation = Rhi->GetDynamicBuffer().Allocate(sizeof(FReflectionCaptureGpuData) * Count, 16);
	if (FrameCaptures.empty())
	{
		std::memset(Allocation.CpuAddress, 0, sizeof(FReflectionCaptureGpuData));
	}
	else
	{
		std::memcpy(Allocation.CpuAddress, FrameCaptures.data(), sizeof(FReflectionCaptureGpuData) * FrameCaptures.size());
	}
	CaptureListAddress = Allocation.GpuAddress;
	return static_cast<uint32>(FrameCaptures.size());
}

void FReflectionCaptures::CopyFace(const FD3D12RenderTarget& SceneColor, uint32 Face)
{
	E_CHECKF(SceneColor.GetWidth() == FReflectionMath::CaptureSize && SceneColor.GetHeight() == FReflectionMath::CaptureSize, "캡처 면 크기가 다릅니다");
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	std::vector<D3D12_RESOURCE_BARRIER> Barriers;
	Barriers.push_back(MakeTransitionBarrier(SceneColor.GetColorResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE));
	if (RawCubeState != D3D12_RESOURCE_STATE_COPY_DEST)
	{
		Barriers.push_back(MakeTransitionBarrier(RawCube.Get(), RawCubeState, D3D12_RESOURCE_STATE_COPY_DEST));
		RawCubeState = D3D12_RESOURCE_STATE_COPY_DEST;
	}
	CommandList->ResourceBarrier(static_cast<UINT>(Barriers.size()), Barriers.data());

	D3D12_TEXTURE_COPY_LOCATION Dest{};
	Dest.pResource        = RawCube.Get();
	Dest.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	Dest.SubresourceIndex = Face;
	D3D12_TEXTURE_COPY_LOCATION Src{};
	Src.pResource        = SceneColor.GetColorResource();
	Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	Src.SubresourceIndex = 0;
	CommandList->CopyTextureRegion(&Dest, 0, 0, 0, &Src, nullptr);

	const D3D12_RESOURCE_BARRIER Back =
		MakeTransitionBarrier(SceneColor.GetColorResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	CommandList->ResourceBarrier(1, &Back);
}

void FReflectionCaptures::FinishBake(const std::string& AssetPath)
{
	const int32 SlotIndex = FindOrAssignSlot(AssetPath);
	if (SlotIndex < 0)
	{
		E_LOG(LogRenderer, Warning, "반사 캡처 칸이 모자라 굽기 결과를 올리지 못했습니다: {}", AssetPath);
		return;
	}
	const uint32               Slot        = static_cast<uint32>(SlotIndex);
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	ID3D12Device*              Device      = Rhi->GetDevice().GetDevice();
	FD3D12DescriptorAllocator& Allocator   = Rhi->GetSrvAllocator();

	// 1) 프리필터: 원시 큐브(SRV) → 아틀라스 칸의 밉마다 (UAV = 배열 조각 Slot*6부터 6장)
	const D3D12_RESOURCE_BARRIER ToFilter[] = {
		MakeTransitionBarrier(RawCube.Get(), RawCubeState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
		MakeTransitionBarrier(Atlas.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
	};
	CommandList->ResourceBarrier(2, ToFilter);
	RawCubeState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	const FD3D12DescriptorHandle Uavs = Allocator.AllocateRange(FReflectionMath::CaptureMipCount);
	for (uint32 Mip = 0; Mip < FReflectionMath::CaptureMipCount; ++Mip)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC Desc{};
		Desc.Format                         = CaptureFormat;
		Desc.ViewDimension                  = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
		Desc.Texture2DArray.MipSlice        = Mip;
		Desc.Texture2DArray.FirstArraySlice = Slot * 6;
		Desc.Texture2DArray.ArraySize       = 6;
		Device->CreateUnorderedAccessView(Atlas.Get(), nullptr, &Desc, Allocator.GetCpuHandle(Uavs.Index + Mip));
	}
	CommandList->SetComputeRootSignature(PrefilterRoot.Get());
	CommandList->SetPipelineState(PrefilterPipeline.Get());
	CommandList->SetComputeRootDescriptorTable(1, RawCubeSrv.Gpu);
	CommandList->SetComputeRootDescriptorTable(3, Uavs.Gpu); // u1 (프리필터는 쓰지 않음)
	for (uint32 Mip = 0; Mip < FReflectionMath::CaptureMipCount; ++Mip)
	{
		FBakeConstants Constants;
		Constants.Size      = FReflectionMath::CaptureSize >> Mip;
		Constants.Roughness = IblMath::MipToRoughness(Mip, FReflectionMath::CaptureMipCount);
		CommandList->SetComputeRoot32BitConstants(0, 4, &Constants, 0);
		CommandList->SetComputeRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{ Uavs.Gpu.ptr + static_cast<UINT64>(Mip) * Allocator.GetIncrementSize() });
		CommandList->Dispatch((Constants.Size + 7) / 8, (Constants.Size + 7) / 8, 6);
	}
	Rhi->DeferFreeDescriptor(Uavs);

	// 2) 리드백 (저장용) → 3) 셰이더 리소스로
	const D3D12_RESOURCE_BARRIER ToCopy = MakeTransitionBarrier(Atlas.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	CommandList->ResourceBarrier(1, &ToCopy);
	FPendingSave Pending;
	Pending.AssetPath = AssetPath;
	Pending.Footprints.resize(FaceMipCount);
	UINT64                    Total     = 0;
	const D3D12_RESOURCE_DESC AtlasDesc = Atlas->GetDesc();
	Device->GetCopyableFootprints(&AtlasDesc, FirstSubresource(Slot), FaceMipCount, 0, Pending.Footprints.data(), nullptr, nullptr, &Total);
	const D3D12_HEAP_PROPERTIES ReadbackHeap = MakeHeapProperties(D3D12_HEAP_TYPE_READBACK);
	const D3D12_RESOURCE_DESC   BufferDesc   = MakeBufferDesc(Total);
	if (SUCCEEDED(Device->CreateCommittedResource(&ReadbackHeap, D3D12_HEAP_FLAG_NONE, &BufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	                                              IID_PPV_ARGS(&Pending.Readback))))
	{
		for (uint32 Index = 0; Index < FaceMipCount; ++Index)
		{
			D3D12_TEXTURE_COPY_LOCATION Dest{};
			Dest.pResource       = Pending.Readback.Get();
			Dest.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			Dest.PlacedFootprint = Pending.Footprints[Index];
			D3D12_TEXTURE_COPY_LOCATION Src{};
			Src.pResource        = Atlas.Get();
			Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			Src.SubresourceIndex = FirstSubresource(Slot) + Index;
			CommandList->CopyTextureRegion(&Dest, 0, 0, 0, &Src, nullptr);
		}
		Pending.ReadyFrame = Rhi->GetFrameNumber() + FD3D12RHI::FrameCount;
		PendingSaves.push_back(std::move(Pending));
	}
	const D3D12_RESOURCE_BARRIER ToRead = MakeTransitionBarrier(Atlas.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	CommandList->ResourceBarrier(1, &ToRead);

	Slots[Slot].bLoaded       = true;
	Slots[Slot].LastUsedFrame = GatherCount;
}

void FReflectionCaptures::ProcessPendingSaves(bool bForce)
{
	const uint64 FrameNumber = Rhi->GetFrameNumber();
	for (size_t Index = 0; Index < PendingSaves.size();)
	{
		FPendingSave& Pending = PendingSaves[Index];
		if (!bForce && FrameNumber < Pending.ReadyFrame)
		{
			++Index;
			continue;
		}
		FReflectionCaptureFile File;
		File.Size     = FReflectionMath::CaptureSize;
		File.MipCount = FReflectionMath::CaptureMipCount;
		File.Data.resize(File.GetExpectedBytes());
		uint8* Mapped = nullptr;
		if (SUCCEEDED(Pending.Readback->Map(0, nullptr, reinterpret_cast<void**>(&Mapped))))
		{
			uint8* Dest = File.Data.data();
			for (uint32 Sub = 0; Sub < FaceMipCount; ++Sub)
			{
				const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Footprint = Pending.Footprints[Sub];
				const size_t                              RowBytes  = static_cast<size_t>(Footprint.Footprint.Width) * 8;
				for (UINT Row = 0; Row < Footprint.Footprint.Height; ++Row)
				{
					std::memcpy(Dest, Mapped + Footprint.Offset + static_cast<UINT64>(Row) * Footprint.Footprint.RowPitch, RowBytes);
					Dest += RowBytes;
				}
			}
			const D3D12_RANGE NoWrite{ 0, 0 };
			Pending.Readback->Unmap(0, &NoWrite);
			const std::filesystem::path Path = ResolveCapturePath(Pending.AssetPath);
			if (File.Save(Path))
			{
				E_LOG(LogRenderer, Display, "반사 캡처 저장: {}", FStringConv::ToUtf8(Path.wstring()));
			}
			else
			{
				E_LOG(LogRenderer, Error, "반사 캡처 저장 실패: {}", FStringConv::ToUtf8(Path.wstring()));
			}
		}
		Rhi->DeferRelease(Pending.Readback);
		PendingSaves.erase(PendingSaves.begin() + static_cast<std::ptrdiff_t>(Index));
	}
}
