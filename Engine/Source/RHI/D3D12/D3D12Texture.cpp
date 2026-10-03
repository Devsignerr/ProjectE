#include "RHI/D3D12/D3D12Texture.h"

#include "RHI/D3D12/D3D12CommandQueue.h"
#include "RHI/D3D12/D3D12Device.h"
#include "RHI/D3D12/D3D12MipGenerator.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12UploadQueue.h"
#include "RHI/TextureUtils.h"

#include <cstring>
#include <utility>
#include <vector>

FD3D12Texture::~FD3D12Texture()
{
	Shutdown();
}

bool FD3D12Texture::Init2D(FD3D12Device& Device, FD3D12CommandQueue& Queue, FD3D12DescriptorAllocator& InSrvAllocator,
                           uint32 InWidth, uint32 InHeight, DXGI_FORMAT InFormat, const void* Pixels, uint32 BytesPerPixel,
                           const wchar_t* DebugName, bool bGenerateMips)
{
	E_CHECKF(Resource == nullptr, "텍스처가 이미 생성되어 있습니다");
	E_CHECKF(Pixels != nullptr && InWidth > 0 && InHeight > 0, "텍스처 데이터가 비어 있습니다");

	ID3D12Device* D3DDevice = Device.GetDevice();
	SrvAllocator            = &InSrvAllocator;
	Width                   = InWidth;
	Height                  = InHeight;
	Format                  = InFormat;

	// 밉 체인 여부 결정: 옵션 + 포맷 지원 + 크기 + 생성기 사용 가능
	MipCount = 1;
	FD3D12MipGenerator* MipGenerator = nullptr;
	if (bGenerateMips && FD3D12MipGenerator::SupportsFormat(Format) && CalculateMipCount(Width, Height) > 1)
	{
		MipGenerator = Device.GetMipGenerator();
		if (MipGenerator != nullptr)
		{
			MipCount = CalculateMipCount(Width, Height);
		}
	}
	const bool bWithMips = MipCount > 1;

	// GPU 텍스처 (복사 대상 상태로 생성).
	// 밉을 생성할 때는 sRGB UAV가 불가능하므로 리소스를 TYPELESS로 만들고 SRV(sRGB)/UAV(UNORM) 뷰를 따로 만든다.
	const D3D12_HEAP_PROPERTIES DefaultHeap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   TextureDesc =
		MakeTexture2DDesc(Width, Height, bWithMips ? GetTypelessFormat(Format) : Format,
	                      bWithMips ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE,
	                      static_cast<uint16>(MipCount));
	E_D3D_VERIFY(D3DDevice->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &TextureDesc,
	                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Resource)));
	Resource->SetName(DebugName);

	// 밉 0 업로드 레이아웃 (행 피치는 256바이트 정렬)
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
	UINT                               NumRows        = 0;
	UINT64                             RowSizeInBytes = 0;
	UINT64                             TotalBytes     = 0;
	D3DDevice->GetCopyableFootprints(&TextureDesc, 0, 1, 0, &Footprint, &NumRows, &RowSizeInBytes, &TotalBytes);

	const uint64 SourceRowPitch = static_cast<uint64>(Width) * BytesPerPixel;
	E_CHECKF(RowSizeInBytes == SourceRowPitch, "포맷과 BytesPerPixel이 일치하지 않습니다 ({} != {})", RowSizeInBytes, SourceRowPitch);

	// 스테이징 업로드 버퍼에 행 단위로 복사
	const D3D12_HEAP_PROPERTIES UploadHeap = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
	const D3D12_RESOURCE_DESC   UploadDesc = MakeBufferDesc(TotalBytes);
	ComPtr<ID3D12Resource>      UploadBuffer;
	E_D3D_VERIFY(D3DDevice->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &UploadDesc,
	                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&UploadBuffer)));
	UploadBuffer->SetName(L"StagingTextureUpload");

	uint8*            Mapped = nullptr;
	const D3D12_RANGE NoRead{ 0, 0 };
	E_D3D_VERIFY(UploadBuffer->Map(0, &NoRead, reinterpret_cast<void**>(&Mapped)));

	const uint8* Source = static_cast<const uint8*>(Pixels);
	for (UINT Row = 0; Row < NumRows; ++Row)
	{
		std::memcpy(Mapped + Footprint.Offset + static_cast<uint64>(Row) * Footprint.Footprint.RowPitch,
		            Source + static_cast<uint64>(Row) * SourceRowPitch, static_cast<size_t>(SourceRowPitch));
	}
	UploadBuffer->Unmap(0, nullptr);

	// 복사 → (밉 생성) → 픽셀 셰이더 리소스 상태로 전이. 밉 생성용 임시 디스크립터는 실행 완료 후 반환.
	std::vector<FD3D12DescriptorHandle> TempDescriptors;
	const bool bCopied = Queue.ExecuteImmediate(D3DDevice, [&](ID3D12GraphicsCommandList* CommandList) {
		D3D12_TEXTURE_COPY_LOCATION Destination{};
		Destination.pResource        = Resource.Get();
		Destination.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		Destination.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION SourceLocation{};
		SourceLocation.pResource       = UploadBuffer.Get();
		SourceLocation.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		SourceLocation.PlacedFootprint = Footprint;

		CommandList->CopyTextureRegion(&Destination, 0, 0, 0, &SourceLocation, nullptr);

		if (bWithMips)
		{
			MipGenerator->RecordGenerateMips(CommandList, Resource.Get(), Format, Width, Height, MipCount, *SrvAllocator,
			                                 TempDescriptors);
		}
		else
		{
			const D3D12_RESOURCE_BARRIER ToShaderResource =
				MakeTransitionBarrier(Resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			CommandList->ResourceBarrier(1, &ToShaderResource);
		}
	});
	for (FD3D12DescriptorHandle& Handle : TempDescriptors)
	{
		SrvAllocator->Free(Handle);
	}
	if (!bCopied)
	{
		return false;
	}

	// 전체 밉 체인을 노출하는 SRV
	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                        = Format;
	SrvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Texture2D.MostDetailedMip     = 0;
	SrvDesc.Texture2D.MipLevels           = MipCount;
	SrvDesc.Texture2D.PlaneSlice          = 0;
	SrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	Srv = SrvAllocator->Allocate();
	D3DDevice->CreateShaderResourceView(Resource.Get(), &SrvDesc, Srv.Cpu);

	E_LOG(LogD3D12, Log, "텍스처 생성: {}x{}, 밉 {}개 ({} bytes 업로드)", Width, Height, MipCount, TotalBytes);
	return true;
}

