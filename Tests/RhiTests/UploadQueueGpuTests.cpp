// GPU 테스트: 복사 큐 + 업로드 링으로 텍스처/버퍼를 올리고 직접 큐에서 되읽어 비교 (디버그 레이어 오류·경고 0건).
// 하드웨어 D3D12 장치가 없으면 건너뛴다.
#include "Core/Testing/TestFramework.h"
#include "RHI/D3D12/D3D12Buffer.h"
#include "RHI/D3D12/D3D12CommandQueue.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12Device.h"
#include "RHI/D3D12/D3D12Texture.h"
#include "RHI/D3D12/D3D12UploadQueue.h"

#include <cstring>
#include <vector>

namespace
{
	// 심각도 Warning 이상 저장 메시지 수 (디버그 레이어)
	uint64 CountDebugIssues(ID3D12Device* Device)
	{
		ComPtr<ID3D12InfoQueue> InfoQueue;
		if (FAILED(Device->QueryInterface(IID_PPV_ARGS(&InfoQueue))))
		{
			return 0;
		}
		uint64 Issues = 0;
		for (uint64 Index = 0; Index < InfoQueue->GetNumStoredMessages(); ++Index)
		{
			SIZE_T Length = 0;
			InfoQueue->GetMessage(Index, nullptr, &Length);
			std::vector<uint8> Storage(Length);
			auto* Message = reinterpret_cast<D3D12_MESSAGE*>(Storage.data());
			if (SUCCEEDED(InfoQueue->GetMessage(Index, Message, &Length)) && Message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
			{
				E_LOG(LogD3D12, Error, "테스트 중 디버그 레이어 메시지: {}", Message->pDescription);
				++Issues;
			}
		}
		return Issues;
	}

	std::vector<uint8> MakePattern(size_t Size, uint32 Seed)
	{
		std::vector<uint8> Data(Size);
		for (size_t Index = 0; Index < Size; ++Index)
		{
			Seed        = Seed * 1664525u + 1013904223u;
			Data[Index] = static_cast<uint8>(Seed >> 24);
		}
		return Data;
	}
} // namespace

E_TEST(UploadQueue_GpuTextureAndBufferRoundtrip)
{
	FD3D12Device Device;
	if (!Device.Init(/*bEnableDebugLayer*/ true))
	{
		E_LOG(LogD3D12, Warning, "D3D12 장치가 없어 UploadQueue GPU 테스트를 건너뜁니다");
		return;
	}
	ID3D12Device* D3DDevice = Device.GetDevice();
	{
		FD3D12CommandQueue        Direct;
		FD3D12DescriptorAllocator SrvAllocator;
		FD3D12UploadQueue         Uploader;
		E_EXPECT_TRUE(Direct.Init(D3DDevice, D3D12_COMMAND_LIST_TYPE_DIRECT));
		E_EXPECT_TRUE(SrvAllocator.Init(D3DDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, true, L"TestSrvHeap"));
		constexpr uint64 RingSize = 1024 * 1024; // 작게: 감기·대기·전용 버퍼 경로까지
		E_EXPECT_TRUE(Uploader.Init(D3DDevice, RingSize));

		// 텍스처 1: 64x64 RGBA8 밉 2개 (링)
		const std::vector<uint8> Mip0 = MakePattern(64 * 64 * 4, 1);
		const std::vector<uint8> Mip1 = MakePattern(32 * 32 * 4, 2);
		const FD3D12Texture::FMipData Mips[2] = { { Mip0.data(), Mip0.size() }, { Mip1.data(), Mip1.size() } };
		FD3D12Texture Small;
		E_EXPECT_TRUE(Small.Init2DFromMipsAsync(Device, Uploader, SrvAllocator, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, Mips, 2, L"TestSmall"));
		E_EXPECT_TRUE(Small.IsUploadPending() && !Small.IsReady());

		// 텍스처 2: 512x512 RGBA8 (1MB → 링 절반보다 커서 전용 스테이징)
		const std::vector<uint8>      LargePixels = MakePattern(512 * 512 * 4, 3);
		const FD3D12Texture::FMipData LargeMip{ LargePixels.data(), LargePixels.size() };
		FD3D12Texture                 Large;
		E_EXPECT_TRUE(Large.Init2DFromMipsAsync(Device, Uploader, SrvAllocator, 512, 512, DXGI_FORMAT_R8G8B8A8_UNORM, &LargeMip, 1, L"TestLarge"));
		E_EXPECT_EQ(Uploader.GetDedicatedCount(), 1ull);

		// 버퍼 12개 × 200KB = 2.4MB > 링 1MB → 제출/감기/완료 대기 경로
		std::vector<std::vector<uint8>>            BufferData;
		std::vector<std::unique_ptr<FD3D12Buffer>> Buffers;
		for (uint32 Index = 0; Index < 12; ++Index)
		{
			BufferData.push_back(MakePattern(200 * 1024 + Index * 13, 100 + Index));
			Buffers.push_back(std::make_unique<FD3D12Buffer>());
			E_EXPECT_TRUE(Buffers.back()->InitStaticAsync(Device, Uploader, BufferData.back().data(), BufferData.back().size(), L"TestBuffer"));
		}
		E_EXPECT_TRUE(Buffers.front()->GetUploadFence() < Buffers.back()->GetUploadFence()); // 중간에 묶음이 제출됨

		// 완료 → 직접 큐에서 텍스처 전이 (RHI::FlushUploads와 같은 순서)
		Uploader.WaitIdle();
		E_EXPECT_EQ(Uploader.GetRingUsedBytes(), 0ull);
		E_EXPECT_EQ(Uploader.GetInFlightBatchCount(), size_t(0));
		std::vector<ComPtr<ID3D12Resource>> KeepAlive;
		E_EXPECT_TRUE(Direct.ExecuteImmediate(D3DDevice, [&](ID3D12GraphicsCommandList* List) { Uploader.RecordFinalize(List, KeepAlive); }));
		E_EXPECT_EQ(KeepAlive.size(), size_t(14));
		E_EXPECT_TRUE(Small.GetUploadFence() <= Uploader.GetFinalizedFence());
		E_EXPECT_TRUE(Buffers.back()->GetUploadFence() <= Uploader.GetFinalizedFence());
		KeepAlive.clear();

		// 되읽기: 텍스처(PIXEL_SHADER_RESOURCE → COPY_SOURCE) 밉 0/1 + 큰 텍스처 + 버퍼 전부
		struct FReadback
		{
			ComPtr<ID3D12Resource>             Buffer;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
		};
		const auto MakeReadback = [&](uint64 Size) {
			ComPtr<ID3D12Resource>      Buffer;
			const D3D12_HEAP_PROPERTIES Heap = MakeHeapProperties(D3D12_HEAP_TYPE_READBACK);
			const D3D12_RESOURCE_DESC   Desc = MakeBufferDesc(Size);
			D3DDevice->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Buffer));
			return Buffer;
		};
		const auto MakeTextureReadback = [&](FD3D12Texture& Texture, uint32 Mip) {
			const D3D12_RESOURCE_DESC Desc = Texture.GetResource()->GetDesc();
			FReadback                 Result;
			UINT64                    Total = 0;
			D3DDevice->GetCopyableFootprints(&Desc, Mip, 1, 0, &Result.Footprint, nullptr, nullptr, &Total);
			Result.Buffer = MakeReadback(Total);
			return Result;
		};
		FReadback ReadSmall0 = MakeTextureReadback(Small, 0);
		FReadback ReadSmall1 = MakeTextureReadback(Small, 1);
		FReadback ReadLarge  = MakeTextureReadback(Large, 0);
		std::vector<ComPtr<ID3D12Resource>> ReadBuffers;
		for (const std::vector<uint8>& Data : BufferData)
		{
			ReadBuffers.push_back(MakeReadback(Data.size()));
		}

