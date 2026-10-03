#include "Renderer/VolumetricCloudRenderer.h"

#include "Core/FrameTime.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/Camera.h"
#include "Renderer/RendererConsoleVariables.h"
#include "Renderer/ScreenPass.h"
#include "Renderer/SkyAtmosphereRenderer.h"
#include "Renderer/TemporalMath.h"
#include "Scene/Scene.h"
#include "Scene/SkyAtmosphere.h"

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	enum ECloudRoot : uint32
	{
		CloudRoot_Constants    = 0,  // b0
		CloudRoot_Atmosphere   = 1,  // b1
		CloudRoot_Fog          = 2,  // b2
		CloudRoot_Transmittance = 3, // t0
		CloudRoot_SkyView      = 4,  // t2
		CloudRoot_Shape        = 5,  // t3
		CloudRoot_Detail       = 6,  // t4
		CloudRoot_Weather      = 7,  // t5
		CloudRoot_HistoryColor = 8,  // t6
		CloudRoot_HistoryDepth = 9,  // t7
		CloudRoot_SceneDepth   = 10, // t8
		CloudRoot_CurrentColor = 11, // t10
		CloudRoot_CurrentDepth = 12, // t11
		CloudRoot_OutColor     = 13, // u0
		CloudRoot_OutDepth     = 14, // u1
		CloudRoot_OutCube      = 15, // u2
	};

	constexpr uint32 ShapeNoiseSize  = 128;
	constexpr uint32 DetailNoiseSize = 32;
	constexpr uint32 WeatherSize     = 256;
	constexpr uint32 CloudCubeSize   = 32;

	FShaderCompileDesc MakeDesc(const wchar_t* File, const wchar_t* Entry, EShaderStage Stage)
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = File;
		Desc.EntryPoint = Entry;
		Desc.Stage      = Stage;
		return Desc;
	}
} // namespace

FVolumetricCloudRenderer::~FVolumetricCloudRenderer()
{
	Shutdown();
}

bool FVolumetricCloudRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary)
{
	Rhi       = &InRhi;
	Library   = &InLibrary;
	StartTime = FFrameTime::GetTotalSeconds();
	const auto Table = [](D3D12_DESCRIPTOR_RANGE_TYPE Type, uint32 Register) {
		return std::vector<D3D12_DESCRIPTOR_RANGE1>{ FD3D12RootSignature::MakeRange(Type, 1, Register, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) };
	};
	uint32 Index = 0;
	bool   bOk   = true;
	const auto Expect = [&](uint32 Got) {
		bOk = bOk && Got == Index;
		++Index;
	};
	Expect(RootSignature.AddConstantBufferView(0));
	Expect(RootSignature.AddConstantBufferView(1));
	Expect(RootSignature.AddConstantBufferView(2));
	for (const uint32 Register : { 0u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 10u, 11u })
	{
		Expect(RootSignature.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, Register)));
	}
	for (const uint32 Register : { 0u, 1u, 2u })
	{
		Expect(RootSignature.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, Register)));
	}
	E_CHECK(bOk && Index == CloudRoot_OutCube + 1);
	RootSignature.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	RootSignature.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_SHADER_VISIBILITY_ALL));
	RootSignature.AddStaticSampler(
		FD3D12RootSignature::MakeStaticSampler(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	if (!RootSignature.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"VolumetricCloudRoot"))
	{
		return false;
	}
	NoiseRoot.AddConstants(4, 0);
	NoiseRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0));
	NoiseRoot.AddDescriptorTable(Table(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1));
	if (!NoiseRoot.Finalize(Rhi->GetDevice().GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"CloudNoiseRoot"))
	{
		return false;
	}
	return CreatePipelines(false);
}