bool FD3D12Texture::Init2DFromMips(FD3D12Device& Device, FD3D12CommandQueue& Queue, FD3D12DescriptorAllocator& InSrvAllocator,
                                   uint32 InWidth, uint32 InHeight, DXGI_FORMAT InFormat, const FMipData* Mips, uint32 InMipCount,
                                   const wchar_t* DebugName)
{
	E_CHECKF(Resource == nullptr, "텍스처가 이미 생성되어 있습니다");
	E_CHECKF(Mips != nullptr && InMipCount > 0 && InMipCount <= CalculateMipCount(InWidth, InHeight), "잘못된 밉 데이터");

	ID3D12Device* D3DDevice = Device.GetDevice();
	SrvAllocator            = &InSrvAllocator;
	Width                   = InWidth;
	Height                  = InHeight;
	Format                  = InFormat;
	MipCount                = InMipCount;

	const D3D12_HEAP_PROPERTIES DefaultHeap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   TextureDesc = MakeTexture2DDesc(Width, Height, Format, D3D12_RESOURCE_FLAG_NONE, static_cast<uint16>(MipCount));
	E_D3D_VERIFY(D3DDevice->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &TextureDesc,
	                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Resource)));
	Resource->SetName(DebugName);

	// 전체 밉의 업로드 레이아웃 (블록 압축 포맷이면 NumRows = 블록 행 수, RowSizeInBytes = 블록 행 바이트)
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints(MipCount);
	std::vector<UINT>                               NumRows(MipCount);
	std::vector<UINT64>                             RowSizes(MipCount);
	UINT64                                          TotalBytes = 0;
	D3DDevice->GetCopyableFootprints(&TextureDesc, 0, MipCount, 0, Footprints.data(), NumRows.data(), RowSizes.data(), &TotalBytes);

	const D3D12_HEAP_PROPERTIES UploadHeap = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
	const D3D12_RESOURCE_DESC   UploadDesc = MakeBufferDesc(TotalBytes);
	ComPtr<ID3D12Resource>      UploadBuffer;
	E_D3D_VERIFY(D3DDevice->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &UploadDesc,
	                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&UploadBuffer)));
	UploadBuffer->SetName(L"StagingTextureUpload");

	uint8*            Mapped = nullptr;
	const D3D12_RANGE NoRead{ 0, 0 };
	E_D3D_VERIFY(UploadBuffer->Map(0, &NoRead, reinterpret_cast<void**>(&Mapped)));
	for (uint32 Mip = 0; Mip < MipCount; ++Mip)
	{
		const uint64 RowBytes = RowSizes[Mip];
		if (Mips[Mip].Data == nullptr || Mips[Mip].Size != RowBytes * NumRows[Mip])
		{
			UploadBuffer->Unmap(0, nullptr);
			E_LOG(LogD3D12, Error, "밉 {} 데이터 크기 불일치 ({} != {})", Mip, Mips[Mip].Size, RowBytes * NumRows[Mip]);
			Shutdown();
			return false;
		}
		const uint8* Source = static_cast<const uint8*>(Mips[Mip].Data);
		for (UINT Row = 0; Row < NumRows[Mip]; ++Row)
		{
			std::memcpy(Mapped + Footprints[Mip].Offset + static_cast<uint64>(Row) * Footprints[Mip].Footprint.RowPitch,
			            Source + Row * RowBytes, static_cast<size_t>(RowBytes));
		}
	}
	UploadBuffer->Unmap(0, nullptr);

	const bool bCopied = Queue.ExecuteImmediate(D3DDevice, [&](ID3D12GraphicsCommandList* CommandList) {
		for (uint32 Mip = 0; Mip < MipCount; ++Mip)
		{
			D3D12_TEXTURE_COPY_LOCATION Destination{};
			Destination.pResource        = Resource.Get();
			Destination.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			Destination.SubresourceIndex = Mip;

			D3D12_TEXTURE_COPY_LOCATION SourceLocation{};
			SourceLocation.pResource       = UploadBuffer.Get();
			SourceLocation.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			SourceLocation.PlacedFootprint = Footprints[Mip];

			CommandList->CopyTextureRegion(&Destination, 0, 0, 0, &SourceLocation, nullptr);
		}
		const D3D12_RESOURCE_BARRIER ToShaderResource =
			MakeTransitionBarrier(Resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		CommandList->ResourceBarrier(1, &ToShaderResource);
	});
	if (!bCopied)
	{
		return false;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                        = Format;
	SrvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Texture2D.MostDetailedMip     = 0;
	SrvDesc.Texture2D.MipLevels           = MipCount;
	SrvDesc.Texture2D.PlaneSlice          = 0;
	SrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	Srv = SrvAllocator->Allocate();
	D3DDevice->CreateShaderResourceView(Resource.Get(), &SrvDesc, Srv.Cpu);

	E_LOG(LogD3D12, Log, "텍스처 생성(사전 밉): {}x{}, 밉 {}개, 포맷 {} ({} bytes 업로드)", Width, Height, MipCount,
	      static_cast<uint32>(Format), TotalBytes);
	return true;
}

bool FD3D12Texture::Init2DFromMipsAsync(FD3D12Device& Device, FD3D12UploadQueue& Uploader, FD3D12DescriptorAllocator& InSrvAllocator,
                                        uint32 InWidth, uint32 InHeight, DXGI_FORMAT InFormat, const FMipData* Mips, uint32 InMipCount,
                                        const wchar_t* DebugName)
{
	E_CHECKF(Resource == nullptr, "텍스처가 이미 생성되어 있습니다");
	E_CHECKF(Mips != nullptr && InMipCount > 0 && InMipCount <= CalculateMipCount(InWidth, InHeight), "잘못된 밉 데이터");

	ID3D12Device*             D3DDevice   = Device.GetDevice();
	const D3D12_RESOURCE_DESC TextureDesc = MakeTexture2DDesc(InWidth, InHeight, InFormat, D3D12_RESOURCE_FLAG_NONE, static_cast<uint16>(InMipCount));

	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints(InMipCount);
	std::vector<UINT>                               NumRows(InMipCount);
	std::vector<UINT64>                             RowSizes(InMipCount);
	UINT64                                          TotalBytes = 0;
	D3DDevice->GetCopyableFootprints(&TextureDesc, 0, InMipCount, 0, Footprints.data(), NumRows.data(), RowSizes.data(), &TotalBytes);
	for (uint32 Mip = 0; Mip < InMipCount; ++Mip)
	{
		if (Mips[Mip].Data == nullptr || Mips[Mip].Size != RowSizes[Mip] * NumRows[Mip])
		{
			E_LOG(LogD3D12, Error, "밉 {} 데이터 크기 불일치 ({} != {})", Mip, Mips[Mip].Size, RowSizes[Mip] * NumRows[Mip]);
			return false;
		}
	}

	// 복사 큐 규칙: 대상은 COMMON으로 만든다 (복사 큐에서 COPY_DEST로 암시적 승격 → 실행이 끝나면 COMMON으로 감쇠)
	const D3D12_HEAP_PROPERTIES DefaultHeap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	E_D3D_VERIFY(D3DDevice->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &TextureDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
	                                                IID_PPV_ARGS(&Resource)));
	Resource->SetName(DebugName);
	SrvAllocator = &InSrvAllocator;
	Width        = InWidth;
	Height       = InHeight;
	Format       = InFormat;
	MipCount     = InMipCount;

	// 스테이징 (링, 512바이트 배치 정렬) → 밉별 행 복사
	const FD3D12UploadAllocation Staging = Uploader.Allocate(TotalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	for (uint32 Mip = 0; Mip < MipCount; ++Mip)
	{
		const uint8* Source = static_cast<const uint8*>(Mips[Mip].Data);
		for (UINT Row = 0; Row < NumRows[Mip]; ++Row)
		{
			std::memcpy(Staging.Cpu + Footprints[Mip].Offset + static_cast<uint64>(Row) * Footprints[Mip].Footprint.RowPitch,
			            Source + Row * RowSizes[Mip], static_cast<size_t>(RowSizes[Mip]));
		}
	}

	ID3D12GraphicsCommandList* CopyList = Uploader.GetCommandList();
	for (uint32 Mip = 0; Mip < MipCount; ++Mip)
	{
		D3D12_TEXTURE_COPY_LOCATION Destination{};
		Destination.pResource        = Resource.Get();
		Destination.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		Destination.SubresourceIndex = Mip;

		D3D12_TEXTURE_COPY_LOCATION SourceLocation{};
		SourceLocation.pResource       = Staging.Resource;
		SourceLocation.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		SourceLocation.PlacedFootprint = Footprints[Mip];
		SourceLocation.PlacedFootprint.Offset += Staging.Offset;

		CopyList->CopyTextureRegion(&Destination, 0, 0, 0, &SourceLocation, nullptr);
	}
	UploadFence    = Uploader.AddDestination(Resource.Get(), /*bTexture*/ true);
	bUploadPending = true;

	// SRV는 지금 만들어도 된다 (데이터를 읽는 것은 준비된 뒤 — 소유자가 그때까지 대체 텍스처를 쓴다)
	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                        = Format;
	SrvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Texture2D.MostDetailedMip     = 0;
	SrvDesc.Texture2D.MipLevels           = MipCount;
	SrvDesc.Texture2D.PlaneSlice          = 0;
	SrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	Srv = SrvAllocator->Allocate();
	D3DDevice->CreateShaderResourceView(Resource.Get(), &SrvDesc, Srv.Cpu);

	E_LOG(LogD3D12, Verbose, "텍스처 비동기 업로드: {}x{}, 밉 {}개, 포맷 {} ({} bytes, 펜스 {})", Width, Height, MipCount,
	      static_cast<uint32>(Format), TotalBytes, UploadFence);
	return true;
}

void FD3D12Texture::SwapContents(FD3D12Texture& Other) noexcept
{
	std::swap(Resource, Other.Resource);
	std::swap(Srv, Other.Srv);
	std::swap(SrvAllocator, Other.SrvAllocator);
	std::swap(Width, Other.Width);
	std::swap(Height, Other.Height);
	std::swap(MipCount, Other.MipCount);
	std::swap(Format, Other.Format);
	std::swap(UploadFence, Other.UploadFence);
	std::swap(bUploadPending, Other.bUploadPending);
}

void FD3D12Texture::Shutdown()
{
	if (SrvAllocator != nullptr)
	{
		SrvAllocator->Free(Srv);
		SrvAllocator = nullptr;
	}
	Resource.Reset();
	Width    = 0;
	Height   = 0;
	MipCount = 1;
	Format   = DXGI_FORMAT_UNKNOWN;
	UploadFence    = 0;
	bUploadPending = false;
}

void FD3D12Texture::ShutdownDeferred(FD3D12RHI& Rhi)
{
	Rhi.DeferRelease(Resource);
	Rhi.DeferFreeDescriptor(Srv);
	Srv          = FD3D12DescriptorHandle{};
	SrvAllocator = nullptr;
	Resource.Reset();
	Width    = 0;
	Height   = 0;
	MipCount = 1;
	Format   = DXGI_FORMAT_UNKNOWN;
	UploadFence    = 0;
	bUploadPending = false;
}
