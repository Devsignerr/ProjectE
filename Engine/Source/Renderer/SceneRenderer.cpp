#include "Renderer/SceneRenderer.h"

#include "Core/CommandLine.h"
#include "Core/Jobs/ParallelFor.h"
#include "Core/Paths.h"
#include "Core/Settings/ProjectSettings.h"
#include "Core/StringConv.h"
#include "Renderer/AssetCache.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/Camera.h"
#include "Renderer/LodMath.h"
#include "Renderer/Material.h"
#include "Renderer/MaterialRender.h"
#include "Renderer/PixelArtMath.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Renderer/ReflectionMath.h"
#include "Renderer/RenderProfiling.h"
#include "Renderer/RendererConsoleVariables.h"
#include "Renderer/TemporalMath.h"
#include "Renderer/UpscaleMath.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>
#include <tuple>
#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// Mesh.hlsl 루트 시그니처 레이아웃
	enum ERootParameter : uint32
	{
		RootParam_DrawConstants   = 0, // b0 (루트 상수 1개: 묶음의 인스턴스 번호 시작 위치)
		RootParam_PerFrame        = 1, // b1
		RootParam_Material        = 2, // b2
		RootParam_MaterialTexture = 3, // t0~t4 (머티리얼 텍스처 테이블)
		RootParam_Shadow          = 4, // b3 (캐스케이드 상수)
		RootParam_ShadowMap       = 5, // t8 (섀도우 맵 배열)
		RootParam_Ibl             = 6, // t5~t7
		RootParam_SkinPalette     = 7, // t15 (프레임 스킨 팔레트 구조화 버퍼, 정점 셰이더)
		RootParam_Cluster         = 8, // b5 (클러스터 상수)
		RootParam_LocalLights     = 9, // t9 (라이트 목록, 루트 SRV)
		RootParam_ClusterData     = 10, // t10 (클러스터별 라이트 인덱스, 루트 SRV)
		RootParam_LocalShadowMatrices = 11, // t11 (로컬 그림자 장별 뷰-투영, 루트 SRV)
		RootParam_LocalShadowMap      = 12, // t12 (로컬 그림자 타일 배열)
		RootParam_Instances           = 13, // t13 (인스턴스 목록, 정점 셰이더)
		RootParam_InstanceIndices     = 14, // t14 (패스의 인스턴스 번호 목록, 정점 셰이더)
		RootParam_AmbientOcclusion    = 15, // t16 (SSAO 결과 표, 픽셀)
		RootParam_DBufferA            = 16, // t17 (데칼 베이스색)
		RootParam_DBufferB            = 17, // t18 (데칼 법선)
		RootParam_DBufferC            = 18, // t19 (데칼 거칠기/금속)
		RootParam_ReflectionCaptures  = 19, // t20 (반사 캡처 목록, 루트 SRV, 픽셀)
		RootParam_CaptureAtlas        = 20, // t21 (반사 캡처 큐브 배열 표)
		RootParam_ScreenReflection    = 21, // t22 (SSR 결과 표)
		RootParam_Fog                 = 22, // b6 (안개 상수 — 반투명 패스, Fog.hlsli)
		RootParam_FogVolume           = 23, // t23 (볼류메트릭 안개 결과 표 — 반투명 패스)
		RootParam_MaterialGraphTextures = 24, // 공간 2 t0~ (그래프 머티리얼 텍스처 테이블, 무제한 범위 — MaterialCommon.hlsli)
		RootParam_RayTracedShadowMask   = 25, // t24 (RT 방향광 그림자 마스크 — PerFrame RayTracedShadows = 1일 때 불투명 메인 패스가 읽음, Phase 50)
		RootParam_LightTextures         = 26, // 공간 3 t0~ (셰이더 가시 힙 전체 — LTC 표·IES·쿠키, Lighting.hlsli LightTextures, Phase 52)
		RootParam_DdgiConstants         = 27, // b9 (DDGI 상수 — VolumeCount 0이면 셰이더는 예전 하늘 IBL 식, Phase 51)
		RootParam_DdgiIrradiance        = 28, // t40 (DDGI 조도 아틀라스)
		RootParam_DdgiDistance          = 29, // t41 (DDGI 거리 아틀라스)
		RootParam_DdgiProbeData         = 30, // t42 (DDGI 프로브 상태)
	};
} // namespace

bool FSceneRenderer::Init(FD3D12RHI& InRhi, FResourceManager& InResources)
{
	E_CHECKF(Rhi == nullptr, "씬 렌더러가 이미 초기화되어 있습니다");
	Rhi       = &InRhi;
	Resources = &InResources;
	// 리소스 수거 루트: 렌더러 내부 캐시가 지금 그리는 지형/폴리지 머티리얼 (씬 컴포넌트는 앱/편집기가 씬째 넣는다)
	ResourceRootProviderId = Resources->AddRootProvider([this](FResourceRoots& Roots) {
		TerrainRenderer.CollectResourceRoots(Roots);
		FoliageRenderer.CollectResourceRoots(Roots);
		LocalLightRenderer.CollectResourceRoots(Roots); // 라이트 쿠키 (Phase 52)
	});

	ID3D12Device* Device = Rhi->GetDevice().GetDevice();

	if (!ShaderCompiler.Init() || !ShaderLibrary.Init(ShaderCompiler))
	{
		return false;
	}

	const uint32 DrawConstantsIndex = RootSignature.AddConstants(1, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 PerFrameIndex  = RootSignature.AddConstantBufferView(1);
	const uint32 MaterialIndex  = RootSignature.AddConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 TextureIndex   = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MaterialSlot_Count, 0) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(DrawConstantsIndex == RootParam_DrawConstants && PerFrameIndex == RootParam_PerFrame &&
	        MaterialIndex == RootParam_Material && TextureIndex == RootParam_MaterialTexture);
	const uint32 ShadowIndex    = RootSignature.AddConstantBufferView(3, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 ShadowMapIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(ShadowIndex == RootParam_Shadow && ShadowMapIndex == RootParam_ShadowMap);
	const uint32 IblIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 5) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(IblIndex == RootParam_Ibl);
	const uint32 SkinPaletteIndex = RootSignature.AddShaderResourceView(15, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	E_CHECK(SkinPaletteIndex == RootParam_SkinPalette);
	const uint32 ClusterIndex     = RootSignature.AddConstantBufferView(5, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 LocalLightsIndex = RootSignature.AddShaderResourceView(9, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 ClusterDataIndex = RootSignature.AddShaderResourceView(10, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(ClusterIndex == RootParam_Cluster && LocalLightsIndex == RootParam_LocalLights && ClusterDataIndex == RootParam_ClusterData);
	const uint32 LocalShadowMatricesIndex = RootSignature.AddShaderResourceView(11, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 LocalShadowMapIndex      = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 12) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(LocalShadowMatricesIndex == RootParam_LocalShadowMatrices && LocalShadowMapIndex == RootParam_LocalShadowMap);
	const uint32 InstancesIndex       = RootSignature.AddShaderResourceView(13, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	const uint32 InstanceIndicesIndex = RootSignature.AddShaderResourceView(14, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	E_CHECK(InstancesIndex == RootParam_Instances && InstanceIndicesIndex == RootParam_InstanceIndices);
	const uint32 AmbientOcclusionIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 16, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) },
		D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(AmbientOcclusionIndex == RootParam_AmbientOcclusion);
	for (uint32 Index = 0; Index < 3; ++Index)
	{
		const uint32 DBufferIndex = RootSignature.AddDescriptorTable(
			{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 17 + Index, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) },
			D3D12_SHADER_VISIBILITY_PIXEL);
		E_CHECK(DBufferIndex == RootParam_DBufferA + Index);
	}
	const uint32 CapturesIndex = RootSignature.AddShaderResourceView(20, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 AtlasIndex    = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 21, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) }, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 SsrIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 22, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(CapturesIndex == RootParam_ReflectionCaptures && AtlasIndex == RootParam_CaptureAtlas && SsrIndex == RootParam_ScreenReflection);
	const uint32 FogIndex       = RootSignature.AddConstantBufferView(6, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 FogVolumeIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 23, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(FogIndex == RootParam_Fog && FogVolumeIndex == RootParam_FogVolume);
	const uint32 GraphTexturesIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE) },
		D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(GraphTexturesIndex == RootParam_MaterialGraphTextures);
	const uint32 RtShadowIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 24, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(RtShadowIndex == RootParam_RayTracedShadowMask);
	const uint32 LightTexturesIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 3, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE) },
		D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(LightTexturesIndex == RootParam_LightTextures);
	E_CHECK(RootSignature.AddConstantBufferView(9, 0, D3D12_SHADER_VISIBILITY_PIXEL) == RootParam_DdgiConstants);
	for (uint32 Index = 0; Index < 3; ++Index)
	{
		const uint32 DdgiIndex = RootSignature.AddDescriptorTable(
			{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 40 + Index, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE) },
			D3D12_SHADER_VISIBILITY_PIXEL);
		E_CHECK(DdgiIndex == RootParam_DdgiIrradiance + Index);
	}
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_ANISOTROPIC));

	// s2: 섀도우 비교 샘플러 (하드웨어 2x2 PCF, 범위 밖은 빛 받음)
	D3D12_STATIC_SAMPLER_DESC ShadowSamplerDesc = FD3D12RootSignature::MakeStaticSampler(
		2, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
	ShadowSamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	ShadowSamplerDesc.BorderColor    = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	ShadowSamplerDesc.MaxAnisotropy  = 1;
	RootSignature.AddStaticSampler(ShadowSamplerDesc);
	// s3: 안개 볼륨 (선형 클램프, Fog.hlsli FogLinearSampler)
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	if (!RootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"MeshRootSignature"))
	{
		return false;
	}

	for (uint32 Pass = 0; Pass < static_cast<uint32>(EMeshPass::Count); ++Pass)
	{
		for (uint32 Variant = 0; Variant < MaterialRender::VariantCount; ++Variant)
		{
			if (IsMeshPipelineUsed(static_cast<EMeshPass>(Pass), Variant) &&
			    !CreateMeshPipeline(MeshPipelines[Pass][Variant], static_cast<EMeshPass>(Pass), Variant))
			{
				return false;
			}
		}
	}
	if (!PostProcessor.Init(*Rhi, ShaderLibrary))
	{
		return false;
	}
	if (!ShadowRenderer.Init(*Rhi, ShaderLibrary) || !IblRenderer.Init(*Rhi, ShaderLibrary) || !LocalLightRenderer.Init(*Rhi, ShaderLibrary, *Resources) ||
	    !OcclusionCuller.Init(*Rhi, ShaderLibrary) || !ScreenPassRoot.Init(Device) || !TemporalAA.Init(*Rhi, ShaderLibrary, ScreenPassRoot) ||
	    !AmbientOcclusion.Init(*Rhi, ShaderLibrary, ScreenPassRoot) || !DecalRenderer.Init(*Rhi, ShaderLibrary, ScreenPassRoot) ||
	    !FogRenderer.Init(*Rhi, ShaderLibrary, ScreenPassRoot) || !ScreenSpaceReflections.Init(*Rhi, ShaderLibrary, ScreenPassRoot) ||
	    !ReflectionCaptures.Init(*Rhi, ShaderLibrary) || !SkyAtmosphere.Init(*Rhi, ShaderLibrary) || !Water.Init(*Rhi, ShaderLibrary) || !Clouds.Init(*Rhi, ShaderLibrary) ||
	    !RayTracingScene.Init(*Rhi, ShaderLibrary) || !RayTracingEffects.Init(*Rhi, ShaderLibrary) ||
	    !Ddgi.Init(*Rhi, ShaderLibrary, ScreenPassRoot, RayTracingEffects))
	{
		return false;
	}
	if (!ParticleRenderer.Init(*Rhi, ShaderLibrary, *Resources, SceneColorFormat, FD3D12RHI::DepthBufferFormat))
	{
		return false;
	}
	// 지형 (Phase 34): 그림자는 두 그림자 렌더러의 추가 캐스터 훅으로
	if (!TerrainRenderer.Init(*Rhi, ShaderLibrary, *Resources, SceneColorFormat, FD3D12RHI::DepthBufferFormat))
	{
		return false;
	}
	FoliageRenderer.Init(*Resources);
	ShadowRenderer.ExtraCasters = LocalLightRenderer.ExtraCasters = [this](ID3D12GraphicsCommandList* List, const FMatrix4x4& ViewProjection,
	                                                                       const FFrustum& Frustum, bool bLocalLight) {
		TerrainRenderer.RenderShadow(List, ViewProjection, Frustum, bLocalLight);
	};
	// 지형은 정적 그림자 캐스터 (방향광 그림자 캐시 — 높이 편집·LOD 변화는 상태 해시가 잡는다)
	ShadowRenderer.ExtraCasterState = [this](const FFrustum& Frustum) { return TerrainRenderer.GetShadowStateHash(Frustum); };

	GpuTimer.Init(Device, Rhi->GetGraphicsQueue().GetQueue(), FD3D12RHI::FrameCount, L"SceneRendererTimestamps"); // 실패해도 GPU 시간만 0
	ComputeGpuTimer.Init(Device, Rhi->GetComputeQueue().GetQueue(), FD3D12RHI::FrameCount, L"SceneRendererComputeTimestamps"); // 비동기 계산 패스 구간
	GraphPool.Init(*Rhi);

	RenderProfiling::AddRef(); // Tracy GPU 컨텍스트 공유 (Shutdown에서 Release)

	// 렌더 토글(--no-ssr 등)은 콘솔 변수 별칭 (RendererConsoleVariables.cpp) → Render마다 ApplyConsoleVariables
	const FCommandLine CommandLine = FCommandLine::FromProcess();
	PerfCapture                    = FPerfCapture{};
	PerfCapture.bEnabled           = CommandLine.HasFlag(L"--perf-capture");
	if (CommandLine.HasFlag(L"--bake-captures"))
	{
		bBakeCapturesRequested = true; // 첫 Render에서 반사 캡처 굽기 (자동 검증용)
	}
	if (const std::wstring Warmup = CommandLine.GetValue(L"--perf-warmup"); !Warmup.empty())
	{
		PerfCapture.WarmupFrames = static_cast<uint32>(std::max(0, std::stoi(Warmup)));
	}
	ApplyConsoleVariables();

	E_LOG(LogRenderer, Display, "씬 렌더러 초기화 완료 (HDR {}, 톤매핑)", "R16G16B16A16_FLOAT");
	return true;
}

void FSceneRenderer::ApplyConsoleVariables()
{
	bEnableOcclusion                = RendererCVars::Occlusion.Get();
	bSkinVisibilityCulling          = RendererCVars::SkinCulling.Get();
	ParticleRenderer.bEnableCulling = RendererCVars::ParticleCulling.Get();
	bDepthPrepass                   = RendererCVars::DepthPrepass.Get();
	DebugView                       = static_cast<uint32>(std::max(0, RendererCVars::DebugView.Get()));
	bTemporalJitter                 = RendererCVars::Jitter.Get();
	bEnableLod                      = RendererCVars::Lod.Get();
	bSkinnedLod                     = RendererCVars::SkinnedLod.Get();
	SkinnedLodScale                 = RendererCVars::SkinnedLodScale.Get();
	ForcedLod                       = RendererCVars::ForceLod.Get();
	LodHysteresis                   = RendererCVars::LodHysteresis.Get();
	MinScreenSize                   = RendererCVars::MinScreenSize.Get();
	MaxDrawDistance                 = RendererCVars::MaxDrawDistance.Get();
	ShadowStaticFrames              = static_cast<uint32>(std::max(1, RendererCVars::ShadowCacheStaticFrames.Get()));
	ShadowSettings.bCacheStatic     = RendererCVars::ShadowCache.Get();
	ShadowSettings.LodBias          = RendererCVars::ShadowLodBias.Get();
	ShadowSettings.MinCasterTexels  = RendererCVars::ShadowMinCasterTexels.Get();
	ShadowSettings.CacheQuantize      = RendererCVars::ShadowCacheQuantize.Get();
	ShadowSettings.CacheQuantizeFirst = static_cast<uint32>(std::max(0, RendererCVars::ShadowCacheQuantizeFirst.Get()));
	bConsoleTemporalAA              = RendererCVars::TemporalAA.Get();
	bConsoleAmbientOcclusion        = RendererCVars::AmbientOcclusion.Get();
	bConsoleReflections             = RendererCVars::Reflections.Get();
}

const char* GetRenderTimerName(ERenderTimer Timer)
{
	switch (Timer)
	{
	case ERenderTimer::Total:       return "전체";
	case ERenderTimer::Gather:      return "수집";
	case ERenderTimer::LocalLights: return "로컬 라이트";
	case ERenderTimer::Shadow:      return "방향광 그림자";
	case ERenderTimer::MainCull:    return "메인 컬링";
	case ERenderTimer::MainSort:    return "메인 정렬";
	case ERenderTimer::Occlusion:   return "오클루전 1단계";
	case ERenderTimer::Hzb:         return "HZB + 2단계";
	case ERenderTimer::MainDraw:    return "메인 드로우";
	case ERenderTimer::Particles:   return "파티클";
	case ERenderTimer::PostProcess: return "포스트";
	case ERenderTimer::DepthPrepass: return "깊이 사전";
	case ERenderTimer::TemporalAA:   return "TAA";
	case ERenderTimer::AmbientOcclusion: return "SSAO";
	case ERenderTimer::Decals:       return "데칼";
	case ERenderTimer::VolumetricFog: return "볼류메트릭 안개";
	case ERenderTimer::Fog:          return "안개 적용";
	case ERenderTimer::Reflections:  return "SSR";
	case ERenderTimer::Translucent:  return "반투명";
	case ERenderTimer::RayTracingBuild:      return "RT 가속 구조";
	case ERenderTimer::RayTracedShadows:     return "RT 그림자";
	case ERenderTimer::RayTracedReflections: return "RT 반사";
	case ERenderTimer::Atmosphere:   return "대기";
	case ERenderTimer::Clouds:       return "구름";
	case ERenderTimer::Water:        return "물";
	case ERenderTimer::DdgiTrace:    return "DDGI 추적";
	case ERenderTimer::DdgiBlend:    return "DDGI 누적";
	case ERenderTimer::RayTracedAmbientOcclusion: return "RTAO";
	default:                        return "?";
	}
}

void FSceneRenderer::BeginTimer(ERenderTimer Timer)
{
	const uint32 Index  = static_cast<uint32>(Timer);
	TimerStarts[Index] = FClock::now();
	GpuTimer.BeginScope(Rhi->GetCommandList(), Index);
	RenderProfiling::BeginZone(Rhi->GetCommandList(), Index, GetRenderTimerName(Timer), true);
}

void FSceneRenderer::BeginCpuTimer(ERenderTimer Timer)
{
	TimerStarts[static_cast<uint32>(Timer)] = FClock::now();
	RenderProfiling::BeginZone(nullptr, static_cast<uint32>(Timer), GetRenderTimerName(Timer), false);
}

void FSceneRenderer::EndCpuTimer(ERenderTimer Timer)
{
	const uint32 Index = static_cast<uint32>(Timer);
	Stats.CpuMs[Index] += std::chrono::duration<float, std::milli>(FClock::now() - TimerStarts[Index]).count();
	RenderProfiling::EndZone(Index);
}

void FSceneRenderer::EndTimer(ERenderTimer Timer)
{
	const uint32 Index = static_cast<uint32>(Timer);
	Stats.CpuMs[Index] += std::chrono::duration<float, std::milli>(FClock::now() - TimerStarts[Index]).count();
	RenderProfiling::EndZone(Index);
	GpuTimer.EndScope(Rhi->GetCommandList(), Index);
}