bool FVolumetricCloudRenderer::CreatePipelines(bool bForceRecompile)
{
	const auto Load = [&](const wchar_t* Entry, EShaderStage Stage) {
		const FShaderCompileDesc Desc = MakeDesc(L"VolumetricClouds.hlsl", Entry, Stage);
		if (bForceRecompile && !Library->CookShader(Desc))
		{
			return ComPtr<IDxcBlob>();
		}
		return Library->GetShader(Desc);
	};
	const ComPtr<IDxcBlob> Trace   = Load(L"TraceCS", EShaderStage::Compute);
	const ComPtr<IDxcBlob> Resolve = Load(L"ResolveCS", EShaderStage::Compute);
	const ComPtr<IDxcBlob> Cube    = Load(L"CubeCS", EShaderStage::Compute);
	const ComPtr<IDxcBlob> Vs      = Load(L"VSComposite", EShaderStage::Vertex);
	const ComPtr<IDxcBlob> Ps      = Load(L"PSComposite", EShaderStage::Pixel);
	if (!Trace || !Resolve || !Cube || !Vs || !Ps)
	{
		return false;
	}
	ID3D12Device*         Device = Rhi->GetDevice().GetDevice();
	FD3D12PipelineState   New[4];
	FGraphicsPipelineDesc Composite;
	Composite.RootSignature          = RootSignature.Get();
	Composite.VertexShader           = FD3D12ShaderCompiler::ToBytecode(Vs.Get());
	Composite.PixelShader            = FD3D12ShaderCompiler::ToBytecode(Ps.Get());
	Composite.RenderTargetFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	Composite.CullMode               = D3D12_CULL_MODE_NONE;
	Composite.BlendMode              = EBlendMode::Premultiplied; // 색 = 구름 빛 + 원래 × 투과율, 알파(TAA 마스크) 유지
	if (!New[0].InitCompute(Device, RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Trace.Get()), L"CloudTrace") ||
	    !New[1].InitCompute(Device, RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Resolve.Get()), L"CloudResolve") ||
	    !New[2].InitCompute(Device, RootSignature.Get(), FD3D12ShaderCompiler::ToBytecode(Cube.Get()), L"CloudCube") ||
	    !New[3].InitGraphics(Device, Composite, L"CloudComposite"))
	{
		return false;
	}
	FD3D12PipelineState* Targets[4] = { &TracePipeline, &ResolvePipeline, &CubePipeline, &CompositePipeline };
	for (uint32 Index = 0; Index < 4; ++Index)
	{
		Targets[Index]->Swap(New[Index]);
		if (New[Index].Get() != nullptr)
		{
			Rhi->DeferRelease(New[Index].Detach());
		}
	}
	return true;
}

