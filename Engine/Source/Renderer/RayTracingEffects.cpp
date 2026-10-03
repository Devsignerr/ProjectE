#include "Renderer/RayTracingEffects.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/DdgiRenderer.h"
#include "Renderer/RayTracingMath.h"
#include "Renderer/RayTracingScene.h"
#include "Renderer/ScreenPass.h"
#include "Renderer/ScreenSpaceReflections.h"

#include <algorithm>
#include <cmath>
#include <cstring>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// RayTracingView.hlsli RayTracingView (b0)와 1:1
	struct alignas(16) FRayTracingViewConstants
	{
		FMatrix4x4 InvViewProjection;
		FMatrix4x4 ViewProjection;
		FMatrix4x4 PrevViewProjection;
		FVector3   CameraPosition;
		uint32     bOrthographic = 0;
		FVector3   CameraForward;
		float      ProjectionScale = 1.0f;
		FVector2   ScreenSize;
		uint32     FrameIndex = 0;
		uint32     bDecals    = 0;
		float      NormalBias      = 1.0f;
		float      MaxDistance     = 100000.0f;
		float      MaxRoughness    = 0.6f;
		float      MaxBlurRadius   = 16.0f;
		float      SunTanHalfAngle = 0.0f;
		float      MinFilterRadius = 2.0f;
		float      MaxFilterRadius = 12.0f;
		float      HistoryWeight   = 0.2f;
		uint32     bHistoryValid   = 0;
		uint32     DebugMode       = 0;
		float      Padding[2]      = {};
	};
	static_assert(sizeof(FRayTracingViewConstants) == 288);

	// RayTracingLighting.hlsli RayTracingLighting (b1)와 1:1
	struct alignas(16) FRayTracingLightingConstants
	{
		FVector3 LightDirection;
		float    LightEnabled = 0.0f;
		FVector3 LightRadiance;
		float    AmbientIntensity = 1.0f;
		uint32   LocalLightCount   = 0;
		uint32   CaptureCount      = 0;
		uint32   MaxHitLocalLights = 16;
		uint32   bHitShadows       = 1;
		float    HitSunTanHalfAngle = 0.0f;
		float    Padding[3]        = {};
	};
	static_assert(sizeof(FRayTracingLightingConstants) == 64);

	FRayTracingViewConstants MakeViewConstants(const FRayTracingViewInputs& View)
	{
		FRayTracingViewConstants Constants;
		Constants.InvViewProjection  = View.InvViewProjection;
		Constants.ViewProjection     = View.ViewProjection;
		Constants.PrevViewProjection = View.PrevViewProjection;
		Constants.CameraPosition     = View.CameraPosition;
		Constants.bOrthographic      = View.bOrthographic ? 1u : 0u;
		Constants.CameraForward      = View.CameraForward;
		Constants.ProjectionScale    = View.ProjectionScale;
		Constants.ScreenSize         = FVector2(static_cast<float>(View.SceneColor->GetWidth()), static_cast<float>(View.SceneColor->GetHeight()));
		Constants.FrameIndex         = static_cast<uint32>(View.FrameIndex % RayTracingMath::InterleavedSampleCount);
		Constants.bDecals            = View.bDecals ? 1u : 0u;
		Constants.bHistoryValid      = View.bHistoryValid ? 1u : 0u;
		return Constants;
	}

	void DrawFullscreenTriangle(ID3D12GraphicsCommandList* CommandList, uint32 Width, uint32 Height)
	{
		SetScreenPassViewport(CommandList, Width, Height);
		CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		CommandList->DrawInstanced(3, 1, 0, 0);
	}
} // namespace

bool FRayTracingPassRoot::Init(ID3D12Device* Device)
{
	using FRange = FD3D12RootSignature;
	const D3D12_DESCRIPTOR_RANGE_FLAGS Volatile = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
	E_CHECK(RootSignature.AddConstantBufferView(0) == Root_View);
	E_CHECK(RootSignature.AddConstantBufferView(1) == Root_Lighting);
	E_CHECK(RootSignature.AddShaderResourceView(0) == Root_Tlas);
	E_CHECK(RootSignature.AddShaderResourceView(1) == Root_Instances);
	E_CHECK(RootSignature.AddShaderResourceView(2) == Root_Materials);
	E_CHECK(RootSignature.AddShaderResourceView(3) == Root_LocalLights);
	E_CHECK(RootSignature.AddShaderResourceView(4) == Root_Captures);
	for (uint32 Index = 0; Index < ScreenCount; ++Index)
	{
		E_CHECK(RootSignature.AddDescriptorTable({ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5 + Index, 0, Volatile) }) == Root_Screen0 + Index);
	}
	E_CHECK(RootSignature.AddDescriptorTable({ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 13, 0, Volatile) }) == Root_Ibl);
	E_CHECK(RootSignature.AddDescriptorTable({ FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 16, 0, Volatile) }) == Root_CaptureAtlas);
	// 바인드리스: 셰이더 가시 힙 처음부터 무제한 (텍스처/버퍼 두 표가 같은 힙 칸을 다른 형식으로 본다 — 칸 번호 = 디스크립터 Index)
	E_CHECK(RootSignature.AddDescriptorTable(
		        { FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE) }) ==
	        Root_BindlessTextures);
	E_CHECK(RootSignature.AddDescriptorTable(
		        { FRange::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE) }) ==
	        Root_BindlessBuffers);
	E_CHECK(RootSignature.AddShaderResourceView(17) == Root_GraphParams);
	E_CHECK(RootSignature.AddConstantBufferView(2) == Root_Extra);
	RootSignature.AddStaticSampler(FRange::MakeStaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_SHADER_VISIBILITY_ALL));
	RootSignature.AddStaticSampler(FRange::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	RootSignature.AddStaticSampler(FRange::MakeStaticSampler(2, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_SHADER_VISIBILITY_ALL));
	return RootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"RayTracingPassRoot");
}

FRayTracingEffects::~FRayTracingEffects()
{
	Shutdown();
}