void FSceneRenderer::AccumulatePerfCapture()
{
	if (!PerfCapture.bEnabled)
	{
		return;
	}
	if (PerfCapture.SeenFrames++ < PerfCapture.WarmupFrames)
	{
		return;
	}
	FPerfCapture& Capture = PerfCapture;
	++Capture.Frames;
	for (uint32 Index = 0; Index < static_cast<uint32>(ERenderTimer::Count); ++Index)
	{
		Capture.CpuMs[Index] += Stats.CpuMs[Index];
		Capture.GpuMs[Index] += Stats.GpuMs[Index];
	}
	Capture.FrameMs += Stats.FrameIntervalMs;
	Capture.DrawCalls += Stats.DrawCalls;
	Capture.ShadowDrawCalls += Stats.ShadowDrawCalls;
	Capture.PrepassDrawCalls += Stats.PrepassDrawCalls;
	Capture.Triangles += static_cast<double>(Stats.Triangles);
	Capture.ShadowTriangles += static_cast<double>(Stats.ShadowTriangles);
	Capture.VisibleMeshes += Stats.VisibleMeshes;
	Capture.OcclusionTested += Stats.OcclusionTested;
	Capture.OcclusionDrawn += Stats.OcclusionPhase1 + Stats.OcclusionPhase2;
	Capture.OcclusionPhase2 += Stats.OcclusionPhase2;
	Capture.TotalMeshes = Stats.TotalMeshes;
	Capture.SkinnedDrawn += Stats.SkinnedDrawn;
	Capture.SkinPalettes += Stats.SkinPalettes;
	Capture.SkinnedCulled += Stats.SkinnedCulled;
	Capture.UploadBytes += static_cast<double>(Stats.UploadBytes);
	Capture.ShadowCacheReused += Stats.ShadowCacheReused;
	Capture.ShadowCacheRebuilt += Stats.ShadowCacheRebuilt;
	Capture.ScreenSizeCulled += Stats.ScreenSizeCulled;
}

void FSceneRenderer::LogPerfCapture() const
{
	const FPerfCapture& Capture = PerfCapture;
	if (!Capture.bEnabled || Capture.Frames == 0)
	{
		return;
	}
	const double Count = static_cast<double>(Capture.Frames);
	std::string  Cpu;
	std::string  Gpu;
	for (uint32 Index = 0; Index < static_cast<uint32>(ERenderTimer::Count); ++Index)
	{
		const char* Name = GetRenderTimerName(static_cast<ERenderTimer>(Index));
		Cpu += std::format("{}{} {:.3f}", Cpu.empty() ? "" : ", ", Name, Capture.CpuMs[Index] / Count);
		Gpu += std::format("{}{} {:.3f}", Gpu.empty() ? "" : ", ", Name, Capture.GpuMs[Index] / Count);
	}
#if E_DEBUG
	const char* Config = "Debug";
#else
	const char* Config = "Release";
#endif
	E_LOG(LogRenderer, Display, "[성능] 화면 비율 {:.0f}% (씬 {}x{}){}", Stats.ScreenPercentage, Stats.InternalWidth, Stats.InternalHeight,
	      bDynamicResolutionActive ? std::format(", 동적 해상도 변경 {}회", DynamicResolution.GetChangeCount()) : std::string());
	E_LOG(LogRenderer, Display,
	      "[성능] {} 프레임 평균 ({}): 프레임 {:.3f} ms, 드로우 {:.1f} (그림자 {:.1f}, 깊이 사전 {:.1f}), 삼각형 {:.0f} (그림자 {:.0f}), 메시 {:.1f}/{}",
	      Capture.Frames, Config, Capture.FrameMs / Count, Capture.DrawCalls / Count, Capture.ShadowDrawCalls / Count, Capture.PrepassDrawCalls / Count,
	      Capture.Triangles / Count, Capture.ShadowTriangles / Count, Capture.VisibleMeshes / Count, Capture.TotalMeshes);
	if (Capture.OcclusionTested > 0.0)
	{
		E_LOG(LogRenderer, Display, "[성능] 오클루전: 정적 인스턴스 {:.1f} 중 그림 {:.1f} (2단계 {:.2f}), 가려짐 {:.1f}", Capture.OcclusionTested / Count,
		      Capture.OcclusionDrawn / Count, Capture.OcclusionPhase2 / Count, (Capture.OcclusionTested - Capture.OcclusionDrawn) / Count);
	}
	E_LOG(LogRenderer, Display, "[성능] 스킨 메시: 엔티티 {:.1f}, 팔레트 {:.1f}, 가시성 제외 {:.1f}, 씬 렌더러 업로드 {:.1f} KB", Capture.SkinnedDrawn / Count,
	      Capture.SkinPalettes / Count, Capture.SkinnedCulled / Count, Capture.UploadBytes / Count / 1024.0);
	E_LOG(LogRenderer, Display, "[성능] 그림자 캐시: 캐스케이드 재사용 {:.2f}, 다시 그림 {:.2f} / 프레임, 화면 크기·거리 컬링 {:.1f}", Capture.ShadowCacheReused / Count,
	      Capture.ShadowCacheRebuilt / Count, Capture.ScreenSizeCulled / Count);
	if (Stats.bRayTracedShadows || Stats.bRayTracedReflections)
	{
		LogRayTracingStats(); // 마지막 프레임 가속 구조 상태 (BLAS/TLAS 크기)
	}
	if (Stats.Ddgi.bActive)
	{
		LogDdgiStats();
	}
	E_LOG(LogRenderer, Display, "[성능] CPU ms: {}", Cpu);
	E_LOG(LogRenderer, Display, "[성능] GPU ms: {}", Gpu);
}

bool FSceneRenderer::IsMeshPipelineUsed(EMeshPass Pass, uint32 Variant)
{
	return Pass != EMeshPass::Wireframe || (Variant & ~MaterialRender::VariantSkinned) == 0;
}

void FSceneRenderer::GetMeshShaderDescs(EMeshPass Pass, uint32 Variant, FShaderCompileDesc& OutVertex, FShaderCompileDesc& OutPixel)
{
	const bool bVariantBit = (Variant & MaterialRender::VariantMaskedOrAdditive) != 0; // 불투명 패스: Masked, 반투명 패스: 가산
	OutVertex            = FShaderCompileDesc{};
	OutVertex.FileName   = L"Mesh.hlsl";
	OutVertex.EntryPoint = (Variant & MaterialRender::VariantSkinned) != 0 ? L"VSSkinned" : L"VSMain";
	OutVertex.Stage      = EShaderStage::Vertex;
	OutPixel             = OutVertex;
	OutPixel.Stage       = EShaderStage::Pixel;
	switch (Pass)
	{
	case EMeshPass::Prepass:     OutPixel.EntryPoint = bVariantBit ? L"PSPrepassMasked" : L"PSPrepass"; break;
	case EMeshPass::Translucent: OutPixel.EntryPoint = bVariantBit ? L"PSAdditive" : L"PSTranslucent"; break;
	case EMeshPass::Wireframe:   OutPixel.EntryPoint = L"PSMain"; break;
	default:                     OutPixel.EntryPoint = bVariantBit ? L"PSMainMasked" : L"PSMain"; break;
	}
}

bool FSceneRenderer::CreateMeshPipeline(FD3D12PipelineState& OutPipeline, EMeshPass Pass, uint32 Variant, const FMaterialShader* GraphShader)
{
	FShaderCompileDesc VertexDesc;
	FShaderCompileDesc PixelDesc;
	GetMeshShaderDescs(Pass, Variant, VertexDesc, PixelDesc);
	if (GraphShader != nullptr)
	{
		PixelDesc = MaterialRender::MakeGraphShaderDesc(PixelDesc.FileName.c_str(), PixelDesc.EntryPoint.c_str(), EShaderStage::Pixel, *GraphShader);
	}
	const ComPtr<IDxcBlob> VertexShader = ShaderLibrary.GetShader(VertexDesc);
	const ComPtr<IDxcBlob> PixelShader  = ShaderLibrary.GetShader(PixelDesc);
	if (!VertexShader || !PixelShader)
	{
		return false;
	}

	const bool            bSkinned  = (Variant & MaterialRender::VariantSkinned) != 0;
	const bool            bTwoSided = (Variant & MaterialRender::VariantTwoSided) != 0;
	const bool            bVariantBit = (Variant & MaterialRender::VariantMaskedOrAdditive) != 0;
	FGraphicsPipelineDesc PsoDesc;
	PsoDesc.RootSignature          = RootSignature.Get();
	PsoDesc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	PsoDesc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	PsoDesc.InputLayout            = bSkinned ? FStaticMesh::GetSkinnedInputLayout() : FStaticMesh::GetInputLayout();
	PsoDesc.RenderTargetFormats[0] = SceneColorFormat;
	PsoDesc.DepthStencilFormat     = FD3D12RHI::DepthBufferFormat;
	PsoDesc.bDepthEnable           = true;
	PsoDesc.CullMode               = bTwoSided ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK; // 양면: 뒷면은 셰이더가 법선을 뒤집는다
	const wchar_t* PassName        = L"Main";
	switch (Pass)
	{
	case EMeshPass::MainDepthEqual:
		PsoDesc.DepthFunc   = D3D12_COMPARISON_FUNC_EQUAL; // 사전 패스와 같은 정점 셰이더 → 같은 깊이
		PsoDesc.bDepthWrite = false;
		PassName            = L"DepthEqual";
		break;
	case EMeshPass::Wireframe:
		PsoDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
		PsoDesc.CullMode = D3D12_CULL_MODE_NONE;
		PassName         = L"Wireframe";
		break;
	case EMeshPass::Prepass:
		PsoDesc.NumRenderTargets       = 2;
		PsoDesc.RenderTargetFormats[0] = SceneNormalFormat;
		PsoDesc.RenderTargetFormats[1] = SceneVelocityFormat;
		PassName                       = L"Prepass";
		break;
	case EMeshPass::Translucent:
		// 알파 블렌드의 알파 = 덮인 정도 누적 (TAA 반응형 마스크), 가산은 색·알파 모두 더한다 (파티클과 같음)
		PsoDesc.bDepthWrite = false;
		PsoDesc.BlendMode   = bVariantBit ? EBlendMode::Additive : EBlendMode::Alpha;
		PassName            = bVariantBit ? L"Additive" : L"Translucent";
		break;
	default:
		break;
	}
	std::wstring DebugName = std::format(L"Mesh{}{}{}{}Pipeline", PassName, bVariantBit && Pass != EMeshPass::Translucent ? L"Masked" : L"",
	                                     bTwoSided ? L"TwoSided" : L"", bSkinned ? L"Skinned" : L"");
	if (GraphShader != nullptr)
	{
		DebugName += std::format(L"_Graph{:016x}", GraphShader->Hash);
	}
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), PsoDesc, DebugName.c_str());
}

