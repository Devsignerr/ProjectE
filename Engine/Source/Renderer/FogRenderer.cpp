#include "Renderer/FogRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/FogMath.h"
#include "Renderer/ScreenPass.h"
#include "Renderer/TemporalMath.h"
#include "Scene/Scene.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// 볼류메트릭 계산 루트 시그니처 (VolumetricFog.hlsl)
	enum EVolumeRootParameter : uint32
	{
		VolumeParam_Constants        = 0, // b0
		VolumeParam_ShadowConstants  = 1, // b1
		VolumeParam_ClusterConstants = 2, // b2
		VolumeParam_ShadowMap        = 3, // t0 표
		VolumeParam_LocalLights      = 4, // t1 루트 SRV
		VolumeParam_History          = 5, // t3 표
		VolumeParam_Inject           = 6, // t4 표
		VolumeParam_Output           = 7, // u0 표
	};

	constexpr float CentimetersPerMeter = 100.0f;
} // namespace

FFogRenderer::~FFogRenderer()
{
	Shutdown();
}

bool FFogRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary, const FScreenPassRootSignature& InRoot)
{
	Rhi     = &InRhi;
	Library = &InLibrary;
	Root    = &InRoot;

	const auto Table = [](D3D12_DESCRIPTOR_RANGE_TYPE Type, uint32 Register) {
		return std::vector<D3D12_DESCRIPTOR_RANGE1>{ FD3D12RootSignature::MakeRange(Type, 1, Register, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) };
	};
	const uint32 ConstantsIndex = VolumeRoot.AddConstantBufferView(0);
	const uint32 ShadowIndex    = VolumeRoot.AddConstantBufferView(1);
	const uint32 ClusterIndex   = VolumeRoot.AddConstantBufferView(2);
	const uint32 ShadowMapIndex = VolumeRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0));
	const uint32 LightsIndex    = VolumeRoot.AddShaderResourceView(1);
	const uint32 HistoryIndexP  = VolumeRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3));
	const uint32 InjectIndex    = VolumeRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4));
	const uint32 OutputIndex    = VolumeRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0));
	E_CHECK(ConstantsIndex == VolumeParam_Constants && ShadowIndex == VolumeParam_ShadowConstants && ClusterIndex == VolumeParam_ClusterConstants &&
	        ShadowMapIndex == VolumeParam_ShadowMap && LightsIndex == VolumeParam_LocalLights && HistoryIndexP == VolumeParam_History &&
	        InjectIndex == VolumeParam_Inject && OutputIndex == VolumeParam_Output);
	VolumeRoot.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	D3D12_STATIC_SAMPLER_DESC Compare = FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
	                                                                            D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_SHADER_VISIBILITY_ALL);
	Compare.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	Compare.BorderColor    = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	Compare.MaxAnisotropy  = 1;
	VolumeRoot.AddStaticSampler(Compare);
	if (!VolumeRoot.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"VolumetricFogRootSignature"))
	{
		return false;
	}
	if (!CreatePipelines(ApplyPipeline, InjectPipeline, IntegratePipeline, false))
	{
		return false;
	}
	EnsureVolumes(1, 1, 1); // 결과 SRV가 항상 유효하도록
	return true;
}

void FFogRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	ReleaseVolumes();
	ApplyPipeline.Shutdown();
	InjectPipeline.Shutdown();
	IntegratePipeline.Shutdown();
	VolumeRoot.Shutdown();
	Rhi = nullptr;
}