bool FRayTracingEffects::Init(FD3D12RHI& InRhi, FShaderLibrary& InLibrary)
{
	Rhi        = &InRhi;
	Library    = &InLibrary;
	bSupported = false;
	EnsureShadowTargets(1, 1); // 메인 패스 t24는 항상 바인딩
	if (!Rhi->GetDevice().SupportsRayTracing())
	{
		return true;
	}
	if (!Root.Init(Rhi->GetDevice().GetDevice()) || !CreatePipelines(Pipelines, false, nullptr))
	{
		E_LOG(LogRenderer, Error, "레이 트레이싱 화면 패스 파이프라인 생성 실패 — 레이 트레이싱 효과를 끕니다");
		return true;
	}
	bSupported = true;
	return true;
}

void FRayTracingEffects::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	ShadowHistory[0].reset();
	ShadowHistory[1].reset();
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		AoHistory[Index].reset();
		AoReferenceAccum[Index].reset();
	}
	ReleaseVariants(true);
	Pipelines.ShadowTrace.Shutdown();
	Pipelines.ShadowFilter.Shutdown();
	Pipelines.ShadowResolve.Shutdown();
	Pipelines.ReflectionTrace.Shutdown();
	Pipelines.Debug.Shutdown();
	Pipelines.AoTrace.Shutdown();
	Pipelines.AoFilter.Shutdown();
	Pipelines.AoResolve.Shutdown();
	Pipelines.AoReference.Shutdown();
	Root.Shutdown();
	Rhi = nullptr;
}

bool FRayTracingEffects::CreatePipelines(FPipelines& Out, bool bForceRecompile, const FRayTracingGraphVariant* Variant)
{
	ID3D12Device* Device = Rhi->GetDevice().GetDevice();
	const auto    Load   = [&](const wchar_t* File, const wchar_t* Entry, EShaderStage Stage, const wchar_t* Define) {
        FShaderCompileDesc Desc;
        Desc.FileName    = File;
        Desc.EntryPoint  = Entry;
        Desc.Stage       = Stage;
        Desc.ShaderModel = L"6_5"; // 인라인 RayQuery (정점 셰이더도 같은 파일의 RT 선언을 포함하므로 같은 모델)
        if (Define != nullptr)
        {
            Desc.Defines.push_back(Define); // 같은 파일의 다른 진입 묶음 (RTAO 기준 = E_RTAO_REFERENCE — 매니페스트와 같은 디파인)
        }
        if (Variant != nullptr && Stage == EShaderStage::Pixel)
        {
            // 그래프 머티리얼 변형: 생성 함수 묶음 (내용 해시가 캐시 키·쿠킹 파일명에 들어간다 — 같은 집합이면 재사용)
            Desc.Defines.push_back(L"E_RT_GRAPH_MATERIALS");
            Desc.VirtualFiles.push_back({ L"RayTracingGraphMaterials.generated.hlsli", Variant->Source });
        }
        if (bForceRecompile && !Library->CookShader(Desc))
        {
            return ComPtr<IDxcBlob>();
        }
        return Library->GetShader(Desc);
	};
	const auto Create = [&](FD3D12PipelineState& Pipeline, const wchar_t* File, const wchar_t* Entry, std::initializer_list<DXGI_FORMAT> Formats,
	                        const wchar_t* Name, const wchar_t* Define = nullptr) {
		const ComPtr<IDxcBlob> VertexShader = Load(File, L"VSMain", EShaderStage::Vertex, Define);
		const ComPtr<IDxcBlob> PixelShader  = Load(File, Entry, EShaderStage::Pixel, Define);
		if (!VertexShader || !PixelShader)
		{
			return false;
		}
		FGraphicsPipelineDesc Desc;
		Desc.RootSignature    = Root.Get();
		Desc.VertexShader     = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
		Desc.PixelShader      = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
		Desc.CullMode         = D3D12_CULL_MODE_NONE;
		Desc.bDepthEnable     = false;
		Desc.NumRenderTargets = static_cast<uint32>(Formats.size());
		uint32 Index          = 0;
		for (const DXGI_FORMAT Format : Formats)
		{
			Desc.RenderTargetFormats[Index++] = Format;
		}
		return Pipeline.InitGraphics(Device, Desc, Name);
	};
	if (Variant != nullptr)
	{
		// 변형은 머티리얼을 평가하는 추적 패스만 (필터·누적은 기본 파이프라인)
		return Create(Out.ShadowTrace, L"RayTracedShadows.hlsl", L"PSTrace", { ShadowTraceFormat }, L"RtShadowTraceGraph") &&
		       Create(Out.ReflectionTrace, L"RayTracedReflections.hlsl", L"PSTrace",
		              { FScreenSpaceReflections::ResultFormat, FScreenSpaceReflections::MotionFormat }, L"RtReflectionTraceGraph") &&
		       Create(Out.Debug, L"RayTracingDebug.hlsl", L"PSInstances", { DebugFormat }, L"RtDebugInstancesGraph") &&
		       Create(Out.AoTrace, L"RayTracedAmbientOcclusion.hlsl", L"PSTrace", { AoTraceFormat }, L"RtAoTraceGraph") &&
		       Create(Out.AoReference, L"RayTracedAmbientOcclusion.hlsl", L"PSReference", { AoReferenceFormat, AoReferenceResultFormat },
		              L"RtAoReferenceGraph", L"E_RTAO_REFERENCE");
	}
	return Create(Out.ShadowTrace, L"RayTracedShadows.hlsl", L"PSTrace", { ShadowTraceFormat }, L"RtShadowTrace") &&
	       Create(Out.ShadowFilter, L"RayTracedShadows.hlsl", L"PSFilter", { ShadowMaskFormat }, L"RtShadowFilter") &&
	       Create(Out.ShadowResolve, L"RayTracedShadows.hlsl", L"PSResolve", { ShadowMaskFormat }, L"RtShadowResolve") &&
	       Create(Out.ReflectionTrace, L"RayTracedReflections.hlsl", L"PSTrace",
	              { FScreenSpaceReflections::ResultFormat, FScreenSpaceReflections::MotionFormat }, L"RtReflectionTrace") &&
	       Create(Out.Debug, L"RayTracingDebug.hlsl", L"PSInstances", { DebugFormat }, L"RtDebugInstances") &&
	       Create(Out.AoTrace, L"RayTracedAmbientOcclusion.hlsl", L"PSTrace", { AoTraceFormat }, L"RtAoTrace") &&
	       Create(Out.AoFilter, L"RayTracedAmbientOcclusion.hlsl", L"PSFilter", { AoResultFormat }, L"RtAoFilter") &&
	       Create(Out.AoResolve, L"RayTracedAmbientOcclusion.hlsl", L"PSResolve", { AoResultFormat }, L"RtAoResolve") &&
	       Create(Out.AoReference, L"RayTracedAmbientOcclusion.hlsl", L"PSReference", { AoReferenceFormat, AoReferenceResultFormat }, L"RtAoReference",
	              L"E_RTAO_REFERENCE");
}