bool FSceneRenderer::ReloadShaders(bool bForceRecompile)
{
	E_CHECKF(Rhi != nullptr, "씬 렌더러가 초기화되지 않았습니다");

	// 강제 재컴파일은 셰이더(엔트리)마다 한 번: 정점 2 (VSMain/VSSkinned) × 픽셀 6 (패스·변형별 엔트리)
	if (bForceRecompile)
	{
		std::vector<std::wstring> Cooked;
		for (uint32 Pass = 0; Pass < static_cast<uint32>(EMeshPass::Count); ++Pass)
		{
			for (uint32 Variant = 0; Variant < MaterialRender::VariantCount; ++Variant)
			{
				FShaderCompileDesc Descs[2];
				GetMeshShaderDescs(static_cast<EMeshPass>(Pass), Variant, Descs[0], Descs[1]);
				for (const FShaderCompileDesc& Desc : Descs)
				{
					if (std::find(Cooked.begin(), Cooked.end(), Desc.EntryPoint) != Cooked.end())
					{
						continue;
					}
					Cooked.push_back(Desc.EntryPoint);
					if (!ShaderLibrary.CookShader(Desc))
					{
						E_LOG(LogRenderer, Error, "메시 셰이더 다시 컴파일 실패 ({}): 기존 파이프라인을 유지합니다", FStringConv::ToUtf8(Desc.EntryPoint));
						return false;
					}
				}
			}
		}
	}

	ReleaseGraphPipelines(); // 그래프 머티리얼 PSO는 다음 그리기에 새 셰이더로 다시 만든다

	// 패스·변형별로 새 PSO를 만들고 성공한 것만 교체 (이전 PSO는 진행 중인 프레임이 참조할 수 있으므로 지연 해제)
	bool bMeshOk = true;
	for (uint32 Pass = 0; Pass < static_cast<uint32>(EMeshPass::Count); ++Pass)
	{
		for (uint32 Variant = 0; Variant < MaterialRender::VariantCount; ++Variant)
		{
			if (!IsMeshPipelineUsed(static_cast<EMeshPass>(Pass), Variant))
			{
				continue;
			}
			FD3D12PipelineState NewPipeline;
			if (!CreateMeshPipeline(NewPipeline, static_cast<EMeshPass>(Pass), Variant))
			{
				E_LOG(LogRenderer, Error, "메시 셰이더 다시 로드 실패 (패스 {}, 변형 {}): 기존 파이프라인을 유지합니다", Pass, Variant);
				bMeshOk = false;
				continue;
			}
			MeshPipelines[Pass][Variant].Swap(NewPipeline);
			Rhi->DeferRelease(NewPipeline.Detach());
		}
	}
	if (!bMeshOk)
	{
		return false;
	}

	if (!ShadowRenderer.ReloadShaders(bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "섀도우 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	if (!IblRenderer.ReloadShaders(bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "IBL 셰이더 다시 로드 실패: 기존 환경광을 유지합니다");
		return false;
	}
	if (!PostProcessor.ReloadShaders(bForceRecompile))
	{
		E_LOG(LogRenderer, Error, "포스트 프로세스 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}
	if (!ParticleRenderer.ReloadShaders(bForceRecompile))
	{
		return false;
	}
	if (!LocalLightRenderer.ReloadShaders(bForceRecompile))
	{
		return false;
	}
	if (!TemporalAA.ReloadShaders(bForceRecompile) || !AmbientOcclusion.ReloadShaders(bForceRecompile) || !DecalRenderer.ReloadShaders(bForceRecompile) ||
	    !FogRenderer.ReloadShaders(bForceRecompile) || !ScreenSpaceReflections.ReloadShaders(bForceRecompile) ||
	    !ReflectionCaptures.ReloadShaders(bForceRecompile) || !SkyAtmosphere.ReloadShaders(bForceRecompile) || !Water.ReloadShaders(bForceRecompile) || !Clouds.ReloadShaders(bForceRecompile))
	{
		return false;
	}
	if (!OcclusionCuller.ReloadShaders(bForceRecompile))
	{
		return false;
	}
	if (!TerrainRenderer.ReloadShaders(bForceRecompile))
	{
		return false;
	}
	if (!RayTracingScene.ReloadShaders(bForceRecompile) || !RayTracingEffects.ReloadShaders(bForceRecompile) || !Ddgi.ReloadShaders(bForceRecompile))
	{
		return false;
	}

	E_LOG(LogRenderer, Display, "셰이더 다시 로드 완료 (메시 파이프라인 재생성)");
	return true;
}

void FSceneRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	Rhi->GetComputeQueue().Flush();
	Rhi->GetGraphicsQueue().Flush();
	LogPerfCapture();
	GpuTimer.Shutdown();
	ComputeGpuTimer.Shutdown();
	RenderProfiling::Release(); // GPU Flush 뒤 (마지막 렌더러면 Tracy GPU 컨텍스트 파괴)
	GraphPool.Shutdown();
	SceneColor.reset();
	SceneNormal.reset();
	SceneVelocity.reset();
	MotionHistory.clear();
	bHasPrevView = false;
	PostProcessor.Shutdown();
	ShadowRenderer.Shutdown();
	IblRenderer.SetLightingOverride(nullptr);
	SkyAtmosphere.Shutdown();
	Water.Shutdown();
	Clouds.Shutdown();
	IblRenderer.Shutdown();
	ParticleRenderer.Shutdown();
	TerrainRenderer.Shutdown();
	FoliageRenderer.Shutdown();
	LocalLightRenderer.Shutdown();
	OcclusionCuller.Shutdown();
	TemporalAA.Shutdown();
	AmbientOcclusion.Shutdown();
	DecalRenderer.Shutdown();
	FogRenderer.Shutdown();
	ScreenSpaceReflections.Shutdown();
	ReflectionCaptures.Shutdown();
	RayTracingScene.Shutdown();
	Ddgi.Shutdown();
	RayTracingEffects.Shutdown();
	ScreenPassRoot.Shutdown();
	for (auto& PassPipelines : MeshPipelines)
	{
		for (FD3D12PipelineState& Pipeline : PassPipelines)
		{
			Pipeline.Shutdown();
		}
	}
	GraphPipelines.clear(); // GPU Flush 이후
	RootSignature.Shutdown();
	ShaderLibrary.Shutdown();
	ShaderCompiler.Shutdown();
	MainBatches.Reset();
	TranslucentBatches.Reset();
	Rhi       = nullptr;
	Resources->RemoveRootProvider(ResourceRootProviderId);
	ResourceRootProviderId = 0;
	Resources = nullptr;
}

void FSceneRenderer::SetFreezeCulling(bool bFreeze)
{
	bCullingFrozen = bFreeze;
}

D3D12_CPU_DESCRIPTOR_HANDLE FSceneRenderer::GetOverlayDepthDsv(uint32 Width, uint32 Height) const
{
	if (SceneColor && SceneColor->GetDesc().bWithDepth && SceneColor->GetWidth() == Width && SceneColor->GetHeight() == Height)
	{
		return SceneColor->GetDsv();
	}
	if (const FD3D12RenderTarget* Overlay = TemporalAA.GetOverlayDepth();
	    bFrameUpscaled && Overlay != nullptr && Overlay->GetWidth() == Width && Overlay->GetHeight() == Height)
	{
		return Overlay->GetDsv();
	}
	return D3D12_CPU_DESCRIPTOR_HANDLE{};
}

float FSceneRenderer::ComputeScreenPercentage()
{
	if (!bAllowScreenPercentage || bWireframe)
	{
		bDynamicResolutionActive = false;
		return 100.0f;
	}
	if (!RendererCVars::DynamicResolution.Get())
	{
		bDynamicResolutionActive = false;
		return FUpscaleMath::ClampScreenPercentage(RendererCVars::ScreenPercentage.Get());
	}
	// 동적 해상도: GPU 씬 렌더 시간(타이머 — 몇 프레임 늦은 값)으로 5% 단계 조절. 켤 때는 최대 비율부터
	FDynamicResolutionSettings Settings;
	Settings.TargetGpuMs   = RendererCVars::DynamicResolutionTargetMs.Get();
	Settings.MinPercentage = RendererCVars::DynamicResolutionMin.Get();
	Settings.MaxPercentage = RendererCVars::DynamicResolutionMax.Get();
	if (!bDynamicResolutionActive)
	{
		DynamicResolution.Reset(FUpscaleMath::ClampScreenPercentage(FMath::Max(Settings.MinPercentage, Settings.MaxPercentage)));
		bDynamicResolutionActive = true;
	}
	const float Previous   = DynamicResolution.GetPercentage();
	const float Percentage = DynamicResolution.Update(Stats.GetGpuMs(ERenderTimer::Total), Settings);
	if (Percentage != Previous)
	{
		E_LOG(LogRenderer, Log, "[동적 해상도] {:.0f}% → {:.0f}% (GPU 평활 {:.2f}ms, 목표 {:.2f}ms)", Previous, Percentage,
		      DynamicResolution.GetSmoothedGpuMs(), Settings.TargetGpuMs);
	}
	return Percentage;
}

void FSceneRenderer::EnsureSceneColor(uint32 Width, uint32 Height)
{
	FRenderTargetDesc SceneDesc = FRenderTargetDesc::MakeHdr(true);
	std::memcpy(SceneDesc.ClearColor, &BackgroundColor.X, sizeof(SceneDesc.ClearColor));
	SceneDesc.ClearColor[3] = 0.0f; // 알파 = TAA 반응형 마스크
	EnsureTarget(SceneColor, Width, Height, L"SceneColorHDR", SceneDesc);

	FRenderTargetDesc NormalDesc = FRenderTargetDesc::MakeColor(SceneNormalFormat);
	NormalDesc.ClearColor[0]     = 0.5f; // 팔면체 (0, 0) = +Z
	NormalDesc.ClearColor[1]     = 0.5f;
	EnsureTarget(SceneNormal, Width, Height, L"SceneNormal", NormalDesc);
	EnsureTarget(SceneVelocity, Width, Height, L"SceneVelocity", FRenderTargetDesc::MakeColor(SceneVelocityFormat));
	AmbientOcclusion.EnsureTargets(Width, Height, AoResolutionDivisor);
	DecalRenderer.EnsureTargets(Width, Height);
	ScreenSpaceReflections.EnsureTargets(Width, Height);
}

void FSceneRenderer::EnsureTarget(std::unique_ptr<FD3D12RenderTarget>& Target, uint32 Width, uint32 Height, const wchar_t* DebugName,
                                  const FRenderTargetDesc& Desc)
{
	if (Target && Target->GetWidth() == Width && Target->GetHeight() == Height)
	{
		return;
	}
	// 이전 타깃은 진행 중인 프레임이 참조할 수 있으므로 지연 해제
	if (Target)
	{
		Target->ShutdownDeferred(*Rhi);
	}
	Target = std::make_unique<FD3D12RenderTarget>();
	if (!Target->Init(Rhi->GetDevice(), Rhi->GetSrvAllocator(), Width, Height, DebugName, Desc))
	{
		E_LOG(LogRenderer, Fatal, "렌더 타깃 생성 실패 ({}x{})", Width, Height);
	}
}

namespace
{
	// 씬에서 처음 찾은 활성 픽셀 아트 설정 (없으면 nullptr)
	const FPixelArtComponent* FindPixelArtSettings(FScene& Scene)
	{
		const FPixelArtComponent* Found = nullptr;
		Scene.GetRegistry().View<FPixelArtComponent>().Each([&](FEntity, FPixelArtComponent& PixelArt) {
			if (Found == nullptr && PixelArt.bEnabled)
			{
				Found = &PixelArt;
			}
		});
		return Found;
	}
} // namespace

void FSceneRenderer::Render(FScene& Scene, const FCamera& Camera, const FRenderOutput& Output)
{
	E_CHECKF(Rhi != nullptr, "씬 렌더러가 초기화되지 않았습니다");
	E_CHECKF(Output.IsValid(), "씬 렌더러 출력 대상이 유효하지 않습니다");

	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	ApplyConsoleVariables(); // 콘솔에서 바꾼 값은 이번 프레임부터
	RenderProfiling::BeginFrame(Rhi->GetDevice().GetDevice(), Rhi->GetGraphicsQueue().GetQueue(), Rhi->GetFrameNumber());

	// 측정: 지난 결과(GPU는 슬롯 수만큼 늦음)를 통계에 옮기고 이번 프레임 칸을 비운다 (계산 큐 구간은 계산 큐 타이머)
	const FClock::time_point Now = FClock::now();
	Stats.FrameIntervalMs        = bHasLastRenderTime ? std::chrono::duration<float, std::milli>(Now - LastRenderTime).count() : 0.0f;
	LastRenderTime               = Now;
	bHasLastRenderTime           = true;
	std::fill(std::begin(Stats.CpuMs), std::end(Stats.CpuMs), 0.0f);
	const bool bGpuTiming = GpuTimer.BeginFrame(Rhi->GetFrameSlot(), Rhi->GetFrameNumber());
	ComputeGpuTimer.BeginFrame(Rhi->GetFrameSlot(), Rhi->GetFrameNumber());
	if (bGpuTiming)
	{
		for (uint32 Index = 0; Index < static_cast<uint32>(ERenderTimer::Count); ++Index)
		{
			Stats.GpuMs[Index] = GpuTimer.GetScopeMs(Index) + ComputeGpuTimer.GetScopeMs(Index);
		}
	}
	BeginTimer(ERenderTimer::Total);
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	const uint64               UploadStart   = DynamicBuffer.GetUsed();

	// 한 Rhi 프레임에 몇 번 불렸는지 (여러 뷰/씬을 번갈아 그리는 렌더러는 시간 이력을 쓰지 않는다)
	const uint64 FrameNumber = Rhi->GetFrameNumber();
	if (FrameNumber != CurrentFrameNumber)
	{
		ViewsLastFrame     = CurrentFrameNumber + 1 == FrameNumber ? ViewsThisFrame : 0;
		CurrentFrameNumber = FrameNumber;
		ViewsThisFrame     = 0;
		GraphPool.Trim(FrameNumber); // 오래 안 쓴 그래프 풀 텍스처 정리 (프레임당 한 번)
	}
	++ViewsThisFrame;

	// HDR 디스플레이 출력 (Phase 49): 출력이 RHI의 HDR 씬 타깃이면 톤매핑을 HDR 곡선으로 (선형, 1 = 종이 흰색)
	const bool bHdrOutput = Rhi->IsHdrOutputActive() && Output.Resource != nullptr && Output.Resource == Rhi->GetHdrSceneResource();
	PostProcessor.SetHdrPeakRatio(bHdrOutput ? Rhi->GetHdrMaxNits() / FMath::Max(Rhi->GetHdrPaperWhiteNits(), 1.0f) : 0.0f);

	// 하늘 환경맵 (하늘광 EnvironmentMap/회전이 바뀌면 IBL 다시 생성)
	UpdateEnvironment(Scene);

	// 반사 캡처: 끝난 굽기 저장 + 요청된 굽기 (큐브 면 6개를 면마다 그래프로 이번 프레임 명령 목록에 먼저 그린다)
	ReflectionCaptures.ProcessPendingSaves();
	if (bBakeCapturesRequested)
	{
		bBakeCapturesRequested = false;
		BakeReflectionCaptures(Scene);
	}

	// 렌더 그래프: 패스 등록(CPU 준비 포함) → 컴파일(컬링·전이·비동기 포크/조인) → 실행(기록)
	{
		FRenderGraph Graph(*Rhi, GraphPool, "SceneRenderer");
		SetupGraph(Graph);
		RenderFrame(Graph, Scene, Camera, Output);
		ExecuteGraph(Graph);
	}
	if (bPendingSnapRestore)
	{
		PixelArtObjectSnap.Restore(Scene); // 씬 렌더(그래프 실행) 동안만 스냅 위치
		bPendingSnapRestore = false;
	}
	FinalizeFrameStats();
	Stats.UploadBytes = DynamicBuffer.GetUsed() - UploadStart;
	if (const uint32 Serial = RendererCVars::GetRayTracingStatsSerial(); Serial != SeenRtStatsSerial)
	{
		SeenRtStatsSerial = Serial;
		LogRayTracingStats();
	}
	if (const uint32 Serial = RendererCVars::GetDdgiStatsSerial(); Serial != SeenDdgiStatsSerial)
	{
		SeenDdgiStatsSerial = Serial;
		LogDdgiStats();
	}

	// 그래프가 마지막에 출력 RTV를 바인딩한 채 끝나지 않을 수 있다 (출력 대상 바인딩 보장 — 에디터 오버레이가 이어서 그린다)
	CommandList->OMSetRenderTargets(1, &Output.Rtv, FALSE, nullptr);
	SetScreenPassViewport(CommandList, Output.Width, Output.Height);

	EndTimer(ERenderTimer::Total);
	GpuTimer.EndFrame(CommandList);
	if (bGpuTiming)
	{
		AccumulatePerfCapture();
	}
}

void FSceneRenderer::LogRayTracingStats() const
{
	if (!bAllowRayTracing)
	{
		return;
	}
	const FRayTracingSceneStats& Rt = Stats.RayTracing;
	E_LOG(LogRenderer, Display,
	      "[레이 트레이싱] 지원 {}, 그림자 {}, 반사 {} | TLAS 인스턴스 {}, BLAS 정적 {} + 스킨 {}(지오메트리 {}, 갱신 건너뜀 {}) + 지형 타일 {} = {:.2f} MB (압축 절약 누적 {:.2f} MB), "
	      "스킨 정점 {:.2f} MB, 지형 정점 {:.2f} MB, "
	      "TLAS {:.2f} MB, 스크래치 {:.2f} MB | 이번 프레임 빌드 {} / 갱신 {} / 압축 {} / 미룸 {}, 준비 CPU {:.3f} ms | GPU ms: 가속 구조 {:.3f}, 그림자 {:.3f}, 반사 추적 {:.3f}, 반사 흐림·누적 {:.3f}",
	      RayTracingScene.IsSupported(), Stats.bRayTracedShadows, Stats.bRayTracedReflections, Rt.TlasInstances, Rt.StaticBlas, Rt.SkinnedBlas, Rt.SkinnedPrimitives, Rt.SkinnedRefitSkipped, Rt.TerrainTiles,
	      static_cast<double>(Rt.BlasBytes) / (1024.0 * 1024.0), static_cast<double>(Rt.CompactionSavedBytes) / (1024.0 * 1024.0),
	      static_cast<double>(Rt.SkinnedVertexBytes) / (1024.0 * 1024.0), static_cast<double>(Rt.TerrainVertexBytes) / (1024.0 * 1024.0),
	      static_cast<double>(Rt.TlasBytes) / (1024.0 * 1024.0),
	      static_cast<double>(Rt.ScratchBytes) / (1024.0 * 1024.0), Rt.BuiltThisFrame, Rt.RefitThisFrame, Rt.CompactedThisFrame, Rt.PendingBuilds, Rt.PrepareCpuMs,
	      Stats.GetGpuMs(ERenderTimer::RayTracingBuild), Stats.GetGpuMs(ERenderTimer::RayTracedShadows), Stats.GetGpuMs(ERenderTimer::RayTracedReflections),
	      Stats.GetGpuMs(ERenderTimer::Reflections));
	FD3D12Device::FVideoMemoryInfo Memory;
	if (Rhi->GetDevice().QueryVideoMemory(Memory))
	{
		E_LOG(LogRenderer, Display, "[레이 트레이싱] VRAM 사용 {:.1f} MB (예산 {:.1f} MB)", static_cast<double>(Memory.LocalUsage) / (1024.0 * 1024.0),
		      static_cast<double>(Memory.LocalBudget) / (1024.0 * 1024.0));
	}
}

void FSceneRenderer::LogDdgiStats() const
{
	if (!bAllowRayTracing)
	{
		return;
	}
	const FDdgiStats& Info = Stats.Ddgi;
	E_LOG(LogRenderer, Display,
	      "[DDGI] 지원 {}, 활성 {} | 볼륨 {}, 프로브 {}, 이번 프레임 갱신 {} (광선 {}), 아틀라스 조도 {}x{} + 거리 {}x{} (이력 2장 + 상태) = {:.2f} MB | "
	      "GPU ms: 추적 {:.3f}, 누적 {:.3f}",
	      Ddgi.IsSupported(), Info.bActive, Info.Volumes, Info.Probes, Info.UpdatedProbes, Info.Rays, Info.IrradianceWidth, Info.IrradianceHeight,
	      Info.DistanceWidth, Info.DistanceHeight, static_cast<double>(Info.AtlasBytes) / (1024.0 * 1024.0), Stats.GetGpuMs(ERenderTimer::DdgiTrace),
	      Stats.GetGpuMs(ERenderTimer::DdgiBlend));
}

void FSceneRenderer::SetupGraph(FRenderGraph& Graph)
{
	Graph.OnTimerBegin = [this](int32 Timer, ID3D12GraphicsCommandList* List, bool bCompute) {
		BeginGraphTimer(static_cast<ERenderTimer>(Timer), List, bCompute);
	};
	Graph.OnTimerEnd = [this](int32 Timer, ID3D12GraphicsCommandList* List, bool bCompute) {
		EndGraphTimer(static_cast<ERenderTimer>(Timer), List, bCompute);
	};
	Graph.OnLastComputeBatchEnd = [this](ID3D12GraphicsCommandList* List) { ComputeGpuTimer.EndFrame(List); };
}

void FSceneRenderer::ExecuteGraph(FRenderGraph& Graph)
{
	FRGCompileOptions Options;
	Options.bCullPasses   = RendererCVars::RenderGraphCull.Get();
	Options.bAsyncCompute = RendererCVars::RenderGraphAsyncCompute.Get();
	Graph.Compile(Options);
	Graph.Execute();
	LastGraphStats = Graph.GetStats();
	// 콘솔 r.RenderGraph.Dump: 요청 뒤 이 렌더러가 처음 실행하는 그래프를 로그로
	if (const uint32 Serial = RendererCVars::GetRenderGraphDumpSerial(); Serial != SeenDumpSerial)
	{
		SeenDumpSerial = Serial;
		for (const std::string& Line : Graph.Dump())
		{
			E_LOG(LogRenderer, Display, "{}", Line);
		}
	}
}

void FSceneRenderer::BeginGraphTimer(ERenderTimer Timer, ID3D12GraphicsCommandList* List, bool bCompute)
{
	const uint32 Index = static_cast<uint32>(Timer);
	TimerStarts[Index] = FClock::now();
	if (bCompute)
	{
		ComputeGpuTimer.BeginScope(List, Index);
		RenderProfiling::BeginZone(nullptr, Index, GetRenderTimerName(Timer), false); // Tracy GPU 컨텍스트는 그래픽스 큐 전용
	}
	else
	{
		GpuTimer.BeginScope(List, Index);
		RenderProfiling::BeginZone(List, Index, GetRenderTimerName(Timer), true);
	}
}

void FSceneRenderer::EndGraphTimer(ERenderTimer Timer, ID3D12GraphicsCommandList* List, bool bCompute)
{
	const uint32 Index = static_cast<uint32>(Timer);
	Stats.CpuMs[Index] += std::chrono::duration<float, std::milli>(FClock::now() - TimerStarts[Index]).count();
	RenderProfiling::EndZone(Index);
	(bCompute ? ComputeGpuTimer : GpuTimer).EndScope(List, Index);
}

void FSceneRenderer::FinalizeFrameStats()
{
	Stats.LocalLights       = LocalLightRenderer.GetLightCount();
	Stats.LocalShadowSlices = LocalLightRenderer.GetShadowSliceCount();
	Stats.ShadowDrawCalls   = ShadowRenderer.GetDrawCalls() + LocalLightRenderer.GetShadowDrawCalls() + TerrainRenderer.GetShadowDrawCalls();
	Stats.ShadowTriangles   = ShadowRenderer.GetTriangles() + LocalLightRenderer.GetShadowTriangles() + TerrainRenderer.GetShadowTriangles();
	Stats.ShadowCacheReused  = ShadowRenderer.GetCacheReusedCascades();
	Stats.ShadowCacheRebuilt = ShadowRenderer.GetCacheRebuiltCascades();
	if (bFrameOcclusion)
	{
		Stats.Triangles       = OcclusionCuller.GetDrawnTriangles() + FrameMainTriangles; // 간접 드로우(정적) + 바로 그린 스킨
		Stats.OcclusionTested = OcclusionCuller.GetTestedInstances();
		Stats.OcclusionPhase1 = OcclusionCuller.GetPhase1Instances();
		Stats.OcclusionPhase2 = OcclusionCuller.GetPhase2Instances();
	}
	else
	{
		Stats.Triangles       = FrameMainTriangles;
		Stats.OcclusionTested = Stats.OcclusionPhase1 = Stats.OcclusionPhase2 = 0;
	}
	Stats.Triangles += FrameTranslucentTriangles;
	Stats.ParticleEmittersCulled = ParticleRenderer.GetCulledEmitterCount();
}

void FSceneRenderer::RenderFrame(FRenderGraph& Graph, FScene& Scene, const FCamera& Camera, const FRenderOutput& Output)
{
	// 출력: 넘겨받은 RENDER_TARGET 상태 그대로 돌려준다 (리소스가 없으면 추적하지 않음 — 출력 패스는 부수 효과)
	FPostProcessGraphOutput PostOutput;
	PostOutput.Output = Output;
	if (Output.Resource != nullptr)
	{
		PostOutput.Ref = Graph.Import("Output", Output.Resource, ERGAccess::RenderTarget, ERGAccess::RenderTarget);
	}

	const FPixelArtComponent* PixelArt = FindPixelArtSettings(Scene);
	if (PixelArt == nullptr)
	{
		// TAAU (Phase 48): 씬은 내부 해상도(출력 × 화면 비율)로, TAA 단계가 출력 해상도로 시간 업샘플. 이후 패스(블룸·톤매핑·오버레이)는 출력 해상도
		const float  Percentage     = ComputeScreenPercentage();
		const uint32 InternalWidth  = FUpscaleMath::ComputeInternalDimension(Output.Width, Percentage);
		const uint32 InternalHeight = FUpscaleMath::ComputeInternalDimension(Output.Height, Percentage);
		const bool   bUpscale       = InternalWidth != Output.Width || InternalHeight != Output.Height;
		Stats.ScreenPercentage      = Percentage;
		Stats.InternalWidth         = InternalWidth;
		Stats.InternalHeight        = InternalHeight;

		FSceneGraphRefs Refs;
		RenderSceneColor(Graph, Scene, Camera, InternalWidth, InternalHeight, true, Refs, Output.Width, Output.Height);

		// TAA: 톤매핑 전 HDR 이력과 섞은 결과가 포스트 입력. 한 렌더러가 여러 뷰를 그리는 경우(미리보기/썸네일)·와이어프레임은 끔
		// 업스케일인데 TAA가 꺼져 있으면 같은 패스가 이력 없이 공간 재구성만 한다 (지터도 없음)
		const bool             bTaa      = PostProcessSettings.bTemporalAA && bConsoleTemporalAA && !bWireframe && ViewsThisFrame == 1 && ViewsLastFrame == 1;
		FPostProcessGraphInput PostInput{ Refs.Color, SceneColor->GetSrv() };
		float                  Sharpness = 0.0f;
		if (bTaa || bUpscale)
		{
			if (!bTaaRanLastFrame)
			{
				TemporalAA.ResetHistory(); // 꺼져 있던 동안의 이력은 낡았다
			}
			FTemporalAAInputs Inputs;
			Inputs.SceneColor    = SceneColor.get();
			Inputs.Velocity      = SceneVelocity.get();
			Inputs.Reprojection  = CurrentReprojection;
			Inputs.bHistoryValid = bTemporalHistoryValid && bTaa;
			Inputs.CurrentWeight = PostProcessSettings.TemporalAACurrentWeight;
			Inputs.OutputWidth   = Output.Width;
			Inputs.OutputHeight  = Output.Height;
			Inputs.JitterNdc     = CurrentJitterNdc;
			FRGResourceRef            ResultRef;
			const FD3D12RenderTarget& Result =
				TemporalAA.AddPass(Graph, Inputs, { Refs.Color, Refs.Depth, Refs.Velocity }, static_cast<int32>(ERenderTimer::TemporalAA), ResultRef);
			PostInput = { ResultRef, Result.GetSrv() };
			Sharpness = bTaa ? PostProcessSettings.TemporalAASharpness : 0.0f;
		}
		if (bUpscale)
		{
			TemporalAA.AddOverlayDepthPass(Graph, *SceneColor, Refs.Depth, CurrentJitterNdc, Output.Width, Output.Height,
			                               static_cast<int32>(ERenderTimer::TemporalAA));
		}
		bTaaRanLastFrame = bTaa;
		bFrameUpscaled   = bUpscale;

		PostProcessor.AddPasses(Graph, PostInput, PostOutput, PostProcessSettings, Sharpness, static_cast<int32>(ERenderTimer::PostProcess));
		AddDebugViewPass(Graph, PostOutput, Refs);
		return;
	}
	bTaaRanLastFrame = false; // 픽셀 아트: 정수 격자 스냅과 충돌하므로 TAA 없음 (TAAU도 없음 — 화면 비율 무시)
	bFrameUpscaled   = false;

	// 픽셀 아트: 저해상도 씬 → 저해상도 포스트(톤매핑) → 합성 확대
	const uint32 PixelSize    = FPixelArtMath::ClampPixelSize(PixelArt->PixelSize);
	const uint32 SourceWidth  = FPixelArtMath::GetSourceDimension(Output.Width, PixelSize);
	const uint32 SourceHeight = FPixelArtMath::GetSourceDimension(Output.Height, PixelSize);

	FPixelArtCompositeParams Params;
	const FCamera            SourceCamera = BuildPixelArtCamera(*PixelArt, Camera, Output, SourceWidth, SourceHeight, Params);

	// 움직인 물체를 카메라와 같은 도트 격자에 맞춰 그린다 (씬 렌더 동안만 — 그래프 실행 뒤 Render가 되돌린다, 직교 전용 — 원근은 깊이마다 도트 크기가 다름)
	const bool bSnapObjects = PixelArt->bSnapMovingObjects && Camera.IsOrthographic();
	if (bSnapObjects)
	{
		const float TexelWorldSize = FPixelArtMath::GetTexelWorldSize(Camera.GetOrthoHeight(), Output.Height, PixelSize);
		PixelArtObjectSnap.Apply(Scene, Camera.GetRightVector(), Camera.GetUpVector(), TexelWorldSize);
		bPendingSnapRestore = true;
	}
	// SSAO: 반해상도면 도트 한 칸 이동에 반 칸씩 어긋나고 화면 고정 노이즈가 물체 위에서 흘러 자글거린다 → 전체 해상도 + 격자 노이즈
	AoResolutionDivisor = 1;
	bAoGridNoise        = PixelArt->bSnapCamera && Camera.IsOrthographic();
	AoGridOrigin[0]     = Params.GridOrigin[0];
	AoGridOrigin[1]     = Params.GridOrigin[1];
	FSceneGraphRefs Refs;
	Stats.ScreenPercentage = 100.0f;
	Stats.InternalWidth    = SourceWidth;
	Stats.InternalHeight   = SourceHeight;
	RenderSceneColor(Graph, Scene, SourceCamera, SourceWidth, SourceHeight, false, Refs, SourceWidth, SourceHeight); // 지터는 정수 격자 스냅과 충돌
	AoResolutionDivisor = 2;
	bAoGridNoise        = false;

	// 저해상도 톤매핑 결과 = 그래프 풀 텍스처 (선형 부동소수점)
	const FRGResourceRef    PixelColorRef = Graph.CreateTexture("PixelArtColor", FRGTextureDesc::MakeRenderTarget(SourceWidth, SourceHeight, SceneColorFormat));
	const FRGPooledTexture* PixelColor    = Graph.GetTexture(PixelColorRef);
	FPostProcessGraphOutput PixelOutput;
	PixelOutput.Ref             = PixelColorRef;
	PixelOutput.Output.Rtv      = PixelColor->GetRtv();
	PixelOutput.Output.Format   = SceneColorFormat;
	PixelOutput.Output.Width    = SourceWidth;
	PixelOutput.Output.Height   = SourceHeight;
	PixelOutput.Output.Resource = PixelColor->Resource.Get();
	PostProcessor.AddPasses(Graph, { Refs.Color, SceneColor->GetSrv() }, PixelOutput, PostProcessSettings, 0.0f, static_cast<int32>(ERenderTimer::PostProcess));
	PostProcessor.AddPixelArtCompositePass(Graph, { PixelColorRef, PixelColor->Srv }, { Refs.Depth, SceneColor->GetDepthSrv() }, PostOutput, Params,
	                                       static_cast<int32>(ERenderTimer::PostProcess));
}

FCamera FSceneRenderer::BuildPixelArtCamera(const FPixelArtComponent& PixelArt, const FCamera& Camera, const FRenderOutput& Output,
                                            uint32 SourceWidth, uint32 SourceHeight, FPixelArtCompositeParams& OutParams) const
{
	const uint32 PixelSize    = FPixelArtMath::ClampPixelSize(PixelArt.PixelSize);
	const float  ExtentScale  = FPixelArtMath::GetSourceExtentScale(SourceHeight, Output.Height, PixelSize);
	const float  SourceAspect = static_cast<float>(SourceWidth) / static_cast<float>(SourceHeight);

	OutParams.PixelSize         = PixelSize;
	OutParams.OutlineStrength   = PixelArt.OutlineStrength;
	OutParams.HighlightStrength = PixelArt.HighlightStrength;
	OutParams.DepthThreshold    = PixelArt.DepthThreshold;
	OutParams.ColorLevels       = PixelArt.ColorLevels;
	OutParams.DitherStrength    = PixelArt.DitherStrength;
	OutParams.bOrthographic     = Camera.IsOrthographic();
	OutParams.NearZ             = Camera.GetNearZ();
	OutParams.FarZ              = Camera.GetFarZ();

	FCamera SourceCamera = Camera;
	if (Camera.IsOrthographic())
	{
		// 도트 격자 스냅: 카메라를 격자 위로 옮겨 렌더하고, 남은 소수 부분은 확대 단계에서 밀어 부드럽게 보이게 한다
		const float TexelWorldSize = FPixelArtMath::GetTexelWorldSize(Camera.GetOrthoHeight(), Output.Height, PixelSize);
		if (PixelArt.bSnapCamera)
		{
			const FPixelArtMath::FSnapResult Snap =
				FPixelArtMath::SnapToTexelGrid(Camera.GetPosition(), Camera.GetRightVector(), Camera.GetUpVector(), TexelWorldSize);
			SourceCamera.SetPosition(Snap.SnappedPosition);
			OutParams.SubPixelOffset  = FPixelArtMath::GetSubPixelOffset(Snap.Remainder);
			OutParams.DitherOrigin[0] = FPixelArtMath::PositiveMod4(Snap.IndexRight);
			OutParams.DitherOrigin[1] = FPixelArtMath::PositiveMod4(-Snap.IndexUp); // 화면 Y는 아래가 +
			OutParams.GridOrigin[0]   = static_cast<int32>(Snap.IndexRight); // 넘치면 감기지만 프레임 사이 차이는 그대로
			OutParams.GridOrigin[1]   = static_cast<int32>(-Snap.IndexUp);
		}
		SourceCamera.SetOrthographic(Camera.GetOrthoHeight() * ExtentScale, SourceAspect, Camera.GetNearZ(), Camera.GetFarZ());
		OutParams.PixelViewScale = TexelWorldSize;
	}
	else
	{
		// 원근은 깊이마다 도트 크기가 달라 정확한 스냅이 불가능 → 여백만큼 시야각만 넓힌다
		const float TanHalfFov = FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f) * ExtentScale;
		SourceCamera.SetPerspective(FMath::RadiansToDegrees(FMath::Atan2(TanHalfFov, 1.0f)) * 2.0f, SourceAspect, Camera.GetNearZ(), Camera.GetFarZ());
		OutParams.PixelViewScale = 2.0f * TanHalfFov / static_cast<float>(SourceHeight);
	}
	return SourceCamera;
}

namespace
{
	// 카메라 컷 판정 (이력 리셋): 한 프레임에 이 거리(cm)·각도(도)를 넘게 바뀌면 이전 프레임과 이어지지 않는 것으로 본다
	constexpr float CameraCutDistance     = 2000.0f;
	constexpr float CameraCutAngleDegrees = 60.0f;

	constexpr int32 TimerId(ERenderTimer Timer) { return static_cast<int32>(Timer); }
} // namespace

void FSceneRenderer::RenderSceneColor(FRenderGraph& Graph, FScene& Scene, const FCamera& Camera, uint32 Width, uint32 Height, bool bAllowJitter,
                                      FSceneGraphRefs& OutRefs, uint32 OutputWidth, uint32 OutputHeight)
{
	++SceneFrameCount;

	// 시간 이력: 이 렌더러가 연속 프레임에 뷰 하나만 그리고, 같은 (출력) 크기이고, 카메라 컷이 없을 때만 이전 프레임 값을 쓴다.
	// 이전 프레임 뷰-투영·움직임 벡터는 UV 단위라 내부 해상도와 무관 (동적 해상도로 내부 크기가 바뀌어도 이어짐 — 내부 크기 버퍼를 쓰는
	// 효과(SSR 누적·볼류메트릭 안개)는 버퍼를 다시 만들 때 스스로 이력을 버린다)
	const bool       bSingleView              = ViewsThisFrame == 1 && ViewsLastFrame == 1;
	const bool       bInternalSizeStable      = SceneColor && SceneColor->GetWidth() == Width && SceneColor->GetHeight() == Height;
	const FMatrix4x4 UnjitteredViewProjection = Camera.GetUnjitteredViewProjectionMatrix();
	bTemporalHistoryValid = bSingleView && bHasPrevView && PrevScene == &Scene && PrevTargetWidth == OutputWidth && PrevTargetHeight == OutputHeight &&
	                        !FTemporalMath::IsCameraCut(PrevCameraPosition, PrevCameraForward, Camera.GetPosition(), Camera.GetForwardVector(),
	                                                    CameraCutDistance, CameraCutAngleDegrees);

	// 지터는 씬 컬러에 그리는 패스(메시/파티클)에만: 그림자 캐스케이드·클러스터·컬링·LOD는 지터 없는 카메라 (그림자 떨림 방지)
	CurrentJitterNdc = FVector2::ZeroVector;
	if ((bTemporalJitter || (PostProcessSettings.bTemporalAA && bConsoleTemporalAA && !bWireframe)) && bAllowJitter && bSingleView)
	{
		// 지터는 내부 해상도 픽셀 기준. TAAU면 출력/내부 면적비만큼 표본 수를 늘린다 (네이티브는 8 그대로)
		const uint32 SampleCount = FUpscaleMath::GetJitterSampleCount(Height, OutputHeight);
		CurrentJitterNdc         = FTemporalMath::JitterPixelsToNdc(FTemporalMath::GetJitterPixels(TemporalFrameIndex++, SampleCount), Width, Height);
	}
	FCamera RenderCamera = Camera;
	RenderCamera.SetProjectionJitter(CurrentJitterNdc);

	// 대기 (Phase 49): 씬의 첫 대기 컴포넌트 + 태양 → 상수·태양 투과율·공중 원근. 실시간 IBL 결과가 있으면 조명 표를 덮는다 (메시/지형/물)
	SkyAtmosphere.Prepare(Scene, Camera, RendererCVars::SkyAtmosphere.Get());
	SkyAtmosphere.ApplyEnvironmentOverride(IblRenderer);

	FPerFrameConstants PerFrame       = BuildPerFrameConstants(Scene, RenderCamera);
	if (SkyAtmosphere.IsActive())
	{
		SkyAtmosphere.ApplyToDirectionalLight(PerFrame.DirectionalLight); // 해질녘 붉은 빛, 밤에는 달빛 (방향까지)
	}
	PerFrame.UnjitteredViewProjection = UnjitteredViewProjection;
	PerFrame.PrevViewProjection       = bTemporalHistoryValid ? PrevUnjitteredViewProjection : UnjitteredViewProjection;
	PerFrame.JitterNdc                = CurrentJitterNdc;
	PerFrame.ScreenSize               = FVector2(static_cast<float>(Width), static_cast<float>(Height));
	PerFrame.MaterialMipBias          = FUpscaleMath::ComputeMipBias(Height, OutputHeight, RendererCVars::UpscaleMipBiasOffset.Get()); // 네이티브 0
	PerFrame.DebugMipView             = DebugView == DebugViewMip ? 1u : 0u; // 메시 패스가 상주 밉 색칠 (화면 패스 없음)
	CurrentReprojection               = FTemporalMath::ComputeReprojectionMatrix(UnjitteredViewProjection, PerFrame.PrevViewProjection);

	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();

	// 컬링 프러스텀 (고정 중이면 고정 시점)
	if (!bCullingFrozen)
	{
		FrozenFrustum = FFrustum::FromViewProjection(UnjitteredViewProjection);
	}

	// 그림자 캐스터 볼륨 먼저 (CPU만): 점광원/스포트라이트 수집 + 그림자 장 배정, 방향광 캐스케이드
	BeginCpuTimer(ERenderTimer::LocalLights);
	LocalLightRenderer.PrepareLights(Scene, Camera, LocalShadowSettings);
	EndCpuTimer(ERenderTimer::LocalLights);
	BeginCpuTimer(ERenderTimer::Shadow);
	ShadowRenderer.PrepareCascades(Camera, PerFrame.DirectionalLight.Direction, ShadowSettings);
	EndCpuTimer(ERenderTimer::Shadow);

	// 스킨 메시 본 팔레트(메인 프러스텀 ∪ 그림자 캐스터 볼륨에 드는 것만) + 프레임 메시 인스턴스 목록 (섀도우/로컬 그림자/메인 패스 공유)
	// 이전 프레임 팔레트/월드는 한 뷰만 그릴 때만 (여러 씬을 번갈아 그리면 엔티티 번호가 겹친다)
	BeginCpuTimer(ERenderTimer::Gather);
	SkinPalettes.bTrackPrevious = bSingleView;
	if (bSkinVisibilityCulling)
	{
		SkinPalettes.Build(Scene, *Resources, DynamicBuffer, [this](const FBox& Bounds) {
			return FrozenFrustum.Intersects(Bounds) || ShadowRenderer.IntersectsCasterVolume(Bounds) ||
			       LocalLightRenderer.IntersectsShadowCaster(Bounds);
		});
	}
	else
	{
		SkinPalettes.Build(Scene, *Resources, DynamicBuffer);
	}
	MeshInstances.Gather(Scene, *Resources, &SkinPalettes);
	ApplyMotionHistory(bTemporalHistoryValid);
	// 풀·나무 (Phase 34-3): 메인 프러스텀 ∪ 그림자 거리 안 캐스터 볼륨의 셀만 인스턴스 목록에 더한다
	FoliageRenderer.Gather(Scene, Camera, FrozenFrustum, [this](const FBox& Bounds) {
		return ShadowRenderer.IntersectsCasterVolume(Bounds) || LocalLightRenderer.IntersectsShadowCaster(Bounds);
	}, MeshInstances);
	MeshInstances.Upload(DynamicBuffer);
	SelectLods(Camera);
	// 지형 텍스처 갱신(업로드 복사 — 그래프 밖, 이 프레임 명령 목록 맨 앞) + 청크 LOD/컬링 (그림자 패스 전)
	TerrainRenderer.Prepare(Scene, Camera, FrozenFrustum);
	// 텍스처 밉 스트리밍: 필요 밉 보고 (자동 검증·동기 로딩은 부족한 밉을 여기서 바로 채워 이 프레임에 그린다)
	ReportTextureStreaming(Scene, Camera, Height, PerFrame.MaterialMipBias);
	Stats.TotalMeshes   = MeshInstances.GetComponentCount();
	Stats.SkinnedDrawn  = static_cast<uint32>(SkinPalettes.GetCount());
	Stats.SkinnedCulled = SkinPalettes.GetCulledCount();
	Stats.SkinPalettes  = SkinPalettes.GetPaletteCount();
	EndCpuTimer(ERenderTimer::Gather);

	// GPU 파티클 계산 (그리기 전에, 계산 셰이더만 → 비동기 계산 가능)
	const bool bAsyncAllowed = RendererCVars::RenderGraphAsyncCompute.Get();
	ParticleRenderer.PrepareSimulation(Scene, FrozenFrustum);
	ParticleRenderer.AddSimulationPass(Graph, bAsyncAllowed && RendererCVars::RenderGraphAsyncParticles.Get() ? ERGQueue::AsyncCompute : ERGQueue::Graphics,
	                                   TimerId(ERenderTimer::Particles));

	// 점광원/스포트라이트 그림자 + 클러스터 컬링 (화면 크기 = 이번 씬 타깃)
	BeginCpuTimer(ERenderTimer::LocalLights);
	LocalLightRenderer.PrepareFrame(Camera, Width, Height, LocalShadowSettings);
	EndCpuTimer(ERenderTimer::LocalLights);
	// 같은 프레임에 이 렌더러가 다시 그리면(반사 캡처 면) 지난 메시 패스의 루트 SRV(클러스터 버퍼)가 그래픽스 루트에 남아 있다 →
	// 클러스터 패스가 다른 루트 시그니처로 바꿔 묶음을 끊는다 (디버그 레이어 1003)
	LocalLightRenderer.AddPasses(Graph, MeshInstances, SkinPalettes.GetGpuData(), ScreenPassRoot.Get(), TimerId(ERenderTimer::LocalLights));
	const FRGResourceRef LocalShadowRef = LocalLightRenderer.ImportShadowMap(Graph);
	const FRGResourceRef ClustersRef    = LocalLightRenderer.ImportClusters(Graph);

	// 0) 방향광 섀도우 패스
	const FRGResourceRef ShadowMapRef = ShadowRenderer.ImportShadowMap(Graph);
	BeginCpuTimer(ERenderTimer::Shadow);
	// 그림자 캐시 키는 메인 LOD를 담지 않으므로 LOD 설정이 바뀌면 캐시를 다시 그린다
	const uint64 LodSignature = ShadowCacheMath::HashValue(
		ShadowCacheMath::HashValue(ShadowCacheMath::HashValue(ShadowCacheMath::HashValue(ShadowCacheMath::HashSeed, bEnableLod), ForcedLod), LodScale),
		LodHysteresis);
	if (LodSignature != ShadowLodSignature)
	{
		ShadowLodSignature = LodSignature;
		ShadowRenderer.InvalidateCache();
	}
	ShadowRenderer.AddPass(Graph, ShadowMapRef, MeshInstances, SkinPalettes.GetGpuData(), TimerId(ERenderTimer::Shadow)); // 캐시 판정 + 캐스케이드 묶음 (CPU)
	EndCpuTimer(ERenderTimer::Shadow);

	// 0.5) 레이 트레이싱 (Phase 50): 허용된 렌더러 + DXR 1.1 + 한 뷰 + 사전 패스 + 지터 허용(픽셀 아트 아님) + 캡처 굽기 아님.
	//   켬/끔 = r.RayTracing*(-1이면 프로젝트 설정 Rendering). 효과가 하나라도 켜져야 BLAS/TLAS를 만든다
	const FRenderingSettings& RenderingSettings = FProjectSettings::Get().Rendering;
	const auto ResolveToggle = [](int32 Value, bool bDefault) { return Value < 0 ? bDefault : Value != 0; };
	const bool bRtAllowed = bAllowRayTracing && RayTracingScene.IsSupported() && RayTracingEffects.IsSupported() && bSingleView && bAllowJitter &&
	                        !bRenderingCaptures && !bWireframe && bDepthPrepass && ResolveToggle(RendererCVars::RayTracing.Get(), RenderingSettings.bRayTracing);
	const bool bRtShadows = bRtAllowed && ResolveToggle(RendererCVars::RayTracingShadows.Get(), RenderingSettings.bRayTracedShadows) &&
	                        PerFrame.DirectionalLight.Intensity > 0.0f && ShadowRenderer.GetConstants().ShadowEnabled > 0.5f;
	const bool bRtReflections = bRtAllowed && ResolveToggle(RendererCVars::RayTracingReflections.Get(), RenderingSettings.bRayTracedReflections) &&
	                            PostProcessSettings.bScreenSpaceReflections && bConsoleReflections;
	const bool bRtDebug = bRtAllowed && DebugView == DebugViewRtInstances;
	// 동적 GI (Phase 51): 씬에 프로브 볼륨이 있으면 TLAS로 프로브 광선을 추적한다
	const bool bDdgiWanted = bRtAllowed && RendererCVars::Ddgi.Get() && FDdgiRenderer::SceneHasVolumes(Scene);
	FRGResourceRef TlasRef;
	FrameRtDebugRef            = {};
	if (bRtShadows || bRtReflections || bRtDebug || bDdgiWanted)
	{
		FRayTracingSceneOptions Options;
		Options.CameraPosition     = Camera.GetPosition();
		Options.bSkinned           = RendererCVars::RayTracingSkinned.Get();
		Options.SkinnedMaxDistance = RendererCVars::RayTracingSkinnedDistance.Get();
		Options.SkinnedRefitDistance = RendererCVars::RayTracingSkinnedRefitDistance.Get();
		Options.SkinnedRefitInterval = static_cast<uint32>(std::max(1, RendererCVars::RayTracingSkinnedRefitInterval.Get()));
		Options.bFoliage           = RendererCVars::RayTracingFoliage.Get();
		Options.bCompaction        = RendererCVars::RayTracingCompaction.Get();
		Options.bGraphMaterials    = RendererCVars::RayTracingGraphMaterials.Get();
		Options.bTerrain           = RendererCVars::RayTracingTerrain.Get();
		std::vector<FTerrainRayTracingInput> Terrains;
		TerrainRenderer.GetRayTracingInputs(Terrains);
		Options.MaxBuildsPerFrame  = static_cast<uint32>(std::max(1, RendererCVars::RayTracingMaxBuilds.Get()));
		if (Resources->GetLoadMode() != EResourceLoadMode::Async)
		{
			// 결정적 로딩(자동 검증 = 프레임마다 비우기, 동기): 준비된 메시의 BLAS를 같은 프레임에 모두 빌드한다.
			// 빌드 상한은 끊김 방지용이라 프레임 시간/로딩 순서에 따라 TLAS 구성이 달라질 여지를 자동 검증에서 없앤다
			Options.MaxBuildsPerFrame = std::numeric_limits<uint32>::max();
			Options.MaxBuildTriangles = std::numeric_limits<uint64>::max();
		}
		BeginCpuTimer(ERenderTimer::RayTracingBuild);
		RayTracingScene.Prepare(MeshInstances, *Resources, SkinPalettes.GetGpuData(), Options, &Terrains, &Scene);
		EndCpuTimer(ERenderTimer::RayTracingBuild);
		TlasRef = RayTracingScene.AddBuildPasses(Graph, TimerId(ERenderTimer::RayTracingBuild));
		Stats.RayTracing = RayTracingScene.GetStats();
	}
	const bool bRtShadowsActive     = bRtShadows && TlasRef.IsValid();
	const bool bRtReflectionsActive = bRtReflections && TlasRef.IsValid();
	if (bAllowRayTracing && (Stats.bRayTracedShadows != bRtShadowsActive || Stats.bRayTracedReflections != bRtReflectionsActive))
	{
		E_LOG(LogRenderer, Log, "레이 트레이싱 효과 변경: 그림자 {}, 반사 {} (지원 {}, 인스턴스 {})", bRtShadowsActive, bRtReflectionsActive,
		      RayTracingScene.IsSupported(), RayTracingScene.GetInstanceCount());
	}
	Stats.bRayTracedShadows         = bRtShadowsActive;
	Stats.bRayTracedReflections     = bRtReflectionsActive;
	PerFrame.RayTracedShadows       = bRtShadowsActive ? 1u : 0u;
	// 그림자 마스크(t24): 메시 패스가 항상 묶으므로 모든 메시 패스가 읽기로 선언 (RT 그림자 누적이 사전 패스와 메인 패스 사이에 쓴다)
	const FRGResourceRef RtShadowMaskRef = RayTracingEffects.BeginShadowFrame(Graph, Width, Height, bRtShadowsActive);

	bool bDdgiActive = false;
	// 0.6) 동적 GI — DDGI (Phase 51): 프로브 광선 추적 + 누적 (TLAS 뒤·사전 패스 전 — 메시 패스는 이번 프레임 아틀라스를 읽는다).
	//   볼륨이 없거나 RT를 못 쓰는 렌더도 상수(VolumeCount 0 + 디버그 뷰)는 올리고 1x1 기본 아틀라스를 묶는다 → 메시는 예전 식
	{
		FDdgiSettings DdgiSettings;
		DdgiSettings.ProbeBudget       = static_cast<uint32>(std::max(0, RendererCVars::DdgiProbeBudget.Get()));
		DdgiSettings.BounceIntensity   = std::max(0.0f, RendererCVars::DdgiBounceIntensity.Get());
		DdgiSettings.ChangeThreshold   = std::max(0.0f, RendererCVars::DdgiChangeThreshold.Get());
		DdgiSettings.MaxHitLocalLights = static_cast<uint32>(std::max(0, RendererCVars::DdgiMaxLocalLights.Get()));
		DdgiSettings.ShowProbes        = RendererCVars::DdgiShowProbes.Get();
		DdgiSettings.bDebugView        = DebugView == DebugViewGi;
		DdgiSettings.LightDirection    = PerFrame.DirectionalLight.Direction;
		DdgiSettings.LightRadiance     = PerFrame.DirectionalLight.Color * PerFrame.DirectionalLight.Intensity;
		DdgiSettings.AmbientIntensity  = PerFrame.AmbientIntensity;
		DdgiSettings.BoostFrames       = static_cast<uint32>(std::max(0, RendererCVars::DdgiBoostFrames.Get()));
		DdgiSettings.BoostHysteresis   = RendererCVars::DdgiBoostHysteresis.Get();
		DdgiSettings.SettleFrames      = static_cast<uint32>(std::max(0, RendererCVars::DdgiSettleFrames.Get()));
		DdgiSettings.SettleHysteresis  = RendererCVars::DdgiSettleHysteresis.Get();
		bDdgiActive = Ddgi.Prepare(Scene, DdgiSettings, bDdgiWanted && TlasRef.IsValid(), RenderCamera.GetViewProjectionMatrix(), Camera.GetPosition());
		Ddgi.ImportFrame(Graph);
		if (bDdgiActive)
		{
			FRayTracingLightingInputs DdgiLighting; // 반사 캡처는 아직 모으기 전 — 히트 반사는 하늘 (확산 프로브라 영향 작음)
			DdgiLighting.LightDirection   = PerFrame.DirectionalLight.Direction;
			DdgiLighting.LightRadiance    = PerFrame.DirectionalLight.Color * PerFrame.DirectionalLight.Intensity;
			DdgiLighting.AmbientIntensity = PerFrame.AmbientIntensity;
			DdgiLighting.LocalLights      = LocalLightRenderer.GetLightList();
			DdgiLighting.LocalLightCount  = LocalLightRenderer.GetLightCount();
			DdgiLighting.IblTable         = IblRenderer.GetLightingTable();
			DdgiLighting.CaptureAtlas     = ReflectionCaptures.GetAtlasSrv();
			Ddgi.AddUpdatePasses(Graph, RayTracingScene, TlasRef, DdgiLighting, TimerId(ERenderTimer::DdgiTrace), TimerId(ERenderTimer::DdgiBlend),
			                     RendererCVars::RenderGraphAsyncCompute.Get() ? ERGQueue::AsyncCompute : ERGQueue::Graphics);
		}
		Stats.Ddgi = Ddgi.GetStats();
	}

	// 씬 타깃 (평소 상태: 색 PIXEL_SHADER_RESOURCE, 깊이 DEPTH_WRITE)
	EnsureSceneColor(Width, Height);
	OutRefs.Color    = Graph.ImportColor("SceneColor", *SceneColor);
	OutRefs.Depth    = Graph.ImportDepth("SceneDepth", *SceneColor);
	OutRefs.Normal   = Graph.ImportColor("SceneNormal", *SceneNormal);
	OutRefs.Velocity = Graph.ImportColor("SceneVelocity", *SceneVelocity);

	// 1) 메인 묶음 (사전 패스와 메인 패스 공유). 오클루전은 와이어프레임에서 끈다 (깊이가 성김)
	const bool bOcclusion = bEnableOcclusion && !bWireframe;
	const bool bPrepass   = bDepthPrepass && !bWireframe;
	bFrameOcclusion       = bOcclusion;
	PrepareMainBatches(Camera, bOcclusion);
	FOcclusionGraphRefs OcclusionRefs;
	if (bOcclusion)
	{
		OcclusionRefs = OcclusionCuller.Import(Graph);
		OcclusionCuller.AddPhase1Passes(Graph, OcclusionRefs, TimerId(ERenderTimer::Occlusion));
	}

	// 메시 패스 공용 선언 — 셰이더가 실제로 읽지 않아도 루트에 묶인 리소스는 모두 선언한다 (디버그 레이어가 그리기 때 묶인 표·루트 SRV의
	// 상태를 검사한다). 그림자 맵·로컬 그림자·클러스터는 이 프레임 앞 패스가 쓰므로 사전 패스도 선언해야 한다
	const auto DeclareShadowReads = [&](FRenderGraph::FPassBuilder& Pass) {
		for (const FRGResourceRef& Ref : { ShadowMapRef, LocalShadowRef, ClustersRef, RtShadowMaskRef })
		{
			if (Ref.IsValid())
			{
				Pass.Read(Ref, ERGAccess::SrvPixel);
			}
		}
		Ddgi.DeclareShadingReads(Pass); // t40~t42 (DDGI 아틀라스 — 볼륨이 없으면 1x1 기본)
	};
	// 오클루전 단계 목록·간접 인자 (정점 셰이더 / ExecuteIndirect)
	const auto DeclareOcclusion = [&](FRenderGraph::FPassBuilder& Pass, EMeshPhase Phase) {
		if (Phase == EMeshPhase::All)
		{
			return;
		}
		Pass.Read(OcclusionRefs.DrawArguments, ERGAccess::IndirectArgs);
		if (Phase != EMeshPhase::Phase2)
		{
			Pass.Read(OcclusionRefs.Phase1Indices, ERGAccess::SrvNonPixel);
		}
		if (Phase != EMeshPhase::Phase1)
		{
			Pass.Read(OcclusionRefs.Phase2Indices, ERGAccess::SrvNonPixel);
		}
	};

	// 2) 깊이 사전 패스: 깊이 + 법선 + 움직임 벡터 (오클루전이면 1단계 → HZB + 2단계 판정 → 2단계)
	Stats.PrepassDrawCalls = 0;
	Stats.DrawCalls        = 0;
	FrameMainTriangles     = 0;
	FrameTranslucentTriangles = 0;
	if (bPrepass)
	{
		const D3D12_GPU_VIRTUAL_ADDRESS PerFrameAddress = DynamicBuffer.AllocateConstants(PerFrame).GpuAddress;
		const D3D12_GPU_VIRTUAL_ADDRESS ShadowAddress   = DynamicBuffer.AllocateConstants(ShadowRenderer.GetConstants()).GpuAddress;
		const auto AddPrepass = [&](EMeshPhase Phase) {
			const bool bFirst = Phase != EMeshPhase::Phase2;
			FRenderGraph::FPassBuilder Pass = Graph.AddPass(Phase == EMeshPhase::Phase2 ? "깊이 사전 패스 (2단계)" : "깊이 사전 패스");
			Pass.Write(OutRefs.Normal, ERGAccess::RenderTarget, FRGSubresourceRange::All(), bFirst)
				.Write(OutRefs.Velocity, ERGAccess::RenderTarget, FRGSubresourceRange::All(), bFirst)
				.Write(OutRefs.Depth, ERGAccess::DepthWrite, FRGSubresourceRange::All(), bFirst)
				.Timer(TimerId(ERenderTimer::DepthPrepass));
			DeclareShadowReads(Pass);
			DeclareOcclusion(Pass, Phase);
			Pass.Execute([this, Phase, bFirst, PerFrameAddress, ShadowAddress](FRGContext& Context) {
				ID3D12GraphicsCommandList* CommandList = Context.CommandList;
				if (bFirst)
				{
					CommandList->ClearRenderTargetView(SceneNormal->GetRtv(), SceneNormal->GetDesc().ClearColor, 0, nullptr);
					CommandList->ClearRenderTargetView(SceneVelocity->GetRtv(), SceneVelocity->GetDesc().ClearColor, 0, nullptr);
					CommandList->ClearDepthStencilView(SceneColor->GetDsv(), D3D12_CLEAR_FLAG_DEPTH, FD3D12DepthBuffer::ClearDepth, 0, 0, nullptr);
				}
				BindPrepassTargets(CommandList);
				uint64 Triangles = 0;
				RecordMeshBatches(CommandList, EMeshPass::Prepass, PerFrameAddress, ShadowAddress, Phase, Stats.PrepassDrawCalls, Triangles);
			});
		};
		AddPrepass(bOcclusion ? EMeshPhase::Phase1 : EMeshPhase::All);
		if (bOcclusion)
		{
			OcclusionCuller.AddHzbAndPhase2Passes(Graph, OcclusionRefs, OutRefs.Depth, *SceneColor, PerFrame.ViewProjection, TimerId(ERenderTimer::Hzb));
			AddPrepass(EMeshPhase::Phase2);
		}
	}
	else
	{
		// 쓰는 쪽(TAA 등)이 있어도 안전하게 지운 값 (+Z 법선, 움직임 0)
		Graph.AddPass("법선·움직임 지우기")
			.Write(OutRefs.Normal, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
			.Write(OutRefs.Velocity, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true)
			.Execute([this](FRGContext& Context) {
				Context.CommandList->ClearRenderTargetView(SceneNormal->GetRtv(), SceneNormal->GetDesc().ClearColor, 0, nullptr);
				Context.CommandList->ClearRenderTargetView(SceneVelocity->GetRtv(), SceneVelocity->GetDesc().ClearColor, 0, nullptr);
			});
	}

	// 2.4) 레이 트레이싱 화면 입력 (사전 패스 깊이·법선) + RT 방향광 그림자 (마스크 = t24, 메인 패스 전)
	FRayTracingViewInputs RtView;
	RtView.SceneColor         = SceneColor.get();
	RtView.SceneNormal        = SceneNormal.get();
	RtView.Velocity           = SceneVelocity.get();
	RtView.DecalNormal        = &DecalRenderer.GetTarget(1);
	RtView.DecalMaterial      = &DecalRenderer.GetTarget(2);
	RtView.InvViewProjection  = RenderCamera.GetViewProjectionMatrix().GetInverse();
	RtView.ViewProjection     = UnjitteredViewProjection;
	RtView.PrevViewProjection = PerFrame.PrevViewProjection;
	RtView.CameraPosition     = Camera.GetPosition();
	RtView.CameraForward      = Camera.GetForwardVector();
	RtView.bOrthographic      = Camera.IsOrthographic();
	RtView.ProjectionScale    = RenderCamera.GetProjectionMatrix().M[1][1] * static_cast<float>(Height) * 0.5f;
	RtView.FrameIndex         = SceneFrameCount;
	RtView.bHistoryValid      = bTemporalHistoryValid;
	FRayTracingViewRefs RtRefs;
	RtRefs.Tlas     = TlasRef;
	RtRefs.Depth    = OutRefs.Depth;
	RtRefs.Normal   = OutRefs.Normal;
	RtRefs.Velocity = OutRefs.Velocity;
	FRayTracingLightingInputs RtLighting;
	RtLighting.LightDirection   = PerFrame.DirectionalLight.Direction;
	RtLighting.LightRadiance    = PerFrame.DirectionalLight.Color * PerFrame.DirectionalLight.Intensity;
	RtLighting.AmbientIntensity = PerFrame.AmbientIntensity;
	RtLighting.LocalLights      = LocalLightRenderer.GetLightList();
	RtLighting.LocalLightCount  = LocalLightRenderer.GetLightCount();
	RtLighting.IblTable         = IblRenderer.GetLightingTable();
	RtLighting.CaptureAtlas     = ReflectionCaptures.GetAtlasSrv();
	if (bRtShadowsActive)
	{
		FRayTracedShadowSettings ShadowRt;
		ShadowRt.SunAngleDegrees = RendererCVars::RayTracingShadowSunAngle.Get();
		ShadowRt.NormalBias      = RendererCVars::RayTracingShadowBias.Get();
		ShadowRt.HistoryWeight   = RendererCVars::RayTracingShadowHistory.Get();
		RayTracingEffects.AddShadowPasses(Graph, RayTracingScene, RtView, RtRefs, RtLighting, ShadowRt, TimerId(ERenderTimer::RayTracedShadows));
	}

	// 2.5) 앰비언트 오클루전 (메인 패스가 간접광에만 곱한다 — t16): RTAO(TLAS 짧은 광선, DDGI가 못 담는 근거리 간접 가림) 또는 SSAO.
	//   r.RayTracing.AO -1 = DDGI 볼륨이 활성인 프레임만 RTAO (프로브 간격보다 작은 접촉·틈새 가림을 SSAO가 거의 못 냈다 — Tests/GI 진단)
	const int32 RtAoMode = RendererCVars::RayTracingAmbientOcclusion.Get();
	bFrameRtAo           = bPrepass && PostProcessSettings.bAmbientOcclusion && TlasRef.IsValid() && (RtAoMode > 0 || (RtAoMode < 0 && bDdgiActive));
	const bool     bAmbientOcclusion = bPrepass && PostProcessSettings.bAmbientOcclusion && (bConsoleAmbientOcclusion || bFrameRtAo);
	FRGResourceRef AmbientOcclusionRef;
	if (bFrameRtAo)
	{
		FRayTracedAmbientOcclusionSettings AoRt;
		AoRt.Radius            = RendererCVars::RayTracingAoRadius.Get();
		AoRt.RaysPerPixel      = static_cast<uint32>(std::max(1, RendererCVars::RayTracingAoRays.Get()));
		AoRt.FalloffPower      = RendererCVars::RayTracingAoFalloff.Get();
		AoRt.Intensity         = RendererCVars::RayTracingAoIntensity.Get();
		AoRt.HistoryWeight     = RendererCVars::RayTracingAoHistory.Get();
		AoRt.NormalBias        = RendererCVars::RayTracingShadowBias.Get();
		AoRt.ResolutionDivisor = static_cast<uint32>(std::clamp(RendererCVars::RayTracingAoDivisor.Get(), 1, 2));
		AoRt.ReferenceRays     = bDdgiActive ? static_cast<uint32>(std::max(0, RendererCVars::RayTracingAoReference.Get())) : 0u;
		AoRt.Ddgi              = &Ddgi;
		AmbientOcclusionRef = RayTracingEffects.AddAmbientOcclusionPasses(Graph, RayTracingScene, RtView, RtRefs, RtLighting, AoRt,
		                                                                  TimerId(ERenderTimer::RayTracedAmbientOcclusion));
	}
	else if (bAmbientOcclusion)
	{
		FAmbientOcclusionInputs Inputs;
		Inputs.SceneDepth    = SceneColor.get();
		Inputs.SceneNormal   = SceneNormal.get();
		Inputs.Projection    = RenderCamera.GetProjectionMatrix();
		Inputs.View          = Camera.GetViewMatrix();
		Inputs.bOrthographic = Camera.IsOrthographic();
		Inputs.Radius        = PostProcessSettings.AmbientOcclusionRadius;
		Inputs.Intensity     = PostProcessSettings.AmbientOcclusionIntensity;
		Inputs.FrameIndex    = (CurrentJitterNdc.X != 0.0f || CurrentJitterNdc.Y != 0.0f) ? static_cast<uint32>(SceneFrameCount) : 0u; // TAA가 누적
		Inputs.ResolutionDivisor = AoResolutionDivisor;
		Inputs.bGridNoise        = bAoGridNoise;
		Inputs.GridOrigin[0]     = AoGridOrigin[0];
		Inputs.GridOrigin[1]     = AoGridOrigin[1];
		AmbientOcclusionRef = AmbientOcclusion.AddPasses(Graph, Inputs, OutRefs.Depth, OutRefs.Normal, TimerId(ERenderTimer::AmbientOcclusion));
	}
	PerFrame.AmbientOcclusionEnabled = bAmbientOcclusion ? 1.0f : 0.0f;

	// 2.6) 데칼 → DBuffer (사전 패스 깊이·법선 필요). 보이는 데칼이 없으면 메인 패스가 읽지 않는다
	bool bDecals = false;
	if (bPrepass)
	{
		BeginCpuTimer(ERenderTimer::Decals);
		bDecals = DecalRenderer.Prepare(Scene, *Resources, RenderCamera, FrozenFrustum, Width, Height);
		EndCpuTimer(ERenderTimer::Decals);
	}
	std::array<FRGResourceRef, 3> DBufferRefs;
	if (bDecals)
	{
		DecalRenderer.AddPass(Graph, *SceneColor, *SceneNormal, OutRefs.Depth, OutRefs.Normal, TimerId(ERenderTimer::Decals), DBufferRefs);
	}
	else
	{
		DBufferRefs = DecalRenderer.ImportTargets(Graph);
	}
	Stats.Decals           = bDecals ? DecalRenderer.GetDrawnCount() : 0;
	PerFrame.DecalsEnabled = bDecals ? 1u : 0u;

	// 2.65) 반사: 캡처 목록(굽는 중에는 쓰지 않음 — 하늘만) + SSR (사전 패스 깊이·법선, 이전 프레임 씬 컬러 = 아직 지우기 전 SceneColor)
	FRGResourceRef ReflectionRef;
	{
		const uint32 CaptureCount       = ReflectionCaptures.Gather(Scene);
		PerFrame.ReflectionCaptureCount = bRenderingCaptures ? 0u : CaptureCount;
		// 반사 색 원본 = 지난 프레임 TAA 결과 (지난 프레임에 TAA가 돌았고 그 이력이 이번 출력 크기일 때 — UV로 읽으므로 TAAU의 출력 해상도 이력도 됨),
		// 아니면 지난 프레임 SceneColor (내부 크기가 그대로일 때만 — 동적 해상도로 방금 다시 만든 SceneColor는 비어 있다)
		const FD3D12RenderTarget* LastTaa = bTaaRanLastFrame ? TemporalAA.GetLastOutput() : nullptr;
		if (LastTaa != nullptr && (LastTaa->GetWidth() != OutputWidth || LastTaa->GetHeight() != OutputHeight))
		{
			LastTaa = nullptr;
		}
		const bool bSsr = !bRtReflectionsActive && bPrepass && PostProcessSettings.bScreenSpaceReflections && bConsoleReflections && bTemporalHistoryValid &&
		                  !bRenderingCaptures && (LastTaa != nullptr || bInternalSizeStable);
		float ReflectionMaxRoughness = FMath::Clamp(PostProcessSettings.SsrMaxRoughness, 0.05f, 1.0f);
		if (bRtReflectionsActive)
		{
			// RT 반사 (Phase 50): SSR 추적 대신 TLAS 추적 → SSR과 같은 흐림·누적 (이전 프레임 색이 필요 없어 첫 프레임/컷에도 동작)
			if (RendererCVars::RayTracingReflectionRoughness.Get() >= 0.0f)
			{
				ReflectionMaxRoughness = FMath::Clamp(RendererCVars::RayTracingReflectionRoughness.Get(), 0.05f, 1.0f);
			}
			FRayTracedReflectionSettings ReflectionRt;
			ReflectionRt.MaxRoughness      = ReflectionMaxRoughness;
			ReflectionRt.NormalBias        = RendererCVars::RayTracingShadowBias.Get();
			ReflectionRt.MaxHitLocalLights = static_cast<uint32>(std::max(0, RendererCVars::RayTracingReflectionLights.Get()));
			ReflectionRt.bHitShadows       = RendererCVars::RayTracingReflectionShadows.Get();
			RtView.bDecals                 = bDecals;
			RtRefs.DecalNormal             = DBufferRefs[1];
			RtRefs.DecalMaterial           = DBufferRefs[2];
			RtLighting.Captures            = ReflectionCaptures.GetCaptureList();
			RtLighting.CaptureCount        = PerFrame.ReflectionCaptureCount;
			FRGResourceRef TraceResult;
			FRGResourceRef TraceMotion;
			RayTracingEffects.AddReflectionTracePass(Graph, RayTracingScene, RtView, RtRefs, RtLighting, ReflectionRt,
			                                         TimerId(ERenderTimer::RayTracedReflections), TraceResult, TraceMotion);
			FScreenSpaceReflectionInputs Inputs;
			Inputs.SceneColor    = SceneColor.get();
			Inputs.SceneNormal   = SceneNormal.get();
			Inputs.DecalNormal   = &DecalRenderer.GetTarget(1);
			Inputs.DecalMaterial = &DecalRenderer.GetTarget(2);
			Inputs.bDecals       = bDecals;
			Inputs.Velocity      = SceneVelocity.get();
			Inputs.bHistoryValid = bTemporalHistoryValid;
			FSsrGraphRefs SsrRefs;
			SsrRefs.SceneDepth    = OutRefs.Depth;
			SsrRefs.SceneNormal   = OutRefs.Normal;
			SsrRefs.DecalNormal   = DBufferRefs[1];
			SsrRefs.DecalMaterial = DBufferRefs[2];
			SsrRefs.Velocity      = OutRefs.Velocity;
			ReflectionRef = ScreenSpaceReflections.AddResolvePasses(Graph, Inputs, SsrRefs, TraceResult, TraceMotion, TimerId(ERenderTimer::Reflections));
		}
		if (bSsr)
		{
			FScreenSpaceReflectionInputs Inputs;
			Inputs.SceneColor    = SceneColor.get();
			Inputs.SceneNormal   = SceneNormal.get();
			Inputs.DecalNormal   = &DecalRenderer.GetTarget(1);
			Inputs.DecalMaterial = &DecalRenderer.GetTarget(2);
			Inputs.bDecals       = bDecals;
			Inputs.Projection    = RenderCamera.GetProjectionMatrix();
			Inputs.View          = Camera.GetViewMatrix();
			Inputs.Reprojection  = CurrentReprojection;
			Inputs.NearZ         = Camera.GetNearZ();
			Inputs.bOrthographic = Camera.IsOrthographic();
			Inputs.MaxDistance   = PostProcessSettings.SsrMaxDistance;
			Inputs.Thickness     = PostProcessSettings.SsrThickness;
			Inputs.MaxRoughness  = FMath::Clamp(PostProcessSettings.SsrMaxRoughness, 0.05f, 1.0f);
			Inputs.Velocity      = SceneVelocity.get();
			Inputs.bHistoryValid = bTemporalHistoryValid;
			FSsrGraphRefs SsrRefs;
			SsrRefs.SceneDepth    = OutRefs.Depth;
			SsrRefs.SceneNormal   = OutRefs.Normal;
			SsrRefs.ColorSource   = OutRefs.Color;
			SsrRefs.DecalNormal   = DBufferRefs[1];
			SsrRefs.DecalMaterial = DBufferRefs[2];
			SsrRefs.Velocity      = OutRefs.Velocity;
			if (LastTaa != nullptr)
			{
				Inputs.PrevColor    = LastTaa;
				SsrRefs.ColorSource = Graph.ImportColor("TaaLastOutput", *LastTaa);
			}
			ReflectionRef = ScreenSpaceReflections.AddPasses(Graph, Inputs, SsrRefs, TimerId(ERenderTimer::Reflections));
		}
		PerFrame.SsrEnabled      = (bSsr || bRtReflectionsActive) ? 1u : 0u;
		PerFrame.SsrMaxRoughness = ReflectionMaxRoughness;
		PerFrame.SsrIntensity    = FMath::Max(PostProcessSettings.SsrIntensity, 0.0f);
	}

	// 2.68) 레이 트레이싱 디버그 (--debug-view rt-instances): 카메라 광선으로 TLAS 직접 보기 (AddDebugViewPass가 출력에 그림)
	if (bRtDebug && TlasRef.IsValid())
	{
		RtLighting.Captures     = ReflectionCaptures.GetCaptureList();
		RtLighting.CaptureCount = PerFrame.ReflectionCaptureCount;
		FrameRtDebugRef = RayTracingEffects.AddDebugPass(Graph, RayTracingScene, RtView, RtRefs, RtLighting,
		                                                 static_cast<uint32>(std::max(0, RendererCVars::RayTracingDebugMode.Get())),
		                                                 TimerId(ERenderTimer::RayTracedReflections), FrameRtDebugSrv);
	}

	// 2.7) 안개 상수 + 볼류메트릭 안개 (3D 격자 주입 → 적분, 계산 셰이더만 → 비동기 계산 가능). 적용은 메인 패스 뒤, 파티클은 정점에서
	{
		BeginCpuTimer(ERenderTimer::VolumetricFog);
		FogRenderer.SetAerialPerspective(SkyAtmosphere.GetAerialParams()); // 공중 원근 = 안개 식에 합성 (Fog.hlsli EvaluateFog)
		FogRenderer.Prepare(Scene, RenderCamera, UnjitteredViewProjection, Width, Height);
		FVolumetricFogInputs FogInputs;
		FogInputs.ShadowConstants    = DynamicBuffer.AllocateConstants(ShadowRenderer.GetConstants()).GpuAddress;
		FogInputs.ShadowMapSrv       = ShadowRenderer.GetShadowMapSrv();
		FogInputs.ClusterConstants   = LocalLightRenderer.GetConstants();
		FogInputs.LocalLights        = LocalLightRenderer.GetLightList();
		FogInputs.LightDirection     = PerFrame.DirectionalLight.Direction;
		FogInputs.LightColor         = PerFrame.DirectionalLight.Color * PerFrame.DirectionalLight.Intensity;
		FogInputs.PrevViewProjection = PerFrame.PrevViewProjection;
		FogInputs.bHistoryValid      = bTemporalHistoryValid;
		FogInputs.FrameIndex         = SceneFrameCount;
		FogRenderer.PrepareVolumetric(FogInputs);
		EndCpuTimer(ERenderTimer::VolumetricFog);
		FogRenderer.AddVolumetricPasses(Graph, ShadowMapRef,
		                                bAsyncAllowed && RendererCVars::RenderGraphAsyncFog.Get() ? ERGQueue::AsyncCompute : ERGQueue::Graphics,
		                                TimerId(ERenderTimer::VolumetricFog));
	}
	const FRGResourceRef FogVolumeRef = FogRenderer.ImportVolume(Graph);

	// 2.8) 대기 LUT (투과율/다중 산란은 매질이 바뀔 때만, 하늘 뷰는 매 렌더) + 실시간 IBL 한 단계 (뒤쪽 버퍼 — 이번 프레임 메시는 앞쪽을 읽는다)
	const ERGQueue SkyQueue = bAsyncAllowed ? ERGQueue::AsyncCompute : ERGQueue::Graphics; // 계산 셰이더만 — 비동기 계산 후보
	SkyAtmosphere.AddLutPasses(Graph, SkyQueue, TimerId(ERenderTimer::Atmosphere));
	// 2.9) 볼류메트릭 구름: 추적(저해상도) + 시간 누적 (+ IBL용 저해상도 큐브) — 합성은 메인 패스 뒤
	FVolumetricCloudRenderer::FPrepareInputs CloudInputs;
	CloudInputs.Camera                   = &Camera;
	CloudInputs.UnjitteredViewProjection = UnjitteredViewProjection;
	CloudInputs.PrevViewProjection       = PerFrame.PrevViewProjection;
	CloudInputs.Width                    = Width;
	CloudInputs.Height                   = Height;
	CloudInputs.bHistoryValid            = bTemporalHistoryValid;
	CloudInputs.bAllowTemporal           = bAllowJitter && !bRenderingCaptures && !bWireframe && bSingleView;
	const bool bClouds                   = Clouds.Prepare(Scene, SkyAtmosphere, CloudInputs);
	if (bClouds)
	{
		Clouds.AddPasses(Graph, SkyAtmosphere, FogRenderer.GetConstantsAddress(), SkyQueue, TimerId(ERenderTimer::Clouds));
	}
	const bool bCloudCube = bClouds && Clouds.AffectsEnvironment();
	SkyAtmosphere.AddEnvironmentPasses(Graph, bCloudCube ? Clouds.GetCubeRef() : FRGResourceRef{}, Clouds.GetCubeSrv(), bClouds && Clouds.IsChanging(),
	                                   !bRenderingCaptures, TimerId(ERenderTimer::Atmosphere));

	// 메시 패스(메인/반투명)가 묶는 조명 리소스 선언 (그림자 맵·로컬 그림자·클러스터 + SSAO·DBuffer·SSR — 꺼져 있어도 묶이므로 항상)
	if (!AmbientOcclusionRef.IsValid())
	{
		AmbientOcclusionRef = Graph.ImportColor("AmbientOcclusion", *AmbientOcclusion.GetResult());
	}
	if (!ReflectionRef.IsValid())
	{
		ReflectionRef = Graph.ImportColor("SsrHistory", ScreenSpaceReflections.GetResult());
	}
	const auto DeclareLighting = [&](FRenderGraph::FPassBuilder& Pass) {
		DeclareShadowReads(Pass);
		Pass.Read(AmbientOcclusionRef, ERGAccess::SrvPixel).Read(ReflectionRef, ERGAccess::SrvPixel);
		for (const FRGResourceRef& Ref : DBufferRefs)
		{
			Pass.Read(Ref, ERGAccess::SrvPixel);
		}
	};

	FWaterPassInputs WaterInputs; // 물 패스 입력 (수면은 반투명 앞, 물속은 파티클 뒤)

	// 3) HDR 씬 패스: 하늘 + 불투명 메시 (사전 패스 뒤면 깊이 같음 테스트). 오클루전이고 사전 패스가 없으면 여기서 1단계 → HZB → 2단계
	{
		const D3D12_GPU_VIRTUAL_ADDRESS PerFrameAddress = DynamicBuffer.AllocateConstants(PerFrame).GpuAddress;
		const D3D12_GPU_VIRTUAL_ADDRESS ShadowAddress   = DynamicBuffer.AllocateConstants(ShadowRenderer.GetConstants()).GpuAddress;
		const EMeshPass                 MainPass        = bWireframe ? EMeshPass::Wireframe : (bPrepass ? EMeshPass::MainDepthEqual : EMeshPass::Main);
		const float                     SkyIntensity     = PerFrame.AmbientIntensity;
		const FCamera                   SkyCamera        = Camera;
		const bool                      bAtmosphereSky   = SkyAtmosphere.IsActive();
		const auto AddMainPass = [&](EMeshPhase Phase) {
			const bool bFirst = Phase != EMeshPhase::Phase2;
			FRenderGraph::FPassBuilder Pass = Graph.AddPass(Phase == EMeshPhase::Phase2 ? "메인 패스 (2단계)" : "메인 패스");
			Pass.Write(OutRefs.Color, ERGAccess::RenderTarget, FRGSubresourceRange::All(), bFirst)
				.Write(OutRefs.Depth, ERGAccess::DepthWrite, FRGSubresourceRange::All(), bFirst && !bPrepass)
				.Timer(TimerId(ERenderTimer::MainDraw));
			DeclareLighting(Pass);
			DeclareOcclusion(Pass, Phase);
			if (bFirst)
			{
				SkyAtmosphere.DeclareSkyReads(Pass);
			}
			Pass.Execute([this, Phase, bFirst, bPrepass, MainPass, PerFrameAddress, ShadowAddress, SkyIntensity, SkyCamera, bAtmosphereSky](FRGContext& Context) {
				ID3D12GraphicsCommandList* CommandList = Context.CommandList;
				if (bFirst)
				{
					const float SceneClear[4] = { BackgroundColor.X, BackgroundColor.Y, BackgroundColor.Z, 0.0f }; // 알파 0 = TAA 반응형 마스크 없음
					SceneColor->Bind(CommandList, SceneClear, !bPrepass);
					if (bDrawSkybox && bAtmosphereSky)
					{
						SkyAtmosphere.RenderSky(CommandList); // 대기 하늘 (하늘 뷰 LUT + 태양 원반 + 별)
					}
					else if (bDrawSkybox)
					{
						IblRenderer.RenderSkybox(SkyCamera, SkyIntensity);
					}
				}
				else
				{
					SceneColor->Bind(CommandList, nullptr);
				}
				RecordMeshBatches(CommandList, MainPass, PerFrameAddress, ShadowAddress, Phase, Stats.DrawCalls, FrameMainTriangles);
			});
		};
		if (bOcclusion && !bPrepass)
		{
			AddMainPass(EMeshPhase::Phase1);
			OcclusionCuller.AddHzbAndPhase2Passes(Graph, OcclusionRefs, OutRefs.Depth, *SceneColor, PerFrame.ViewProjection, TimerId(ERenderTimer::Hzb));
			AddMainPass(EMeshPhase::Phase2);
		}
		else
		{
			AddMainPass(bOcclusion ? EMeshPhase::Both : EMeshPhase::All);
		}
		if (bOcclusion)
		{
			OcclusionCuller.AddFinishPass(Graph, OcclusionRefs);
		}
		// DDGI 프로브 구 표시 (볼륨 DebugProbes / r.DDGI.ShowProbes — 씬 깊이 테스트, 안개·TAA 전)
		Ddgi.AddProbeDebugPass(Graph, *SceneColor, OutRefs.Color, OutRefs.Depth, TimerId(ERenderTimer::DdgiBlend));

		// 구름 합성 (하늘 + 구름보다 먼 기하, 안개 적용 전 — 높이 안개는 하늘처럼 구름 위에도)
		if (bClouds)
		{
			Clouds.AddCompositePass(Graph, *SceneColor, OutRefs.Color, OutRefs.Depth, TimerId(ERenderTimer::Clouds));
		}

		// 안개 적용 (불투명 메시 + 하늘, 씬 깊이) → 파티클은 정점에서 같은 식
		if (FogRenderer.IsEnabled())
		{
			FogRenderer.AddApplyPass(Graph, *SceneColor, OutRefs.Color, OutRefs.Depth, TimerId(ERenderTimer::Fog));
		}
		ParticleRenderer.SetFog(FogRenderer.GetConstantsAddress(), FogRenderer.GetVolumeSrv());

		// 물 (Phase 49): 안개 적용 뒤·반투명 메시 전 — 굴절 원본 복사 + 수면 (씬 컬러 + 움직임 벡터, 깊이는 셰이더 비교)
		if (Water.Prepare(Scene, FrozenFrustum, Camera.GetPosition()) > 0)
		{
			WaterInputs.SceneColor               = SceneColor.get();
			WaterInputs.SceneVelocity            = SceneVelocity.get();
			WaterInputs.ColorRef                 = OutRefs.Color;
			WaterInputs.DepthRef                 = OutRefs.Depth;
			WaterInputs.VelocityRef              = OutRefs.Velocity;
			WaterInputs.ShadowMapRef             = ShadowMapRef;
			WaterInputs.FogVolumeRef             = FogVolumeRef;
			WaterInputs.ViewProjection           = PerFrame.ViewProjection;
			WaterInputs.UnjitteredViewProjection = UnjitteredViewProjection;
			WaterInputs.PrevViewProjection       = PerFrame.PrevViewProjection;
			WaterInputs.CameraPosition           = Camera.GetPosition();
			WaterInputs.SunDirection             = -PerFrame.DirectionalLight.Direction;
			WaterInputs.SunColor                 = PerFrame.DirectionalLight.Color * PerFrame.DirectionalLight.Intensity;
			WaterInputs.AmbientIntensity         = PerFrame.AmbientIntensity;
			WaterInputs.ReflectionCaptureCount   = PerFrame.ReflectionCaptureCount;
			WaterInputs.ShadowConstants          = DynamicBuffer.AllocateConstants(ShadowRenderer.GetConstants()).GpuAddress;
			WaterInputs.FogConstants             = FogRenderer.GetConstantsAddress();
			WaterInputs.CaptureList              = ReflectionCaptures.GetCaptureList();
			WaterInputs.ShadowMapSrv             = ShadowRenderer.GetShadowMapSrv();
			WaterInputs.FogVolumeSrv             = FogRenderer.GetVolumeSrv();
			WaterInputs.IblTable                 = IblRenderer.GetLightingTable();
			WaterInputs.CaptureAtlasSrv          = ReflectionCaptures.GetAtlasSrv();
			Water.AddSurfacePass(Graph, WaterInputs, TimerId(ERenderTimer::Water));
		}

		// 반투명/가산 메시 (먼 것부터, 깊이 테스트만): 안개는 셰이더가 직접, 파티클보다 먼저
		Stats.TranslucentDrawCalls = 0;
		if (!TranslucentBatches.IsEmpty())
		{
			const D3D12_GPU_VIRTUAL_ADDRESS FogConstants = FogRenderer.GetConstantsAddress();
			FRenderGraph::FPassBuilder      Pass         = Graph.AddPass("반투명");
			Pass.Write(OutRefs.Color, ERGAccess::RenderTarget).Write(OutRefs.Depth, ERGAccess::DepthWrite).Read(FogVolumeRef, ERGAccess::SrvPixel)
				.Timer(TimerId(ERenderTimer::Translucent));
			DeclareLighting(Pass);
			Pass.Execute([this, PerFrameAddress, ShadowAddress, FogConstants](FRGContext& Context) {
				SceneColor->Bind(Context.CommandList, nullptr);
				DrawTranslucentBatches(Context.CommandList, PerFrameAddress, ShadowAddress, FogConstants, Stats.TranslucentDrawCalls, FrameTranslucentTriangles);
			});
		}
	}

	// 파티클 (씬 컬러 + 깊이 테스트, GPU 풀·안개 볼륨 읽기)
	FParticleRenderTargets ParticleTargets;
	ParticleTargets.SceneColor    = SceneColor.get();
	ParticleTargets.SceneColorRef = OutRefs.Color;
	ParticleTargets.DepthRef      = OutRefs.Depth;
	ParticleTargets.FogVolume     = FogVolumeRef;
	ParticleRenderer.AddRenderPass(Graph, Scene, RenderCamera, FrozenFrustum, ParticleTargets, TimerId(ERenderTimer::Particles), &Stats.Particles);

	// 물속 카메라 (Phase 49): 씬 컬러의 마지막 — 카메라 → 장면/상자 출구 물속 흡수·산란
	if (Water.IsCameraUnderwater() && WaterInputs.SceneColor != nullptr)
	{
		Water.AddUnderwaterPass(Graph, WaterInputs, TimerId(ERenderTimer::Water));
	}

	// 다음 프레임 이력
	PrevUnjitteredViewProjection = UnjitteredViewProjection;
	PrevCameraPosition           = Camera.GetPosition();
	PrevCameraForward            = Camera.GetForwardVector();
	PrevTargetWidth              = OutputWidth;
	PrevTargetHeight             = OutputHeight;
	bHasPrevView                 = true;
	PrevScene                    = &Scene;
}

void FSceneRenderer::UpdateEnvironment(FScene& Scene)
{
	std::string Map;
	float       Rotation = 0.0f;
	bool        bFound   = false;
	Scene.GetRegistry().View<FSkyLightComponent>().Each([&](FEntity, FSkyLightComponent& SkyLight) {
		if (!bFound)
		{
			Map      = SkyLight.EnvironmentMap;
			Rotation = SkyLight.EnvironmentRotation;
			bFound   = true;
		}
	});
	if (Map == AppliedEnvironmentMap && (Map.empty() || Rotation == AppliedEnvironmentRotation))
	{
		return;
	}
	AppliedEnvironmentMap      = Map;
	AppliedEnvironmentRotation = Rotation;
	if (Map.empty())
	{
		IblRenderer.SetEnvironment(nullptr, 0.0f);
		return;
	}
	const std::filesystem::path Relative = FStringConv::ToWide(Map);
	const std::filesystem::path Path =
		Relative.is_absolute() ? Relative : (FPaths::HasProject() ? FPaths::GetProjectContentDirectory() : FPaths::GetEngineDirectory()) / Relative;
	FEnvironmentImage Image;
	if (FAssetCache::LoadEnvironmentAsset(Path, Image) == FAssetCache::ESource::Failed)
	{
		E_LOG(LogRenderer, Warning, "환경맵을 읽지 못해 절차적 하늘을 씁니다: {}", Map);
		IblRenderer.SetEnvironment(nullptr, 0.0f);
		return;
	}
	IblRenderer.SetEnvironment(&Image, Rotation);
}

void FSceneRenderer::BakeReflectionCaptures(FScene& Scene)
{
	struct FJob
	{
		FVector3    Position;
		std::string AssetPath;
	};
	std::vector<FJob> Jobs;
	FRegistry&        Registry = Scene.GetRegistry();
	Registry.View<FTransformComponent, FReflectionCaptureComponent>().Each([&](FEntity Entity, FTransformComponent& Transform, FReflectionCaptureComponent& Capture) {
		if (Capture.CaptureAsset.empty())
		{
			const FNameComponent* Name = Registry.TryGet<FNameComponent>(Entity);
			Capture.CaptureAsset       = "Captures/" + (Name != nullptr && !Name->Name.empty() ? Name->Name : "Capture_" + std::to_string(Entity.Index)) + ".ecapture";
		}
		Jobs.push_back({ Transform.GetWorldPosition(), Capture.CaptureAsset });
	});
	if (Jobs.empty())
	{
		E_LOG(LogRenderer, Display, "반사 캡처 굽기: 씬에 반사 캡처가 없습니다");
		return;
	}
	Scene.UpdateTransforms();

	// 면마다 지터 없는 90도 카메라로 HDR 씬을 그려 원시 큐브 면으로 복사 (면마다 그래프 하나) → 프리필터 그래프
	// (캡처끼리는 서로 비추지 않고 하늘만)
	bRenderingCaptures = true;
	for (const FJob& Job : Jobs)
	{
		for (uint32 Face = 0; Face < 6; ++Face)
		{
			FVector3 Forward;
			FVector3 Right;
			FVector3 Up;
			FReflectionMath::GetCubeFaceBasis(Face, Forward, Right, Up);
			FCamera FaceCamera;
			FaceCamera.SetPosition(Job.Position);
			FaceCamera.SetRotation(FReflectionMath::MakeBasisRotation(Forward, Right, Up));
			FaceCamera.SetPerspective(90.0f, 1.0f, 5.0f, 200000.0f);
			FRenderGraph FaceGraph(*Rhi, GraphPool, "ReflectionCaptureFace");
			SetupGraph(FaceGraph);
			FSceneGraphRefs Refs;
			RenderSceneColor(FaceGraph, Scene, FaceCamera, FReflectionMath::CaptureSize, FReflectionMath::CaptureSize, false, Refs, FReflectionMath::CaptureSize,
			                 FReflectionMath::CaptureSize);
			ReflectionCaptures.AddCopyFacePass(FaceGraph, *SceneColor, Refs.Color, Face);
			ExecuteGraph(FaceGraph);
		}
		FRenderGraph BakeGraph(*Rhi, GraphPool, "ReflectionCaptureBake");
		SetupGraph(BakeGraph);
		ReflectionCaptures.AddFinishBakePasses(BakeGraph, Job.AssetPath);
		ExecuteGraph(BakeGraph);
		E_LOG(LogRenderer, Display, "반사 캡처 굽기: {} ({:.0f}, {:.0f}, {:.0f})", Job.AssetPath, Job.Position.X, Job.Position.Y, Job.Position.Z);
	}
	bRenderingCaptures = false;
}

void FSceneRenderer::ApplyMotionHistory(bool bValid)
{
	// 정적 인스턴스만 (스킨은 팔레트가 이전 프레임 본을 따로 가진다). Gather 직후라 엔티티 하나 = 인스턴스 하나 →
	// 칸 크기를 먼저 맞추고 병렬 (쓰는 것은 자기 인스턴스·엔티티 칸뿐)
	std::vector<FMeshInstance>& Instances = MeshInstances.GetInstances();
	const uint32                Count     = MeshInstances.GetGatheredCount();
	uint32                      MaxIndex  = 0;
	bool                        bAny      = false;
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		if (!Instances[Index].IsSkinned())
		{
			MaxIndex = std::max(MaxIndex, Instances[Index].Entity.Index);
			bAny     = true;
		}
	}
	if (bAny && MaxIndex >= MotionHistory.size())
	{
		MotionHistory.resize(static_cast<size_t>(MaxIndex) + 1);
	}
	FParallel::ParallelFor(Count, 256, [&](uint32 Begin, uint32 End) {
		for (uint32 InstanceIndex = Begin; InstanceIndex < End; ++InstanceIndex)
		{
			FMeshInstance& Instance = Instances[InstanceIndex];
			if (Instance.IsSkinned())
			{
				continue;
			}
			FMotionHistory& History     = MotionHistory[Instance.Entity.Index];
			const bool      bContinuous = History.Generation == Instance.Entity.Generation && History.Frame + 1 == SceneFrameCount;
			if (bValid && bContinuous)
			{
				Instance.PrevWorld = History.World;
			}
			// 그림자 캐시 정적 판정: 월드 행렬이 연속 프레임 비트 단위로 같았던 횟수 (ShadowCacheMath.h)
			const bool bSame       = bContinuous && std::memcmp(&History.World, &Instance.World, sizeof(FMatrix4x4)) == 0;
			History.StableFrames   = bSame ? std::min(History.StableFrames + 1, 0x7FFFFFFFu) : 0u;
			Instance.bShadowStatic = ShadowCacheMath::IsStatic(History.StableFrames, ShadowStaticFrames);
			History.Generation = Instance.Entity.Generation;
			History.Frame      = SceneFrameCount;
			History.World      = Instance.World;
		}
	});
}

void FSceneRenderer::AddDebugViewPass(FRenderGraph& Graph, const FPostProcessGraphOutput& Output, const FSceneGraphRefs& Refs)
{
	if (DebugView == 0 || DebugView == DebugViewMip || DebugView == DebugViewGi || !SceneColor || SceneColor->GetWidth() != Output.Output.Width ||
	    SceneColor->GetHeight() != Output.Output.Height)
	{
		return;
	}
	FPostProcessGraphInput Source;
	switch (DebugView)
	{
	case 1:  Source = { Refs.Normal, SceneNormal->GetSrv() }; break;
	case 2:  Source = { Refs.Velocity, SceneVelocity->GetSrv() }; break;
	case 3:  Source = { Refs.Depth, SceneColor->GetDepthSrv() }; break; // 깊이는 읽는 동안만 셰이더 리소스 (그래프가 전이)
	case 4:
		Source = bFrameRtAo ? FPostProcessGraphInput{ Graph.ImportColor("RtAoHistory", *RayTracingEffects.GetAmbientOcclusionResult()),
		                                              RayTracingEffects.GetAmbientOcclusionSrv() }
		                    : FPostProcessGraphInput{ Graph.ImportColor("AmbientOcclusion", *AmbientOcclusion.GetResult()), AmbientOcclusion.GetResultSrv() };
		break;
	case DebugViewRtShadows: Source = { Graph.ImportColor("RtShadowHistory", RayTracingEffects.GetShadowMask()), RayTracingEffects.GetShadowMask().GetSrv() }; break;
	case DebugViewRtInstances:
		if (!FrameRtDebugRef.IsValid())
		{
			return; // 레이 트레이싱이 꺼져 있음
		}
		Source = { FrameRtDebugRef, FrameRtDebugSrv };
		break;
	default: Source = { Graph.ImportColor("SsrHistory", ScreenSpaceReflections.GetResult()), ScreenSpaceReflections.GetResultSrv() }; break;
	}
	PostProcessor.AddDebugViewPass(Graph, Source, Output, DebugView, -1);
}

void FSceneRenderer::BindPrepassTargets(ID3D12GraphicsCommandList* CommandList)
{
	const D3D12_CPU_DESCRIPTOR_HANDLE Rtvs[] = { SceneNormal->GetRtv(), SceneVelocity->GetRtv() };
	const D3D12_CPU_DESCRIPTOR_HANDLE Dsv    = SceneColor->GetDsv();
	CommandList->OMSetRenderTargets(2, Rtvs, FALSE, &Dsv);
	SetScreenPassViewport(CommandList, SceneNormal->GetWidth(), SceneNormal->GetHeight());
}

void FSceneRenderer::PrepareMainBatches(const FCamera& Camera, bool bOcclusion)
{
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();

	// 컬링 + 묶음 키: PSO 변형(Masked/양면/스킨) → 머티리얼 → 메시 → LOD, 묶음 안은 가까운 순 (상태 변경 최소화 + 초기 깊이 기각). 스킨도 인스턴싱
	// 반투명/가산은 따로 먼 것부터 (와이어프레임은 모두 메인 묶음)
	BeginCpuTimer(ERenderTimer::MainCull);
	const FVector3 CameraPosition = Camera.GetPosition();
	MainBatches.Reset();
	TranslucentBatches.Reset();
	Stats.VisibleMeshes    = 0;
	Stats.ScreenSizeCulled = 0;
	// 거리·화면 크기 컬링 (사전 패스와 메인이 이 묶음을 함께 쓰므로 둘이 같은 집합을 그린다). 화면 크기 = LOD와 같은 식
	const bool  bSizeCulling   = MinScreenSize > 0.0f || MaxDrawDistance > 0.0f;
	const bool  bOrthographic  = Camera.IsOrthographic();
	const float TanHalfFov     = FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f);
	const std::vector<FMeshInstance>& Instances = MeshInstances.GetInstances();
	for (uint32 Index = 0; Index < static_cast<uint32>(Instances.size()); ++Index)
	{
		const FMeshInstance& Instance = Instances[Index];
		if (!FrozenFrustum.Intersects(Instance.WorldBounds))
		{
			continue;
		}
		if (bSizeCulling)
		{
			const float Radius     = Instance.WorldBounds.GetExtent().Length();
			const float Distance   = FVector3::Distance(Instance.WorldBounds.GetCenter(), CameraPosition);
			const float ScreenSize = bOrthographic ? LodMath::ComputeOrthographicScreenSize(Radius, Camera.GetOrthoHeight())
			                                       : LodMath::ComputePerspectiveScreenSize(Radius, Distance, TanHalfFov);
			if (LodMath::ShouldCullInstance(ScreenSize, Distance - Radius, MinScreenSize, MaxDrawDistance))
			{
				++Stats.ScreenSizeCulled;
				continue;
			}
		}
		++Stats.VisibleMeshes;
		const float Depth = FVector3::DistanceSquared(Instance.WorldBounds.GetCenter(), CameraPosition);
		if (Instance.IsTranslucent() && !bWireframe)
		{
			TranslucentBatches.Add(MakeMainBatchKey(Instance), Depth, Index);
			continue;
		}
		MainBatches.Add(MakeMainBatchKey(Instance), Depth, Index);
	}
	EndCpuTimer(ERenderTimer::MainCull);

	BeginCpuTimer(ERenderTimer::MainSort);
	MainBatches.Finalize(DynamicBuffer);
	TranslucentBatches.Finalize(DynamicBuffer, true);
	EndCpuTimer(ERenderTimer::MainSort);

	// 오클루전 1단계 준비 (항목·간접 인자 업로드 — 판정은 그래프 계산 패스)
	if (bOcclusion)
	{
		BeginCpuTimer(ERenderTimer::Occlusion);
		OcclusionCuller.PreparePhase1(MeshInstances, MainBatches, SceneColor->GetWidth(), SceneColor->GetHeight());
		EndCpuTimer(ERenderTimer::Occlusion);
	}
}

