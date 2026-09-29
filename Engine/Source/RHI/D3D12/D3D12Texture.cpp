#include "RHI/D3D12/D3D12Texture.h"

#include "RHI/D3D12/D3D12CommandQueue.h"
#include "RHI/D3D12/D3D12Device.h"
#include "RHI/D3D12/D3D12RHI.h"

#include <cstring>

FD3D12Texture::~FD3D12Texture()
{
	Shutdown();
}

bool FD3D12Texture::Init2D(FD3D12Device& Device, FD3D12CommandQueue& Queue, FD3D12DescriptorAllocator& InSrvAllocator,
                           uint32 InWidth, uint32 InHeight, DXGI_FORMAT InFormat, const void* Pixels, uint32 BytesPerPixel,
                           const wchar_t* DebugName)
{
	E_CHECKF(Resource == nullptr, "텍스처가 이미 생성되어 있습니다");
	E_CHECKF(Pixels != nullptr && InWidth > 0 && InHeight > 0, "텍스처 데이터가 비어 있습니다");

	ID3D12Device* D3DDevice = Device.GetDevice();
	SrvAllocator            = &InSrvAllocator;
	Width                   = InWidth;
	Height                  = InHeight;
	Format                  = InFormat;

	// GPU 텍스처 (복사 대상 상태로 생성)
	const D3D12_HEAP_PROPERTIES DefaultHeap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   TextureDesc = MakeTexture2DDesc(Width, Height, Format);
	E_D3D_VERIFY(D3DDevice->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &TextureDesc,
	                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Resource)));
	Resource->SetName(DebugName);

	// 업로드 레이아웃 (행 피치는 256바이트 정렬)
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

	// 복사 → 픽셀 셰이더 리소스 상태로 전이
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

		const D3D12_RESOURCE_BARRIER ToShaderResource =
			MakeTransitionBarrier(Resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		CommandList->ResourceBarrier(1, &ToShaderResource);
	});
	if (!bCopied)
	{
		return false;
	}

	// SRV
	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                        = Format;
	SrvDesc.ViewDimension                 = D3D12_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Texture2D.MostDetailedMip     = 0;
	SrvDesc.Texture2D.MipLevels           = 1;
	SrvDesc.Texture2D.PlaneSlice          = 0;
	SrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	Srv = SrvAllocator->Allocate();
	D3DDevice->CreateShaderResourceView(Resource.Get(), &SrvDesc, Srv.Cpu);

	E_LOG(LogD3D12, Log, "텍스처 생성: {}x{} ({} bytes)", Width, Height, TotalBytes);
	return true;
}

void FD3D12Texture::Shutdown()
{
	if (SrvAllocator != nullptr)
	{
		SrvAllocator->Free(Srv);
		SrvAllocator = nullptr;
	}
	Resource.Reset();
	Width  = 0;
	Height = 0;
	Format = DXGI_FORMAT_UNKNOWN;
}

void FD3D12Texture::ShutdownDeferred(FD3D12RHI& Rhi)
{
	Rhi.DeferRelease(Resource);
	Rhi.DeferFreeDescriptor(Srv);
	Srv          = FD3D12DescriptorHandle{};
	SrvAllocator = nullptr;
	Resource.Reset();
	Width  = 0;
	Height = 0;
	Format = DXGI_FORMAT_UNKNOWN;
}
