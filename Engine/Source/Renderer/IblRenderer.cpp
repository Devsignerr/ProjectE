#include "Renderer/IblRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/Image.h"
#include "RHI/D3D12/D3D12Texture.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	constexpr DXGI_FORMAT IblFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	const wchar_t* BakeEntries[] = { L"SkyCS", L"IrradianceCS", L"PrefilterCS", L"BrdfCS", L"EquirectCS" };
	constexpr uint32 BakeEntryCount = 5;

	FShaderCompileDesc ShaderDesc(const wchar_t* File, const wchar_t* Entry, EShaderStage Stage)
	{
		FShaderCompileDesc Desc;
		Desc.FileName = File;
		Desc.EntryPoint = Entry;
		Desc.Stage = Stage;
		return Desc;
	}

	struct FBakeConstants
	{
		uint32 Size;
		uint32 SampleCount = IblMath::IntegrationSampleCount;
		float Roughness = 0.0f;
		float Rotation = 0.0f; // EquirectCS (라디안)
	};
	static_assert(sizeof(FBakeConstants) == 16);

	struct FSkyConstants
	{
		FVector3 Forward;
		float TanHalfFov;
		FVector3 Right;
		float Aspect;
		FVector3 Up;
		float Intensity;
	};
	static_assert(sizeof(FSkyConstants) == 48);
}

FIblRenderer::~FIblRenderer()
{
	Shutdown();
}

bool FIblRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& Library)
{
	Rhi = &InRhi;
	ShaderLibrary = &Library;
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	SkyRoot.AddConstants(12, 0);
	SkyRoot.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) }, D3D12_SHADER_VISIBILITY_PIXEL);
	SkyRoot.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	if (!SkyRoot.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"SkyRoot") || !CreateSkyPipeline(Library, SkyPipeline))
	{
		return false;
	}
	SkySrv = Rhi->GetSrvAllocator().Allocate();
	LightingTable = Rhi->GetSrvAllocator().AllocateRange(3);
	if (!CreateTexture(IblMath::SkyCubeSize, 6, 1, Sky, SkySrv.Cpu, L"IblSky") ||
		!CreateTexture(IblMath::IrradianceSize, 6, 1, Irradiance, LightingTable.Cpu, L"IblIrradiance") ||
		!CreateTexture(IblMath::PrefilterSize, 6, static_cast<uint16>(PrefilterMipCount), Prefilter,
			Rhi->GetSrvAllocator().GetCpuHandle(LightingTable.Index + 1), L"IblPrefilter") ||
		!CreateTexture(IblMath::BrdfLutSize, 1, 1, BrdfLut,
			Rhi->GetSrvAllocator().GetCpuHandle(LightingTable.Index + 2), L"IblBrdf"))
	{
		return false;
	}
	bReady = Generate(Library);
	if (bReady)
	{
		E_LOG(LogRenderer, Display, "IBL 생성 완료: 환경 큐브맵, 조도, 프리필터 {}밉, BRDF LUT", PrefilterMipCount);
	}
	return bReady;
}

bool FIblRenderer::CreateTexture(uint32 Size, uint16 Slices, uint16 Mips, ComPtr<ID3D12Resource>& Texture,
	D3D12_CPU_DESCRIPTOR_HANDLE Srv, const wchar_t* Name, D3D12_RESOURCE_STATES InitialState)
{
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	auto Desc = MakeTexture2DDesc(Size, Size, IblFormat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, Mips);
	Desc.DepthOrArraySize = Slices;
	const auto Heap = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	E_D3D_VERIFY(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc,
		InitialState, nullptr, IID_PPV_ARGS(&Texture)));
	Texture->SetName(Name);
	D3D12_SHADER_RESOURCE_VIEW_DESC View{};
	View.Format = IblFormat;
	View.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	if (Slices == 6)
	{
		View.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		View.TextureCube.MipLevels = Mips;
	}
	else
	{
		View.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		View.Texture2D.MipLevels = Mips;
	}
	Device->CreateShaderResourceView(Texture.Get(), &View, Srv);
	return true;
}