void FSceneRenderer::RecordMeshBatches(ID3D12GraphicsCommandList* CommandList, EMeshPass Pass, D3D12_GPU_VIRTUAL_ADDRESS PerFrameAddress,
                                       D3D12_GPU_VIRTUAL_ADDRESS ShadowAddress, EMeshPhase Phase, uint32& InOutDrawCalls, uint64& InOutTriangles)
{
	const bool                 bPrepassPass  = Pass == EMeshPass::Prepass;

	// 지형: 메시보다 먼저 (큰 가림막 — 초기 깊이 기각). 자기 루트 시그니처를 쓰므로 아래에서 메시 상태를 다시 설정한다
	// 사전 패스에도 그린다 (지형 깊이가 없으면 TAA/SSAO/안개/SSR이 하늘로 본다), 메인은 같은 VS로 깊이 EQUAL. 오클루전 2단계 패스는 그리지 않음
	if (Phase != EMeshPhase::Phase2)
	{
		FTerrainScreenInputs Screen;
		Screen.AmbientOcclusion = bFrameRtAo ? RayTracingEffects.GetAmbientOcclusionSrv() : AmbientOcclusion.GetResultSrv();
		for (uint32 Index = 0; Index < 3; ++Index)
		{
			Screen.DBuffer[Index] = DecalRenderer.GetTarget(Index).GetSrv();
		}
		Screen.ReflectionCaptures = ReflectionCaptures.GetCaptureList();
		Screen.CaptureAtlas       = ReflectionCaptures.GetAtlasSrv();
		Screen.ScreenReflection   = ScreenSpaceReflections.GetResultSrv();
		Screen.RayTracedShadowMask = RayTracingEffects.GetShadowMask().GetSrv();
		Screen.DdgiConstants       = Ddgi.GetShadingConstants();
		Screen.DdgiIrradiance      = Ddgi.GetIrradianceSrv();
		Screen.DdgiDistance        = Ddgi.GetDistanceSrv();
		Screen.DdgiProbeData       = Ddgi.GetProbeDataSrv();
		const ETerrainPass TerrainPass =
			bPrepassPass ? ETerrainPass::Prepass : (Pass == EMeshPass::MainDepthEqual ? ETerrainPass::MainDepthEqual : ETerrainPass::Main);
		TerrainRenderer.RenderMain(TerrainPass, PerFrameAddress, ShadowAddress, ShadowRenderer, IblRenderer, LocalLightRenderer, Screen);
		InOutDrawCalls += TerrainRenderer.GetDrawCalls();
		InOutTriangles += TerrainRenderer.GetTriangles();
	}

	// 오클루전 단계별 첫 번호 목록 (Both = 1단계 → 2단계를 한 패스에서)
	const bool bOcclusion   = Phase != EMeshPhase::All;
	const uint32 FirstPhase = Phase == EMeshPhase::Phase2 ? 2u : (bOcclusion ? 1u : 0u);
	CommandList->SetPipelineState(GetMeshPipeline(Pass, 0).Get());
	BindMeshPassRoot(CommandList, PerFrameAddress, ShadowAddress, bOcclusion ? OcclusionCuller.GetIndices(FirstPhase) : MainBatches.GetIndexBuffer());

	// 머티리얼 상수는 패스 안에서 한 번만 업로드 (사전 패스는 거칠기만 읽는다)
	std::unordered_map<const FMaterial*, D3D12_GPU_VIRTUAL_ADDRESS> MaterialConstantCache;

	const std::vector<FMeshInstance>& Instances     = MeshInstances.GetInstances();
	const FMaterial*                  BoundMaterial = nullptr;
	ID3D12PipelineState*              BoundPipeline = GetMeshPipeline(Pass, 0).Get(); // 변형 0(정적 불투명)이 바인딩된 상태로 시작
	bool                              bSkinnedIndices = false; // 오클루전 1단계: 스킨 묶음용 번호 목록으로 바꿨는가

	// DrawPhase 0 = 오클루전 없음(바로 그림), 1/2 = 오클루전 단계 (정적 묶음은 간접 드로우, 스킨 묶음은 1단계에서 바로)
	// 삼각형 = 바로 그린 삼각형 (간접 드로우 정적 삼각형은 오클루전 통계가 센다)
	auto DrawBatches = [&](uint32 DrawPhase) {
		const std::vector<FInstanceBatch>& Batches = MainBatches.GetBatches();
		for (uint32 BatchIndex = 0; BatchIndex < static_cast<uint32>(Batches.size()); ++BatchIndex)
		{
			const FInstanceBatch& Batch    = Batches[BatchIndex];
			const FMeshInstance&  Instance = Instances[Batch.Instance];
			const bool            bSkinned = Instance.IsSkinned();
			if (bSkinned && DrawPhase == 2)
			{
				continue;
			}
			const uint32         Variant  = Instance.GetPipelineVariant();
			const FMaterial*     Material = Instance.Material;
			ID3D12PipelineState* Pipeline = GetMaterialPipeline(Pass, Variant, *Material);
			if (Pipeline == nullptr) // 그래프 셰이더 실패: 기본 머티리얼로
			{
				Material = &Resources->ResolveMaterial(Resources->GetDefaultMaterial());
				Pipeline = GetMeshPipeline(Pass, Variant).Get();
			}
			if (Pipeline != BoundPipeline)
			{
				CommandList->SetPipelineState(Pipeline);
				BoundPipeline = Pipeline;
			}
			if (bSkinned && DrawPhase == 1 && !bSkinnedIndices)
			{
				// 오클루전 1단계 목록은 정적 묶음만 채운다 → 스킨 묶음은 메인 묶음의 번호 목록으로 (스킨 변형 비트가 최상위라 정적 뒤에 정렬됨)
				CommandList->SetGraphicsRootShaderResourceView(RootParam_InstanceIndices, MainBatches.GetIndexBuffer());
				bSkinnedIndices = true;
			}
			if (Material != BoundMaterial) // 사전 패스도 거칠기(SSR)를 위해 머티리얼을 평가한다
			{
				BindMeshMaterial(CommandList, *Material, MaterialConstantCache);
				BoundMaterial = Material;
			}

			CommandList->SetGraphicsRoot32BitConstant(RootParam_DrawConstants, Batch.First, 0);
			if (bSkinned)
			{
				Instance.Mesh->DrawSkinned(CommandList, Batch.Count, Instance.Lod);
				InOutTriangles += static_cast<uint64>(Instance.Mesh->GetLod(Instance.Lod).IndexCount / 3) * Batch.Count;
			}
			else if (DrawPhase == 0)
			{
				Instance.Mesh->DrawInstanced(CommandList, Batch.Count, Instance.Lod);
				InOutTriangles += static_cast<uint64>(Instance.Mesh->GetLod(Instance.Lod).IndexCount / 3) * Batch.Count;
			}
			else
			{
				Instance.Mesh->Bind(CommandList);
				OcclusionCuller.DrawIndirect(CommandList, BatchIndex, DrawPhase);
			}
			++InOutDrawCalls;
		}
	};

	switch (Phase)
	{
	case EMeshPhase::All:
		DrawBatches(0);
		break;
	case EMeshPhase::Phase1:
		DrawBatches(1);
		break;
	case EMeshPhase::Phase2:
		DrawBatches(2);
		break;
	case EMeshPhase::Both:
		DrawBatches(1);
		CommandList->SetPipelineState(GetMeshPipeline(Pass, 0).Get());
		BoundPipeline = GetMeshPipeline(Pass, 0).Get();
		CommandList->SetGraphicsRootShaderResourceView(RootParam_InstanceIndices, OcclusionCuller.GetIndices(2));
		DrawBatches(2);
		break;
	}
}