bool FFogRenderer::CreatePipelines(FD3D12PipelineState& OutApply, FD3D12PipelineState& OutInject, FD3D12PipelineState& OutIntegrate,
                                   bool bForceRecompile)
{
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	if (!Root->CreateGraphicsPipeline(OutApply, Device, *Library, L"FogApply.hlsl", L"PSMain", { DXGI_FORMAT_R16G16B16A16_FLOAT },
	                                  EBlendMode::Premultiplied, bForceRecompile, L"FogApplyPipeline"))
	{
		return false;
	}
	const auto LoadCompute = [&](const wchar_t* Entry, bool bCook) {
		FShaderCompileDesc Desc;
		Desc.FileName   = L"VolumetricFog.hlsl";
		Desc.EntryPoint = Entry;
		Desc.Stage      = EShaderStage::Compute;
		if (bCook && !Library->CookShader(Desc))
		{
			return ComPtr<IDxcBlob>();
		}
		return Library->GetShader(Desc);
	};
	const ComPtr<IDxcBlob> Inject    = LoadCompute(L"CSInject", bForceRecompile);
	const ComPtr<IDxcBlob> Integrate = LoadCompute(L"CSIntegrate", bForceRecompile);
	if (!Inject || !Integrate)
	{
		return false;
	}
	return OutInject.InitCompute(Device, VolumeRoot.Get(), FD3D12ShaderCompiler::ToBytecode(Inject.Get()), L"VolumetricFogInject") &&
	       OutIntegrate.InitCompute(Device, VolumeRoot.Get(), FD3D12ShaderCompiler::ToBytecode(Integrate.Get()), L"VolumetricFogIntegrate");
}

bool FFogRenderer::ReloadShaders(bool bForceRecompile)
{
	FD3D12PipelineState NewApply;
	FD3D12PipelineState NewInject;
	FD3D12PipelineState NewIntegrate;
	if (!CreatePipelines(NewApply, NewInject, NewIntegrate, bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "안개 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	ApplyPipeline.Swap(NewApply);
	Rhi->DeferRelease(NewApply.Detach());
	InjectPipeline.Swap(NewInject);
	Rhi->DeferRelease(NewInject.Detach());
	IntegratePipeline.Swap(NewIntegrate);
	Rhi->DeferRelease(NewIntegrate.Detach());
	return true;
}

void FFogRenderer::ReleaseVolumes()
{
	for (FVolume& Volume : Volumes)
	{
		if (Volume.Resource)
		{
			Rhi->DeferRelease(Volume.Resource);
			Rhi->DeferFreeDescriptor(Volume.Srv);
			Rhi->DeferFreeDescriptor(Volume.Uav);
		}
		Volume = FVolume{};
	}
	GridSize[0] = GridSize[1] = GridSize[2] = 0;
	bHasHistory = false;
}

void FFogRenderer::EnsureVolumes(uint32 GridX, uint32 GridY, uint32 GridZ)
{
	if (GridSize[0] == GridX && GridSize[1] == GridY && GridSize[2] == GridZ)
	{
		return;
	}
	ReleaseVolumes();
	ID3D12Device*               Device = Rhi->GetDevice().GetDevice();
	const D3D12_HEAP_PROPERTIES Heap   = MakeHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	D3D12_RESOURCE_DESC         Desc{};
	Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	Desc.Width            = GridX;
	Desc.Height           = GridY;
	Desc.DepthOrArraySize = static_cast<UINT16>(GridZ);
	Desc.MipLevels        = 1;
	Desc.Format           = VolumeFormat;
	Desc.SampleDesc.Count = 1;
	Desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	Desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	const wchar_t* const Names[VolumeCount] = { L"FogInjectVolume0", L"FogInjectVolume1", L"FogIntegratedVolume" };
	FD3D12DescriptorAllocator& Allocator     = Rhi->GetSrvAllocator();
	for (uint32 Index = 0; Index < VolumeCount; ++Index)
	{
		FVolume& Volume = Volumes[Index];
		if (FAILED(Device->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&Volume.Resource))))
		{
			E_LOG(LogRenderer, Fatal, "안개 볼륨 생성 실패 ({}x{}x{})", GridX, GridY, GridZ);
		}
		Volume.Resource->SetName(Names[Index]);
		Volume.State = D3D12_RESOURCE_STATE_COMMON;

		D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc{};
		SrvDesc.Format                  = VolumeFormat;
		SrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE3D;
		SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SrvDesc.Texture3D.MipLevels     = 1;
		Volume.Srv                      = Allocator.Allocate();
		Device->CreateShaderResourceView(Volume.Resource.Get(), &SrvDesc, Volume.Srv.Cpu);

		D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc{};
		UavDesc.Format          = VolumeFormat;
		UavDesc.ViewDimension   = D3D12_UAV_DIMENSION_TEXTURE3D;
		UavDesc.Texture3D.WSize = GridZ;
		Volume.Uav              = Allocator.Allocate();
		Device->CreateUnorderedAccessView(Volume.Resource.Get(), nullptr, &UavDesc, Volume.Uav.Cpu);
	}
	GridSize[0] = GridX;
	GridSize[1] = GridY;
	GridSize[2] = GridZ;
	bHasHistory = false;
}

