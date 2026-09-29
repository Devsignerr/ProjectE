#include "RHI/D3D12/D3D12MipGenerator.h"

#include "RHI/TextureUtils.h"

namespace
{
	// GenerateMips.hlsl 루트 시그니처 레이아웃
	enum ERootParameter : uint32
	{
		RootParam_Constants = 0, // b0
		RootParam_Source    = 1, // t0
		RootParam_Dest      = 2, // u0
	};

	// cbuffer MipConstants와 동일한 레이아웃 (루트 상수 5개)
	struct FMipConstants
	{
		uint32 DstWidth;
		uint32 DstHeight;
		float  DstTexelWidth;
		float  DstTexelHeight;
		uint32 bSRGB;
	};
	static_assert(sizeof(FMipConstants) == 5 * sizeof(uint32));

	constexpr uint32 ThreadGroupSize = 8;
} // namespace

FD3D12MipGenerator::~FD3D12MipGenerator()
{
	Shutdown();
}

bool FD3D12MipGenerator::Init(ID3D12Device* InDevice)
{
	E_CHECKF(!IsInitialized(), "밉 생성기가 이미 초기화되어 있습니다");
	Device = InDevice;

	if (!ShaderCompiler.Init())
	{
		return false;
	}

	FShaderCompileDesc ComputeDesc;
	ComputeDesc.FileName   = L"GenerateMips.hlsl";
	ComputeDesc.EntryPoint = L"CSMain";
	ComputeDesc.Stage      = EShaderStage::Compute;
	const ComPtr<IDxcBlob> ComputeShader = ShaderCompiler.Compile(ComputeDesc);
	if (!ComputeShader)
	{
		return false;
	}

	const uint32 ConstantsIndex = RootSignature.AddConstants(sizeof(FMipConstants) / sizeof(uint32), 0);
	const uint32 SourceIndex    = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0) });
	const uint32 DestIndex      = RootSignature.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0) });
	E_CHECK(ConstantsIndex == RootParam_Constants && SourceIndex == RootParam_Source && DestIndex == RootParam_Dest);
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
	                                                                      D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	if (!RootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"GenerateMipsRootSignature"))
	{
		return false;
	}

	if (!PipelineState.InitCompute(Device, RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(ComputeShader.Get()),
	                               L"GenerateMipsPipeline"))
	{
		return false;
	}

	E_LOG(LogD3D12, Display, "밉맵 생성기 초기화 완료");
	return true;
}

void FD3D12MipGenerator::Shutdown()
{
	PipelineState.Shutdown();
	RootSignature.Shutdown();
	ShaderCompiler.Shutdown();
	Device = nullptr;
}

bool FD3D12MipGenerator::SupportsFormat(DXGI_FORMAT Format)
{
	return GetTypelessFormat(Format) != DXGI_FORMAT_UNKNOWN;
}

void FD3D12MipGenerator::RecordGenerateMips(ID3D12GraphicsCommandList* CommandList, ID3D12Resource* Texture,
                                            DXGI_FORMAT ViewFormat, uint32 Width, uint32 Height, uint32 MipCount,
                                            FD3D12DescriptorAllocator& Allocator,
                                            std::vector<FD3D12DescriptorHandle>& OutTempDescriptors)
{
	E_CHECKF(IsInitialized(), "밉 생성기가 초기화되지 않았습니다");
	E_CHECKF(MipCount > 1, "밉 레벨이 1개면 생성할 것이 없습니다");
	E_CHECKF(SupportsFormat(ViewFormat), "밉 생성을 지원하지 않는 포맷입니다");

	const DXGI_FORMAT UavFormat = GetUnormFormat(ViewFormat);
	const bool        bSRGB     = IsSrgbFormat(ViewFormat);

	CommandList->SetComputeRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(PipelineState.Get());
	ID3D12DescriptorHeap* Heaps[] = { Allocator.GetHeap() };
	CommandList->SetDescriptorHeaps(1, Heaps);

	OutTempDescriptors.reserve(OutTempDescriptors.size() + static_cast<size_t>(MipCount - 1) * 2);

	for (uint32 Mip = 1; Mip < MipCount; ++Mip)
	{
		const uint32 SourceMip = Mip - 1;
		const uint32 DstWidth  = GetMipDimension(Width, Mip);
		const uint32 DstHeight = GetMipDimension(Height, Mip);

		// 소스 밉: 밉 0은 업로드 직후(COPY_DEST), 그 외는 방금 쓴 UAV 상태에서 읽기 상태로
		const D3D12_RESOURCE_STATES SourceBefore = (SourceMip == 0) ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		const D3D12_RESOURCE_BARRIER Barriers[] = {
			MakeTransitionBarrier(Texture, SourceBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, SourceMip),
			MakeTransitionBarrier(Texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, Mip),
		};
		CommandList->ResourceBarrier(2, Barriers);

		// 소스 밉 하나만 보는 SRV
		D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
		SrvDesc.Format                    = ViewFormat;
		SrvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
		SrvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SrvDesc.Texture2D.MostDetailedMip = SourceMip;
		SrvDesc.Texture2D.MipLevels       = 1;
		const FD3D12DescriptorHandle SourceHandle = Allocator.Allocate();
		Device->CreateShaderResourceView(Texture, &SrvDesc, SourceHandle.Cpu);

		// 목적지 밉 UAV (UNORM 뷰)
		D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc{};
		UavDesc.Format             = UavFormat;
		UavDesc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
		UavDesc.Texture2D.MipSlice = Mip;
		const FD3D12DescriptorHandle DestHandle = Allocator.Allocate();
		Device->CreateUnorderedAccessView(Texture, nullptr, &UavDesc, DestHandle.Cpu);

		OutTempDescriptors.push_back(SourceHandle);
		OutTempDescriptors.push_back(DestHandle);

		const FMipConstants Constants{ DstWidth, DstHeight, 1.0f / static_cast<float>(DstWidth), 1.0f / static_cast<float>(DstHeight),
			                           bSRGB ? 1u : 0u };
		CommandList->SetComputeRoot32BitConstants(RootParam_Constants, sizeof(FMipConstants) / sizeof(uint32), &Constants, 0);
		CommandList->SetComputeRootDescriptorTable(RootParam_Source, SourceHandle.Gpu);
		CommandList->SetComputeRootDescriptorTable(RootParam_Dest, DestHandle.Gpu);
		CommandList->Dispatch(AlignUp(DstWidth, ThreadGroupSize) / ThreadGroupSize, AlignUp(DstHeight, ThreadGroupSize) / ThreadGroupSize, 1);

		// 다음 레벨이 이 밉을 읽기 전에 UAV 쓰기 완료 보장
		const D3D12_RESOURCE_BARRIER UavBarrier = MakeUavBarrier(Texture);
		CommandList->ResourceBarrier(1, &UavBarrier);
	}

	// 모든 밉을 픽셀 셰이더 리소스 상태로
	std::vector<D3D12_RESOURCE_BARRIER> FinalBarriers;
	FinalBarriers.reserve(MipCount);
	for (uint32 Mip = 0; Mip + 1 < MipCount; ++Mip)
	{
		FinalBarriers.push_back(MakeTransitionBarrier(Texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, Mip));
	}
	FinalBarriers.push_back(MakeTransitionBarrier(Texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
	                                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, MipCount - 1));
	CommandList->ResourceBarrier(static_cast<UINT>(FinalBarriers.size()), FinalBarriers.data());
}