void FSceneRenderer::BindMeshPassRoot(ID3D12GraphicsCommandList* CommandList, D3D12_GPU_VIRTUAL_ADDRESS PerFrameAddress, D3D12_GPU_VIRTUAL_ADDRESS ShadowAddress,
                                      D3D12_GPU_VIRTUAL_ADDRESS InstanceIndices)
{
	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetGraphicsRootConstantBufferView(RootParam_PerFrame, PerFrameAddress);
	CommandList->SetGraphicsRootConstantBufferView(RootParam_Shadow, ShadowAddress);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_ShadowMap, ShadowRenderer.GetShadowMapSrv().Gpu);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_Ibl, IblRenderer.GetLightingTable().Gpu);
	CommandList->SetGraphicsRootConstantBufferView(RootParam_Cluster, LocalLightRenderer.GetConstants());
	CommandList->SetGraphicsRootShaderResourceView(RootParam_LocalLights, LocalLightRenderer.GetLightList());
	CommandList->SetGraphicsRootShaderResourceView(RootParam_ClusterData, LocalLightRenderer.GetClusterData());
	CommandList->SetGraphicsRootShaderResourceView(RootParam_LocalShadowMatrices, LocalLightRenderer.GetShadowMatrices());
	CommandList->SetGraphicsRootDescriptorTable(RootParam_LocalShadowMap, LocalLightRenderer.GetShadowMapSrv().Gpu);
	CommandList->SetGraphicsRootShaderResourceView(RootParam_Instances, MeshInstances.GetGpuData());
	CommandList->SetGraphicsRootShaderResourceView(RootParam_InstanceIndices, InstanceIndices);
	CommandList->SetGraphicsRootShaderResourceView(RootParam_SkinPalette, SkinPalettes.GetGpuData());
	CommandList->SetGraphicsRootDescriptorTable(RootParam_AmbientOcclusion,
	                                            (bFrameRtAo ? RayTracingEffects.GetAmbientOcclusionSrv() : AmbientOcclusion.GetResultSrv()).Gpu);
	for (uint32 Index = 0; Index < 3; ++Index)
	{
		CommandList->SetGraphicsRootDescriptorTable(RootParam_DBufferA + Index, DecalRenderer.GetTarget(Index).GetSrv().Gpu);
	}
	CommandList->SetGraphicsRootShaderResourceView(RootParam_ReflectionCaptures, ReflectionCaptures.GetCaptureList());
	CommandList->SetGraphicsRootDescriptorTable(RootParam_CaptureAtlas, ReflectionCaptures.GetAtlasSrv().Gpu);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_ScreenReflection, ScreenSpaceReflections.GetResultSrv().Gpu);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_RayTracedShadowMask, RayTracingEffects.GetShadowMask().GetSrv().Gpu);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_LightTextures, Rhi->GetSrvAllocator().GetHeap()->GetGPUDescriptorHandleForHeapStart());
	CommandList->SetGraphicsRootConstantBufferView(RootParam_DdgiConstants, Ddgi.GetShadingConstants());
	CommandList->SetGraphicsRootDescriptorTable(RootParam_DdgiIrradiance, Ddgi.GetIrradianceSrv().Gpu);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_DdgiDistance, Ddgi.GetDistanceSrv().Gpu);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_DdgiProbeData, Ddgi.GetProbeDataSrv().Gpu);
	// 안개(b6/t23)는 반투명 패스만 읽는다 → DrawTranslucentBatches가 바인딩
}