		E_EXPECT_TRUE(Direct.ExecuteImmediate(D3DDevice, [&](ID3D12GraphicsCommandList* List) {
			const D3D12_RESOURCE_BARRIER ToCopy[2] = {
				MakeTransitionBarrier(Small.GetResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
				MakeTransitionBarrier(Large.GetResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
			};
			List->ResourceBarrier(2, ToCopy);
			const auto CopyTexture = [&](FD3D12Texture& Texture, uint32 Mip, FReadback& Target) {
				D3D12_TEXTURE_COPY_LOCATION Source{};
				Source.pResource        = Texture.GetResource();
				Source.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				Source.SubresourceIndex = Mip;
				D3D12_TEXTURE_COPY_LOCATION Destination{};
				Destination.pResource       = Target.Buffer.Get();
				Destination.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				Destination.PlacedFootprint = Target.Footprint;
				Destination.PlacedFootprint.Offset = 0;
				List->CopyTextureRegion(&Destination, 0, 0, 0, &Source, nullptr);
			};
			CopyTexture(Small, 0, ReadSmall0);
			CopyTexture(Small, 1, ReadSmall1);
			CopyTexture(Large, 0, ReadLarge);
			for (size_t Index = 0; Index < Buffers.size(); ++Index)
			{
				// 버퍼는 COMMON → COPY_SOURCE 암시적 승격
				List->CopyBufferRegion(ReadBuffers[Index].Get(), 0, Buffers[Index]->GetResource(), 0, BufferData[Index].size());
			}
		}));

		const auto CompareTexture = [](FReadback& Read, const std::vector<uint8>& Expected, uint32 Width, uint32 Height) {
			uint8* Mapped = nullptr;
			Read.Buffer->Map(0, nullptr, reinterpret_cast<void**>(&Mapped));
			bool bSame = Mapped != nullptr;
			for (uint32 Row = 0; bSame && Row < Height; ++Row)
			{
				bSame = std::memcmp(Mapped + static_cast<size_t>(Row) * Read.Footprint.Footprint.RowPitch,
				                    Expected.data() + static_cast<size_t>(Row) * Width * 4, static_cast<size_t>(Width) * 4) == 0;
			}
			Read.Buffer->Unmap(0, nullptr);
			return bSame;
		};
		E_EXPECT_TRUE(CompareTexture(ReadSmall0, Mip0, 64, 64));
		E_EXPECT_TRUE(CompareTexture(ReadSmall1, Mip1, 32, 32));
		E_EXPECT_TRUE(CompareTexture(ReadLarge, LargePixels, 512, 512));
		for (size_t Index = 0; Index < ReadBuffers.size(); ++Index)
		{
			uint8* Mapped = nullptr;
			ReadBuffers[Index]->Map(0, nullptr, reinterpret_cast<void**>(&Mapped));
			E_EXPECT_TRUE(Mapped != nullptr && std::memcmp(Mapped, BufferData[Index].data(), BufferData[Index].size()) == 0);
			ReadBuffers[Index]->Unmap(0, nullptr);
		}

		E_EXPECT_EQ(CountDebugIssues(D3DDevice), 0ull);

		Direct.Flush();
		Small.Shutdown();
		Large.Shutdown();
		Buffers.clear();
		Uploader.Shutdown();
		SrvAllocator.Shutdown();
		Direct.Shutdown();
	}
	Device.Shutdown();
}
