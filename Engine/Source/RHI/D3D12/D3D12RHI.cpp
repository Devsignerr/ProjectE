#include "RHI/D3D12/D3D12RHI.h"

#include "Core/CommandLine.h"
#include "Core/Profiling.h"
#include "Core/StringConv.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

#pragma warning(push, 0)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#pragma warning(pop)

FD3D12RHI::~FD3D12RHI()
{
	Shutdown();
}

bool FD3D12RHI::Init(const FD3D12RHIDesc& Desc)
{
	E_CHECKF(!bInitialized, "FD3D12RHI가 이미 초기화되어 있습니다");
	E_CHECKF(Desc.WindowHandle != nullptr, "유효한 창 핸들이 필요합니다");

	bVSync = Desc.bVSync;

	if (!Device.Init(Desc.bEnableDebugLayer))
	{
		return false;
	}

	ID3D12Device* D3DDevice = Device.GetDevice();

	if (!GraphicsQueue.Init(D3DDevice, D3D12_COMMAND_LIST_TYPE_DIRECT) || !ComputeQueue.Init(D3DDevice, D3D12_COMMAND_LIST_TYPE_COMPUTE))
	{
		return false;
	}

	if (!UploadQueue.Init(D3DDevice, Desc.UploadRingSize))
	{
		return false;
	}

	if (!SwapChain.Init(Device, GraphicsQueue, Desc.WindowHandle, Desc.Width, Desc.Height))
	{
		return false;
	}

	if (!DepthBuffer.Init(D3DDevice, Desc.Width, Desc.Height))
	{
		return false;
	}

	if (!SrvAllocator.Init(D3DDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, Desc.SrvDescriptorCount, true, L"SrvDescriptorHeap"))
	{
		return false;
	}

	// 확인용: --dynamic-buffer-kb N (작게 잡아 업로드 버퍼 확장 경로를 시험)
	uint64 DynamicBufferSize = Desc.DynamicBufferSize;
	if (const std::wstring Override = FCommandLine::FromProcess().GetValue(L"--dynamic-buffer-kb"); !Override.empty())
	{
		DynamicBufferSize = static_cast<uint64>(std::max(64, std::stoi(Override))) * 1024;
	}
	for (uint32 Index = 0; Index < FrameCount; ++Index)
	{
		uint32 Used = 0;
		if (AcquireAllocator(CommandAllocators[Index], Used, D3D12_COMMAND_LIST_TYPE_DIRECT, L"CommandAllocator") == nullptr)
		{
			return false;
		}

		if (!DynamicBuffers[Index].Init(D3DDevice, DynamicBufferSize, std::format(L"DynamicUploadBuffer_{}", Index).c_str()))
		{
			return false;
		}
	}

	E_D3D_VERIFY(D3DDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CommandAllocators[0][0].Get(), nullptr,
	                                          IID_PPV_ARGS(&CommandList)));
	CommandList->SetName(L"GraphicsCommandList");

	// 생성 직후는 기록 상태이므로 닫아 두어야 BeginFrame에서 Reset할 수 있다
	E_D3D_VERIFY(CommandList->Close());

	// 비동기 계산 명령 목록 (렌더 그래프 계산 큐 패스)
	{
		uint32 Used = 0;
		ID3D12CommandAllocator* ComputeAllocator = AcquireAllocator(ComputeAllocators[0], Used, D3D12_COMMAND_LIST_TYPE_COMPUTE, L"ComputeAllocator");
		if (ComputeAllocator == nullptr)
		{
			return false;
		}
		E_D3D_VERIFY(D3DDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, ComputeAllocator, nullptr, IID_PPV_ARGS(&ComputeCommandList)));
		ComputeCommandList->SetName(L"AsyncComputeCommandList");
		E_D3D_VERIFY(ComputeCommandList->Close());
	}

	bInitialized = true;
	E_LOG(LogD3D12, Display, "D3D12 RHI 초기화 완료");
	return true;
}

void FD3D12RHI::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}

	// GPU가 모든 리소스 사용을 끝낸 뒤 해제
	ComputeQueue.Flush();
	GraphicsQueue.Flush();
	UploadQueue.Shutdown(); // 복사 완료 대기 + 전이 대기 참조 해제

	CommandList.Reset();
	ComputeCommandList.Reset();
	ProcessPendingReleases(RecordingReleases);
	for (uint32 Index = 0; Index < FrameCount; ++Index)
	{
		ProcessPendingReleases(PendingReleases[Index]);
		CommandAllocators[Index].clear();
		ComputeAllocators[Index].clear();
		DynamicBuffers[Index].Shutdown();
	}

	SrvAllocator.Shutdown();
	DepthBuffer.Shutdown();
	SwapChain.Shutdown();
	ComputeQueue.Shutdown();
	GraphicsQueue.Shutdown();
	Device.Shutdown();

	bInitialized = false;
	E_LOG(LogD3D12, Display, "D3D12 RHI 종료 완료");
}

