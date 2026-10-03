#include "Renderer/DdgiRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/RayTracingEffects.h"
#include "Renderer/RayTracingScene.h"
#include "Renderer/ScreenPass.h"
#include "Scene/Components.h"
#include "Scene/IrradianceVolume.h"
#include "Scene/Scene.h"

#include <algorithm>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// 프로브 구 표시 (DdgiProbes.hlsl): 16 x 8 사각형 × 6 정점
	constexpr uint32 ProbeSphereVertices = 16 * 8 * 6;

	struct FFrameVolume
	{
		const FIrradianceVolumeComponent* Component = nullptr;
		DdgiMath::FProbeGrid              Grid;
		float                             Volume = 0.0f; // 상자 부피 (같은 우선순위면 작은 상자 먼저)
	};
} // namespace

FDdgiRenderer::~FDdgiRenderer()
{
	Shutdown();
}

bool FDdgiRenderer::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary, const FScreenPassRootSignature& InScreenRoot, const FRayTracingEffects& InRayTracing)
{
	Rhi        = &InRhi;
	Library    = &InLibrary;
	AtlasPool.Init(InRhi);
	ScreenRoot = &InScreenRoot;
	RayTracing = &InRayTracing;
	bSupported = false;

	// 비활성 렌더에서도 메시 패스 t40~t42는 묶이므로 1x1 기본 텍스처
	const auto MakeDummy = [&](std::unique_ptr<FD3D12RenderTarget>& Target, DXGI_FORMAT Format, const wchar_t* Name) {
		Target = std::make_unique<FD3D12RenderTarget>();
		return Target->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), 1, 1, Name, FRenderTargetDesc::MakeColor(Format));
	};
	if (!MakeDummy(DummyIrradiance, IrradianceFormat, L"DdgiDummyIrradiance") || !MakeDummy(DummyDistance, DistanceFormat, L"DdgiDummyDistance") ||
	    !MakeDummy(DummyProbeData, ProbeDataFormat, L"DdgiDummyProbeData"))
	{
		return false;
	}
	if (!RayTracing->IsSupported())
	{
		return true; // RT 없음: 기능 꺼짐 (메시는 예전 하늘 IBL 식)
	}
	if (!CreatePipelines(false))
	{
		E_LOG(LogRenderer, Error, "DDGI 파이프라인 생성 실패 — 동적 GI를 끕니다");
		return true;
	}
	bSupported = true;
	return true;
}

void FDdgiRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	ReleaseVariants(true);
	ReleaseAtlases();
	AtlasPool.Shutdown();
	DummyIrradiance.reset();
	DummyDistance.reset();
	DummyProbeData.reset();
	for (FD3D12PipelineState* Pipeline : { &TracePipeline, &IrradiancePipeline, &DistancePipeline, &ProbeDataPipeline, &ProbeDebugPipeline })
	{
		Pipeline->Shutdown();
	}
	Rhi = nullptr;
}

bool FDdgiRenderer::CreateTracePipeline(FD3D12PipelineState& Out, bool bForceRecompile, const FRayTracingGraphVariant* Variant)
{
	const auto Load = [&](const wchar_t* Entry, EShaderStage Stage) {
		FShaderCompileDesc Desc;
		Desc.FileName    = L"DdgiTrace.hlsl";
		Desc.EntryPoint  = Entry;
		Desc.Stage       = Stage;
		Desc.ShaderModel = L"6_5"; // 인라인 RayQuery
		if (Variant != nullptr && Stage == EShaderStage::Pixel)
		{
			// 그래프 머티리얼 히트 (FRayTracingEffects와 같은 변형 규칙)
			Desc.Defines.push_back(L"E_RT_GRAPH_MATERIALS");
			Desc.VirtualFiles.push_back({ L"RayTracingGraphMaterials.generated.hlsli", Variant->Source });
		}
		if (bForceRecompile && !Library->CookShader(Desc))
		{
			return ComPtr<IDxcBlob>();
		}
		return Library->GetShader(Desc);
	};
	const ComPtr<IDxcBlob> VertexShader = Load(L"VSMain", EShaderStage::Vertex);
	const ComPtr<IDxcBlob> PixelShader  = Load(L"PSTrace", EShaderStage::Pixel);
	if (!VertexShader || !PixelShader)
	{
		return false;
	}
	FGraphicsPipelineDesc Desc;
	Desc.RootSignature          = RayTracing->GetRoot().Get();
	Desc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	Desc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	Desc.CullMode               = D3D12_CULL_MODE_NONE;
	Desc.bDepthEnable           = false;
	Desc.NumRenderTargets       = 1;
	Desc.RenderTargetFormats[0] = RayDataFormat;
	return Out.InitGraphics(Rhi->GetDevice().GetDevice(), Desc, Variant != nullptr ? L"DdgiTraceGraph" : L"DdgiTrace");
}