bool FVolumetricCloudRenderer::ReloadShaders(bool bForceRecompile)
{
	if (Rhi == nullptr)
	{
		return false;
	}
	if (!CreatePipelines(bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "구름 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	bNoiseReady = false; // 노이즈 셰이더가 바뀌었을 수 있다
	bHasHistory = false;
	return true;
}

void FVolumetricCloudRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	for (FPersistentTexture* Texture : { &ShapeNoise, &DetailNoise, &WeatherMap, &TraceColor, &TraceDepth, &HistoryColor[0], &HistoryColor[1],
	                                     &HistoryDepth[0], &HistoryDepth[1], &CloudCube })
	{
		Texture->Release(*Rhi);
	}
	TracePipeline.Shutdown();
	ResolvePipeline.Shutdown();
	CubePipeline.Shutdown();
	CompositePipeline.Shutdown();
	RootSignature.Shutdown();
	NoiseRoot.Shutdown();
	Rhi     = nullptr;
	Library = nullptr;
}

bool FVolumetricCloudRenderer::GenerateNoise()
{
	FPersistentTextureDesc Desc;
	Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	Desc.b3D    = true;
	Desc.Width = Desc.Height = Desc.Depth = ShapeNoiseSize;
	if (!ShapeNoise.Ensure(*Rhi, Desc, L"CloudShapeNoise"))
	{
		return false;
	}
	Desc.Width = Desc.Height = Desc.Depth = DetailNoiseSize;
	if (!DetailNoise.Ensure(*Rhi, Desc, L"CloudDetailNoise"))
	{
		return false;
	}
	FPersistentTextureDesc WeatherDesc;
	WeatherDesc.Format = DXGI_FORMAT_R8G8_UNORM;
	WeatherDesc.Width = WeatherDesc.Height = WeatherSize;
	if (!WeatherMap.Ensure(*Rhi, WeatherDesc, L"CloudWeatherMap"))
	{
		return false;
	}
	const auto LoadNoise = [&](const wchar_t* Entry) { return Library->GetShader(MakeDesc(L"CloudNoise.hlsl", Entry, EShaderStage::Compute)); };
	const ComPtr<IDxcBlob> ShapeCs   = LoadNoise(L"ShapeNoiseCS");
	const ComPtr<IDxcBlob> DetailCs  = LoadNoise(L"DetailNoiseCS");
	const ComPtr<IDxcBlob> WeatherCs = LoadNoise(L"WeatherCS");
	if (!ShapeCs || !DetailCs || !WeatherCs)
	{
		return false;
	}
	ID3D12Device*       Device = Rhi->GetDevice().GetDevice();
	FD3D12PipelineState Pipelines[3];
	if (!Pipelines[0].InitCompute(Device, NoiseRoot.Get(), FD3D12ShaderCompiler::ToBytecode(ShapeCs.Get()), L"CloudShapeNoise") ||
	    !Pipelines[1].InitCompute(Device, NoiseRoot.Get(), FD3D12ShaderCompiler::ToBytecode(DetailCs.Get()), L"CloudDetailNoise") ||
	    !Pipelines[2].InitCompute(Device, NoiseRoot.Get(), FD3D12ShaderCompiler::ToBytecode(WeatherCs.Get()), L"CloudWeather"))
	{
		return false;
	}

	// 즉시 실행 그래프 (로딩 시점 — 비동기 계산 없음)
	FRGResourcePool Pool;
	Pool.Init(*Rhi);
	FRenderGraph         Graph(*Rhi, Pool, "CloudNoise");
	const FRGResourceRef ShapeRef   = ShapeNoise.Import(Graph, "CloudShapeNoise");
	const FRGResourceRef DetailRef  = DetailNoise.Import(Graph, "CloudDetailNoise");
	const FRGResourceRef WeatherRef = WeatherMap.Import(Graph, "CloudWeatherMap");
	const auto Bind = [this](ID3D12GraphicsCommandList* List) {
		ID3D12DescriptorHeap* Heaps[] = { Rhi->GetSrvAllocator().GetHeap() };
		List->SetDescriptorHeaps(1, Heaps);
		List->SetComputeRootSignature(NoiseRoot.Get());
	};
	Graph.AddPass("구름 모양 노이즈").Write(ShapeRef, ERGAccess::Uav, FRGSubresourceRange::All(), true).Execute([&](FRGContext& Context) {
		Bind(Context.CommandList);
		const uint32 Constants[4] = { ShapeNoiseSize, 0, 0, 0 };
		Context.CommandList->SetComputeRoot32BitConstants(0, 4, Constants, 0);
		Context.CommandList->SetComputeRootDescriptorTable(1, ShapeNoise.Uavs[0].Gpu);
		Context.CommandList->SetPipelineState(Pipelines[0].Get());
		Context.CommandList->Dispatch(ShapeNoiseSize / 4, ShapeNoiseSize / 4, ShapeNoiseSize / 4);
	});
	Graph.AddPass("구름 세부 노이즈").Write(DetailRef, ERGAccess::Uav, FRGSubresourceRange::All(), true).Execute([&](FRGContext& Context) {
		Bind(Context.CommandList);
		const uint32 Constants[4] = { DetailNoiseSize, 1, 0, 0 };
		Context.CommandList->SetComputeRoot32BitConstants(0, 4, Constants, 0);
		Context.CommandList->SetComputeRootDescriptorTable(1, DetailNoise.Uavs[0].Gpu);
		Context.CommandList->SetPipelineState(Pipelines[1].Get());
		Context.CommandList->Dispatch(DetailNoiseSize / 4, DetailNoiseSize / 4, DetailNoiseSize / 4);
	});
	Graph.AddPass("구름 덮임 분포").Write(WeatherRef, ERGAccess::Uav, FRGSubresourceRange::All(), true).Execute([&](FRGContext& Context) {
		Bind(Context.CommandList);
		const uint32 Constants[4] = { WeatherSize, 2, 0, 0 };
		Context.CommandList->SetComputeRoot32BitConstants(0, 4, Constants, 0);
		Context.CommandList->SetComputeRootDescriptorTable(2, WeatherMap.Uavs[0].Gpu);
		Context.CommandList->SetPipelineState(Pipelines[2].Get());
		Context.CommandList->Dispatch(WeatherSize / 8, WeatherSize / 8, 1);
	});
	FRGCompileOptions Options;
	Options.bAsyncCompute = false;
	Graph.CompileCacheEnabled = false;
	Graph.Compile(Options);
	const bool bSuccess = Rhi->GetGraphicsQueue().ExecuteImmediate(Device, [&](ID3D12GraphicsCommandList* List) { Graph.Execute(List); });
	if (bSuccess)
	{
		E_LOG(LogRenderer, Display, "구름 노이즈 생성: 모양 {}^3, 세부 {}^3, 덮임 {}^2", ShapeNoiseSize, DetailNoiseSize, WeatherSize);
	}
	return bSuccess;
}

void FVolumetricCloudRenderer::EnsureTargets(uint32 Width, uint32 Height)
{
	FPersistentTextureDesc Color;
	Color.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	Color.Width  = Width;
	Color.Height = Height;
	FPersistentTextureDesc Depth = Color;
	Depth.Format                 = DXGI_FORMAT_R32_FLOAT;
	if (!TraceColor.Matches(Color) || !TraceColor.IsValid())
	{
		bHasHistory = false; // 내부 크기가 바뀌면 이력을 버린다 (TAAU 규칙)
	}
	TraceColor.Ensure(*Rhi, Color, L"CloudTraceColor");
	TraceDepth.Ensure(*Rhi, Depth, L"CloudTraceDepth");
	HistoryColor[0].Ensure(*Rhi, Color, L"CloudHistoryColor0");
	HistoryColor[1].Ensure(*Rhi, Color, L"CloudHistoryColor1");
	HistoryDepth[0].Ensure(*Rhi, Depth, L"CloudHistoryDepth0");
	HistoryDepth[1].Ensure(*Rhi, Depth, L"CloudHistoryDepth1");
	FPersistentTextureDesc Cube;
	Cube.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	Cube.bCube  = true;
	Cube.Depth  = 6;
	Cube.Width = Cube.Height = CloudCubeSize;
	CloudCube.Ensure(*Rhi, Cube, L"CloudCube");
}

bool FVolumetricCloudRenderer::Prepare(FScene& Scene, const FSkyAtmosphereRenderer& Atmosphere, const FPrepareInputs& Inputs)
{
	ConstantsAddress = 0;
	CubeRef          = FRGResourceRef{};
	const FVolumetricCloudComponent* Component = nullptr;
	Scene.GetRegistry().View<FVolumetricCloudComponent>().Each([&](FEntity, FVolumetricCloudComponent& Cloud) {
		if (Component == nullptr)
		{
			Component = &Cloud;
		}
	});
	bActive = Component != nullptr && Atmosphere.IsActive() && RendererCVars::VolumetricClouds.Get() && Inputs.Camera != nullptr && Inputs.Width > 0;
	if (!bActive)
	{
		return false;
	}
	if (!bNoiseReady)
	{
		bNoiseReady = GenerateNoise();
		if (!bNoiseReady)
		{
			bActive = false;
			return false;
		}
	}
	const uint32 Divisor = Inputs.bAllowTemporal ? static_cast<uint32>(FMath::Clamp(RendererCVars::VolumetricCloudsDivisor.Get(), 1, 8)) : 1u;
	const uint32 TraceW  = FMath::Max((Inputs.Width + Divisor - 1) / Divisor, 1u);
	const uint32 TraceH  = FMath::Max((Inputs.Height + Divisor - 1) / Divisor, 1u);
	EnsureTargets(TraceW, TraceH);

	const FAtmosphereConstants& Atmo = Atmosphere.GetConstants();
	const float Seconds              = static_cast<float>(FFrameTime::GetTotalSeconds() - StartTime); // 앱 프레임 시간
	const float WindRadians          = FMath::DegreesToRadians(Component->WindDirection);
	const float WindKm               = FMath::Max(Component->WindSpeed, 0.0f) * Seconds * 0.001f;
	bWindMoving                      = Component->WindSpeed > 0.0f;
	bAffectEnvironment               = Component->bAffectEnvironmentLighting;

	Constants                     = FCloudConstants{};
	Constants.InvViewProjection   = Inputs.UnjitteredViewProjection.GetInverse();
	Constants.PrevViewProjection  = Inputs.PrevViewProjection;
	Constants.ViewProjection      = Inputs.UnjitteredViewProjection;
	Constants.CameraPositionWorld = Inputs.Camera->GetPosition();
	Constants.LayerBottomRadius   = Atmo.BottomRadius + FMath::Max(Component->LayerBottomAltitude, 0.0f);
	Constants.LayerTopRadius      = Constants.LayerBottomRadius + FMath::Max(Component->LayerThickness, 0.05f);
	Constants.WindOffset          = FVector3(FMath::Cos(WindRadians), FMath::Sin(WindRadians), 0.0f) * WindKm;
	Constants.Coverage            = FMath::Clamp(Component->Coverage, 0.0f, 1.0f);
	Constants.CloudType           = FMath::Clamp(Component->CloudType, 0.0f, 1.0f);
	Constants.Extinction          = FMath::Max(Component->Extinction, 0.0f);
	Constants.ShapeTile           = FMath::Max(Component->ShapeTileSize, 0.1f);
	Constants.DetailTile          = FMath::Max(Component->DetailTileSize, 0.01f);
	Constants.WeatherTile         = FMath::Max(Component->WeatherTileSize, 0.5f);
	Constants.DetailStrength      = FMath::Clamp(Component->DetailStrength, 0.0f, 1.0f);
	Constants.SilverLining        = FMath::Clamp(Component->SilverLining, 0.0f, 0.95f);
	Constants.Albedo              = Component->Albedo;
	Constants.AmbientScale        = FMath::Max(Component->AmbientScale, 0.0f);
	// 빛: 태양이 지평선 위면 태양, 아래면 달 (달빛이 있을 때)
	const FVector3 Up         = Atmo.CameraPosition.GetNormalized();
	const bool     bMoon      = FVector3::Dot(Atmo.SunDirection, Up) < -0.02f && Atmo.MoonIlluminance.LengthSquared() > 0.0f;
	Constants.LightDirection  = bMoon ? Atmo.MoonDirection : Atmo.SunDirection;
	Constants.LightIlluminance = bMoon ? Atmo.MoonIlluminance : Atmo.SunIlluminance;
	Constants.CirrusCoverage  = FMath::Clamp(Component->CirrusCoverage, 0.0f, 1.0f);
	Constants.CirrusRadius    = Atmo.BottomRadius + FMath::Max(Component->CirrusAltitude, Component->LayerBottomAltitude + Component->LayerThickness + 0.1f);
	Constants.TraceSize       = FVector2(static_cast<float>(TraceW), static_cast<float>(TraceH));
	Constants.InvTraceSize    = FVector2(1.0f / static_cast<float>(TraceW), 1.0f / static_cast<float>(TraceH));
	Constants.MaxSteps        = static_cast<uint32>(FMath::Clamp(RendererCVars::VolumetricCloudsSteps.Get(), 8, 256));
	Constants.FrameIndex      = FrameCounter++;
	const bool bTemporal      = Inputs.bAllowTemporal && RendererCVars::VolumetricCloudsTemporal.Get();
	const bool bHistory       = bTemporal && Inputs.bHistoryValid && bHasHistory;
	Constants.Jitter          = bTemporal ? FVector2(FTemporalMath::Halton(Constants.FrameIndex % 16 + 1, 2), FTemporalMath::Halton(Constants.FrameIndex % 16 + 1, 3))
	                                      : FVector2(0.5f, 0.5f);
	Constants.HistoryWeight   = bTemporal ? 0.9f : 0.0f;
	Constants.bHistoryValid   = bHistory ? 1u : 0u;
	Constants.MaxDistance     = 60.0f;
	Constants.CubeSize        = static_cast<float>(CloudCubeSize);
	ConstantsAddress          = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;
	return true;
}

void FVolumetricCloudRenderer::BindCompute(ID3D12GraphicsCommandList* List, D3D12_GPU_VIRTUAL_ADDRESS Address, const FSkyAtmosphereRenderer& Atmosphere,
                                           D3D12_GPU_VIRTUAL_ADDRESS FogConstants) const
{
	List->SetComputeRootSignature(RootSignature.Get());
	List->SetComputeRootConstantBufferView(CloudRoot_Constants, Address);
	List->SetComputeRootConstantBufferView(CloudRoot_Atmosphere, Atmosphere.GetConstantsAddress());
	List->SetComputeRootConstantBufferView(CloudRoot_Fog, FogConstants);
}

void FVolumetricCloudRenderer::AddPasses(FRenderGraph& Graph, const FSkyAtmosphereRenderer& Atmosphere, D3D12_GPU_VIRTUAL_ADDRESS FogConstants, ERGQueue Queue,
                                         int32 Timer)
{
	if (!bActive)
	{
		return;
	}
	const FRGResourceRef TransmittanceRef = Atmosphere.GetTransmittanceRef();
	const FRGResourceRef SkyViewRef       = Atmosphere.GetSkyViewRef();
	const FRGResourceRef ShapeRef         = ShapeNoise.Import(Graph, "CloudShapeNoise");
	const FRGResourceRef DetailRef        = DetailNoise.Import(Graph, "CloudDetailNoise");
	const FRGResourceRef WeatherRef       = WeatherMap.Import(Graph, "CloudWeatherMap");
	const FRGResourceRef TraceColorRef    = TraceColor.Import(Graph, "CloudTraceColor");
	const FRGResourceRef TraceDepthRef    = TraceDepth.Import(Graph, "CloudTraceDepth");
	const uint32         Current          = HistoryIndex;
	const uint32         Previous         = HistoryIndex ^ 1u;
	const FRGResourceRef HistoryColorRef  = HistoryColor[Current].Import(Graph, Current == 0 ? "CloudHistoryColor0" : "CloudHistoryColor1");
	const FRGResourceRef HistoryDepthRef  = HistoryDepth[Current].Import(Graph, Current == 0 ? "CloudHistoryDepth0" : "CloudHistoryDepth1");
	const FRGResourceRef PrevColorRef     = HistoryColor[Previous].Import(Graph, Previous == 0 ? "CloudHistoryColor0" : "CloudHistoryColor1");
	const FRGResourceRef PrevDepthRef     = HistoryDepth[Previous].Import(Graph, Previous == 0 ? "CloudHistoryDepth0" : "CloudHistoryDepth1");
	const D3D12_GPU_VIRTUAL_ADDRESS Address = ConstantsAddress;
	const uint32 TraceW = static_cast<uint32>(Constants.TraceSize.X);
	const uint32 TraceH = static_cast<uint32>(Constants.TraceSize.Y);
	const FSkyAtmosphereRenderer* AtmospherePtr = &Atmosphere;

	const auto DeclareLookups = [&](FRenderGraph::FPassBuilder& Pass) {
		Pass.Read(TransmittanceRef, ERGAccess::SrvNonPixel)
			.Read(SkyViewRef, ERGAccess::SrvNonPixel)
			.Read(ShapeRef, ERGAccess::SrvNonPixel)
			.Read(DetailRef, ERGAccess::SrvNonPixel)
			.Read(WeatherRef, ERGAccess::SrvNonPixel);
	};
	const auto BindLookups = [this, AtmospherePtr](ID3D12GraphicsCommandList* List) {
		List->SetComputeRootDescriptorTable(CloudRoot_Transmittance, AtmospherePtr->GetTransmittanceSrv().Gpu);
		List->SetComputeRootDescriptorTable(CloudRoot_SkyView, AtmospherePtr->GetSkyViewSrv().Gpu);
		List->SetComputeRootDescriptorTable(CloudRoot_Shape, ShapeNoise.Srv.Gpu);
		List->SetComputeRootDescriptorTable(CloudRoot_Detail, DetailNoise.Srv.Gpu);
		List->SetComputeRootDescriptorTable(CloudRoot_Weather, WeatherMap.Srv.Gpu);
	};

	FRenderGraph::FPassBuilder Trace = Graph.AddPass("구름 추적", Queue);
	DeclareLookups(Trace);
	Trace.Write(TraceColorRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
		.Write(TraceDepthRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, AtmospherePtr, FogConstants, Address, BindLookups, TraceW, TraceH](FRGContext& Context) {
			ID3D12GraphicsCommandList* List = Context.CommandList;
			BindCompute(List, Address, *AtmospherePtr, FogConstants);
			BindLookups(List);
			List->SetComputeRootDescriptorTable(CloudRoot_OutColor, TraceColor.Uavs[0].Gpu);
			List->SetComputeRootDescriptorTable(CloudRoot_OutDepth, TraceDepth.Uavs[0].Gpu);
			List->SetPipelineState(TracePipeline.Get());
			List->Dispatch((TraceW + 7) / 8, (TraceH + 7) / 8, 1);
		});

	Graph.AddPass("구름 누적", Queue)
		.Read(TraceColorRef, ERGAccess::SrvNonPixel)
		.Read(TraceDepthRef, ERGAccess::SrvNonPixel)
		.Read(PrevColorRef, ERGAccess::SrvNonPixel)
		.Read(PrevDepthRef, ERGAccess::SrvNonPixel)
		.Write(HistoryColorRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
		.Write(HistoryDepthRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, AtmospherePtr, FogConstants, Address, Current, Previous, TraceW, TraceH](FRGContext& Context) {
			ID3D12GraphicsCommandList* List = Context.CommandList;
			BindCompute(List, Address, *AtmospherePtr, FogConstants);
			List->SetComputeRootDescriptorTable(CloudRoot_CurrentColor, TraceColor.Srv.Gpu);
			List->SetComputeRootDescriptorTable(CloudRoot_CurrentDepth, TraceDepth.Srv.Gpu);
			List->SetComputeRootDescriptorTable(CloudRoot_HistoryColor, HistoryColor[Previous].Srv.Gpu);
			List->SetComputeRootDescriptorTable(CloudRoot_HistoryDepth, HistoryDepth[Previous].Srv.Gpu);
			List->SetComputeRootDescriptorTable(CloudRoot_OutColor, HistoryColor[Current].Uavs[0].Gpu);
			List->SetComputeRootDescriptorTable(CloudRoot_OutDepth, HistoryDepth[Current].Uavs[0].Gpu);
			List->SetPipelineState(ResolvePipeline.Get());
			List->Dispatch((TraceW + 7) / 8, (TraceH + 7) / 8, 1);
		});
	bHasHistory  = Constants.HistoryWeight > 0.0f;
	WrittenIndex = Current;
	HistoryIndex ^= 1u; // 다음 렌더는 다른 칸에 쓴다 (이번 칸이 그때의 이전 이력)

	if (bAffectEnvironment)
	{
		CubeRef                    = CloudCube.Import(Graph, "CloudCube");
		FRenderGraph::FPassBuilder Cube = Graph.AddPass("구름 IBL 큐브", Queue);
		DeclareLookups(Cube);
		Cube.Write(CubeRef, ERGAccess::Uav, FRGSubresourceRange::All(), true)
			.Timer(Timer)
			.Execute([this, AtmospherePtr, FogConstants, Address, BindLookups](FRGContext& Context) {
				ID3D12GraphicsCommandList* List = Context.CommandList;
				BindCompute(List, Address, *AtmospherePtr, FogConstants);
				BindLookups(List);
				List->SetComputeRootDescriptorTable(CloudRoot_OutCube, CloudCube.Uavs[0].Gpu);
				List->SetPipelineState(CubePipeline.Get());
				List->Dispatch((CloudCubeSize + 7) / 8, (CloudCubeSize + 7) / 8, 6);
			});
	}
}

void FVolumetricCloudRenderer::AddCompositePass(FRenderGraph& Graph, const FD3D12RenderTarget& SceneColor, FRGResourceRef ColorRef, FRGResourceRef DepthRef,
                                                int32 Timer)
{
	if (!bActive)
	{
		return;
	}
	const uint32         Current         = WrittenIndex;
	const FRGResourceRef HistoryColorRef = Graph.FindImported(HistoryColor[Current].Resource.Get());
	const FRGResourceRef HistoryDepthRef = Graph.FindImported(HistoryDepth[Current].Resource.Get());
	const D3D12_GPU_VIRTUAL_ADDRESS Address = ConstantsAddress;
	if (!HistoryColorRef.IsValid() || !HistoryDepthRef.IsValid())
	{
		return;
	}
	Graph.AddPass("구름 합성")
		.Read(HistoryColorRef, ERGAccess::SrvPixel)
		.Read(HistoryDepthRef, ERGAccess::SrvPixel)
		.Read(DepthRef, ERGAccess::SrvPixel)
		.Write(ColorRef, ERGAccess::RenderTarget)
		.Timer(Timer)
		.Execute([this, &SceneColor, Address, Current](FRGContext& Context) {
			ID3D12GraphicsCommandList* List = Context.CommandList;
			SceneColor.Bind(List, nullptr, false, false);
			List->SetGraphicsRootSignature(RootSignature.Get());
			List->SetGraphicsRootConstantBufferView(CloudRoot_Constants, Address);
			List->SetGraphicsRootDescriptorTable(CloudRoot_HistoryColor, HistoryColor[Current].Srv.Gpu);
			List->SetGraphicsRootDescriptorTable(CloudRoot_HistoryDepth, HistoryDepth[Current].Srv.Gpu);
			List->SetGraphicsRootDescriptorTable(CloudRoot_SceneDepth, SceneColor.GetDepthSrv().Gpu);
			List->SetPipelineState(CompositePipeline.Get());
			List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			List->DrawInstanced(3, 1, 0, 0);
		});
}