bool FRayTracingEffects::ReloadShaders(bool bForceRecompile)
{
	if (!bSupported)
	{
		return true;
	}
	ReleaseVariants(true); // 그래프 변형은 다음 사용 때 다시 컴파일
	FPipelines NewPipelines;
	if (!CreatePipelines(NewPipelines, bForceRecompile, nullptr))
	{
		E_LOG(LogRenderer, Error, "레이 트레이싱 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	for (auto [Current, Next] : { std::pair{ &Pipelines.ShadowTrace, &NewPipelines.ShadowTrace }, std::pair{ &Pipelines.ShadowFilter, &NewPipelines.ShadowFilter },
	                              std::pair{ &Pipelines.ShadowResolve, &NewPipelines.ShadowResolve },
	                              std::pair{ &Pipelines.ReflectionTrace, &NewPipelines.ReflectionTrace }, std::pair{ &Pipelines.Debug, &NewPipelines.Debug },
	                              std::pair{ &Pipelines.AoTrace, &NewPipelines.AoTrace }, std::pair{ &Pipelines.AoFilter, &NewPipelines.AoFilter },
	                              std::pair{ &Pipelines.AoResolve, &NewPipelines.AoResolve }, std::pair{ &Pipelines.AoReference, &NewPipelines.AoReference } })
	{
		Current->Swap(*Next);
		Rhi->DeferRelease(Next->Detach());
	}
	return true;
}

void FRayTracingEffects::ReleaseVariants(bool bAll)
{
	const uint64 FrameNumber = Rhi->GetFrameNumber();
	for (auto It = VariantPipelines.begin(); It != VariantPipelines.end();)
	{
		if (bAll || It->second->LastUsedFrame + 600 < FrameNumber)
		{
			for (FD3D12PipelineState* Pipeline : { &It->second->Pipelines.ShadowTrace, &It->second->Pipelines.ReflectionTrace, &It->second->Pipelines.Debug,
			                                       &It->second->Pipelines.AoTrace, &It->second->Pipelines.AoReference })
			{
				if (Pipeline->Get() != nullptr)
				{
					Rhi->DeferRelease(Pipeline->Detach());
				}
			}
			It = VariantPipelines.erase(It);
		}
		else
		{
			++It;
		}
	}
}

const FRayTracingEffects::FPipelines& FRayTracingEffects::SelectPipelines(const FRayTracingScene& Scene)
{
	const FRayTracingGraphVariant& Variant = Scene.GetGraphVariant();
	if (Variant.Key == 0)
	{
		return Pipelines;
	}
	if (VariantPipelines.find(Variant.Key) == VariantPipelines.end())
	{
		ReleaseVariants(false); // 오래 안 쓴 변형 정리 (새 변형을 만들 때만 — 맵에 넣기 전에)
	}
	std::unique_ptr<FVariantPipelines>& Entry = VariantPipelines[Variant.Key];
	if (!Entry)
	{
		Entry = std::make_unique<FVariantPipelines>();
		Entry->bFailed = !CreatePipelines(Entry->Pipelines, false, &Variant);
		if (Entry->bFailed)
		{
			E_LOG(LogRenderer, Warning, "레이 트레이싱 그래프 머티리얼 변형 컴파일 실패 (키 {:016x}) — 그래프 머티리얼 히트는 회색 근사", Variant.Key);
		}
	}
	Entry->LastUsedFrame = Rhi->GetFrameNumber();
	return Entry->bFailed ? Pipelines : Entry->Pipelines;
}

void FRayTracingEffects::EnsureShadowTargets(uint32 Width, uint32 Height)
{
	Width  = std::max(Width, 1u);
	Height = std::max(Height, 1u);
	if (ShadowHistory[0] && ShadowWidth == Width && ShadowHeight == Height)
	{
		return;
	}
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		if (ShadowHistory[Index])
		{
			ShadowHistory[Index]->ShutdownDeferred(*Rhi);
		}
		ShadowHistory[Index] = std::make_unique<FD3D12RenderTarget>();
		if (!ShadowHistory[Index]->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), Width, Height, Index == 0 ? L"RtShadowHistory0" : L"RtShadowHistory1",
		                                FRenderTargetDesc::MakeColor(ShadowMaskFormat)))
		{
			E_LOG(LogRenderer, Fatal, "RT 그림자 누적 버퍼 생성 실패 ({}x{})", Width, Height);
		}
	}
	ShadowWidth     = Width;
	ShadowHeight    = Height;
	LastShadowFrame = 0; // 새 버퍼에는 이력이 없다
}

FRGResourceRef FRayTracingEffects::BeginShadowFrame(FRenderGraph& Graph, uint32 Width, uint32 Height, bool bActive)
{
	if (bActive)
	{
		EnsureShadowTargets(Width, Height);
		ShadowHistoryIndex ^= 1u; // 이번 프레임 누적 칸 (지난 칸 = 이력)
	}
	FrameShadowRef = Graph.ImportColor("RtShadowHistory", *ShadowHistory[ShadowHistoryIndex]);
	return FrameShadowRef;
}