bool FDdgiRenderer::CreatePipelines(bool bForceRecompile)
{
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	FD3D12PipelineState NewTrace, NewIrradiance, NewDistance, NewProbeData, NewDebug;
	if (!CreateTracePipeline(NewTrace, bForceRecompile, nullptr) ||
	    !ScreenRoot->CreateComputePipeline(NewIrradiance, Device, *Library, L"DdgiBlend.hlsl", L"CSIrradiance", bForceRecompile, L"DdgiBlendIrradiance") ||
	    !ScreenRoot->CreateComputePipeline(NewDistance, Device, *Library, L"DdgiBlend.hlsl", L"CSDistance", bForceRecompile, L"DdgiBlendDistance") ||
	    !ScreenRoot->CreateComputePipeline(NewProbeData, Device, *Library, L"DdgiBlend.hlsl", L"CSProbeData", bForceRecompile, L"DdgiProbeData"))
	{
		return false;
	}
	// 프로브 구 (씬 컬러 + 깊이 테스트·쓰기)
	{
		const auto Load = [&](const wchar_t* Entry, EShaderStage Stage) {
			FShaderCompileDesc Desc;
			Desc.FileName   = L"DdgiProbes.hlsl";
			Desc.EntryPoint = Entry;
			Desc.Stage      = Stage;
			if (bForceRecompile && !Library->CookShader(Desc))
			{
				return ComPtr<IDxcBlob>();
			}
			return Library->GetShader(Desc);
		};
		const ComPtr<IDxcBlob> VertexShader = Load(L"VSProbe", EShaderStage::Vertex);
		const ComPtr<IDxcBlob> PixelShader  = Load(L"PSProbe", EShaderStage::Pixel);
		if (!VertexShader || !PixelShader)
		{
			return false;
		}
		FGraphicsPipelineDesc Desc;
		Desc.RootSignature          = ScreenRoot->Get();
		Desc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
		Desc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
		Desc.CullMode               = D3D12_CULL_MODE_NONE;
		Desc.NumRenderTargets       = 1;
		Desc.RenderTargetFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // 씬 컬러
		Desc.DepthStencilFormat     = FD3D12RHI::DepthBufferFormat;
		Desc.bDepthEnable           = true;
		Desc.bDepthWrite            = true;
		Desc.DepthFunc              = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		if (!NewDebug.InitGraphics(Device, Desc, L"DdgiProbeDebug"))
		{
			return false;
		}
	}
	for (auto [Current, Next] : { std::pair{ &TracePipeline, &NewTrace }, std::pair{ &IrradiancePipeline, &NewIrradiance },
	                              std::pair{ &DistancePipeline, &NewDistance }, std::pair{ &ProbeDataPipeline, &NewProbeData },
	                              std::pair{ &ProbeDebugPipeline, &NewDebug } })
	{
		Current->Swap(*Next);
		if (Next->Get() != nullptr)
		{
			Rhi->DeferRelease(Next->Detach());
		}
	}
	return true;
}