bool FFogRenderer::Prepare(FScene& Scene, const FCamera& Camera, const FMatrix4x4& UnjitteredViewProjection, uint32 Width, uint32 Height)
{
	Constants        = FFogConstants{};
	ConstantsAddress = 0;
	TargetWidth      = Width;
	TargetHeight     = Height;

	const FHeightFogComponent* Fog     = nullptr;
	float                      FogBase = 0.0f;
	Scene.GetRegistry().View<FTransformComponent, FHeightFogComponent>().Each([&](FEntity, FTransformComponent& Transform, FHeightFogComponent& Component) {
		if (Fog == nullptr)
		{
			Fog     = &Component;
			FogBase = Transform.GetWorldPosition().Z;
		}
	});
	if (Fog != nullptr)
	{
		Constants.Color                    = Fog->Color;
		Constants.Density                  = FMath::Max(Fog->Density, 0.0f) / CentimetersPerMeter;
		Constants.DirectionalColor         = Fog->DirectionalInscatteringColor;
		Constants.HeightFalloff            = FMath::Max(Fog->HeightFalloff, 0.0f) / CentimetersPerMeter;
		Constants.BaseHeight               = FogBase;
		Constants.StartDistance            = FMath::Max(Fog->StartDistance, 0.0f);
		Constants.MaxOpacity               = FMath::Clamp(Fog->MaxOpacity, 0.0f, 1.0f);
		Constants.DirectionalExponent      = FMath::Max(Fog->DirectionalInscatteringExponent, 1.0f);
		Constants.DirectionalStartDistance = FMath::Max(Fog->DirectionalInscatteringStartDistance, 0.0f);
		Constants.bEnabled                 = 1;
		Constants.bVolumetric              = Fog->bVolumetric && !Camera.IsOrthographic() ? 1u : 0u; // 직교 카메라는 해석식만
		Constants.VolumetricDistance       = FMath::Max(Fog->VolumetricDistance, 100.0f);

		VolumeConstants                  = FVolumetricFogConstants{};
		VolumeConstants.Density          = Constants.Density;
		VolumeConstants.HeightFalloff    = Constants.HeightFalloff;
		VolumeConstants.BaseHeight       = FogBase;
		VolumeConstants.Albedo           = Fog->VolumetricAlbedo;
		VolumeConstants.ExtinctionScale  = FMath::Max(Fog->VolumetricExtinctionScale, 0.0f);
		VolumeConstants.AmbientColor     = Fog->Color;
		VolumeConstants.Anisotropy       = FMath::Clamp(Fog->VolumetricAnisotropy, -0.95f, 0.95f);
		VolumeConstants.DirectionalScale = FMath::Max(Fog->VolumetricDirectionalScale, 0.0f);
		VolumeConstants.LocalLightScale  = FMath::Max(Fog->VolumetricLocalLightScale, 0.0f);
	}
	Constants.CameraPosition    = Camera.GetPosition();
	Constants.CameraForward     = Camera.GetForwardVector();
	Constants.SkyDistance       = Camera.GetFarZ();
	Constants.ScreenSize        = FVector2(static_cast<float>(Width), static_cast<float>(Height));
	Constants.ViewProjection    = UnjitteredViewProjection;
	Constants.InvViewProjection = Camera.GetViewProjectionMatrix().GetInverse();
	if (bAerial)
	{
		Constants.AerialRayleighScattering  = AerialParams.RayleighScattering;
		Constants.AerialRayleighScaleHeight = AerialParams.RayleighScaleHeight;
		Constants.AerialMieScattering       = AerialParams.MieScattering;
		Constants.AerialMieScaleHeight      = AerialParams.MieScaleHeight;
		Constants.AerialMieExtinction       = AerialParams.MieExtinction;
		Constants.AerialMieAnisotropy       = AerialParams.MieAnisotropy;
		Constants.AerialSunIlluminance      = AerialParams.SunIlluminance;
		Constants.AerialEnabled             = 1;
		Constants.AerialSunDirection        = AerialParams.SunDirection;
		Constants.AerialGroundHeight        = AerialParams.GroundHeight;
		Constants.AerialMultiScattering     = AerialParams.MultiScattering;
		Constants.AerialDistanceScale       = AerialParams.DistanceScale;
	}
	return Constants.bEnabled != 0;
}