D3D12_GPU_VIRTUAL_ADDRESS FRayTracingEffects::UploadLighting(const FRayTracingLightingInputs& Lighting, uint32 MaxHitLocalLights, bool bHitShadows) const
{
	FRayTracingLightingConstants Constants;
	Constants.LightDirection    = Lighting.LightDirection;
	Constants.LightRadiance     = Lighting.LightRadiance;
	Constants.LightEnabled      = Lighting.LightRadiance.LengthSquared() > 0.0f ? 1.0f : 0.0f;
	Constants.AmbientIntensity  = Lighting.AmbientIntensity;
	Constants.LocalLightCount   = Lighting.LocalLights != 0 ? Lighting.LocalLightCount : 0u;
	Constants.CaptureCount      = Lighting.Captures != 0 ? Lighting.CaptureCount : 0u;
	Constants.MaxHitLocalLights = MaxHitLocalLights;
	Constants.bHitShadows       = bHitShadows ? 1u : 0u;
	return Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;
}

void FRayTracingEffects::BindRoot(ID3D12GraphicsCommandList* CommandList, D3D12_GPU_VIRTUAL_ADDRESS ViewConstants, D3D12_GPU_VIRTUAL_ADDRESS LightingConstants,
                                  const FRayTracingScene& Scene, const FRayTracingLightingInputs& Lighting) const
{
	const D3D12_GPU_DESCRIPTOR_HANDLE HeapStart = Rhi->GetSrvAllocator().GetHeap()->GetGPUDescriptorHandleForHeapStart();
	CommandList->SetGraphicsRootSignature(Root.Get());
	CommandList->SetGraphicsRootConstantBufferView(FRayTracingPassRoot::Root_View, ViewConstants);
	CommandList->SetGraphicsRootConstantBufferView(FRayTracingPassRoot::Root_Lighting, LightingConstants);
	CommandList->SetGraphicsRootShaderResourceView(FRayTracingPassRoot::Root_Tlas, Scene.GetTlasAddress());
	CommandList->SetGraphicsRootShaderResourceView(FRayTracingPassRoot::Root_Instances, Scene.GetInstanceBuffer());
	CommandList->SetGraphicsRootShaderResourceView(FRayTracingPassRoot::Root_Materials, Scene.GetMaterialBuffer());
	CommandList->SetGraphicsRootShaderResourceView(FRayTracingPassRoot::Root_LocalLights, Lighting.LocalLights);
	CommandList->SetGraphicsRootShaderResourceView(FRayTracingPassRoot::Root_Captures, Lighting.Captures);
	if (Lighting.IblTable.IsValid())
	{
		CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Ibl, Lighting.IblTable.Gpu);
	}
	if (Lighting.CaptureAtlas.IsValid())
	{
		CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_CaptureAtlas, Lighting.CaptureAtlas.Gpu);
	}
	CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_BindlessTextures, HeapStart);
	CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_BindlessBuffers, HeapStart);
	CommandList->SetGraphicsRootShaderResourceView(FRayTracingPassRoot::Root_GraphParams, Scene.GetGraphParamBuffer());
	CommandList->SetGraphicsRootConstantBufferView(FRayTracingPassRoot::Root_Extra, ViewConstants); // 쓰는 패스만 다시 묶는다
}