void FD3D12RHI::Resize(uint32 Width, uint32 Height)
{
	if (!bInitialized || Width == 0 || Height == 0)
	{
		return;
	}
	if (Width == SwapChain.GetWidth() && Height == SwapChain.GetHeight())
	{
		return;
	}

	// 백버퍼/깊이 버퍼를 참조 중인 GPU 작업이 모두 끝나야 재생성할 수 있다
	ComputeQueue.Flush();
	GraphicsQueue.Flush();

	if (!SwapChain.Resize(Width, Height))
	{
		E_LOG(LogD3D12, Fatal, "스왑체인 리사이즈 실패");
	}
	if (!DepthBuffer.Resize(Device.GetDevice(), Width, Height))
	{
		E_LOG(LogD3D12, Fatal, "깊이 버퍼 리사이즈 실패");
	}
}

void FD3D12RHI::BeginFrame(const float ClearColor[4])
{
	CurrentBackBufferIndex = SwapChain.GetCurrentBackBufferIndex();
	++FrameNumber;

	// 이 백버퍼를 마지막으로 사용한 프레임이 GPU에서 끝날 때까지 대기 (계산 큐는 프레임 끝 전에 그래픽스가 조인하지만 할당자 재사용 전에 확인)
	GraphicsQueue.WaitForFenceValue(FrameFenceValues[CurrentBackBufferIndex]);
	ComputeQueue.WaitForFenceValue(ComputeFenceValues[CurrentBackBufferIndex]);

	// 이 인덱스의 이전 프레임에서 예약된 해제를 실행 (GPU 사용 완료 보장됨)
	ProcessPendingReleases(PendingReleases[CurrentBackBufferIndex]);
	DynamicBuffers[CurrentBackBufferIndex].Reset();

	// 지난번 이 슬롯이 쓴 할당자를 모두 비우고 첫 할당자로 시작
	for (uint32 Index = 0; Index < UsedCommandAllocators[CurrentBackBufferIndex]; ++Index)
	{
		E_D3D_CHECK(CommandAllocators[CurrentBackBufferIndex][Index]->Reset());
	}
	for (uint32 Index = 0; Index < UsedComputeAllocators[CurrentBackBufferIndex]; ++Index)
	{
		E_D3D_CHECK(ComputeAllocators[CurrentBackBufferIndex][Index]->Reset());
	}
	UsedCommandAllocators[CurrentBackBufferIndex] = 0;
	UsedComputeAllocators[CurrentBackBufferIndex] = 0;
	GraphicsSubmitsThisFrame                      = 0;
	ID3D12CommandAllocator* Allocator =
		AcquireAllocator(CommandAllocators[CurrentBackBufferIndex], UsedCommandAllocators[CurrentBackBufferIndex], D3D12_COMMAND_LIST_TYPE_DIRECT,
		                 L"CommandAllocator");
	E_CHECKF(Allocator != nullptr, "명령 할당자 생성 실패");
	E_D3D_CHECK(CommandList->Reset(Allocator, nullptr));

	// 셰이더 가시 힙 바인딩 (프레임 내내 동일)
	ID3D12DescriptorHeap* DescriptorHeaps[] = { SrvAllocator.GetHeap() };
	CommandList->SetDescriptorHeaps(1, DescriptorHeaps);

	// 비동기 업로드: 지난 프레임 동안 쌓인 복사를 제출하고, 끝난 텍스처를 이 프레임 맨 앞에서 셰이더 리소스 상태로 전이
	{
		E_PROFILE_SCOPE("업로드 완료 처리");
		UploadQueue.Submit();
		UploadQueue.Poll();
		std::vector<ComPtr<ID3D12Resource>> KeepAlive;
		UploadQueue.RecordFinalize(CommandList.Get(), KeepAlive);
		for (ComPtr<ID3D12Resource>& Resource : KeepAlive)
		{
			DeferRelease(std::move(Resource)); // 이 프레임이 GPU에서 끝날 때까지
		}
		for (const auto& [Id, Callback] : BeginFrameCallbacks)
		{
			Callback();
		}
	}

	ID3D12Resource* BackBuffer = SwapChain.GetCurrentBackBuffer();

	const D3D12_RESOURCE_BARRIER ToRenderTarget =
		MakeTransitionBarrier(BackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	CommandList->ResourceBarrier(1, &ToRenderTarget);

	SetRenderTargetToBackBuffer(false);
	CommandList->ClearRenderTargetView(SwapChain.GetCurrentRenderTargetView(false), ClearColor, 0, nullptr);
	CommandList->ClearDepthStencilView(DepthBuffer.GetDepthStencilView(), D3D12_CLEAR_FLAG_DEPTH, FD3D12DepthBuffer::ClearDepth, 0, 0, nullptr);
}

void FD3D12RHI::SetRenderTargetToBackBuffer(bool bLinearView)
{
	const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = SwapChain.GetCurrentRenderTargetView(bLinearView);
	const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = DepthBuffer.GetDepthStencilView();
	CommandList->OMSetRenderTargets(1, &Rtv, FALSE, &Dsv);

	const D3D12_VIEWPORT Viewport{ 0.0f, 0.0f,
	                               static_cast<float>(SwapChain.GetWidth()), static_cast<float>(SwapChain.GetHeight()),
	                               D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
	const D3D12_RECT Scissor{ 0, 0, static_cast<LONG>(SwapChain.GetWidth()), static_cast<LONG>(SwapChain.GetHeight()) };
	CommandList->RSSetViewports(1, &Viewport);
	CommandList->RSSetScissorRects(1, &Scissor);
}

FRenderOutput FD3D12RHI::GetBackBufferOutput() const
{
	FRenderOutput Output;
	Output.Rtv      = SwapChain.GetCurrentRenderTargetView(false);
	Output.Format   = RenderTargetFormat;
	Output.Width    = SwapChain.GetWidth();
	Output.Height   = SwapChain.GetHeight();
	Output.Resource = SwapChain.GetCurrentBackBuffer();
	return Output;
}

void FD3D12RHI::FlushUploads()
{
	E_PROFILE_SCOPE("업로드 비우기");
	UploadQueue.WaitIdle();
	if (!UploadQueue.HasPendingFinalize())
	{
		return;
	}
	std::vector<ComPtr<ID3D12Resource>> KeepAlive;
	GraphicsQueue.ExecuteImmediate(Device.GetDevice(), [&](ID3D12GraphicsCommandList* List) { UploadQueue.RecordFinalize(List, KeepAlive); });
	// ExecuteImmediate가 완료까지 기다렸으므로 KeepAlive는 여기서 놓아도 된다
}

uint32 FD3D12RHI::AddBeginFrameCallback(std::function<void()> Callback)
{
	const uint32 Id = NextBeginFrameCallbackId++;
	BeginFrameCallbacks.emplace_back(Id, std::move(Callback));
	return Id;
}

void FD3D12RHI::RemoveBeginFrameCallback(uint32 Id)
{
	std::erase_if(BeginFrameCallbacks, [Id](const auto& Entry) { return Entry.first == Id; });
}

void FD3D12RHI::DeferRelease(ComPtr<ID3D12Object> Object)
{
	if (Object)
	{
		RecordingReleases.Objects.push_back(std::move(Object));
	}
}

void FD3D12RHI::DeferFreeDescriptor(const FD3D12DescriptorHandle& Handle)
{
	if (Handle.IsValid())
	{
		RecordingReleases.SrvDescriptors.push_back(Handle);
	}
}

void FD3D12RHI::ProcessPendingReleases(FPendingReleases& Pending)
{
	Pending.Objects.clear();
	for (FD3D12DescriptorHandle& Handle : Pending.SrvDescriptors)
	{
		SrvAllocator.Free(Handle);
	}
	Pending.SrvDescriptors.clear();
}

ID3D12CommandAllocator* FD3D12RHI::AcquireAllocator(std::vector<ComPtr<ID3D12CommandAllocator>>& Pool, uint32& Used, D3D12_COMMAND_LIST_TYPE Type,
                                                    const wchar_t* Prefix)
{
	if (Used == Pool.size())
	{
		ComPtr<ID3D12CommandAllocator> Allocator;
		if (FAILED(Device.GetDevice()->CreateCommandAllocator(Type, IID_PPV_ARGS(&Allocator))))
		{
			E_LOG(LogD3D12, Error, "명령 할당자 생성 실패");
			return nullptr;
		}
		Allocator->SetName(std::format(L"{}_{}", Prefix, Pool.size()).c_str());
		Pool.push_back(std::move(Allocator));
	}
	return Pool[Used++].Get();
}

void FD3D12RHI::WaitForUploadsBeforeSubmit()
{
	// 이 프레임에 기록·참조한 업로드가 GPU에서 끝난 뒤 실행되도록 (전이는 복사 완료를 CPU로 확인한 것만 기록하므로 보통 바로 통과)
	UploadQueue.Submit();
	if (UploadQueue.GetFinalizedFence() > 0)
	{
		E_D3D_CHECK(GraphicsQueue.GetQueue()->Wait(UploadQueue.GetFence(), UploadQueue.GetFinalizedFence()));
	}
}

uint64 FD3D12RHI::SubmitGraphicsCommands()
{
	E_PROFILE_SCOPE("그래픽스 명령 중간 제출");
	E_D3D_CHECK(CommandList->Close());
	WaitForUploadsBeforeSubmit();
	const uint64 FenceValue = GraphicsQueue.ExecuteCommandList(CommandList.Get());
	++GraphicsSubmitsThisFrame;

	ID3D12CommandAllocator* Allocator =
		AcquireAllocator(CommandAllocators[CurrentBackBufferIndex], UsedCommandAllocators[CurrentBackBufferIndex], D3D12_COMMAND_LIST_TYPE_DIRECT,
		                 L"CommandAllocator");
	E_CHECKF(Allocator != nullptr, "명령 할당자 생성 실패");
	E_D3D_CHECK(CommandList->Reset(Allocator, nullptr));
	ID3D12DescriptorHeap* DescriptorHeaps[] = { SrvAllocator.GetHeap() };
	CommandList->SetDescriptorHeaps(1, DescriptorHeaps);
	return FenceValue;
}

ID3D12GraphicsCommandList* FD3D12RHI::BeginComputeCommands(uint64 WaitGraphicsFence)
{
	E_CHECKF(!bComputeRecording, "계산 명령 목록이 이미 기록 중입니다");
	ID3D12CommandAllocator* Allocator =
		AcquireAllocator(ComputeAllocators[CurrentBackBufferIndex], UsedComputeAllocators[CurrentBackBufferIndex], D3D12_COMMAND_LIST_TYPE_COMPUTE,
		                 L"ComputeAllocator");
	E_CHECKF(Allocator != nullptr, "계산 명령 할당자 생성 실패");
	E_D3D_CHECK(ComputeCommandList->Reset(Allocator, nullptr));
	ID3D12DescriptorHeap* DescriptorHeaps[] = { SrvAllocator.GetHeap() };
	ComputeCommandList->SetDescriptorHeaps(1, DescriptorHeaps);
	if (WaitGraphicsFence > 0)
	{
		E_D3D_CHECK(ComputeQueue.GetQueue()->Wait(GraphicsQueue.GetFence(), WaitGraphicsFence));
	}
	bComputeRecording = true;
	return ComputeCommandList.Get();
}

uint64 FD3D12RHI::SubmitComputeCommands()
{
	E_CHECKF(bComputeRecording, "기록 중인 계산 명령 목록이 없습니다");
	E_D3D_CHECK(ComputeCommandList->Close());
	bComputeRecording                          = false;
	const uint64 FenceValue                    = ComputeQueue.ExecuteCommandList(ComputeCommandList.Get());
	ComputeFenceValues[CurrentBackBufferIndex] = FenceValue;
	return FenceValue;
}

void FD3D12RHI::WaitForCompute(uint64 ComputeFence)
{
	if (ComputeFence > 0)
	{
		E_D3D_CHECK(GraphicsQueue.GetQueue()->Wait(ComputeQueue.GetFence(), ComputeFence));
	}
}

void FD3D12RHI::EndFrame()
{
	ID3D12Resource* BackBuffer = SwapChain.GetCurrentBackBuffer();

	// 스크린샷: 백버퍼 → 리드백 버퍼 복사
	ComPtr<ID3D12Resource>             Readback;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
	D3D12_RESOURCE_STATES              BackBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	if (!PendingScreenshot.empty())
	{
		const D3D12_RESOURCE_DESC Desc       = BackBuffer->GetDesc();
		UINT64                    TotalBytes = 0;
		Device.GetDevice()->GetCopyableFootprints(&Desc, 0, 1, 0, &Footprint, nullptr, nullptr, &TotalBytes);

		const D3D12_HEAP_PROPERTIES ReadbackHeap = MakeHeapProperties(D3D12_HEAP_TYPE_READBACK);
		const D3D12_RESOURCE_DESC   BufferDesc   = MakeBufferDesc(TotalBytes);
		if (SUCCEEDED(Device.GetDevice()->CreateCommittedResource(&ReadbackHeap, D3D12_HEAP_FLAG_NONE, &BufferDesc,
		                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Readback))))
		{
			const D3D12_RESOURCE_BARRIER ToCopy =
				MakeTransitionBarrier(BackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			CommandList->ResourceBarrier(1, &ToCopy);
			BackBufferState = D3D12_RESOURCE_STATE_COPY_SOURCE;

			D3D12_TEXTURE_COPY_LOCATION Source{};
			Source.pResource        = BackBuffer;
			Source.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			Source.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION Destination{};
			Destination.pResource       = Readback.Get();
			Destination.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			Destination.PlacedFootprint = Footprint;
			CommandList->CopyTextureRegion(&Destination, 0, 0, 0, &Source, nullptr);
		}
	}

	const D3D12_RESOURCE_BARRIER ToPresent = MakeTransitionBarrier(BackBuffer, BackBufferState, D3D12_RESOURCE_STATE_PRESENT);
	CommandList->ResourceBarrier(1, &ToPresent);

	E_D3D_CHECK(CommandList->Close());
	E_CHECKF(!bComputeRecording, "계산 명령 목록이 제출되지 않은 채 프레임이 끝났습니다");

	WaitForUploadsBeforeSubmit();
	FrameFenceValues[CurrentBackBufferIndex] = GraphicsQueue.ExecuteCommandList(CommandList.Get());
	++GraphicsSubmitsThisFrame;

	// 이번 프레임 도중(BeginFrame 전 UI 단계 포함) 해제 요청된 것은 방금 제출한 프레임이 끝난 뒤 해제
	FPendingReleases& Pending = PendingReleases[CurrentBackBufferIndex];
	std::move(RecordingReleases.Objects.begin(), RecordingReleases.Objects.end(), std::back_inserter(Pending.Objects));
	Pending.SrvDescriptors.insert(Pending.SrvDescriptors.end(), RecordingReleases.SrvDescriptors.begin(), RecordingReleases.SrvDescriptors.end());
	RecordingReleases.Objects.clear();
	RecordingReleases.SrvDescriptors.clear();

	if (Readback)
	{
		GraphicsQueue.WaitForFenceValue(FrameFenceValues[CurrentBackBufferIndex]);
		WriteScreenshot(Readback.Get(), Footprint);
	}
	PendingScreenshot.clear();

	SwapChain.Present(bVSync);
}

bool FD3D12RHI::WriteScreenshot(ID3D12Resource* Readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Footprint)
{
	const uint32 Width  = Footprint.Footprint.Width;
	const uint32 Height = Footprint.Footprint.Height;

	uint8* Mapped = nullptr;
	if (FAILED(Readback->Map(0, nullptr, reinterpret_cast<void**>(&Mapped))))
	{
		return false;
	}
	// 백버퍼는 R8G8B8A8_UNORM(값은 sRGB로 인코딩됨) → 행 피치 제거 + 알파 불투명
	std::vector<uint8> Pixels(static_cast<size_t>(Width) * Height * 4);
	for (uint32 Row = 0; Row < Height; ++Row)
	{
		const uint8* Source = Mapped + Footprint.Offset + static_cast<size_t>(Row) * Footprint.Footprint.RowPitch;
		uint8*       Dest   = Pixels.data() + static_cast<size_t>(Row) * Width * 4;
		std::memcpy(Dest, Source, static_cast<size_t>(Width) * 4);
		for (uint32 X = 0; X < Width; ++X)
		{
			Dest[X * 4 + 3] = 255;
		}
	}
	const D3D12_RANGE NoWrite{ 0, 0 };
	Readback->Unmap(0, &NoWrite);

	std::error_code ErrorCode;
	std::filesystem::create_directories(PendingScreenshot.parent_path(), ErrorCode);
	const std::string PathUtf8 = FStringConv::ToUtf8(PendingScreenshot.wstring());
	// 유니코드 경로 지원을 위해 파일은 직접 열고 stb에는 쓰기 콜백만 넘긴다
	FILE* File = nullptr;
	if (_wfopen_s(&File, PendingScreenshot.c_str(), L"wb") != 0 || File == nullptr)
	{
		E_LOG(LogD3D12, Error, "스크린샷 파일을 열 수 없습니다: {}", PathUtf8);
		return false;
	}
	const int bOk = stbi_write_png_to_func(
		[](void* Context, void* Data, int Size) { std::fwrite(Data, 1, static_cast<size_t>(Size), static_cast<FILE*>(Context)); },
		File, static_cast<int>(Width), static_cast<int>(Height), 4, Pixels.data(), static_cast<int>(Width * 4));
	std::fclose(File);
	E_LOG(LogD3D12, Display, "스크린샷 저장: {} ({}x{})", PathUtf8, Width, Height);
	return bOk != 0;
}