void FFogRenderer::PrepareVolumetric(const FVolumetricFogInputs& Inputs)
{
	Constants.LightDirection = Inputs.LightDirection;
	ConstantsAddress         = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress; // 적용/파티클용 (볼륨 여부와 무관)
	bVolumetricThisFrame     = Constants.bVolumetric != 0;
	VolumeConstantsAddress   = 0;
	FrameInputs              = Inputs;
	if (!bVolumetricThisFrame)
	{
		return;
	}
	const uint32 GridX = FFogMath::GetVolumeDimension(TargetWidth);
	const uint32 GridY = FFogMath::GetVolumeDimension(TargetHeight);
	const uint32 GridZ = FFogMath::VolumeSliceCount;
	EnsureVolumes(GridX, GridY, GridZ);

	VolumeConstants.InvViewProjection  = Constants.ViewProjection.GetInverse();
	VolumeConstants.PrevViewProjection = Inputs.PrevViewProjection;
	VolumeConstants.CameraPosition     = Constants.CameraPosition;
	VolumeConstants.CameraForward      = Constants.CameraForward;
	VolumeConstants.VolumetricDistance = Constants.VolumetricDistance;
	VolumeConstants.LightDirection     = Inputs.LightDirection;
	VolumeConstants.LightColor         = Inputs.LightColor;
	VolumeConstants.GridX              = GridX;
	VolumeConstants.GridY              = GridY;
	VolumeConstants.GridZ              = GridZ;
	VolumeConstants.bHistoryValid      = (Inputs.bHistoryValid && bHasHistory) ? 1u : 0u;
	// 이력이 있으면 조각 안 표본을 프레임마다 흔들고 많이 섞는다, 없으면 가운데 표본만
	VolumeConstants.SliceJitter   = VolumeConstants.bHistoryValid ? FTemporalMath::Halton(static_cast<uint32>(Inputs.FrameIndex % 16) + 1, 2) : 0.5f;
	VolumeConstants.HistoryWeight = 0.9f;
	VolumeConstantsAddress        = Rhi->GetDynamicBuffer().AllocateConstants(VolumeConstants).GpuAddress;
}

FRGResourceRef FFogRenderer::ImportVolume(FRenderGraph& Graph)
{
	FVolume& Result = Volumes[IntegratedVolume];
	return Graph.ImportTracked("FogIntegratedVolume", Result.Resource.Get(), &Result.State);
}

