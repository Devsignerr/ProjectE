#include "RHI/D3D12/D3D12DepthBuffer.h"

FD3D12DepthBuffer::~FD3D12DepthBuffer()
{
	Shutdown();
}

bool FD3D12DepthBuffer::Init(ID3D12Device* Device, uint32 InWidth, uint32 InHeight)
{
	Width  = InWidth;
	Height = InHeight;

	if (!DsvHeap.Init(Device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false, L"DepthStencilHeap"))
	{
		return false;
	}
	return CreateResource(Device);
}

void FD3D12DepthBuffer::Shutdown()
{
	Resource.Reset();
	DsvHeap.Shutdown();
	Width  = 0;
	Height = 0;
}

bool FD3D12DepthBuffer::Resize(ID3D12Device* Device, uint32 InWidth, uint32 InHeight)
{
	E_CHECKF(InWidth > 0 && InHeight > 0, "깊이 버퍼 크기는 0일 수 없습니다");

	Width  = InWidth;
	Height = InHeight;
	Resource.Reset();
	return CreateResource(Device);
}

bool FD3D12DepthBuffer::CreateResource(ID3D12Device* Device)
{
	const D3D12_HEAP_PROPERTIES DefaultHeap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   TextureDesc = MakeTexture2DDesc(Width, Height, Format, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

	// 클리어 값을 리소스와 일치시키면 클리어가 빠른 경로를 탄다
	D3D12_CLEAR_VALUE ClearValue{};
	ClearValue.Format               = Format;
	ClearValue.DepthStencil.Depth   = ClearDepth;
	ClearValue.DepthStencil.Stencil = 0;

	E_D3D_VERIFY(Device->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &TextureDesc,
	                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &ClearValue, IID_PPV_ARGS(&Resource)));
	Resource->SetName(L"DepthBuffer");

	D3D12_DEPTH_STENCIL_VIEW_DESC DsvDesc{};
	DsvDesc.Format             = Format;
	DsvDesc.ViewDimension      = D3D12_DSV_DIMENSION_TEXTURE2D;
	DsvDesc.Flags              = D3D12_DSV_FLAG_NONE;
	DsvDesc.Texture2D.MipSlice = 0;
	Device->CreateDepthStencilView(Resource.Get(), &DsvDesc, DsvHeap.GetCpuHandle(0));

	return true;
}