bool FDdgiRenderer::ReloadShaders(bool bForceRecompile)
{
	if (!bSupported)
	{
		return true;
	}
	ReleaseVariants(true);
	if (!CreatePipelines(bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "DDGI 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	return true;
}

void FDdgiRenderer::ReleaseVariants(bool bAll)
{
	const uint64 FrameNumber = Rhi->GetFrameNumber();
	for (auto It = Variants.begin(); It != Variants.end();)
	{
		if (bAll || It->second->LastUsedFrame + 600 < FrameNumber)
		{
			if (It->second->Trace.Get() != nullptr)
			{
				Rhi->DeferRelease(It->second->Trace.Detach());
			}
			It = Variants.erase(It);
		}
		else
		{
			++It;
		}
	}
}

ID3D12PipelineState* FDdgiRenderer::SelectTracePipeline(const FRayTracingScene& Scene)
{
	const FRayTracingGraphVariant& Variant = Scene.GetGraphVariant();
	if (Variant.Key == 0)
	{
		return TracePipeline.Get();
	}
	if (Variants.find(Variant.Key) == Variants.end())
	{
		ReleaseVariants(false);
	}
	std::unique_ptr<FVariant>& Entry = Variants[Variant.Key];
	if (!Entry)
	{
		Entry          = std::make_unique<FVariant>();
		Entry->bFailed = !CreateTracePipeline(Entry->Trace, false, &Variant);
		if (Entry->bFailed)
		{
			E_LOG(LogRenderer, Warning, "DDGI 그래프 머티리얼 변형 컴파일 실패 (키 {:016x}) — 그래프 머티리얼 히트는 회색 근사", Variant.Key);
		}
	}
	Entry->LastUsedFrame = Rhi->GetFrameNumber();
	return Entry->bFailed ? TracePipeline.Get() : Entry->Trace.Get();
}

void FDdgiRenderer::ReleaseAtlases()
{
	if (Irradiance[0] != nullptr)
	{
		AtlasPool.Shutdown(); // 지연 해제 (마지막 사용 프레임이 아직 GPU에 있을 수 있다)
		AtlasPool.Init(*Rhi);
	}
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		Irradiance[Index] = Distance[Index] = ProbeData[Index] = nullptr;
	}
	AtlasProbes   = 0;
	bHistoryValid = false;
}

void FDdgiRenderer::EnsureAtlases(uint32 TotalProbes)
{
	using namespace DdgiMath;
	const uint32 Rows     = GetAtlasRows(TotalProbes);
	const uint32 Capacity = Rows * AtlasTilesPerRow;
	if (Irradiance[0] && AtlasProbes == Capacity)
	{
		return;
	}
	ReleaseAtlases();
	const uint32 Columns = AtlasTilesPerRow; // 행 단위로 만든다 (프로브 수가 조금 바뀌어도 같은 크기)
	const auto MakeDesc = [](uint32 Width, uint32 Height, DXGI_FORMAT Format) {
		FRGTextureDesc Desc;
		Desc.Width            = Width;
		Desc.Height           = Height;
		Desc.Format           = Format;
		Desc.bUnorderedAccess = true;
		return Desc;
	};
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		Irradiance[Index] = AtlasPool.Acquire(MakeDesc(Columns * IrradianceTileSize, Rows * IrradianceTileSize, IrradianceFormat),
		                                      Index == 0 ? "DdgiIrradiance0" : "DdgiIrradiance1");
		Distance[Index]   = AtlasPool.Acquire(MakeDesc(Columns * DistanceTileSize, Rows * DistanceTileSize, DistanceFormat), Index == 0 ? "DdgiDistance0" : "DdgiDistance1");
		ProbeData[Index]  = AtlasPool.Acquire(MakeDesc(Columns, Rows, ProbeDataFormat), Index == 0 ? "DdgiProbeData0" : "DdgiProbeData1");
		if (Irradiance[Index] == nullptr || Distance[Index] == nullptr || ProbeData[Index] == nullptr)
		{
			E_LOG(LogRenderer, Fatal, "DDGI 아틀라스 생성 실패 (프로브 {})", TotalProbes);
		}
	}
	AtlasProbes   = Capacity;
	bHistoryValid = false;
	E_LOG(LogRenderer, Log, "DDGI 아틀라스: 프로브 {} (행 {}), 조도 {}x{}, 거리 {}x{}", TotalProbes, Rows, Columns * IrradianceTileSize, Rows * IrradianceTileSize,
	      Columns * DistanceTileSize, Rows * DistanceTileSize);
}

bool FDdgiRenderer::SceneHasVolumes(FScene& Scene)
{
	bool bFound = false;
	Scene.GetRegistry().View<FIrradianceVolumeComponent>().Each([&](FEntity, FIrradianceVolumeComponent&) { bFound = true; });
	return bFound;
}