void FSceneRenderer::DrawTranslucentBatches(ID3D12GraphicsCommandList* CommandList, D3D12_GPU_VIRTUAL_ADDRESS PerFrameAddress,
                                            D3D12_GPU_VIRTUAL_ADDRESS ShadowAddress, D3D12_GPU_VIRTUAL_ADDRESS FogConstants, uint32& OutDrawCalls,
                                            uint64& OutTriangles)
{

	CommandList->SetPipelineState(GetMeshPipeline(EMeshPass::Translucent, 0).Get());
	BindMeshPassRoot(CommandList, PerFrameAddress, ShadowAddress, TranslucentBatches.GetIndexBuffer());
	CommandList->SetGraphicsRootConstantBufferView(RootParam_Fog, FogConstants); // PrepareVolumetric이 프레임마다 올림
	CommandList->SetGraphicsRootDescriptorTable(RootParam_FogVolume, FogRenderer.GetVolumeSrv().Gpu);

	// 정렬 순서(먼 것부터)를 지키므로 PSO/머티리얼은 바뀔 때마다 바꾼다
	std::unordered_map<const FMaterial*, D3D12_GPU_VIRTUAL_ADDRESS> MaterialConstantCache;
	const std::vector<FMeshInstance>&                               Instances     = MeshInstances.GetInstances();
	const FMaterial*                                                BoundMaterial = nullptr;
	ID3D12PipelineState*                                            BoundPipeline = GetMeshPipeline(EMeshPass::Translucent, 0).Get();
	OutDrawCalls                                                        = 0;
	OutTriangles                                                        = 0;
	for (const FInstanceBatch& Batch : TranslucentBatches.GetBatches())
	{
		const FMeshInstance& Instance = Instances[Batch.Instance];
		const uint32         Variant  = Instance.GetPipelineVariant();
		const FMaterial*     Material = Instance.Material;
		ID3D12PipelineState* Pipeline = GetMaterialPipeline(EMeshPass::Translucent, Variant, *Material);
		if (Pipeline == nullptr) // 그래프 셰이더 실패: 기본 머티리얼로
		{
			Material = &Resources->ResolveMaterial(Resources->GetDefaultMaterial());
			Pipeline = GetMeshPipeline(EMeshPass::Translucent, Variant).Get();
		}
		if (Pipeline != BoundPipeline)
		{
			CommandList->SetPipelineState(Pipeline);
			BoundPipeline = Pipeline;
		}
		if (Material != BoundMaterial)
		{
			BindMeshMaterial(CommandList, *Material, MaterialConstantCache);
			BoundMaterial = Material;
		}
		CommandList->SetGraphicsRoot32BitConstant(RootParam_DrawConstants, Batch.First, 0);
		if (Instance.IsSkinned())
		{
			Instance.Mesh->DrawSkinned(CommandList, Batch.Count, Instance.Lod);
			OutTriangles += static_cast<uint64>(Instance.Mesh->GetLod(Instance.Lod).IndexCount / 3) * Batch.Count;
		}
		else
		{
			Instance.Mesh->DrawInstanced(CommandList, Batch.Count, Instance.Lod);
			OutTriangles += static_cast<uint64>(Instance.Mesh->GetLod(Instance.Lod).IndexCount / 3) * Batch.Count;
		}
		++OutDrawCalls;
	}
}