FRGResourceRef FRayTracingEffects::AddShadowPasses(FRenderGraph& Graph, const FRayTracingScene& Scene, const FRayTracingViewInputs& View,
                                                   const FRayTracingViewRefs& Refs, const FRayTracingLightingInputs& Lighting,
                                                   const FRayTracedShadowSettings& Settings, int32 Timer)
{
	const uint32 Width  = View.SceneColor->GetWidth();
	const uint32 Height = View.SceneColor->GetHeight();
	E_CHECKF(FrameShadowRef.IsValid() && ShadowWidth == Width && ShadowHeight == Height, "RT 그림자: BeginShadowFrame(활성)을 먼저 불러야 합니다");

	FRayTracingViewConstants Constants = MakeViewConstants(View);
	Constants.NormalBias      = Settings.NormalBias;
	Constants.MaxDistance     = Settings.MaxDistance;
	Constants.SunTanHalfAngle = std::tan(FMath::DegreesToRadians(std::max(Settings.SunAngleDegrees, 0.0f)) * 0.5f);
	Constants.MinFilterRadius = Settings.MinFilterRadius;
	Constants.MaxFilterRadius = std::max(Settings.MaxFilterRadius, Settings.MinFilterRadius);
	Constants.HistoryWeight   = std::clamp(Settings.HistoryWeight, 0.01f, 1.0f);
	const uint64 FrameNumber  = Rhi->GetFrameNumber();
	const bool   bHistory     = View.bHistoryValid && LastShadowFrame != 0 && LastShadowFrame + 1 == FrameNumber;
	Constants.bHistoryValid   = bHistory ? 1u : 0u;
	FD3D12DynamicUploadBuffer& Dynamic = Rhi->GetDynamicBuffer();
	const D3D12_GPU_VIRTUAL_ADDRESS ViewAddress     = Dynamic.AllocateConstants(Constants).GpuAddress;
	const D3D12_GPU_VIRTUAL_ADDRESS LightingAddress = UploadLighting(Lighting, 0, false);

	const FRGResourceRef    TraceRef  = Graph.CreateTexture("RtShadowTrace", FRGTextureDesc::MakeRenderTarget(Width, Height, ShadowTraceFormat));
	const FRGResourceRef    FilterRef = Graph.CreateTexture("RtShadowFiltered", FRGTextureDesc::MakeRenderTarget(Width, Height, ShadowMaskFormat));
	const FRGPooledTexture* Trace     = Graph.GetTexture(TraceRef);
	const FRGPooledTexture* Filtered  = Graph.GetTexture(FilterRef);
	const FD3D12DescriptorHandle DepthSrv    = View.SceneColor->GetDepthSrv();
	const FD3D12DescriptorHandle NormalSrv   = View.SceneNormal->GetSrv();
	const FD3D12DescriptorHandle VelocitySrv = View.Velocity->GetSrv();

	// 1) 추적 (그래프 머티리얼 Masked 알파 테스트 → 변형 파이프라인)
	{
		ID3D12PipelineState* const TracePipeline = SelectPipelines(Scene).ShadowTrace.Get();
		FRenderGraph::FPassBuilder Pass = Graph.AddPass("RT 그림자 추적");
		Scene.DeclareTraceReads(Pass, Refs.Tlas);
		Pass.Read(Refs.Depth, ERGAccess::SrvPixel)
			.Read(Refs.Normal, ERGAccess::SrvPixel)
			.Write(TraceRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
			.Timer(Timer)
			.Execute([this, &Scene, Lighting, Trace, TracePipeline, ViewAddress, LightingAddress, DepthSrv, NormalSrv, Width, Height](FRGContext& Context) {
				ID3D12GraphicsCommandList* CommandList = Context.CommandList;
				const D3D12_CPU_DESCRIPTOR_HANDLE Rtv  = Trace->GetRtv();
				CommandList->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
				BindRoot(CommandList, ViewAddress, LightingAddress, Scene, Lighting);
				CommandList->SetPipelineState(TracePipeline);
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 0, DepthSrv.Gpu);
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 1, NormalSrv.Gpu);
				DrawFullscreenTriangle(CommandList, Width, Height);
			});
	}
	// 2) 공간 필터
	{
		Graph.AddPass("RT 그림자 공간 필터")
			.Read(Refs.Depth, ERGAccess::SrvPixel)
			.Read(Refs.Normal, ERGAccess::SrvPixel)
			.Read(TraceRef, ERGAccess::SrvPixel)
			.Write(FilterRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
			.Timer(Timer)
			.Execute([this, &Scene, Lighting, Trace, Filtered, ViewAddress, LightingAddress, DepthSrv, NormalSrv, Width, Height](FRGContext& Context) {
				ID3D12GraphicsCommandList* CommandList = Context.CommandList;
				const D3D12_CPU_DESCRIPTOR_HANDLE Rtv  = Filtered->GetRtv();
				CommandList->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
				BindRoot(CommandList, ViewAddress, LightingAddress, Scene, Lighting);
				CommandList->SetPipelineState(Pipelines.ShadowFilter.Get());
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 0, DepthSrv.Gpu);
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 1, NormalSrv.Gpu);
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 5, Trace->Srv.Gpu); // t10
				DrawFullscreenTriangle(CommandList, Width, Height);
			});
	}
	// 3) 시간 누적 (이력 2장 핑퐁 — BeginShadowFrame이 이번 프레임 칸을 정해 두었다)
	const FD3D12RenderTarget& Previous    = *ShadowHistory[ShadowHistoryIndex ^ 1u];
	const FD3D12RenderTarget& Current     = *ShadowHistory[ShadowHistoryIndex];
	const FRGResourceRef      PreviousRef = Graph.ImportColor("RtShadowHistoryPrevious", Previous);
	const FRGResourceRef      CurrentRef  = FrameShadowRef;
	const FD3D12DescriptorHandle PreviousSrv = Previous.GetSrv();
	Graph.AddPass("RT 그림자 누적")
		.Read(Refs.Depth, ERGAccess::SrvPixel)
		.Read(Refs.Velocity, ERGAccess::SrvPixel)
		.Read(FilterRef, ERGAccess::SrvPixel)
		.Read(PreviousRef, ERGAccess::SrvPixel)
		.Write(CurrentRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, &Scene, &Current, Lighting, Filtered, ViewAddress, LightingAddress, DepthSrv, VelocitySrv, PreviousSrv, Width, Height](
					 FRGContext& Context) {
			ID3D12GraphicsCommandList* CommandList = Context.CommandList;
			Current.Bind(CommandList, nullptr, false, false);
			BindRoot(CommandList, ViewAddress, LightingAddress, Scene, Lighting);
			CommandList->SetPipelineState(Pipelines.ShadowResolve.Get());
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 0, DepthSrv.Gpu);
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 4, VelocitySrv.Gpu); // t9
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 5, Filtered->Srv.Gpu); // t10
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 6, PreviousSrv.Gpu);   // t11
			DrawFullscreenTriangle(CommandList, Width, Height);
		});
	LastShadowFrame = FrameNumber;
	return CurrentRef;
}