bool FDdgiRenderer::Prepare(FScene& Scene, const FDdgiSettings& Settings, bool bAllowUpdate, const FMatrix4x4& DebugViewProjection,
                            const FVector3& CameraPosition)
{
	using namespace DdgiMath;
	FrameSettings   = Settings;
	bFrameActive    = false;
	bAnyDebugProbes = false;
	Stats           = FDdgiStats{};
	Constants       = FDdgiConstants{};
	Constants.DebugView           = Settings.bDebugView ? 1u : 0u;
	Constants.DebugViewProjection = DebugViewProjection;
	Constants.DebugCameraPosition = CameraPosition;
	Constants.BounceIntensity     = Settings.BounceIntensity;

	// 볼륨 수집 (우선순위 큰 것 → 작은 상자 먼저, 최대 MaxVolumes, 전체 프로브 MaxTotalProbes)
	std::vector<FFrameVolume> Volumes;
	if (bSupported && bAllowUpdate)
	{
		Scene.GetRegistry().View<FTransformComponent, FIrradianceVolumeComponent>().Each(
			[&](FEntity, FTransformComponent& Transform, FIrradianceVolumeComponent& Component) {
				FFrameVolume Volume;
				Volume.Component = &Component;
				Volume.Grid      = MakeGrid(Transform.GetWorldPosition(), Component.HalfExtents, Component.Spacing);
				Volume.Volume    = Component.HalfExtents.X * Component.HalfExtents.Y * Component.HalfExtents.Z;
				Volumes.push_back(Volume);
			});
		std::stable_sort(Volumes.begin(), Volumes.end(), [](const FFrameVolume& A, const FFrameVolume& B) {
			if (A.Component->Priority != B.Component->Priority)
			{
				return A.Component->Priority > B.Component->Priority;
			}
			return A.Volume < B.Volume;
		});
	}
	uint32 TotalProbes = 0;
	uint32 Kept        = 0;
	for (const FFrameVolume& Volume : Volumes)
	{
		if (Kept >= MaxVolumes || TotalProbes + Volume.Grid.GetProbeCount() > MaxTotalProbes)
		{
			break; // 넘는 볼륨은 이번 프레임 무시 (우선순위 순)
		}
		TotalProbes += Volume.Grid.GetProbeCount();
		++Kept;
	}
	Volumes.resize(Kept);

	if (Volumes.empty())
	{
		LayoutKey.clear();
		ShadingConstants = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;
		return false;
	}

	// 배치 키: 볼륨 순서별 격자 수 — 바뀌면 이력·커서 초기화
	std::vector<uint32> NewKey;
	NewKey.reserve(Volumes.size() * 3);
	for (const FFrameVolume& Volume : Volumes)
	{
		NewKey.insert(NewKey.end(), { Volume.Grid.Counts[0], Volume.Grid.Counts[1], Volume.Grid.Counts[2] });
	}
	EnsureAtlases(TotalProbes);
	if (NewKey != LayoutKey)
	{
		LayoutKey     = NewKey;
		bHistoryValid = false;
		Cursors.assign(Volumes.size(), 0u);
	}
	// 같은 Rhi 프레임에 이 렌더러가 다시 갱신하지 않는다 (한 뷰만 — RT가 허용된 렌더는 원래 한 뷰)
	const uint64 FrameNumber = Rhi->GetFrameNumber();
	if (LastUpdateFrame == FrameNumber)
	{
		ShadingConstants = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;
		return false;
	}
	LastUpdateFrame = FrameNumber;

	// 조명 변화 가속 (방향광·하늘이 기준에서 2도 / 10% 넘게 바뀌면 기준을 옮기고 BoostFrames 동안 빠르게)
	if (!bHasLightReference ||
	    ComputeLightChange(ReferenceLightDirection, ReferenceLightRadiance, ReferenceAmbient, Settings.LightDirection, Settings.LightRadiance,
	                       Settings.AmbientIntensity) > 1.0f)
	{
		if (bHasLightReference)
		{
			BoostFramesLeft = Settings.BoostFrames;
		}
		bHasLightReference      = true;
		ReferenceLightDirection = Settings.LightDirection;
		ReferenceLightRadiance  = Settings.LightRadiance;
		ReferenceAmbient        = Settings.AmbientIntensity;
	}
	const bool bBoost = BoostFramesLeft > 0;
	BoostFramesLeft   = BoostFramesLeft > 0 ? BoostFramesLeft - 1 : 0;
	Stats.bLightBoost = bBoost;

	// 이력 핑퐁: 지난 프레임 쓴 장이 이번 이전 장
	ReadIndex  = WriteIndex;
	WriteIndex = ReadIndex ^ 1u;

	const FRotation Rotation = MakeRayRotation(RotationFrame++);
	for (uint32 Row = 0; Row < 3; ++Row)
	{
		Constants.RayRotation[Row] = FVector4(Rotation.Rows[Row].X, Rotation.Rows[Row].Y, Rotation.Rows[Row].Z, 0.0f);
	}
	Constants.VolumeCount = static_cast<uint32>(Volumes.size());
	Constants.Reset       = bHistoryValid ? 0u : 1u;
	Constants.TotalProbes = TotalProbes;
	Constants.IrradianceTexelSize = FVector2(1.0f / static_cast<float>(Irradiance[0]->Desc.Width), 1.0f / static_cast<float>(Irradiance[0]->Desc.Height));
	Constants.DistanceTexelSize   = FVector2(1.0f / static_cast<float>(Distance[0]->Desc.Width), 1.0f / static_cast<float>(Distance[0]->Desc.Height));

	uint32 ProbeOffset = 0;
	uint32 RowOffset   = 0;
	TraceWidth         = 0;
	for (uint32 Index = 0; Index < static_cast<uint32>(Volumes.size()); ++Index)
	{
		const FFrameVolume&               Volume    = Volumes[Index];
		const FIrradianceVolumeComponent& Component = *Volume.Component;
		const uint32                      Count     = Volume.Grid.GetProbeCount();
		FDdgiVolumeGpu&                   Gpu       = Constants.Volumes[Index];
		Gpu.Origin         = Volume.Grid.Origin;
		Gpu.Spacing        = Volume.Grid.Spacing;
		Gpu.Counts[0]      = Volume.Grid.Counts[0];
		Gpu.Counts[1]      = Volume.Grid.Counts[1];
		Gpu.Counts[2]      = Volume.Grid.Counts[2];
		Gpu.ProbeOffset    = ProbeOffset;
		Gpu.Intensity      = std::max(Component.Intensity, 0.0f);
		Gpu.NormalBias     = std::max(Component.NormalBias, 0.0f);
		Gpu.ViewBias       = std::max(Component.ViewBias, 0.0f);
		Gpu.FadeDistance   = std::max(Component.FadeDistance, 1.0f);
		Gpu.DistanceClamp  = Volume.Grid.GetDistanceClamp();
		Gpu.MaxRayDistance = std::max(Component.MaxRayDistance, 1.0f);
		Gpu.RaysPerProbe   = static_cast<uint32>(std::clamp(Component.RaysPerProbe, static_cast<int32>(MinRaysPerProbe), static_cast<int32>(MaxRaysPerProbe)));
		Gpu.Flags          = (Component.bRelocation ? 1u : 0u) | (Component.bClassification ? 2u : 0u);
		Gpu.FixedRays      = Gpu.Flags != 0 ? FixedRayCount : 0u;
		Gpu.Hysteresis     = std::clamp(Component.Hysteresis, 0.0f, 0.995f);
		if (bBoost)
		{
			Gpu.Hysteresis = std::min(Gpu.Hysteresis, std::clamp(Settings.BoostHysteresis, 0.0f, 0.995f));
		}
		Gpu.ChangeThreshold      = std::max(Settings.ChangeThreshold, 0.0f);
		Gpu.MinFrontfaceDistance = 0.3f * Volume.Grid.GetMinSpacing();
		Gpu.BackfaceThreshold    = 0.25f;
		Gpu.DebugProbes          = static_cast<uint32>(std::clamp(Settings.ShowProbes >= 0 ? Settings.ShowProbes : Component.DebugProbes, 0, 3));
		Gpu.DebugRadius          = std::max(Component.DebugProbeRadius, 0.5f);
		bAnyDebugProbes |= Gpu.DebugProbes != 0;

		// 갱신 일정: 볼륨 커서부터 순환
		const uint32 UpdateCount =
			ComputeVolumeUpdateCount(Count, static_cast<uint32>(std::max(Component.ProbeUpdateBudget, 0)), TotalProbes, Settings.ProbeBudget);
		Gpu.UpdateStart = Cursors[Index] % Count;
		Gpu.UpdateCount = UpdateCount;
		Gpu.RowOffset   = RowOffset;
		Cursors[Index]  = (Gpu.UpdateStart + UpdateCount) % Count;

		ProbeOffset += Count;
		RowOffset += UpdateCount;
		TraceWidth = std::max(TraceWidth, Gpu.RaysPerProbe);
		Stats.Rays += UpdateCount * Gpu.RaysPerProbe;
	}
	TraceRows = RowOffset;

	ShadingConstants = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;
	bFrameActive     = true;
	bHistoryValid    = true; // 이번 프레임 누적이 끝나면 다음 프레임의 이전 장은 유효

	Stats.bActive          = true;
	Stats.Volumes          = Constants.VolumeCount;
	Stats.Probes           = TotalProbes;
	Stats.UpdatedProbes    = TraceRows;
	Stats.IrradianceWidth  = Irradiance[0]->Desc.Width;
	Stats.IrradianceHeight = Irradiance[0]->Desc.Height;
	Stats.DistanceWidth    = Distance[0]->Desc.Width;
	Stats.DistanceHeight   = Distance[0]->Desc.Height;
	Stats.AtlasBytes       = AtlasPool.GetTotalBytes();
	return true;
}