void FSceneRenderer::ReportTextureStreaming(FScene& Scene, const FCamera& Camera, uint32 Height, float MipBias)
{
	if (!Resources->IsTextureStreamingActive())
	{
		return;
	}
	FTextureStreamingView View;
	View.Instances      = &MeshInstances;
	View.CameraPosition = Camera.GetPosition();
	View.CameraForward  = Camera.GetForwardVector();
	View.bOrthographic  = Camera.IsOrthographic();
	View.TanHalfFovY    = FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f);
	View.OrthoHeight    = Camera.GetOrthoHeight();
	View.NearZ          = Camera.GetNearZ();
	View.ScreenHeight   = Height;
	View.MipBias        = MipBias;
	View.IsInMainView   = [this](const FBox& Bounds) { return FrozenFrustum.Intersects(Bounds); };
	View.IsShadowCaster = [this](const FBox& Bounds) {
		return ShadowRenderer.IntersectsCasterVolume(Bounds) || LocalLightRenderer.IntersectsShadowCaster(Bounds);
	};
	// 메시 인스턴스 밖에서 그리는 머티리얼 (지형 레이어 — 이번 Prepare가 쓴 것, 데칼): 화면 크기 식을 적용할 수 없어 전체 밉
	FResourceRoots Roots;
	TerrainRenderer.CollectResourceRoots(Roots);
	View.FullResidencyMaterials.assign(Roots.Materials.begin(), Roots.Materials.end());
	Scene.GetRegistry().View<FDecalComponent>().Each([&](FEntity, FDecalComponent& Decal) {
		if (Decal.Material.IsValid())
		{
			View.FullResidencyMaterials.push_back(Decal.Material);
		}
	});
	Resources->ReportTextureStreamingView(View);
}