void FRayTracingEffects::AddReflectionTracePass(FRenderGraph& Graph, const FRayTracingScene& Scene, const FRayTracingViewInputs& View,
                                                const FRayTracingViewRefs& Refs, const FRayTracingLightingInputs& Lighting,
                                                const FRayTracedReflectionSettings& Settings, int32 Timer, FRGResourceRef& OutResult, FRGResourceRef& OutMotion)
{
	const uint32 Width  = View.SceneColor->GetWidth();
	const uint32 Height = View.SceneColor->GetHeight();
	FRayTracingViewConstants Constants = MakeViewConstants(View);
	Constants.NormalBias    = Settings.NormalBias;
	Constants.MaxDistance   = Settings.MaxDistance;
	Constants.MaxRoughness  = Settings.MaxRoughness;
	Constants.MaxBlurRadius = Settings.MaxBlurRadius;
	FD3D12DynamicUploadBuffer& Dynamic = Rhi->GetDynamicBuffer();
	const D3D12_GPU_VIRTUAL_ADDRESS ViewAddress     = Dynamic.AllocateConstants(Constants).GpuAddress;
	const D3D12_GPU_VIRTUAL_ADDRESS LightingAddress = UploadLighting(Lighting, Settings.MaxHitLocalLights, Settings.bHitShadows);

	OutResult = Graph.CreateTexture("RtReflectionResult", FRGTextureDesc::MakeRenderTarget(Width, Height, FScreenSpaceReflections::ResultFormat));
	OutMotion = Graph.CreateTexture("RtReflectionMotion", FRGTextureDesc::MakeRenderTarget(Width, Height, FScreenSpaceReflections::MotionFormat));
	const FRGPooledTexture*      Result      = Graph.GetTexture(OutResult);
	const FRGPooledTexture*      Motion      = Graph.GetTexture(OutMotion);
	const FD3D12DescriptorHandle DepthSrv    = View.SceneColor->GetDepthSrv();
	const FD3D12DescriptorHandle NormalSrv   = View.SceneNormal->GetSrv();
	const FD3D12DescriptorHandle DecalNormal = View.DecalNormal->GetSrv();
	const FD3D12DescriptorHandle DecalMat    = View.DecalMaterial->GetSrv();

	ID3D12PipelineState* const TracePipeline = SelectPipelines(Scene).ReflectionTrace.Get();
	FRenderGraph::FPassBuilder Pass        = Graph.AddPass("RT 반사 추적");
	Scene.DeclareTraceReads(Pass, Refs.Tlas);
	Pass.Read(Refs.Depth, ERGAccess::SrvPixel)
		.Read(Refs.Normal, ERGAccess::SrvPixel)
		.Read(Refs.DecalNormal, ERGAccess::SrvPixel)
		.Read(Refs.DecalMaterial, ERGAccess::SrvPixel)
		.Write(OutResult, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
		.Write(OutMotion, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, &Scene, Lighting, Result, Motion, TracePipeline, ViewAddress, LightingAddress, DepthSrv, NormalSrv, DecalNormal, DecalMat, Width,
	              Height](FRGContext& Context) {
			ID3D12GraphicsCommandList*        CommandList = Context.CommandList;
			const D3D12_CPU_DESCRIPTOR_HANDLE Targets[]   = { Result->GetRtv(), Motion->GetRtv() };
			CommandList->OMSetRenderTargets(2, Targets, FALSE, nullptr);
			BindRoot(CommandList, ViewAddress, LightingAddress, Scene, Lighting);
			CommandList->SetPipelineState(TracePipeline);
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 0, DepthSrv.Gpu);
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 1, NormalSrv.Gpu);
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 2, DecalNormal.Gpu);
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 3, DecalMat.Gpu);
			DrawFullscreenTriangle(CommandList, Width, Height);
		});
}

FRGResourceRef FRayTracingEffects::AddDebugPass(FRenderGraph& Graph, const FRayTracingScene& Scene, const FRayTracingViewInputs& View,
                                                const FRayTracingViewRefs& Refs, const FRayTracingLightingInputs& Lighting, uint32 DebugMode, int32 Timer,
                                                FD3D12DescriptorHandle& OutSrv)
{
	const uint32 Width  = View.SceneColor->GetWidth();
	const uint32 Height = View.SceneColor->GetHeight();
	FRayTracingViewConstants Constants = MakeViewConstants(View);
	Constants.DebugMode = DebugMode;
	const D3D12_GPU_VIRTUAL_ADDRESS ViewAddress     = Rhi->GetDynamicBuffer().AllocateConstants(Constants).GpuAddress;
	const D3D12_GPU_VIRTUAL_ADDRESS LightingAddress = UploadLighting(Lighting, 16, true);
	const FRGResourceRef    OutRef = Graph.CreateTexture("RtDebug", FRGTextureDesc::MakeRenderTarget(Width, Height, DebugFormat));
	const FRGPooledTexture* Out    = Graph.GetTexture(OutRef);
	OutSrv                         = Out->Srv;
	ID3D12PipelineState* const DebugPipeline = SelectPipelines(Scene).Debug.Get();
	FRenderGraph::FPassBuilder Pass          = Graph.AddPass("RT 디버그 (인스턴스)");
	Scene.DeclareTraceReads(Pass, Refs.Tlas);
	Pass.Write(OutRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, &Scene, Lighting, Out, DebugPipeline, ViewAddress, LightingAddress, Width, Height](FRGContext& Context) {
			ID3D12GraphicsCommandList*        CommandList = Context.CommandList;
			const D3D12_CPU_DESCRIPTOR_HANDLE Rtv         = Out->GetRtv();
			CommandList->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
			BindRoot(CommandList, ViewAddress, LightingAddress, Scene, Lighting);
			CommandList->SetPipelineState(DebugPipeline);
			DrawFullscreenTriangle(CommandList, Width, Height);
		});
	return OutRef;
}

void FRayTracingEffects::EnsureAoTargets(uint32 Width, uint32 Height, bool bReference)
{
	Width  = std::max(Width, 1u);
	Height = std::max(Height, 1u);
	if (AoHistory[0] && AoWidth == Width && AoHeight == Height && bAoReference == bReference)
	{
		return;
	}
	for (uint32 Index = 0; Index < 2; ++Index)
	{
		for (std::unique_ptr<FD3D12RenderTarget>* Target : { &AoHistory[Index], &AoReferenceAccum[Index] })
		{
			if (*Target)
			{
				(*Target)->ShutdownDeferred(*Rhi);
				Target->reset();
			}
		}
		AoHistory[Index] = std::make_unique<FD3D12RenderTarget>();
		if (!AoHistory[Index]->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), Width, Height, Index == 0 ? L"RtAoHistory0" : L"RtAoHistory1",
		                            FRenderTargetDesc::MakeColor(bReference ? AoReferenceResultFormat : AoResultFormat)))
		{
			E_LOG(LogRenderer, Fatal, "RTAO 누적 버퍼 생성 실패 ({}x{})", Width, Height);
		}
		if (bReference)
		{
			AoReferenceAccum[Index] = std::make_unique<FD3D12RenderTarget>();
			if (!AoReferenceAccum[Index]->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), Width, Height,
			                                   Index == 0 ? L"RtAoReference0" : L"RtAoReference1", FRenderTargetDesc::MakeColor(AoReferenceFormat)))
			{
				E_LOG(LogRenderer, Fatal, "RTAO 기준 누적 버퍼 생성 실패 ({}x{})", Width, Height);
			}
		}
	}
	AoWidth      = Width;
	AoHeight     = Height;
	bAoReference = bReference;
	LastAoFrame  = 0; // 새 버퍼에는 이력이 없다
}