const FD3D12DescriptorHandle& FDdgiRenderer::GetIrradianceSrv() const
{
	return bFrameActive ? Irradiance[WriteIndex]->Srv : DummyIrradiance->GetSrv();
}

const FD3D12DescriptorHandle& FDdgiRenderer::GetDistanceSrv() const
{
	return bFrameActive ? Distance[WriteIndex]->Srv : DummyDistance->GetSrv();
}

const FD3D12DescriptorHandle& FDdgiRenderer::GetProbeDataSrv() const
{
	return bFrameActive ? ProbeData[WriteIndex]->Srv : DummyProbeData->GetSrv();
}

void FDdgiRenderer::ImportFrame(FRenderGraph& Graph)
{
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		IrradianceRefs[Index] = {};
		DistanceRefs[Index]   = {};
		ProbeDataRefs[Index]  = {};
	}
	if (!bFrameActive)
	{
		IrradianceRefs[WriteIndex] = Graph.ImportColor("DdgiDummyIrradiance", *DummyIrradiance);
		DistanceRefs[WriteIndex]   = Graph.ImportColor("DdgiDummyDistance", *DummyDistance);
		ProbeDataRefs[WriteIndex]  = Graph.ImportColor("DdgiDummyProbeData", *DummyProbeData);
		return;
	}
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		// 평소 상태 = PIXEL_SHADER_RESOURCE (메시 패스), 누적 패스만 UAV
		IrradianceRefs[Index] = Graph.Import(Index == 0 ? "DdgiIrradiance0" : "DdgiIrradiance1", Irradiance[Index]->Resource.Get(), ERGAccess::SrvPixel,
		                                     ERGAccess::SrvPixel);
		DistanceRefs[Index]   = Graph.Import(Index == 0 ? "DdgiDistance0" : "DdgiDistance1", Distance[Index]->Resource.Get(), ERGAccess::SrvPixel,
		                                     ERGAccess::SrvPixel);
		ProbeDataRefs[Index]  = Graph.Import(Index == 0 ? "DdgiProbeData0" : "DdgiProbeData1", ProbeData[Index]->Resource.Get(), ERGAccess::SrvPixel,
		                                     ERGAccess::SrvPixel);
	}
}