void FSceneRenderer::SelectLods(const FCamera& Camera)
{
	if (!bEnableLod)
	{
		return; // 모두 LOD0 (Gather 기본값)
	}
	// 메인 카메라 화면 크기로 고르고 그림자 패스도 같은 LOD를 쓴다 (그림자와 본체 모양이 어긋나지 않게).
	// 스킨 메시도 같은 규칙 (경계 = 스킨 팔레트의 월드 경계). 레이 트레이싱 스킨 BLAS는 항상 LOD0
	const bool     bOrthographic  = Camera.IsOrthographic();
	const float    TanHalfFov     = FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f);
	const FVector3 CameraPosition = Camera.GetPosition();
	std::vector<FMeshInstance>& Instances = MeshInstances.GetInstances();
	auto SelectOne = [&](FMeshInstance& Instance) {
		if ((Instance.IsSkinned() && !bSkinnedLod) || Instance.bFixedLod || Instance.Mesh->GetLodCount() <= 1)
		{
			return;
		}
		if (ForcedLod >= 0)
		{
			Instance.Lod = std::min(static_cast<uint32>(ForcedLod), Instance.Mesh->GetLodCount() - 1);
			return;
		}
		const float Radius = Instance.WorldBounds.GetExtent().Length();
		const float ScreenSize =
			bOrthographic ? LodMath::ComputeOrthographicScreenSize(Radius, Camera.GetOrthoHeight())
			              : LodMath::ComputePerspectiveScreenSize(Radius, FVector3::Distance(Instance.WorldBounds.GetCenter(), CameraPosition), TanHalfFov);
		// 히스테리시스: 엔티티별 이전 LOD (처음이거나 엔티티가 바뀌었으면 없음). 칸은 호출 전에 크기를 맞춘다
		FLodHistory& History  = LodHistory[Instance.Entity.Index];
		const uint32 Previous = History.Generation == Instance.Entity.Generation ? History.Lod : ~0u;
		Instance.Lod = LodMath::SelectLodWithHysteresis(ScreenSize, Instance.Mesh->GetLodScreenSizes(), Instance.Mesh->GetLodCount(),
		                                                Instance.IsSkinned() ? LodScale * SkinnedLodScale : LodScale, Previous,
		                                                LodHysteresis);
		History = { Instance.Entity.Generation, Instance.Lod };
	};
	// 칸 크기 먼저 (엔티티 번호 최댓값)
	uint32 MaxIndex = 0;
	for (const FMeshInstance& Instance : Instances)
	{
		MaxIndex = std::max(MaxIndex, Instance.Entity.Index);
	}
	if (!Instances.empty() && MaxIndex >= LodHistory.size())
	{
		LodHistory.resize(static_cast<size_t>(MaxIndex) + 1);
	}
	// Gather 부분은 엔티티마다 하나라 병렬 (자기 칸만 쓴다), AddExternal 부분은 엔티티가 겹칠 수 있어 순서대로
	const uint32 GatheredCount = MeshInstances.GetGatheredCount();
	FParallel::ParallelFor(GatheredCount, 256, [&](uint32 Begin, uint32 End) {
		for (uint32 Index = Begin; Index < End; ++Index)
		{
			SelectOne(Instances[Index]);
		}
	});
	for (size_t Index = GatheredCount; Index < Instances.size(); ++Index)
	{
		SelectOne(Instances[Index]);
	}
}

FPerFrameConstants FSceneRenderer::BuildPerFrameConstants(FScene& Scene, const FCamera& Camera) const
{
	FPerFrameConstants PerFrame;
	PerFrame.ViewProjection = Camera.GetViewProjectionMatrix();
	PerFrame.CameraPosition = Camera.GetPosition();
	PerFrame.SkyColor         = SkyColor;
	PerFrame.GroundColor      = GroundColor;
	PerFrame.AmbientIntensity = AmbientIntensity;

	// 하늘광: 씬의 첫 FSkyLightComponent가 환경광/하늘 밝기를 곱한다
	bool bFoundSkyLight = false;
	Scene.GetRegistry().View<FSkyLightComponent>().Each([&](FEntity, FSkyLightComponent& SkyLight) {
		if (!bFoundSkyLight)
		{
			PerFrame.AmbientIntensity *= FMath::Max(SkyLight.Intensity, 0.0f);
			bFoundSkyLight = true;
		}
	});

	// 첫 번째 방향광 사용 (여러 광원은 Phase 6)
	bool bFoundLight = false;
	Scene.GetRegistry().View<FTransformComponent, FDirectionalLightComponent>().Each(
		[&](FEntity, FTransformComponent& Transform, FDirectionalLightComponent& Light) {
			if (bFoundLight)
			{
				return;
			}
			PerFrame.DirectionalLight.Direction = Transform.GetWorldForward();
			PerFrame.DirectionalLight.Color     = Light.Color;
			PerFrame.DirectionalLight.Intensity = Light.Intensity;
			bFoundLight                         = true;
		});

	if (!bFoundLight)
	{
		PerFrame.DirectionalLight.Direction = FVector3(1.0f, 0.5f, -1.0f).GetNormalized();
		PerFrame.DirectionalLight.Intensity = 3.0f; // HDR 단위 (확산 BRDF에 1/π가 있어 흰 면 ≈ 0.95)
	}
	return PerFrame;
}

ID3D12PipelineState* FSceneRenderer::GetMaterialPipeline(EMeshPass Pass, uint32 Variant, const FMaterial& Material)
{
	if (Material.Shader == nullptr)
	{
		return GetMeshPipeline(Pass, Variant).Get();
	}
	if (Pass == EMeshPass::Wireframe)
	{
		Variant &= MaterialRender::VariantSkinned;
	}
	std::unique_ptr<FGraphPipelineSet>& Set = GraphPipelines[Material.Shader->Hash];
	if (!Set)
	{
		// 오래 쓰지 않은 세트 정리 (편집하며 해시가 바뀐 옛 셰이더) — 진행 중인 프레임이 참조할 수 있어 지연 해제
		constexpr uint64 UnusedFrames = 600;
		for (auto It = GraphPipelines.begin(); It != GraphPipelines.end();)
		{
			if (It->second && It->second->LastUsedFrame + UnusedFrames < SceneFrameCount)
			{
				for (auto& PassPipelines : It->second->Pipelines)
				{
					for (FD3D12PipelineState& Pipeline : PassPipelines)
					{
						if (Pipeline.Get() != nullptr)
						{
							Rhi->DeferRelease(Pipeline.Detach());
						}
					}
				}
				It = GraphPipelines.erase(It);
			}
			else
			{
				++It;
			}
		}
		std::unique_ptr<FGraphPipelineSet>& NewSet = GraphPipelines[Material.Shader->Hash];
		NewSet                                     = std::make_unique<FGraphPipelineSet>();
		return GetMaterialPipeline(Pass, Variant, Material);
	}
	Set->LastUsedFrame = SceneFrameCount;
	const uint32 PassIndex = static_cast<uint32>(Pass);
	uint8&       State     = Set->State[PassIndex][Variant];
	if (State == 0)
	{
		State = CreateMeshPipeline(Set->Pipelines[PassIndex][Variant], Pass, Variant, Material.Shader.get()) ? 1 : 2;
		if (State == 2)
		{
			E_LOG(LogRenderer, Error, "그래프 머티리얼 셰이더 PSO 생성 실패 ({}, {:016x}, 패스 {}, 변형 {}) — 기본 머티리얼로 그립니다", Material.Name,
			      Material.Shader->Hash, PassIndex, Variant);
		}
	}
	return State == 1 ? Set->Pipelines[PassIndex][Variant].Get() : nullptr;
}

void FSceneRenderer::BindMeshMaterial(ID3D12GraphicsCommandList* CommandList, const FMaterial& Material,
                                      std::unordered_map<const FMaterial*, D3D12_GPU_VIRTUAL_ADDRESS>& ConstantCache)
{
	auto Found = ConstantCache.find(&Material);
	if (Found == ConstantCache.end())
	{
		Found = ConstantCache.emplace(&Material, MaterialRender::UploadMaterialConstants(Rhi->GetDynamicBuffer(), Material)).first;
	}
	CommandList->SetGraphicsRootConstantBufferView(RootParam_Material, Found->second);
	// 테이블은 항상 5칸 이상(BuildMaterialTable)이라 두 자리에 같은 테이블을 묶는다 — 고정 PBR은 t0~t4, 그래프는 공간 2만 읽는다
	CommandList->SetGraphicsRootDescriptorTable(RootParam_MaterialTexture, Material.TextureTable.Gpu);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_MaterialGraphTextures, Material.TextureTable.Gpu);
}

void FSceneRenderer::ReleaseGraphPipelines()
{
	for (auto& [Hash, Set] : GraphPipelines)
	{
		for (auto& PassPipelines : Set->Pipelines)
		{
			for (FD3D12PipelineState& Pipeline : PassPipelines)
			{
				if (Pipeline.Get() != nullptr)
				{
					Rhi->DeferRelease(Pipeline.Detach());
				}
			}
		}
	}
	GraphPipelines.clear();
}