bool FIblRenderer::CreateSkyPipeline(FShaderLibrary& Library, FD3D12PipelineState& OutPipeline)
{
	const auto VS = Library.GetShader(ShaderDesc(L"Skybox.hlsl", L"VSMain", EShaderStage::Vertex));
	const auto PS = Library.GetShader(ShaderDesc(L"Skybox.hlsl", L"PSMain", EShaderStage::Pixel));
	if (!VS || !PS)
	{
		return false;
	}
	FGraphicsPipelineDesc Desc;
	Desc.RootSignature = SkyRoot.Get();
	Desc.VertexShader = FD3D12ShaderCompiler::ToBytecode(VS.Get());
	Desc.PixelShader = FD3D12ShaderCompiler::ToBytecode(PS.Get());
	Desc.RenderTargetFormats[0] = IblFormat;
	Desc.DepthStencilFormat = FD3D12RHI::DepthBufferFormat;
	Desc.bDepthEnable = true;
	Desc.bDepthWrite = false;
	Desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	Desc.CullMode = D3D12_CULL_MODE_NONE;
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, L"SkyPipeline");
}

bool FIblRenderer::Generate(FShaderLibrary& Library, bool bRebuild)
{
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	FD3D12RootSignature Root;
	Root.AddConstants(4, 0);
	Root.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
	Root.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
	Root.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) });
	Root.AddDescriptorTable({ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) }); // t1 등장방형
	Root.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	Root.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
		D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_SHADER_VISIBILITY_ALL));
	if (!Root.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"IblBakeRoot"))
	{
		return false;
	}
	FD3D12PipelineState Pipelines[BakeEntryCount];
	for (uint32 Index = 0; Index < BakeEntryCount; ++Index)
	{
		const auto Shader = Library.GetShader(ShaderDesc(L"Ibl.hlsl", BakeEntries[Index], EShaderStage::Compute));
		if (!Shader || !Pipelines[Index].InitCompute(Device, Root.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()), BakeEntries[Index]))
		{
			return false;
		}
	}

	// 초기화 명령이 끝날 때까지만 필요한 UAV 테이블.
	auto& Allocator = Rhi->GetSrvAllocator();
	FD3D12DescriptorHandle Uavs = Allocator.AllocateRange(PrefilterMipCount + 3);
	const auto UavGpu = [&](uint32 Offset)
	{
		return D3D12_GPU_DESCRIPTOR_HANDLE{ Uavs.Gpu.ptr + static_cast<UINT64>(Offset) * Allocator.GetIncrementSize() };
	};
	const auto CreateUav = [&](ID3D12Resource* Resource, uint32 Slot, uint32 Mip, bool bCube)
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC Desc{};
		Desc.Format = IblFormat;
		Desc.ViewDimension = bCube ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
		if (bCube)
		{
			Desc.Texture2DArray.MipSlice = Mip;
			Desc.Texture2DArray.ArraySize = 6;
		}
		Device->CreateUnorderedAccessView(Resource, nullptr, &Desc, Allocator.GetCpuHandle(Uavs.Index + Slot));
	};
	CreateUav(Sky.Get(), 0, 0, true);
	CreateUav(Irradiance.Get(), 1, 0, true);
	for (uint32 Mip = 0; Mip < PrefilterMipCount; ++Mip)
	{
		CreateUav(Prefilter.Get(), 2 + Mip, Mip, true);
	}
	CreateUav(BrdfLut.Get(), PrefilterMipCount + 2, 0, false);

	const bool bSuccess = Rhi->GetGraphicsQueue().ExecuteImmediate(Device, [&](ID3D12GraphicsCommandList* List)
	{
		const auto Transition = [&](ID3D12Resource* Resource, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
		{
			const auto Barrier = MakeTransitionBarrier(Resource, Before, After);
			List->ResourceBarrier(1, &Barrier);
		};
		if (bRebuild)
		{
			for (ID3D12Resource* Resource : { Sky.Get(), Irradiance.Get(), Prefilter.Get(), BrdfLut.Get() })
			{
				Transition(Resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			}
		}
		ID3D12DescriptorHeap* Heaps[] = { Allocator.GetHeap() };
		List->SetDescriptorHeaps(1, Heaps);
		List->SetComputeRootSignature(Root.Get());
		List->SetComputeRootDescriptorTable(1, SkySrv.Gpu);
		List->SetComputeRootDescriptorTable(3, UavGpu(PrefilterMipCount + 2));
		const auto Dispatch = [&](uint32 Pipeline, uint32 Size, uint32 Slot, uint32 Slices, float Roughness)
		{
			const FBakeConstants Constants{ Size, IblMath::IntegrationSampleCount, Roughness, EnvironmentRotation };
			List->SetPipelineState(Pipelines[Pipeline].Get());
			List->SetComputeRoot32BitConstants(0, 4, &Constants, 0);
			List->SetComputeRootDescriptorTable(2, UavGpu(Slot));
			List->Dispatch((Size + 7) / 8, (Size + 7) / 8, Slices);
		};
		// 하늘 큐브: 외부 환경맵이 있으면 등장방형 변환, 없으면 절차적 하늘
		if (EnvironmentTexture)
		{
			// 업로드 후 PIXEL_SHADER_RESOURCE → 계산에서 읽는 동안만 NON_PIXEL
			Transition(EnvironmentTexture->GetResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			List->SetComputeRootDescriptorTable(4, EnvironmentTexture->GetSrv().Gpu);
			Dispatch(4, SkySize, 0, 6, 0.0f);
			Transition(EnvironmentTexture->GetResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
		else
		{
			Dispatch(0, SkySize, 0, 6, 0.0f);
		}
		Transition(Sky.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		Dispatch(1, IblMath::IrradianceSize, 1, 6, 0.0f);
		for (uint32 Mip = 0; Mip < PrefilterMipCount; ++Mip)
		{
			Dispatch(2, IblMath::PrefilterSize >> Mip, 2 + Mip, 6, IblMath::MipToRoughness(Mip, PrefilterMipCount));
		}
		Dispatch(3, IblMath::BrdfLutSize, 1, 1, 0.0f);
		Transition(Sky.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		for (ID3D12Resource* Resource : { Irradiance.Get(), Prefilter.Get(), BrdfLut.Get() })
		{
			Transition(Resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}
	});
	Allocator.Free(Uavs);
	return bSuccess;
}

bool FIblRenderer::RecreateSky(uint32 Size)
{
	if (Sky && SkySize == Size)
	{
		return true;
	}
	if (Sky)
	{
		Rhi->DeferRelease(Sky); // 진행 중인 프레임이 하늘을 그릴 수 있다
		Sky.Reset();
	}
	SkySize = Size;
	// Generate(bRebuild)가 PIXEL_SHADER_RESOURCE → UAV로 전이하므로 그 상태로 만든다. SRV는 같은 칸에 다시 기록
	return CreateTexture(Size, 6, 1, Sky, SkySrv.Cpu, L"IblSky", D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool FIblRenderer::SetEnvironment(const FEnvironmentImage* Image, float RotationDegrees)
{
	if (!bReady)
	{
		return false;
	}
	std::unique_ptr<FD3D12Texture> NewTexture;
	if (Image != nullptr)
	{
		if (!Image->IsValid())
		{
			return false;
		}
		const FD3D12Texture::FMipData Mip{ Image->Pixels.data(), Image->Pixels.size() * sizeof(uint16) };
		NewTexture = std::make_unique<FD3D12Texture>();
		if (!NewTexture->Init2DFromMips(Rhi->GetDevice(), Rhi->GetGraphicsQueue(), Rhi->GetSrvAllocator(), Image->Width, Image->Height,
		                                DXGI_FORMAT_R16G16B16A16_FLOAT, &Mip, 1, L"IblEnvironmentEquirect"))
		{
			E_LOG(LogRenderer, Error, "환경맵 텍스처 생성 실패 ({}x{})", Image->Width, Image->Height);
			return false;
		}
	}
	// 진행 중인 프레임이 하늘 SRV 칸·조도/프리필터를 읽고 있을 수 있으므로 GPU를 기다린 뒤 바꾼다 (바뀔 때만이라 드물다)
	Rhi->GetGraphicsQueue().Flush();
	if (EnvironmentTexture)
	{
		EnvironmentTexture->ShutdownDeferred(*Rhi);
	}
	EnvironmentTexture  = std::move(NewTexture);
	EnvironmentRotation = FMath::DegreesToRadians(RotationDegrees);
	if (!RecreateSky(EnvironmentTexture ? IblMath::EnvironmentSkyCubeSize : IblMath::SkyCubeSize) || !Generate(*ShaderLibrary, true))
	{
		E_LOG(LogRenderer, Error, "환경광 다시 생성 실패");
		return false;
	}
	E_LOG(LogRenderer, Display, "환경광 다시 생성: {} (하늘 큐브 {}, 회전 {:.1f}도)", EnvironmentTexture ? "외부 HDR 환경맵" : "절차적 하늘", SkySize,
	      RotationDegrees);
	return true;
}

bool FIblRenderer::ReloadShaders(bool bForceRecompile)
{
	if (!bReady)
	{
		return false;
	}
	if (bForceRecompile)
	{
		for (const wchar_t* Entry : BakeEntries)
		{
			if (!ShaderLibrary->CookShader(ShaderDesc(L"Ibl.hlsl", Entry, EShaderStage::Compute)))
			{
				return false;
			}
		}
		if (!ShaderLibrary->CookShader(ShaderDesc(L"Skybox.hlsl", L"VSMain", EShaderStage::Vertex)) ||
			!ShaderLibrary->CookShader(ShaderDesc(L"Skybox.hlsl", L"PSMain", EShaderStage::Pixel)))
		{
			return false;
		}
	}
	FD3D12PipelineState NewPipeline;
	if (!CreateSkyPipeline(*ShaderLibrary, NewPipeline) || !Generate(*ShaderLibrary, true))
	{
		return false;
	}
	SkyPipeline.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());
	return true;
}

void FIblRenderer::RenderSkybox(const FCamera& Camera, float Intensity)
{
	if (!bReady)
	{
		return;
	}
	const FSkyConstants Constants{ Camera.GetForwardVector(), FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f),
		Camera.GetRightVector(), Camera.GetAspectRatio(), Camera.GetUpVector(), Intensity };
	auto* List = Rhi->GetCommandList();
	List->SetGraphicsRootSignature(SkyRoot.Get());
	List->SetPipelineState(SkyPipeline.Get());
	List->SetGraphicsRoot32BitConstants(0, 12, &Constants, 0);
	List->SetGraphicsRootDescriptorTable(1, SkySrv.Gpu);
	List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	List->DrawInstanced(3, 1, 0, 0);
}

void FIblRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	// 씬 렌더러가 GPU 완료를 기다린 뒤 호출한다. 부분 초기화 실패도 동일하게 정리한다.
	EnvironmentTexture.reset();
	Sky.Reset();
	Irradiance.Reset();
	Prefilter.Reset();
	BrdfLut.Reset();
	if (SkySrv.IsValid())
	{
		Rhi->GetSrvAllocator().Free(SkySrv);
	}
	if (LightingTable.IsValid())
	{
		Rhi->GetSrvAllocator().Free(LightingTable);
	}
	SkyPipeline.Shutdown();
	SkyRoot.Shutdown();
	bReady = false;
	Rhi = nullptr;
	ShaderLibrary = nullptr;
}