FRGResourceRef FRayTracingEffects::AddAmbientOcclusionPasses(FRenderGraph& Graph, const FRayTracingScene& Scene, const FRayTracingViewInputs& View,
                                                             const FRayTracingViewRefs& Refs, const FRayTracingLightingInputs& Lighting,
                                                             const FRayTracedAmbientOcclusionSettings& Settings, int32 Timer)
{
	E_CHECKF(Settings.Ddgi != nullptr, "RTAO: DDGI 셰이딩 입력(Settings.Ddgi)이 필요합니다 (볼륨이 없어도 기본 상수·1x1 아틀라스)");
	const bool   bReference = Settings.ReferenceRays > 0;
	// AO 버퍼 크기 = 씬 ÷ 나눔 (올림 — 추적·필터·누적·결과 모두). 기준은 항상 씬 해상도
	const uint32 Divisor = bReference ? 1u : std::clamp(Settings.ResolutionDivisor, 1u, 2u);
	const uint32 Width   = (View.SceneColor->GetWidth() + Divisor - 1) / Divisor;
	const uint32 Height  = (View.SceneColor->GetHeight() + Divisor - 1) / Divisor;
	EnsureAoTargets(Width, Height, bReference);
	AoHistoryIndex ^= 1u; // 이번 프레임 칸 (지난 칸 = 이력)

	FRayTracingViewConstants Constants = MakeViewConstants(View);
	Constants.NormalBias      = Settings.NormalBias;
	Constants.MaxDistance     = bReference ? Settings.ReferenceDistance : std::max(Settings.Radius, 1.0f); // RtAoRadius
	Constants.DebugMode       = std::clamp(Settings.RaysPerPixel, 1u, 4u);                               // RtAoRayCount
	Constants.SunTanHalfAngle = std::max(Settings.FalloffPower, 0.01f);                                 // RtAoFalloffPower
	Constants.MinFilterRadius = std::max(Settings.Intensity, 0.0f);                                     // RtAoIntensity
	Constants.bDecals         = bReference ? Settings.ReferenceRays : 0u;                               // RtAoReferenceRays
	Constants.HistoryWeight   = std::clamp(Settings.HistoryWeight, 0.01f, 1.0f);
	Constants.MaxRoughness    = static_cast<float>(Divisor);                                       // RtAoDivisor
	const uint64 FrameNumber  = Rhi->GetFrameNumber();
	bool         bHistory     = View.bHistoryValid && LastAoFrame != 0 && LastAoFrame + 1 == FrameNumber;
	if (bReference)
	{
		// 기준은 같은 카메라·같은 광선 수에서만 이어서 평균 (움직이면 처음부터 — 정지 카메라 비교용)
		bHistory = bHistory && AoReferenceRays == Settings.ReferenceRays &&
		           std::memcmp(&AoReferenceViewProjection, &View.ViewProjection, sizeof(FMatrix4x4)) == 0;
		AoReferenceRays           = Settings.ReferenceRays;
		AoReferenceViewProjection = View.ViewProjection;
	}
	Constants.bHistoryValid = bHistory ? 1u : 0u;
	FD3D12DynamicUploadBuffer&      Dynamic         = Rhi->GetDynamicBuffer();
	const D3D12_GPU_VIRTUAL_ADDRESS ViewAddress     = Dynamic.AllocateConstants(Constants).GpuAddress;
	const D3D12_GPU_VIRTUAL_ADDRESS LightingAddress = UploadLighting(Lighting, 16, true);

	const FD3D12RenderTarget&    Previous    = *AoHistory[AoHistoryIndex ^ 1u];
	const FD3D12RenderTarget&    Current     = *AoHistory[AoHistoryIndex];
	const FRGResourceRef         CurrentRef  = Graph.ImportColor("RtAoHistory", Current);
	const FD3D12DescriptorHandle DepthSrv    = View.SceneColor->GetDepthSrv();
	const FD3D12DescriptorHandle NormalSrv   = View.SceneNormal->GetSrv();
	const FD3D12DescriptorHandle VelocitySrv = View.Velocity->GetSrv();
	LastAoFrame                              = FrameNumber;
	const FDdgiRenderer&            Ddgi          = *Settings.Ddgi;
	const D3D12_GPU_VIRTUAL_ADDRESS DdgiAddress   = Ddgi.GetShadingConstants();
	const FD3D12DescriptorHandle    IrradianceSrv = Ddgi.GetIrradianceSrv();
	const FD3D12DescriptorHandle    DistanceSrv   = Ddgi.GetDistanceSrv();
	const FD3D12DescriptorHandle    ProbeDataSrv  = Ddgi.GetProbeDataSrv();

	if (bReference)
	{
		// 기준: 경로 추적 간접 확산 (누적 2장 핑퐁) + 밝기 비 → 이번 결과 칸
		const FD3D12RenderTarget&       AccumPrevious = *AoReferenceAccum[AoHistoryIndex ^ 1u];
		const FD3D12RenderTarget&       AccumCurrent  = *AoReferenceAccum[AoHistoryIndex];
		const FRGResourceRef            AccumPrevRef  = Graph.ImportColor("RtAoReferencePrevious", AccumPrevious);
		const FRGResourceRef            AccumCurRef   = Graph.ImportColor("RtAoReference", AccumCurrent);
		const FD3D12DescriptorHandle    AccumPrevSrv  = AccumPrevious.GetSrv();
		ID3D12PipelineState* const      Pipeline      = SelectPipelines(Scene).AoReference.Get();
		FRenderGraph::FPassBuilder      Pass          = Graph.AddPass("RTAO 기준 (경로 추적)");
		Scene.DeclareTraceReads(Pass, Refs.Tlas);
		Ddgi.DeclareShadingReads(Pass);
		Pass.Read(Refs.Depth, ERGAccess::SrvPixel)
			.Read(Refs.Normal, ERGAccess::SrvPixel)
			.Read(AccumPrevRef, ERGAccess::SrvPixel)
			.Write(AccumCurRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
			.Write(CurrentRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
			.Timer(Timer)
			.Execute([this, &Scene, &AccumCurrent, &Current, Lighting, Pipeline, ViewAddress, LightingAddress, DdgiAddress, DepthSrv, NormalSrv, AccumPrevSrv,
			          IrradianceSrv, DistanceSrv, ProbeDataSrv, Width, Height](FRGContext& Context) {
				ID3D12GraphicsCommandList*        CommandList = Context.CommandList;
				const D3D12_CPU_DESCRIPTOR_HANDLE Targets[]   = { AccumCurrent.GetRtv(), Current.GetRtv() };
				CommandList->OMSetRenderTargets(2, Targets, FALSE, nullptr);
				BindRoot(CommandList, ViewAddress, LightingAddress, Scene, Lighting);
				CommandList->SetGraphicsRootConstantBufferView(FRayTracingPassRoot::Root_Extra, DdgiAddress); // b2 DDGI 상수
				CommandList->SetPipelineState(Pipeline);
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 0, DepthSrv.Gpu);
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 1, NormalSrv.Gpu);
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 2, AccumPrevSrv.Gpu);  // t7
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 5, IrradianceSrv.Gpu); // t10
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 6, DistanceSrv.Gpu);   // t11
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 7, ProbeDataSrv.Gpu);  // t12
				DrawFullscreenTriangle(CommandList, Width, Height);
			});
		return CurrentRef;
	}

	const FRGResourceRef    TraceRef  = Graph.CreateTexture("RtAoTrace", FRGTextureDesc::MakeRenderTarget(Width, Height, AoTraceFormat));
	const FRGResourceRef    FilterRef = Graph.CreateTexture("RtAoFiltered", FRGTextureDesc::MakeRenderTarget(Width, Height, AoResultFormat));
	const FRGPooledTexture* Trace     = Graph.GetTexture(TraceRef);
	const FRGPooledTexture* Filtered  = Graph.GetTexture(FilterRef);
	// 1) 추적 (그래프 머티리얼 Masked 알파 테스트 → 변형 파이프라인)
	{
		ID3D12PipelineState* const TracePipeline = SelectPipelines(Scene).AoTrace.Get();
		FRenderGraph::FPassBuilder Pass          = Graph.AddPass("RTAO 추적");
		Scene.DeclareTraceReads(Pass, Refs.Tlas);
		Pass.Read(Refs.Depth, ERGAccess::SrvPixel)
			.Read(Refs.Normal, ERGAccess::SrvPixel)
			.Write(TraceRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
			.Timer(Timer)
			.Execute([this, &Scene, Lighting, Trace, TracePipeline, ViewAddress, LightingAddress, DepthSrv, NormalSrv, Width, Height](FRGContext& Context) {
				ID3D12GraphicsCommandList*        CommandList = Context.CommandList;
				const D3D12_CPU_DESCRIPTOR_HANDLE Rtv         = Trace->GetRtv();
				CommandList->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
				BindRoot(CommandList, ViewAddress, LightingAddress, Scene, Lighting);
				CommandList->SetPipelineState(TracePipeline);
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 0, DepthSrv.Gpu);
				CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 1, NormalSrv.Gpu);
				DrawFullscreenTriangle(CommandList, Width, Height);
			});
	}
	// 2) 공간 필터 (5x5 텐트 + 깊이·법선)
	Graph.AddPass("RTAO 공간 필터")
		.Read(Refs.Depth, ERGAccess::SrvPixel)
		.Read(Refs.Normal, ERGAccess::SrvPixel)
		.Read(TraceRef, ERGAccess::SrvPixel)
		.Write(FilterRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, &Scene, Lighting, Trace, Filtered, ViewAddress, LightingAddress, DepthSrv, NormalSrv, Width, Height](FRGContext& Context) {
			ID3D12GraphicsCommandList*        CommandList = Context.CommandList;
			const D3D12_CPU_DESCRIPTOR_HANDLE Rtv         = Filtered->GetRtv();
			CommandList->OMSetRenderTargets(1, &Rtv, FALSE, nullptr);
			BindRoot(CommandList, ViewAddress, LightingAddress, Scene, Lighting);
			CommandList->SetPipelineState(Pipelines.AoFilter.Get());
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 0, DepthSrv.Gpu);
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 1, NormalSrv.Gpu);
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 2, Trace->Srv.Gpu); // t7
			DrawFullscreenTriangle(CommandList, Width, Height);
		});
	// 3) 시간 누적 (이력 2장 핑퐁)
	const FRGResourceRef         PreviousRef = Graph.ImportColor("RtAoHistoryPrevious", Previous);
	const FD3D12DescriptorHandle PreviousSrv = Previous.GetSrv();
	Graph.AddPass("RTAO 누적")
		.Read(Refs.Depth, ERGAccess::SrvPixel)
		.Read(Refs.Velocity, ERGAccess::SrvPixel)
		.Read(FilterRef, ERGAccess::SrvPixel)
		.Read(PreviousRef, ERGAccess::SrvPixel)
		.Write(CurrentRef, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
		.Timer(Timer)
		.Execute([this, &Scene, &Current, Lighting, Filtered, ViewAddress, LightingAddress, DepthSrv, VelocitySrv, PreviousSrv, Width, Height](FRGContext& Context) {
			ID3D12GraphicsCommandList* CommandList = Context.CommandList;
			Current.Bind(CommandList, nullptr, false, false);
			BindRoot(CommandList, ViewAddress, LightingAddress, Scene, Lighting);
			CommandList->SetPipelineState(Pipelines.AoResolve.Get());
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 0, DepthSrv.Gpu);
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 2, Filtered->Srv.Gpu); // t7
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 3, PreviousSrv.Gpu);   // t8
			CommandList->SetGraphicsRootDescriptorTable(FRayTracingPassRoot::Root_Screen0 + 4, VelocitySrv.Gpu); // t9
			DrawFullscreenTriangle(CommandList, Width, Height);
		});
	return CurrentRef;
}
