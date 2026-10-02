#include "Renderer/PersistentTexture.h"

#include "RHI/D3D12/D3D12RHI.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

bool FPersistentTexture::Matches(const FPersistentTextureDesc& Other) const
{
	return Desc.Width == Other.Width && Desc.Height == Other.Height && Desc.Depth == Other.Depth && Desc.MipCount == Other.MipCount &&
	       Desc.Format == Other.Format && Desc.b3D == Other.b3D && Desc.bCube == Other.bCube && Desc.bUnorderedAccess == Other.bUnorderedAccess &&
	       Desc.bRenderTarget == Other.bRenderTarget;
}

void FPersistentTexture::Release(FD3D12RHI& Rhi)
{
	if (Resource)
	{
		Rhi.DeferRelease(Resource);
		Resource.Reset();
	}
	if (Srv.IsValid())
	{
		Rhi.DeferFreeDescriptor(Srv);
		Srv = FD3D12DescriptorHandle{};
	}
	for (FD3D12DescriptorHandle& Uav : Uavs)
	{
		if (Uav.IsValid())
		{
			Rhi.DeferFreeDescriptor(Uav);
		}
	}
	Uavs.clear();
	State = D3D12_RESOURCE_STATE_COMMON;
}

bool FPersistentTexture::Ensure(FD3D12RHI& Rhi, const FPersistentTextureDesc& InDesc, const wchar_t* DebugName)
{
	if (IsValid() && Matches(InDesc))
	{
		return true;
	}
	Release(Rhi);
	Desc = InDesc;

	ID3D12Device*       Device = Rhi.GetDevice().GetDevice();
	D3D12_RESOURCE_DESC ResourceDesc{};
	ResourceDesc.Dimension        = InDesc.b3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	ResourceDesc.Width            = InDesc.Width;
	ResourceDesc.Height           = InDesc.Height;
	ResourceDesc.DepthOrArraySize = static_cast<UINT16>(InDesc.Depth);
	ResourceDesc.MipLevels        = static_cast<UINT16>(InDesc.MipCount);
	ResourceDesc.Format           = InDesc.Format;
	ResourceDesc.SampleDesc       = { 1, 0 };
	ResourceDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	ResourceDesc.Flags            = (InDesc.bUnorderedAccess ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE) |
	                     (InDesc.bRenderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE);
	const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &ResourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Resource))))
	{
		E_LOG(LogRenderer, Error, "계산용 텍스처 생성 실패 ({}x{}x{})", InDesc.Width, InDesc.Height, InDesc.Depth);
		Resource.Reset();
		return false;
	}
	Resource->SetName(DebugName);
	State = D3D12_RESOURCE_STATE_COMMON;

	FD3D12DescriptorAllocator&      Allocator = Rhi.GetSrvAllocator();
	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                  = InDesc.Format;
	SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	if (InDesc.b3D)
	{
		SrvDesc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE3D;
		SrvDesc.Texture3D.MipLevels = InDesc.MipCount;
	}
	else if (InDesc.bCube)
	{
		SrvDesc.ViewDimension         = D3D12_SRV_DIMENSION_TEXTURECUBE;
		SrvDesc.TextureCube.MipLevels = InDesc.MipCount;
	}
	else if (InDesc.Depth > 1)
	{
		SrvDesc.ViewDimension            = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		SrvDesc.Texture2DArray.MipLevels = InDesc.MipCount;
		SrvDesc.Texture2DArray.ArraySize = InDesc.Depth;
	}
	else
	{
		SrvDesc.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
		SrvDesc.Texture2D.MipLevels = InDesc.MipCount;
	}
	Srv = Allocator.Allocate();
	Device->CreateShaderResourceView(Resource.Get(), &SrvDesc, Srv.Cpu);

	if (InDesc.bUnorderedAccess)
	{
		for (uint32 Mip = 0; Mip < InDesc.MipCount; ++Mip)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc{};
			UavDesc.Format = InDesc.Format;
			if (InDesc.b3D)
			{
				UavDesc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE3D;
				UavDesc.Texture3D.MipSlice = Mip;
				UavDesc.Texture3D.WSize    = FMath::Max(InDesc.Depth >> Mip, 1u);
			}
			else if (InDesc.Depth > 1 || InDesc.bCube)
			{
				UavDesc.ViewDimension            = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
				UavDesc.Texture2DArray.MipSlice  = Mip;
				UavDesc.Texture2DArray.ArraySize = InDesc.Depth;
			}
			else
			{
				UavDesc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
				UavDesc.Texture2D.MipSlice = Mip;
			}
			FD3D12DescriptorHandle Uav = Allocator.Allocate();
			Device->CreateUnorderedAccessView(Resource.Get(), nullptr, &UavDesc, Uav.Cpu);
			Uavs.push_back(Uav);
		}
	}
	return true;
}