void FFogRenderer::AddVolumetricPasses(FRenderGraph& Graph, FRGResourceRef ShadowMap, ERGQueue Queue, int32 Timer)
{
	if (!bVolumetricThisFrame)
	{
		return;
	}
	FVolume&             Current     = Volumes[HistoryIndex];
	FVolume&             Previous    = Volumes[HistoryIndex ^ 1];
	FVolume&             Result      = Volumes[IntegratedVolume];
	const FRGResourceRef CurrentRef  = Graph.ImportTracked(HistoryIndex == 0 ? "FogInjectVolume0" : "FogInjectVolume1", Current.Resource.Get(), &Current.State);
	const FRGResourceRef PreviousRef = Graph.ImportTracked(HistoryIndex == 0 ? "FogInjectVolume1" : "FogInjectVolume0", Previous.Resource.Get(), &Previous.State);
	const FRGResourceRef ResultRef   = ImportVolume(Graph);
	const uint32         GridX       = GridSize[0];
	const uint32         GridY       = GridSize[1];
	const uint32         GridZ       = GridSize[2];

	// 볼륨 루트 공용 인자 (계산 큐 명령 목록에서도 그대로)
	const auto BindCommon = [this](ID3D12GraphicsCommandList* CommandList) {
		CommandList->SetComputeRootSignature(VolumeRoot.Get());
		CommandList->SetComputeRootConstantBufferView(VolumeParam_Constants, VolumeConstantsAddress);
		CommandList->SetComputeRootConstantBufferView(VolumeParam_ShadowConstants, FrameInputs.ShadowConstants);
		CommandList->SetComputeRootConstantBufferView(VolumeParam_ClusterConstants, FrameInputs.ClusterConstants);
		CommandList->SetComputeRootDescriptorTable(VolumeParam_ShadowMap, FrameInputs.ShadowMapSrv.Gpu);
		CommandList->SetComputeRootShaderResourceView(VolumeParam_LocalLights, FrameInputs.LocalLights);
	};

	// 주입: 그림자 맵 + 이전 이력 → 이번 이력
	FRenderGraph::FPassBuilder Inject = Graph.AddPass("볼류메트릭 안개 주입", Queue);
	if (ShadowMap.IsValid())
	{
		Inject.Read(ShadowMap, ERGAccess::SrvNonPixel);
	}
	Inject.Read(PreviousRef, ERGAccess::SrvNonPixel)
		.Write(CurrentRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, BindCommon, &Current, &Previous, GridX, GridY, GridZ](FRGContext& Context) {
			ID3D12GraphicsCommandList* CommandList = Context.CommandList;
			BindCommon(CommandList);
			CommandList->SetComputeRootDescriptorTable(VolumeParam_History, Previous.Srv.Gpu);
			CommandList->SetComputeRootDescriptorTable(VolumeParam_Inject, Previous.Srv.Gpu); // 주입 단계는 읽지 않음 (유효한 표만)
			CommandList->SetComputeRootDescriptorTable(VolumeParam_Output, Current.Uav.Gpu);
			CommandList->SetPipelineState(InjectPipeline.Get());
			CommandList->Dispatch((GridX + 3) / 4, (GridY + 3) / 4, (GridZ + 3) / 4);
		});

	// 적분: 이번 주입 결과 → 결과 볼륨
	FRenderGraph::FPassBuilder Integrate = Graph.AddPass("볼류메트릭 안개 적분", Queue);
	if (ShadowMap.IsValid())
	{
		Integrate.Read(ShadowMap, ERGAccess::SrvNonPixel); // 루트 표가 그대로 묶여 있다 (읽지는 않음)
	}
	Integrate.Read(CurrentRef, ERGAccess::SrvNonPixel)
		.Read(PreviousRef, ERGAccess::SrvNonPixel)
		.Write(ResultRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, BindCommon, &Current, &Previous, &Result, GridX, GridY](FRGContext& Context) {
			ID3D12GraphicsCommandList* CommandList = Context.CommandList;
			BindCommon(CommandList);
			CommandList->SetComputeRootDescriptorTable(VolumeParam_History, Previous.Srv.Gpu);
			CommandList->SetComputeRootDescriptorTable(VolumeParam_Inject, Current.Srv.Gpu);
			CommandList->SetComputeRootDescriptorTable(VolumeParam_Output, Result.Uav.Gpu);
			CommandList->SetPipelineState(IntegratePipeline.Get());
			CommandList->Dispatch((GridX + 7) / 8, (GridY + 7) / 8, 1);
		});
	HistoryIndex ^= 1;
	bHasHistory = true;
}

void FFogRenderer::AddApplyPass(FRenderGraph& Graph, const FD3D12RenderTarget& SceneColor, FRGResourceRef SceneColorRef, FRGResourceRef DepthRef,
                                int32 Timer)
{
	if ((Constants.bEnabled == 0 && Constants.AerialEnabled == 0) || ConstantsAddress == 0)
	{
		return;
	}
	const FRGResourceRef VolumeRef = ImportVolume(Graph);
	Graph.AddPass("안개 적용")
		.Read(DepthRef, ERGAccess::SrvPixel)
		.Read(VolumeRef, ERGAccess::SrvPixel)
		.Write(SceneColorRef, ERGAccess::RenderTarget)
		.Timer(Timer)
		.Execute([this, &SceneColor](FRGContext& Context) {
			SceneColor.Bind(Context.CommandList, nullptr, false, false);
			DrawScreenPass(Context.CommandList, *Root, ApplyPipeline, ConstantsAddress, { SceneColor.GetDepthSrv(), GetVolumeSrv() }, SceneColor.GetWidth(),
			               SceneColor.GetHeight());
		});
}
