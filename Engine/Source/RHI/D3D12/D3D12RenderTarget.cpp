#include "RHI/D3D12/D3D12RenderTarget.h"

#include "RHI/D3D12/D3D12Device.h"
#include "RHI/D3D12/D3D12RHI.h"

#include <cstring>

FD3D12RenderTarget::~FD3D12RenderTarget()
{
	Shutdown();
}

bool FD3D12RenderTarget::Init(FD3D12Device& Device, FD3D12DescriptorAllocator& InSrvAllocator, uint32 InWidth, uint32 InHeight,
                              const wchar_t* DebugName, const FRenderTargetDesc& InDesc)
{
	E_CHECKF(ColorResource == nullptr, "렌더 타깃이 이미 생성되어 있습니다");
	E_CHECKF(InWidth > 0 && InHeight > 0, "렌더 타깃 크기는 0일 수 없습니다");

	ID3D12Device* D3DDevice = Device.GetDevice();
	SrvAllocator            = &InSrvAllocator;
	Width                   = InWidth;
	Height                  = InHeight;
	Desc                    = InDesc;

	const D3D12_HEAP_PROPERTIES DefaultHeap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   TextureDesc = MakeTexture2DDesc(Width, Height, Desc.ResourceFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

	D3D12_CLEAR_VALUE ClearValue{};
	ClearValue.Format = Desc.RtvFormat;
	std::memcpy(ClearValue.Color, Desc.ClearColor, sizeof(ClearValue.Color));

	E_D3D_VERIFY(D3DDevice->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &TextureDesc,
	                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ClearValue,
	                                                IID_PPV_ARGS(&ColorResource)));
	ColorResource->SetName(DebugName);
	bInRenderState = false;

	if (!RtvHeap.Init(D3DDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, L"RenderTargetRtvHeap"))
	{
		return false;
	}
	D3D12_RENDER_TARGET_VIEW_DESC RtvDesc{};
	RtvDesc.Format        = Desc.RtvFormat;
	RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	D3DDevice->CreateRenderTargetView(ColorResource.Get(), &RtvDesc, RtvHeap.GetCpuHandle(0));

	D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
	SrvDesc.Format                    = Desc.SrvFormat;
	SrvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SrvDesc.Texture2D.MipLevels       = 1;
	Srv = SrvAllocator->Allocate();
	D3DDevice->CreateShaderResourceView(ColorResource.Get(), &SrvDesc, Srv.Cpu);

	return !Desc.bWithDepth || DepthBuffer.Init(D3DDevice, Width, Height);
}

void FD3D12RenderTarget::Shutdown()
{
	if (SrvAllocator != nullptr)
	{
		SrvAllocator->Free(Srv);
		SrvAllocator = nullptr;
	}
	DepthBuffer.Shutdown();
	RtvHeap.Shutdown();
	ColorResource.Reset();
	Width  = 0;
	Height = 0;
}

void FD3D12RenderTarget::ShutdownDeferred(FD3D12RHI& Rhi)
{
	Rhi.DeferRelease(ColorResource);
	Rhi.DeferFreeDescriptor(Srv);
	Srv          = FD3D12DescriptorHandle{};
	SrvAllocator = nullptr;
	DepthBuffer.ShutdownDeferred(Rhi);
	RtvHeap.Shutdown();
	ColorResource.Reset();
	Width  = 0;
	Height = 0;
}

void FD3D12RenderTarget::Begin(ID3D12GraphicsCommandList* CommandList, const float ClearColor[4])
{
	E_CHECKF(ColorResource != nullptr && !bInRenderState, "렌더 타깃 Begin/End 순서 오류");

	const D3D12_RESOURCE_BARRIER ToRenderTarget =
		MakeTransitionBarrier(ColorResource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	CommandList->ResourceBarrier(1, &ToRenderTarget);
	bInRenderState = true;

	const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = RtvHeap.GetCpuHandle(0);
	if (Desc.bWithDepth)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DepthBuffer.GetDepthStencilView();
		CommandList->OMSetRenderTargets(1, &Rtv, FALSE, &Dsv);
		CommandList->ClearDepthStencilView(Dsv, D3D12_CLEAR_FLAG_DEPTH, FD3D12DepthBuffer::ClearDepth, 0, 0, nullptr);
	}
	else
	{
		CommandList->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
	}
	if (ClearColor != nullptr)
	{
		CommandList->ClearRenderTargetView(Rtv, ClearColor, 0, nullptr);
	}

	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
	const D3D12_RECT     Scissor{ 0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);
}

void FD3D12RenderTarget::End(ID3D12GraphicsCommandList* CommandList)
{
	E_CHECKF(bInRenderState, "Begin 없이 End가 호출되었습니다");

	const D3D12_RESOURCE_BARRIER ToShaderResource =
		MakeTransitionBarrier(ColorResource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	CommandList->ResourceBarrier(1, &ToShaderResource);
	bInRenderState = false;
}

FRenderOutput FD3D12RenderTarget::GetOutput() const
{
	FRenderOutput Output;
	Output.Rtv    = RtvHeap.GetCpuHandle(0);
	Output.Format = Desc.RtvFormat;
	Output.Width  = Width;
	Output.Height = Height;
	return Output;
}