void FDdgiRenderer::DeclareShadingReads(FRenderGraph::FPassBuilder& Pass) const
{
	for (const FRGResourceRef& Ref : { IrradianceRefs[WriteIndex], DistanceRefs[WriteIndex], ProbeDataRefs[WriteIndex] })
	{
		if (Ref.IsValid())
		{
			Pass.Read(Ref, ERGAccess::SrvPixel);
		}
	}
}

void FDdgiRenderer::AddUpdatePasses(FRenderGraph& Graph, const FRayTracingScene& Scene, FRGResourceRef Tlas, const FRayTracingLightingInputs& Lighting,
                                    int32 TraceTimer, int32 BlendTimer, ERGQueue BlendQueue)
{
	if (!bFrameActive || TraceRows == 0 || TraceWidth == 0)
	{
		return;
	}
	const uint32 Read  = ReadIndex;
	const uint32 Write = WriteIndex;
	const D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress = ShadingConstants;
	const D3D12_GPU_VIRTUAL_ADDRESS LightingAddress  = RayTracing->UploadLighting(Lighting, FrameSettings.MaxHitLocalLights, FrameSettings.bHitShadows);
	const FD3D12DescriptorHandle    PrevIrradiance   = Irradiance[Read]->Srv;
	const FD3D12DescriptorHandle    PrevDistance     = Distance[Read]->Srv;
	const FD3D12DescriptorHandle    PrevProbeData    = ProbeData[Read]->Srv;

	// 1) 프로브 광선 추적 → "광선 × 갱신 프로브" (RGBA32F)
	const FRGResourceRef    RayRef  = Graph.CreateTexture("DdgiRayData", FRGTextureDesc::MakeRenderTarget(TraceWidth, TraceRows, RayDataFormat));
	const FRGPooledTexture* RayData = Graph.GetTexture(RayRef);
	{
		ID3D12PipelineState* const Pipeline = SelectTracePipeline(Scene);
		const uint32               Width    = TraceWidth;
		const uint32               Height   = TraceRows;
		FRenderGraph::FPassBuilder Pass     = Graph.AddPass("DDGI 프로브 추적");
		Scene.DeclareTraceReads(Pass, Tlas);
		Pass.Read(IrradianceRefs[Read], ERGAccess::SrvPixel)
			.Read(DistanceRefs[Read], ERGAccess::SrvPixel)
			.Read(ProbeDataRefs[Read], ERGAccess::SrvPixel)
			.Write(RayRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
			.Timer(TraceTimer)
			.Execute([this, &Scene, Lighting, RayData, Pipeline, ConstantsAddress, LightingAddress, PrevIrradiance, PrevDistance, PrevProbeData, Width,
		              Height](FRGContext& Context) {
				ID3D12GraphicsCommandList*        CommandList = Context.CommandList;
				const D3D12_CPU_DESCRIPTOR_HANDLE Rtv         = RayData->GetRtv();
				CommandList->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
				RayTracing->BindRoot(CommandList, ConstantsAddress, LightingAddress, Scene, Lighting);
				CommandList->SetPipelineState(Pipeline);
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 4, PrevIrradiance.Gpu); // t9
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 5, PrevDistance.Gpu);   // t10
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 6, PrevProbeData.Gpu);  // t11
				SetScreenPassViewport(CommandList, Width, Height);
				CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				CommandList->DrawInstanced(3, 1, 0, 0);
			});
	}

	// 2) 누적 (계산): 이전 장 → 이번 장 — 그룹 = 프로브 하나 (광선을 그룹 공유 메모리에 한 번), 갱신 안 한 프로브는 복사
	const FD3D12DescriptorHandle RaySrv     = RayData->Srv;
	const uint32                 ProbeCount = Constants.TotalProbes;
	const auto AddBlend = [&](const char* Name, FRGResourceRef Target, const FRGPooledTexture& TargetTexture, const FD3D12PipelineState& Pipeline,
	                          uint32 UavSlot, uint32 Groups) {
		const FD3D12DescriptorHandle Uav = TargetTexture.Uavs[0];
		Graph.AddPass(Name, BlendQueue)
			.Read(RayRef, ERGAccess::SrvNonPixel)
			.Read(IrradianceRefs[Read], ERGAccess::SrvNonPixel)
			.Read(DistanceRefs[Read], ERGAccess::SrvNonPixel)
			.Read(ProbeDataRefs[Read], ERGAccess::SrvNonPixel)
			.Write(Target, ERGAccess::Uav, FRGSubresourceRange::All(), true)
			.Timer(BlendTimer)
			.Execute([this, &Pipeline, Uav, UavSlot, Groups, ConstantsAddress, RaySrv, PrevIrradiance, PrevDistance, PrevProbeData](FRGContext& Context) {
				ID3D12GraphicsCommandList* CommandList = Context.CommandList;
				CommandList->SetComputeRootSignature(ScreenRoot->Get());
				CommandList->SetPipelineState(Pipeline.Get());
				CommandList->SetComputeRootConstantBufferView(FScreenPassRootSignature::Root_Constants, ConstantsAddress);
				const FD3D12DescriptorHandle Srvs[4] = { RaySrv, PrevIrradiance, PrevDistance, PrevProbeData };
				for (uint32 Index = 0; Index < 4; ++Index)
				{
					CommandList->SetComputeRootDescriptorTable(FScreenPassRootSignature::Root_Srv0 + Index, Srvs[Index].Gpu);
				}
				CommandList->SetComputeRootDescriptorTable(FScreenPassRootSignature::Root_Uav0 + UavSlot, Uav.Gpu);
				CommandList->Dispatch(Groups, 1, 1);
			});
	};
	AddBlend("DDGI 조도 누적", IrradianceRefs[Write], *Irradiance[Write], IrradiancePipeline, 0, ProbeCount);
	AddBlend("DDGI 거리 누적", DistanceRefs[Write], *Distance[Write], DistancePipeline, 1, ProbeCount);
	AddBlend("DDGI 프로브 상태", ProbeDataRefs[Write], *ProbeData[Write], ProbeDataPipeline, 0, (ProbeCount + 63) / 64);
	Stats.UpdatedProbes = TraceRows;
}

