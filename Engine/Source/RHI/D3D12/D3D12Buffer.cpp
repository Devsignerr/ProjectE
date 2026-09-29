#include "RHI/D3D12/D3D12Buffer.h"

#include "RHI/D3D12/D3D12CommandQueue.h"
#include "RHI/D3D12/D3D12Device.h"
#include "RHI/D3D12/D3D12RHI.h"

#include <cstring>

FD3D12Buffer::~FD3D12Buffer()
{
	Shutdown();
}

bool FD3D12Buffer::InitStatic(FD3D12Device& Device, FD3D12CommandQueue& Queue, const void* Data, uint64 InSizeInBytes,
                              const wchar_t* DebugName)
{
	E_CHECKF(Resource == nullptr, "버퍼가 이미 생성되어 있습니다");
	E_CHECKF(Data != nullptr && InSizeInBytes > 0, "업로드할 데이터가 비어 있습니다");

	ID3D12Device* D3DDevice = Device.GetDevice();
	SizeInBytes             = InSizeInBytes;

	// GPU 전용 버퍼. 버퍼는 COMMON 상태에서 생성하며 복사/사용 시 암시적 상태 승격·감쇠 규칙을 따른다.
	const D3D12_HEAP_PROPERTIES DefaultHeap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_DESC   BufferDesc  = MakeBufferDesc(SizeInBytes);
	E_D3D_VERIFY(D3DDevice->CreateCommittedResource(&DefaultHeap, D3D12_HEAP_FLAG_NONE, &BufferDesc,
	                                                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Resource)));
	Resource->SetName(DebugName);

	// 스테이징용 업로드 버퍼
	const D3D12_HEAP_PROPERTIES UploadHeap = MakeHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
	ComPtr<ID3D12Resource>      UploadBuffer;
	E_D3D_VERIFY(D3DDevice->CreateCommittedResource(&UploadHeap, D3D12_HEAP_FLAG_NONE, &BufferDesc,
	                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&UploadBuffer)));
	UploadBuffer->SetName(L"StagingUploadBuffer");

	void*             Mapped = nullptr;
	const D3D12_RANGE NoRead{ 0, 0 }; // CPU는 읽지 않음
	E_D3D_VERIFY(UploadBuffer->Map(0, &NoRead, &Mapped));
	std::memcpy(Mapped, Data, static_cast<size_t>(SizeInBytes));
	UploadBuffer->Unmap(0, nullptr);

	// 복사 후 완료 대기
	const bool bCopied = Queue.ExecuteImmediate(D3DDevice, [&](ID3D12GraphicsCommandList* CommandList) {
		CommandList->CopyBufferRegion(Resource.Get(), 0, UploadBuffer.Get(), 0, SizeInBytes);
	});
	if (!bCopied)
	{
		return false;
	}

	E_LOG(LogD3D12, Log, "정적 버퍼 생성: {} bytes", SizeInBytes);
	return true;
}

void FD3D12Buffer::Shutdown()
{
	Resource.Reset();
	SizeInBytes = 0;
}

void FD3D12Buffer::ShutdownDeferred(FD3D12RHI& Rhi)
{
	Rhi.DeferRelease(Resource);
	Resource.Reset();
	SizeInBytes = 0;
}

D3D12_VERTEX_BUFFER_VIEW FD3D12Buffer::GetVertexBufferView(uint32 StrideInBytes) const
{
	E_CHECKF(Resource != nullptr, "버퍼가 생성되지 않았습니다");

	D3D12_VERTEX_BUFFER_VIEW View{};
	View.BufferLocation = Resource->GetGPUVirtualAddress();
	View.SizeInBytes    = static_cast<UINT>(SizeInBytes);
	View.StrideInBytes  = StrideInBytes;
	return View;
}

D3D12_INDEX_BUFFER_VIEW FD3D12Buffer::GetIndexBufferView(DXGI_FORMAT Format) const
{
	E_CHECKF(Resource != nullptr, "버퍼가 생성되지 않았습니다");

	D3D12_INDEX_BUFFER_VIEW View{};
	View.BufferLocation = Resource->GetGPUVirtualAddress();
	View.SizeInBytes    = static_cast<UINT>(SizeInBytes);
	View.Format         = Format;
	return View;
}