void FDdgiRenderer::AddProbeDebugPass(FRenderGraph& Graph, const FD3D12RenderTarget& SceneColor, FRGResourceRef ColorRef, FRGResourceRef DepthRef, int32 Timer)
{
	if (!bFrameActive || !bAnyDebugProbes)
	{
		return;
	}
	const uint32                    Write            = WriteIndex;
	const uint32                    ProbeCount       = Constants.TotalProbes;
	const D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress = ShadingConstants;
	const FD3D12DescriptorHandle    Srvs[3]          = { Irradiance[Write]->Srv, Distance[Write]->Srv, ProbeData[Write]->Srv };
	const FD3D12RenderTarget*       Target           = &SceneColor;
	Graph.AddPass("DDGI 프로브 표시")
		.Read(IrradianceRefs[Write], ERGAccess::SrvPixel | ERGAccess::SrvNonPixel)
		.Read(DistanceRefs[Write], ERGAccess::SrvPixel | ERGAccess::SrvNonPixel)
		.Read(ProbeDataRefs[Write], ERGAccess::SrvPixel | ERGAccess::SrvNonPixel)
		.Write(ColorRef, ERGAccess::RenderTarget)
		.Write(DepthRef, ERGAccess::DepthWrite)
		.Timer(Timer)
		.Execute([this, Target, ProbeCount, ConstantsAddress, Srvs](FRGContext& Context) {
			ID3D12GraphicsCommandList*        CommandList = Context.CommandList;
			const D3D12_CPU_DESCRIPTOR_HANDLE Rtv         = Target->GetRtv();
			const D3D12_CPU_DESCRIPTOR_HANDLE Dsv         = Target->GetDsv();
			CommandList->OMSetRenderTargets(1, &Rtv, FALSE, &Dsv);
			SetScreenPassViewport(CommandList, Target->GetWidth(), Target->GetHeight());
			CommandList->SetGraphicsRootSignature(ScreenRoot->Get());
			CommandList->SetPipelineState(ProbeDebugPipeline.Get());
			CommandList->SetGraphicsRootConstantBufferView(FScreenPassRootSignature::Root_Constants, ConstantsAddress);
			for (uint32 Index = 0; Index < 3; ++Index)
			{
				CommandList->SetGraphicsRootDescriptorTable(FScreenPassRootSignature::Root_Srv0 + 1 + Index, Srvs[Index].Gpu);
			}
			CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			CommandList->DrawInstanced(ProbeSphereVertices, ProbeCount, 0, 0);
		});
}
