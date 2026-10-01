#include "Renderer/SceneRenderer.h"

#include "Core/CommandLine.h"
#include "Core/Paths.h"
#include "Core/StringConv.h"
#include "Renderer/AssetCache.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/Camera.h"
#include "Renderer/LodMath.h"
#include "Renderer/Material.h"
#include "Renderer/PixelArtMath.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Renderer/ReflectionMath.h"
#include "Renderer/TemporalMath.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cstring>
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
	};
} // namespace

bool FSceneRenderer::Init(FD3D12RHI& InRhi, FResourceManager& InResources)
{
	E_CHECKF(Rhi == nullptr, "씬 렌더러가 이미 초기화되어 있습니다");
	Rhi       = &InRhi;
	Resources = &InResources;

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
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP));
	RootSignature.AddStaticSampler(FD3D12RootSignature::MakeStaticSampler(0, D3D12_FILTER_ANISOTROPIC));

	// s2: 섀도우 비교 샘플러 (하드웨어 2x2 PCF, 범위 밖은 빛 받음)
	D3D12_STATIC_SAMPLER_DESC ShadowSamplerDesc = FD3D12RootSignature::MakeStaticSampler(
		2, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER);
	ShadowSamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
	ShadowSamplerDesc.BorderColor    = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	ShadowSamplerDesc.MaxAnisotropy  = 1;
	RootSignature.AddStaticSampler(ShadowSamplerDesc);
	if (!RootSignature.Finalize(Device, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"MeshRootSignature"))
	{
		return false;
	}

	for (uint32 Pass = 0; Pass < static_cast<uint32>(EMeshPass::Count); ++Pass)
	{
		for (uint32 Skinned = 0; Skinned < 2; ++Skinned)
		{
			if (!CreateMeshPipeline(MeshPipelines[Pass][Skinned], static_cast<EMeshPass>(Pass), Skinned != 0, false))
			{
				return false;
			}
		}
	}
	if (!PostProcessor.Init(*Rhi, ShaderLibrary))
	{
		return false;
	}
	if (!ShadowRenderer.Init(*Rhi, ShaderLibrary) || !IblRenderer.Init(*Rhi, ShaderLibrary) || !LocalLightRenderer.Init(*Rhi, ShaderLibrary) ||
	    !OcclusionCuller.Init(*Rhi, ShaderLibrary) || !ScreenPassRoot.Init(Device) || !TemporalAA.Init(*Rhi, ShaderLibrary, ScreenPassRoot) ||
	    !AmbientOcclusion.Init(*Rhi, ShaderLibrary, ScreenPassRoot) || !DecalRenderer.Init(*Rhi, ShaderLibrary, ScreenPassRoot) ||
	    !FogRenderer.Init(*Rhi, ShaderLibrary, ScreenPassRoot) || !ScreenSpaceReflections.Init(*Rhi, ShaderLibrary, ScreenPassRoot) ||
	    !ReflectionCaptures.Init(*Rhi, ShaderLibrary))
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

	GpuTimer.Init(Device, Rhi->GetGraphicsQueue().GetQueue(), FD3D12RHI::FrameCount, L"SceneRendererTimestamps"); // 실패해도 GPU 시간만 0

	const FCommandLine CommandLine = FCommandLine::FromProcess();
	PerfCapture                    = FPerfCapture{};
	PerfCapture.bEnabled           = CommandLine.HasFlag(L"--perf-capture");
	if (CommandLine.HasFlag(L"--occlusion"))
	{
		bEnableOcclusion = true; // 측정/비교용 (기본 끔)
	}
	if (CommandLine.HasFlag(L"--no-skin-culling"))
	{
		bSkinVisibilityCulling = false; // 측정/비교용
	}
	if (CommandLine.HasFlag(L"--no-particle-culling"))
	{
		ParticleRenderer.bEnableCulling = false; // 측정/비교용
	}
	if (CommandLine.HasFlag(L"--no-depth-prepass"))
	{
		bDepthPrepass = false; // 측정/비교용
	}
	if (const std::wstring View = CommandLine.GetValue(L"--debug-view"); !View.empty())
	{
		DebugView = View == L"normal" ? 1u : View == L"velocity" ? 2u : View == L"depth" ? 3u : View == L"ao" ? 4u : View == L"ssr" ? 5u : 0u;
	}
	if (CommandLine.HasFlag(L"--no-taa"))
	{
		PostProcessSettings.bTemporalAA = false; // 비교용
	}
	if (CommandLine.HasFlag(L"--no-ssao"))
	{
		PostProcessSettings.bAmbientOcclusion = false; // 비교용
	}
	if (CommandLine.HasFlag(L"--no-ssr"))
	{
		PostProcessSettings.bScreenSpaceReflections = false; // 비교용
	}
	if (CommandLine.HasFlag(L"--bake-captures"))
	{
		bBakeCapturesRequested = true; // 첫 Render에서 반사 캡처 굽기 (자동 검증용)
	}
	if (CommandLine.HasFlag(L"--jitter"))
	{
		bTemporalJitter = true; // 지터 확인용 (TAA 없이 켜면 화면이 떨린다)
	}
	if (CommandLine.HasFlag(L"--no-lod"))
	{
		bEnableLod = false; // 측정/비교용
	}
	if (const std::wstring ForceLod = CommandLine.GetValue(L"--force-lod"); !ForceLod.empty())
	{
		ForcedLod = std::stoi(ForceLod); // LOD 모양 확인용
	}
	if (const std::wstring Hysteresis = CommandLine.GetValue(L"--lod-hysteresis"); !Hysteresis.empty())
	{
		LodHysteresis = std::stof(Hysteresis); // 비교용 (0 = 끔)
	}
	if (const std::wstring Warmup = CommandLine.GetValue(L"--perf-warmup"); !Warmup.empty())
	{
		PerfCapture.WarmupFrames = static_cast<uint32>(std::max(0, std::stoi(Warmup)));
	}

	E_LOG(LogRenderer, Display, "씬 렌더러 초기화 완료 (HDR {}, 톤매핑)", "R16G16B16A16_FLOAT");
	return true;
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
	default:                        return "?";
	}
}

void FSceneRenderer::BeginTimer(ERenderTimer Timer)
{
	const uint32 Index  = static_cast<uint32>(Timer);
	TimerStarts[Index] = FClock::now();
	GpuTimer.BeginScope(Rhi->GetCommandList(), Index);
}

void FSceneRenderer::BeginCpuTimer(ERenderTimer Timer)
{
	TimerStarts[static_cast<uint32>(Timer)] = FClock::now();
}

void FSceneRenderer::EndCpuTimer(ERenderTimer Timer)
{
	const uint32 Index = static_cast<uint32>(Timer);
	Stats.CpuMs[Index] += std::chrono::duration<float, std::milli>(FClock::now() - TimerStarts[Index]).count();
}

void FSceneRenderer::EndTimer(ERenderTimer Timer)
{
	const uint32 Index = static_cast<uint32>(Timer);
	Stats.CpuMs[Index] += std::chrono::duration<float, std::milli>(FClock::now() - TimerStarts[Index]).count();
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
	Capture.SkinnedCulled += Stats.SkinnedCulled;
	Capture.UploadBytes += static_cast<double>(Stats.UploadBytes);
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
	E_LOG(LogRenderer, Display,
	      "[성능] {} 프레임 평균 ({}): 프레임 {:.3f} ms, 드로우 {:.1f} (그림자 {:.1f}, 깊이 사전 {:.1f}), 삼각형 {:.0f} (그림자 {:.0f}), 메시 {:.1f}/{}",
	      Capture.Frames, Config, Capture.FrameMs / Count, Capture.DrawCalls / Count, Capture.ShadowDrawCalls / Count, Capture.PrepassDrawCalls / Count,
	      Capture.Triangles / Count, Capture.ShadowTriangles / Count, Capture.VisibleMeshes / Count, Capture.TotalMeshes);
	if (Capture.OcclusionTested > 0.0)
	{
		E_LOG(LogRenderer, Display, "[성능] 오클루전: 정적 인스턴스 {:.1f} 중 그림 {:.1f} (2단계 {:.2f}), 가려짐 {:.1f}", Capture.OcclusionTested / Count,
		      Capture.OcclusionDrawn / Count, Capture.OcclusionPhase2 / Count, (Capture.OcclusionTested - Capture.OcclusionDrawn) / Count);
	}
	E_LOG(LogRenderer, Display, "[성능] 스킨 메시: 팔레트 {:.1f}, 가시성 제외 {:.1f}, 씬 렌더러 업로드 {:.1f} KB", Capture.SkinnedDrawn / Count,
	      Capture.SkinnedCulled / Count, Capture.UploadBytes / Count / 1024.0);
	E_LOG(LogRenderer, Display, "[성능] CPU ms: {}", Cpu);
	E_LOG(LogRenderer, Display, "[성능] GPU ms: {}", Gpu);
}

bool FSceneRenderer::CreateMeshPipeline(FD3D12PipelineState& OutPipeline, EMeshPass Pass, bool bSkinned, bool bForceRecompile)
{
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Mesh.hlsl";
	VertexDesc.EntryPoint = bSkinned ? L"VSSkinned" : L"VSMain";
	VertexDesc.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc PixelDesc = VertexDesc;
	PixelDesc.EntryPoint         = Pass == EMeshPass::Prepass ? L"PSPrepass" : L"PSMain";
	PixelDesc.Stage              = EShaderStage::Pixel;

	if (bForceRecompile && (!ShaderLibrary.CookShader(VertexDesc) || !ShaderLibrary.CookShader(PixelDesc)))
	{
		return false;
	}

	const ComPtr<IDxcBlob> VertexShader = ShaderLibrary.GetShader(VertexDesc);
	const ComPtr<IDxcBlob> PixelShader  = ShaderLibrary.GetShader(PixelDesc);
	if (!VertexShader || !PixelShader)
	{
		return false;
	}

	FGraphicsPipelineDesc PsoDesc;
	PsoDesc.RootSignature          = RootSignature.Get();
	PsoDesc.VertexShader           = FD3D12ShaderCompiler::ToBytecode(VertexShader.Get());
	PsoDesc.PixelShader            = FD3D12ShaderCompiler::ToBytecode(PixelShader.Get());
	PsoDesc.InputLayout            = bSkinned ? FStaticMesh::GetSkinnedInputLayout() : FStaticMesh::GetInputLayout();
	PsoDesc.RenderTargetFormats[0] = SceneColorFormat;
	PsoDesc.DepthStencilFormat     = FD3D12RHI::DepthBufferFormat;
	PsoDesc.bDepthEnable           = true;
	const wchar_t* DebugName       = bSkinned ? L"SkinnedMeshPipeline" : L"MeshPipeline";
	switch (Pass)
	{
	case EMeshPass::MainDepthEqual:
		PsoDesc.DepthFunc   = D3D12_COMPARISON_FUNC_EQUAL; // 사전 패스와 같은 정점 셰이더 → 같은 깊이
		PsoDesc.bDepthWrite = false;
		DebugName           = bSkinned ? L"SkinnedMeshDepthEqualPipeline" : L"MeshDepthEqualPipeline";
		break;
	case EMeshPass::Wireframe:
		PsoDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
		PsoDesc.CullMode = D3D12_CULL_MODE_NONE;
		DebugName        = bSkinned ? L"SkinnedMeshWireframePipeline" : L"MeshWireframePipeline";
		break;
	case EMeshPass::Prepass:
		PsoDesc.NumRenderTargets       = 2;
		PsoDesc.RenderTargetFormats[0] = SceneNormalFormat;
		PsoDesc.RenderTargetFormats[1] = SceneVelocityFormat;
		DebugName                      = bSkinned ? L"SkinnedMeshPrepassPipeline" : L"MeshPrepassPipeline";
		break;
	default:
		break;
	}
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), PsoDesc, DebugName);
}

bool FSceneRenderer::ReloadShaders(bool bForceRecompile)
{
	E_CHECKF(Rhi != nullptr, "씬 렌더러가 초기화되지 않았습니다");

	// 패스·종류별로 새 PSO를 만들고 성공한 것만 교체 (이전 PSO는 진행 중인 프레임이 참조할 수 있으므로 지연 해제).
	// 강제 재컴파일은 셰이더 조합(정점 2 × 픽셀 2)마다 한 번: Main(VS + PSMain), Prepass(VS + PSPrepass)
	bool bMeshOk = true;
	for (uint32 Pass = 0; Pass < static_cast<uint32>(EMeshPass::Count); ++Pass)
	{
		const EMeshPass MeshPass = static_cast<EMeshPass>(Pass);
		const bool      bCook    = bForceRecompile && (MeshPass == EMeshPass::Main || MeshPass == EMeshPass::Prepass);
		for (uint32 Skinned = 0; Skinned < 2; ++Skinned)
		{
			FD3D12PipelineState NewPipeline;
			if (!CreateMeshPipeline(NewPipeline, MeshPass, Skinned != 0, bCook))
			{
				E_LOG(LogRenderer, Error, "메시 셰이더 다시 로드 실패 (패스 {}, 스킨 {}): 기존 파이프라인을 유지합니다", Pass, Skinned);
				bMeshOk = false;
				continue;
			}
			MeshPipelines[Pass][Skinned].Swap(NewPipeline);
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
	    !ReflectionCaptures.ReloadShaders(bForceRecompile))
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

	E_LOG(LogRenderer, Display, "셰이더 다시 로드 완료 (메시 파이프라인 재생성)");
	return true;
}

void FSceneRenderer::Shutdown()
{
	if (Rhi == nullptr)
	{
		return;
	}
	Rhi->GetGraphicsQueue().Flush();
	LogPerfCapture();
	GpuTimer.Shutdown();
	SceneColor.reset();
	SceneNormal.reset();
	SceneVelocity.reset();
	PixelArtColor.reset();
	MotionHistory.clear();
	bHasPrevView = false;
	PostProcessor.Shutdown();
	ShadowRenderer.Shutdown();
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
	ScreenPassRoot.Shutdown();
	for (auto& PassPipelines : MeshPipelines)
	{
		for (FD3D12PipelineState& Pipeline : PassPipelines)
		{
			Pipeline.Shutdown();
		}
	}
	RootSignature.Shutdown();
	ShaderLibrary.Shutdown();
	ShaderCompiler.Shutdown();
	MainBatches.Reset();
	Rhi       = nullptr;
	Resources = nullptr;
}

void FSceneRenderer::SetFreezeCulling(bool bFreeze)
{
	bCullingFrozen = bFreeze;
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
	AmbientOcclusion.EnsureTargets(Width, Height);
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

	// 측정: 지난 결과(GPU는 슬롯 수만큼 늦음)를 통계에 옮기고 이번 프레임 칸을 비운다
	const FClock::time_point Now = FClock::now();
	Stats.FrameIntervalMs        = bHasLastRenderTime ? std::chrono::duration<float, std::milli>(Now - LastRenderTime).count() : 0.0f;
	LastRenderTime               = Now;
	bHasLastRenderTime           = true;
	std::fill(std::begin(Stats.CpuMs), std::end(Stats.CpuMs), 0.0f);
	const bool bGpuTiming = GpuTimer.BeginFrame(Rhi->GetFrameSlot(), Rhi->GetFrameNumber());
	if (bGpuTiming)
	{
		for (uint32 Index = 0; Index < static_cast<uint32>(ERenderTimer::Count); ++Index)
		{
			Stats.GpuMs[Index] = GpuTimer.GetScopeMs(Index);
		}
	}
	BeginTimer(ERenderTimer::Total);

	// 한 Rhi 프레임에 몇 번 불렸는지 (여러 뷰/씬을 번갈아 그리는 렌더러는 시간 이력을 쓰지 않는다)
	const uint64 FrameNumber = Rhi->GetFrameNumber();
	if (FrameNumber != CurrentFrameNumber)
	{
		ViewsLastFrame     = CurrentFrameNumber + 1 == FrameNumber ? ViewsThisFrame : 0;
		CurrentFrameNumber = FrameNumber;
		ViewsThisFrame     = 0;
	}
	++ViewsThisFrame;

	// 하늘 환경맵 (하늘광 EnvironmentMap/회전이 바뀌면 IBL 다시 생성)
	UpdateEnvironment(Scene);

	// 반사 캡처: 끝난 굽기 저장 + 요청된 굽기 (큐브 면 6개를 이번 프레임 명령 목록에 먼저 그린다)
	ReflectionCaptures.ProcessPendingSaves();
	if (bBakeCapturesRequested)
	{
		bBakeCapturesRequested = false;
		BakeReflectionCaptures(Scene);
	}

	RenderFrame(Scene, Camera, Output);

	EndTimer(ERenderTimer::Total);
	GpuTimer.EndFrame(CommandList);
	if (bGpuTiming)
	{
		AccumulatePerfCapture();
	}
}

void FSceneRenderer::RenderFrame(FScene& Scene, const FCamera& Camera, const FRenderOutput& Output)
{
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();

	const FPixelArtComponent* PixelArt = FindPixelArtSettings(Scene);
	if (PixelArt == nullptr)
	{
		RenderSceneColor(Scene, Camera, Output.Width, Output.Height, true);

		// TAA: 톤매핑 전 HDR 이력과 섞은 결과가 포스트 입력. 한 렌더러가 여러 뷰를 그리는 경우(미리보기/썸네일)·와이어프레임은 끔
		const bool                    bTaa      = PostProcessSettings.bTemporalAA && !bWireframe && ViewsThisFrame == 1 && ViewsLastFrame == 1;
		const FD3D12DescriptorHandle* PostInput = &SceneColor->GetSrv();
		float                         Sharpness = 0.0f;
		if (bTaa)
		{
			if (!bTaaRanLastFrame)
			{
				TemporalAA.ResetHistory(); // 꺼져 있던 동안의 이력은 낡았다
			}
			BeginTimer(ERenderTimer::TemporalAA);
			FTemporalAAInputs Inputs;
			Inputs.SceneColor    = SceneColor.get();
			Inputs.Velocity      = SceneVelocity.get();
			Inputs.Reprojection  = CurrentReprojection;
			Inputs.bHistoryValid = bTemporalHistoryValid;
			Inputs.CurrentWeight = PostProcessSettings.TemporalAACurrentWeight;
			PostInput            = &TemporalAA.Resolve(Inputs).GetSrv();
			Sharpness            = PostProcessSettings.TemporalAASharpness;
			EndTimer(ERenderTimer::TemporalAA);
		}
		bTaaRanLastFrame = bTaa;

		BeginTimer(ERenderTimer::PostProcess);
		PostProcessor.Render(CommandList, *PostInput, Output, PostProcessSettings, Sharpness);
		EndTimer(ERenderTimer::PostProcess);
		RenderDebugView(Output);
		return;
	}
	bTaaRanLastFrame = false; // 픽셀 아트: 정수 격자 스냅과 충돌하므로 TAA 없음

	// 픽셀 아트: 저해상도 씬 → 저해상도 포스트(톤매핑) → 합성 확대
	const uint32 PixelSize    = FPixelArtMath::ClampPixelSize(PixelArt->PixelSize);
	const uint32 SourceWidth  = FPixelArtMath::GetSourceDimension(Output.Width, PixelSize);
	const uint32 SourceHeight = FPixelArtMath::GetSourceDimension(Output.Height, PixelSize);

	FPixelArtCompositeParams Params;
	const FCamera            SourceCamera = BuildPixelArtCamera(*PixelArt, Camera, Output, SourceWidth, SourceHeight, Params);
	RenderSceneColor(Scene, SourceCamera, SourceWidth, SourceHeight, false); // 지터는 정수 격자 스냅과 충돌

	BeginTimer(ERenderTimer::PostProcess);
	EnsureTarget(PixelArtColor, SourceWidth, SourceHeight, L"PixelArtColor", FRenderTargetDesc::MakeHdr(false));
	PixelArtColor->Begin(CommandList, nullptr); // 톤매핑이 전체를 덮어쓴다
	PostProcessor.Render(CommandList, SceneColor->GetSrv(), PixelArtColor->GetOutput(), PostProcessSettings);
	PixelArtColor->End(CommandList);

	PostProcessor.RenderPixelArtComposite(CommandList, *PixelArtColor, *SceneColor, Output, Params);
	EndTimer(ERenderTimer::PostProcess);
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
} // namespace

void FSceneRenderer::RenderSceneColor(FScene& Scene, const FCamera& Camera, uint32 Width, uint32 Height, bool bAllowJitter)
{
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	++SceneFrameCount;

	// 시간 이력: 이 렌더러가 연속 프레임에 뷰 하나만 그리고, 같은 크기이고, 카메라 컷이 없을 때만 이전 프레임 값을 쓴다
	const bool       bSingleView              = ViewsThisFrame == 1 && ViewsLastFrame == 1;
	const FMatrix4x4 UnjitteredViewProjection = Camera.GetUnjitteredViewProjectionMatrix();
	bTemporalHistoryValid = bSingleView && bHasPrevView && PrevScene == &Scene && PrevTargetWidth == Width && PrevTargetHeight == Height &&
	                        !FTemporalMath::IsCameraCut(PrevCameraPosition, PrevCameraForward, Camera.GetPosition(), Camera.GetForwardVector(),
	                                                    CameraCutDistance, CameraCutAngleDegrees);

	// 지터는 씬 컬러에 그리는 패스(메시/파티클)에만: 그림자 캐스케이드·클러스터·컬링·LOD는 지터 없는 카메라 (그림자 떨림 방지)
	CurrentJitterNdc = FVector2::ZeroVector;
	if ((bTemporalJitter || (PostProcessSettings.bTemporalAA && !bWireframe)) && bAllowJitter && bSingleView)
	{
		CurrentJitterNdc = FTemporalMath::JitterPixelsToNdc(FTemporalMath::GetJitterPixels(TemporalFrameIndex++), Width, Height);
	}
	FCamera RenderCamera = Camera;
	RenderCamera.SetProjectionJitter(CurrentJitterNdc);

	FPerFrameConstants PerFrame       = BuildPerFrameConstants(Scene, RenderCamera);
	PerFrame.UnjitteredViewProjection = UnjitteredViewProjection;
	PerFrame.PrevViewProjection       = bTemporalHistoryValid ? PrevUnjitteredViewProjection : UnjitteredViewProjection;
	PerFrame.JitterNdc                = CurrentJitterNdc;
	PerFrame.ScreenSize               = FVector2(static_cast<float>(Width), static_cast<float>(Height));
	CurrentReprojection               = FTemporalMath::ComputeReprojectionMatrix(UnjitteredViewProjection, PerFrame.PrevViewProjection);

	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	const uint64               UploadStart   = DynamicBuffer.GetUsed();

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
	BeginTimer(ERenderTimer::Gather);
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
	TerrainRenderer.Prepare(Scene, Camera, FrozenFrustum); // 지형 텍스처 갱신 + 청크 LOD/컬링 (그림자 패스 전)
	Stats.TotalMeshes   = MeshInstances.GetComponentCount();
	Stats.SkinnedDrawn  = static_cast<uint32>(SkinPalettes.GetCount());
	Stats.SkinnedCulled = SkinPalettes.GetCulledCount();
	EndTimer(ERenderTimer::Gather);

	// GPU 파티클 계산 (그리기 전에)
	ParticleRenderer.Simulate(Scene, FrozenFrustum);

	// 점광원/스포트라이트 그림자 + 클러스터 컬링 (화면 크기 = 이번 씬 타깃)
	BeginTimer(ERenderTimer::LocalLights);
	// 같은 프레임에 이 렌더러가 다시 그리면(반사 캡처 면) 지난 메시 패스의 루트 SRV(클러스터 버퍼)가 그래픽스 루트에 남아 있다 →
	// 클러스터 버퍼가 UAV로 바뀌기 전에 다른 루트 시그니처로 바꿔 묶음을 끊는다 (디버그 레이어 1003)
	CommandList->SetGraphicsRootSignature(ScreenPassRoot.Get());
	LocalLightRenderer.Render(MeshInstances, SkinPalettes.GetGpuData(), Camera, Width, Height, LocalShadowSettings);
	EndTimer(ERenderTimer::LocalLights);
	Stats.LocalLights       = LocalLightRenderer.GetLightCount();
	Stats.LocalShadowSlices = LocalLightRenderer.GetShadowSliceCount();

	// 0) 방향광 섀도우 패스
	BeginTimer(ERenderTimer::Shadow);
	ShadowRenderer.Render(MeshInstances, SkinPalettes.GetGpuData());
	EndTimer(ERenderTimer::Shadow);
	Stats.ShadowDrawCalls = ShadowRenderer.GetDrawCalls() + LocalLightRenderer.GetShadowDrawCalls() + TerrainRenderer.GetShadowDrawCalls();
	Stats.ShadowTriangles = ShadowRenderer.GetTriangles() + LocalLightRenderer.GetShadowTriangles() + TerrainRenderer.GetShadowTriangles();

	// 1) 메인 묶음 (사전 패스와 메인 패스 공유). 오클루전은 와이어프레임에서 끈다 (깊이가 성김)
	EnsureSceneColor(Width, Height);
	const bool bOcclusion = bEnableOcclusion && !bWireframe;
	const bool bPrepass   = bDepthPrepass && !bWireframe;
	PrepareMainBatches(Camera, bOcclusion);

	// 2) 깊이 사전 패스: 깊이 + 법선 + 움직임 벡터 (오클루전이면 여기서 HZB + 2단계 판정)
	uint64 PrepassTriangles = 0;
	Stats.PrepassDrawCalls  = 0;
	if (bPrepass)
	{
		BeginTimer(ERenderTimer::DepthPrepass);
		SceneNormal->Begin(CommandList, SceneNormal->GetDesc().ClearColor);
		SceneVelocity->Begin(CommandList, SceneVelocity->GetDesc().ClearColor);
		CommandList->ClearDepthStencilView(SceneColor->GetDsv(), D3D12_CLEAR_FLAG_DEPTH, FD3D12DepthBuffer::ClearDepth, 0, 0, nullptr);
		BindPrepassTargets();
		DrawMainBatches(EMeshPass::Prepass, PerFrame, bOcclusion, true, Stats.PrepassDrawCalls, PrepassTriangles);
		SceneNormal->End(CommandList);
		SceneVelocity->End(CommandList);
		EndTimer(ERenderTimer::DepthPrepass);
	}
	else
	{
		// 쓰는 쪽(TAA 등)이 있어도 안전하게 지운 값 (+Z 법선, 움직임 0)
		SceneNormal->Begin(CommandList, SceneNormal->GetDesc().ClearColor);
		SceneNormal->End(CommandList);
		SceneVelocity->Begin(CommandList, SceneVelocity->GetDesc().ClearColor);
		SceneVelocity->End(CommandList);
	}

	// 2.5) SSAO: 사전 패스 깊이 + 법선 → 반해상도 가시도 (메인 패스가 간접광에만 곱한다)
	const bool bAmbientOcclusion = bPrepass && PostProcessSettings.bAmbientOcclusion;
	if (bAmbientOcclusion)
	{
		BeginTimer(ERenderTimer::AmbientOcclusion);
		FAmbientOcclusionInputs Inputs;
		Inputs.SceneDepth    = SceneColor.get();
		Inputs.SceneNormal   = SceneNormal.get();
		Inputs.Projection    = RenderCamera.GetProjectionMatrix();
		Inputs.View          = Camera.GetViewMatrix();
		Inputs.bOrthographic = Camera.IsOrthographic();
		Inputs.Radius        = PostProcessSettings.AmbientOcclusionRadius;
		Inputs.Intensity     = PostProcessSettings.AmbientOcclusionIntensity;
		Inputs.FrameIndex    = (CurrentJitterNdc.X != 0.0f || CurrentJitterNdc.Y != 0.0f) ? static_cast<uint32>(SceneFrameCount) : 0u; // TAA가 누적
		AmbientOcclusion.Render(Inputs);
		EndTimer(ERenderTimer::AmbientOcclusion);
	}
	PerFrame.AmbientOcclusionEnabled = bAmbientOcclusion ? 1.0f : 0.0f;

	// 2.6) 데칼 → DBuffer (사전 패스 깊이·법선 필요). 보이는 데칼이 없으면 메인 패스가 읽지 않는다
	bool bDecals = false;
	if (bPrepass)
	{
		BeginTimer(ERenderTimer::Decals);
		bDecals = DecalRenderer.Render(Scene, *Resources, RenderCamera, FrozenFrustum, *SceneColor, *SceneNormal);
		EndTimer(ERenderTimer::Decals);
	}
	Stats.Decals           = bDecals ? DecalRenderer.GetDrawnCount() : 0;
	PerFrame.DecalsEnabled = bDecals ? 1u : 0u;

	// 2.65) 반사: 캡처 목록(굽는 중에는 쓰지 않음 — 하늘만) + SSR (사전 패스 깊이·법선, 이전 프레임 씬 컬러 = 아직 지우기 전 SceneColor)
	{
		const uint32 CaptureCount       = ReflectionCaptures.Gather(Scene);
		PerFrame.ReflectionCaptureCount = bRenderingCaptures ? 0u : CaptureCount;
		const bool bSsr = bPrepass && PostProcessSettings.bScreenSpaceReflections && bTemporalHistoryValid && !bRenderingCaptures;
		if (bSsr)
		{
			BeginTimer(ERenderTimer::Reflections);
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
			// 반사 색은 지난 프레임 TAA 결과에서 (지난 프레임에 TAA가 돌았고 크기가 같을 때만 — 아니면 지터된 SceneColor)
			if (const FD3D12RenderTarget* LastTaa = bTaaRanLastFrame ? TemporalAA.GetLastOutput() : nullptr;
			    LastTaa != nullptr && LastTaa->GetWidth() == SceneColor->GetWidth() && LastTaa->GetHeight() == SceneColor->GetHeight())
			{
				Inputs.PrevColor = LastTaa;
			}
			ScreenSpaceReflections.Render(Inputs);
			EndTimer(ERenderTimer::Reflections);
		}
		PerFrame.SsrEnabled      = bSsr ? 1u : 0u;
		PerFrame.SsrMaxRoughness = FMath::Clamp(PostProcessSettings.SsrMaxRoughness, 0.05f, 1.0f);
		PerFrame.SsrIntensity    = FMath::Max(PostProcessSettings.SsrIntensity, 0.0f);
	}

	// 2.7) 안개 상수 + 볼류메트릭 안개 (3D 격자 주입 → 적분). 적용은 메인 패스 뒤, 파티클은 정점에서
	{
		BeginTimer(ERenderTimer::VolumetricFog);
		FogRenderer.Prepare(Scene, RenderCamera, UnjitteredViewProjection, Width, Height);
		FVolumetricFogInputs FogInputs;
		FogInputs.ShadowConstants    = DynamicBuffer.AllocateConstants(ShadowRenderer.GetConstants()).GpuAddress;
		FogInputs.ShadowMapSrv       = ShadowRenderer.GetShadowMapSrv();
		FogInputs.ShadowMap          = ShadowRenderer.GetShadowMapResource();
		FogInputs.ClusterConstants   = LocalLightRenderer.GetConstants();
		FogInputs.LocalLights        = LocalLightRenderer.GetLightList();
		FogInputs.LightDirection     = PerFrame.DirectionalLight.Direction;
		FogInputs.LightColor         = PerFrame.DirectionalLight.Color * PerFrame.DirectionalLight.Intensity;
		FogInputs.PrevViewProjection = PerFrame.PrevViewProjection;
		FogInputs.bHistoryValid      = bTemporalHistoryValid;
		FogInputs.FrameIndex         = SceneFrameCount;
		FogRenderer.RenderVolumetric(FogInputs);
		EndTimer(ERenderTimer::VolumetricFog);
	}

	// 3) HDR 씬 패스: 하늘 + 불투명 메시 (사전 패스 뒤면 깊이 같음 테스트) + 파티클
	const float SceneClear[4] = { BackgroundColor.X, BackgroundColor.Y, BackgroundColor.Z, 0.0f }; // 알파 0 = TAA 반응형 마스크 없음
	SceneColor->Begin(CommandList, SceneClear, !bPrepass);
	BeginTimer(ERenderTimer::MainDraw);
	if (bDrawSkybox)
	{
		IblRenderer.RenderSkybox(Camera, PerFrame.AmbientIntensity);
	}
	const EMeshPass MainPass = bWireframe ? EMeshPass::Wireframe : (bPrepass ? EMeshPass::MainDepthEqual : EMeshPass::Main);
	uint64          MainTriangles = 0;
	DrawMainBatches(MainPass, PerFrame, bOcclusion, !bPrepass, Stats.DrawCalls, MainTriangles);
	if (bOcclusion)
	{
		OcclusionCuller.FinishFrame();
		Stats.Triangles       = OcclusionCuller.GetDrawnTriangles() + MainTriangles; // 간접 드로우(정적) + 바로 그린 스킨
		Stats.OcclusionTested = OcclusionCuller.GetTestedInstances();
		Stats.OcclusionPhase1 = OcclusionCuller.GetPhase1Instances();
		Stats.OcclusionPhase2 = OcclusionCuller.GetPhase2Instances();
	}
	else
	{
		Stats.Triangles       = MainTriangles;
		Stats.OcclusionTested = Stats.OcclusionPhase1 = Stats.OcclusionPhase2 = 0;
	}
	EndTimer(ERenderTimer::MainDraw);

	// 안개 적용 (불투명 메시 + 하늘, 씬 깊이) → 파티클은 정점에서 같은 식
	if (FogRenderer.IsEnabled())
	{
		BeginTimer(ERenderTimer::Fog);
		FogRenderer.Apply(*SceneColor);
		EndTimer(ERenderTimer::Fog);
	}
	ParticleRenderer.SetFog(FogRenderer.GetConstantsAddress(), FogRenderer.GetVolumeSrv());

	BeginTimer(ERenderTimer::Particles);
	Stats.Particles              = ParticleRenderer.Render(Scene, RenderCamera, FrozenFrustum);
	Stats.ParticleEmittersCulled = ParticleRenderer.GetCulledEmitterCount();
	EndTimer(ERenderTimer::Particles);
	SceneColor->End(CommandList);
	Stats.UploadBytes = DynamicBuffer.GetUsed() - UploadStart;

	// 다음 프레임 이력
	PrevUnjitteredViewProjection = UnjitteredViewProjection;
	PrevCameraPosition           = Camera.GetPosition();
	PrevCameraForward            = Camera.GetForwardVector();
	PrevTargetWidth              = Width;
	PrevTargetHeight             = Height;
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

	// 면마다 지터 없는 90도 카메라로 HDR 씬을 그려 원시 큐브 면으로 복사 → 프리필터 (캡처끼리는 서로 비추지 않고 하늘만)
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
			RenderSceneColor(Scene, FaceCamera, FReflectionMath::CaptureSize, FReflectionMath::CaptureSize, false);
			ReflectionCaptures.CopyFace(*SceneColor, Face);
		}
		ReflectionCaptures.FinishBake(Job.AssetPath);
		E_LOG(LogRenderer, Display, "반사 캡처 굽기: {} ({:.0f}, {:.0f}, {:.0f})", Job.AssetPath, Job.Position.X, Job.Position.Y, Job.Position.Z);
	}
	bRenderingCaptures = false;
}

void FSceneRenderer::ApplyMotionHistory(bool bValid)
{
	// 정적 인스턴스만 (스킨은 팔레트가 이전 프레임 본을 따로 가진다). 엔티티 하나 = 인스턴스 하나
	for (FMeshInstance& Instance : MeshInstances.GetInstances())
	{
		if (Instance.IsSkinned())
		{
			continue;
		}
		const uint32 Index = Instance.Entity.Index;
		if (Index >= MotionHistory.size())
		{
			MotionHistory.resize(Index + 1);
		}
		FMotionHistory& History = MotionHistory[Index];
		if (bValid && History.Generation == Instance.Entity.Generation && History.Frame + 1 == SceneFrameCount)
		{
			Instance.PrevWorld = History.World;
		}
		History.Generation = Instance.Entity.Generation;
		History.Frame      = SceneFrameCount;
		History.World      = Instance.World;
	}
}

void FSceneRenderer::RenderDebugView(const FRenderOutput& Output)
{
	if (DebugView == 0 || !SceneColor || SceneColor->GetWidth() != Output.Width || SceneColor->GetHeight() != Output.Height)
	{
		return;
	}
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();
	if (DebugView == 3)
	{
		// 깊이는 평소 DEPTH_WRITE → 읽는 동안만 셰이더 리소스
		const D3D12_RESOURCE_BARRIER ToRead = MakeTransitionBarrier(SceneColor->GetDepthResource(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
		                                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		CommandList->ResourceBarrier(1, &ToRead);
		PostProcessor.RenderDebugView(CommandList, SceneColor->GetDepthSrv(), Output, DebugView);
		const D3D12_RESOURCE_BARRIER ToWrite = MakeTransitionBarrier(SceneColor->GetDepthResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		                                                             D3D12_RESOURCE_STATE_DEPTH_WRITE);
		CommandList->ResourceBarrier(1, &ToWrite);
		return;
	}
	if (DebugView == 4 || DebugView == 5)
	{
		PostProcessor.RenderDebugView(CommandList, DebugView == 4 ? AmbientOcclusion.GetResultSrv() : ScreenSpaceReflections.GetResultSrv(), Output,
		                              DebugView);
		return;
	}
	const FD3D12RenderTarget* Source = DebugView == 1 ? SceneNormal.get() : SceneVelocity.get();
	PostProcessor.RenderDebugView(CommandList, Source->GetSrv(), Output, DebugView);
}

void FSceneRenderer::BindPrepassTargets()
{
	const D3D12_CPU_DESCRIPTOR_HANDLE Rtvs[] = { SceneNormal->GetRtv(), SceneVelocity->GetRtv() };
	const D3D12_CPU_DESCRIPTOR_HANDLE Dsv    = SceneColor->GetDsv();
	Rhi->GetCommandList()->OMSetRenderTargets(2, Rtvs, FALSE, &Dsv);
}

void FSceneRenderer::PrepareMainBatches(const FCamera& Camera, bool bOcclusion)
{
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();

	// 컬링 + 묶음 키: (정적/스킨) → 머티리얼 → 메시 → LOD, 묶음 안은 가까운 순 (상태 변경 최소화 + 초기 깊이 기각). 스킨도 인스턴싱
	BeginTimer(ERenderTimer::MainCull);
	const FVector3 CameraPosition = Camera.GetPosition();
	MainBatches.Reset();
	Stats.VisibleMeshes = 0;
	const std::vector<FMeshInstance>& Instances = MeshInstances.GetInstances();
	for (uint32 Index = 0; Index < static_cast<uint32>(Instances.size()); ++Index)
	{
		const FMeshInstance& Instance = Instances[Index];
		if (!FrozenFrustum.Intersects(Instance.WorldBounds))
		{
			continue;
		}
		++Stats.VisibleMeshes;
		const uint64 Key = InstanceBatching::MakeKey(Instance.IsSkinned() ? 1 : 0, Instance.MaterialHandle.Index, Instance.MeshHandle.Index,
		                                             Instance.IsSkinned() ? 0 : Instance.Lod);
		MainBatches.Add(Key, FVector3::DistanceSquared(Instance.WorldBounds.GetCenter(), CameraPosition), Index);
	}
	EndTimer(ERenderTimer::MainCull);

	BeginTimer(ERenderTimer::MainSort);
	MainBatches.Finalize(DynamicBuffer);
	EndTimer(ERenderTimer::MainSort);

	// 오클루전 1단계: 이전 프레임 HZB로 정적 인스턴스 판정
	if (bOcclusion)
	{
		BeginTimer(ERenderTimer::Occlusion);
		OcclusionCuller.CullPhase1(MeshInstances, MainBatches, SceneColor->GetWidth(), SceneColor->GetHeight());
		EndTimer(ERenderTimer::Occlusion);
	}
}

void FSceneRenderer::DrawMainBatches(EMeshPass Pass, const FPerFrameConstants& PerFrame, bool bOcclusion, bool bBuildHzb, uint32& OutDrawCalls,
                                     uint64& OutTriangles)
{
	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	ID3D12GraphicsCommandList* CommandList   = Rhi->GetCommandList();
	const bool                 bPrepassPass  = Pass == EMeshPass::Prepass;

	const FD3D12DynamicAllocation PerFrameAllocation = DynamicBuffer.AllocateConstants(PerFrame);
	const FD3D12DynamicAllocation ShadowAllocation   = DynamicBuffer.AllocateConstants(ShadowRenderer.GetConstants());
	// 지형: 메시보다 먼저 (큰 가림막 — 초기 깊이 기각). 자기 루트 시그니처를 쓰므로 아래에서 메시 상태를 다시 설정한다
	// 사전 패스에도 그린다 (지형 깊이가 없으면 TAA/SSAO/안개/SSR이 하늘로 본다), 메인은 같은 VS로 깊이 EQUAL
	{
		FTerrainScreenInputs Screen;
		Screen.AmbientOcclusion = AmbientOcclusion.GetResultSrv();
		for (uint32 Index = 0; Index < 3; ++Index)
		{
			Screen.DBuffer[Index] = DecalRenderer.GetTarget(Index).GetSrv();
		}
		Screen.ReflectionCaptures = ReflectionCaptures.GetCaptureList();
		Screen.CaptureAtlas       = ReflectionCaptures.GetAtlasSrv();
		Screen.ScreenReflection   = ScreenSpaceReflections.GetResultSrv();
		const ETerrainPass TerrainPass =
			bPrepassPass ? ETerrainPass::Prepass : (Pass == EMeshPass::MainDepthEqual ? ETerrainPass::MainDepthEqual : ETerrainPass::Main);
		TerrainRenderer.RenderMain(TerrainPass, PerFrameAllocation.GpuAddress, ShadowAllocation.GpuAddress, ShadowRenderer, IblRenderer, LocalLightRenderer,
		                           Screen);
	}

	FD3D12PipelineState& StaticPipeline  = GetMeshPipeline(Pass, false);
	FD3D12PipelineState& SkinnedPipeline = GetMeshPipeline(Pass, true);

	CommandList->SetGraphicsRootSignature(RootSignature.Get());
	CommandList->SetPipelineState(StaticPipeline.Get());
	CommandList->SetGraphicsRootConstantBufferView(RootParam_PerFrame, PerFrameAllocation.GpuAddress);
	CommandList->SetGraphicsRootConstantBufferView(RootParam_Shadow, ShadowAllocation.GpuAddress);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_ShadowMap, ShadowRenderer.GetShadowMapSrv().Gpu);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_Ibl, IblRenderer.GetLightingTable().Gpu);
	CommandList->SetGraphicsRootConstantBufferView(RootParam_Cluster, LocalLightRenderer.GetConstants());
	CommandList->SetGraphicsRootShaderResourceView(RootParam_LocalLights, LocalLightRenderer.GetLightList());
	CommandList->SetGraphicsRootShaderResourceView(RootParam_ClusterData, LocalLightRenderer.GetClusterData());
	CommandList->SetGraphicsRootShaderResourceView(RootParam_LocalShadowMatrices, LocalLightRenderer.GetShadowMatrices());
	CommandList->SetGraphicsRootDescriptorTable(RootParam_LocalShadowMap, LocalLightRenderer.GetShadowMapSrv().Gpu);
	CommandList->SetGraphicsRootShaderResourceView(RootParam_Instances, MeshInstances.GetGpuData());
	CommandList->SetGraphicsRootShaderResourceView(RootParam_InstanceIndices, bOcclusion ? OcclusionCuller.GetIndices(1) : MainBatches.GetIndexBuffer());
	CommandList->SetGraphicsRootShaderResourceView(RootParam_SkinPalette, SkinPalettes.GetGpuData());
	CommandList->SetGraphicsRootDescriptorTable(RootParam_AmbientOcclusion, AmbientOcclusion.GetResultSrv().Gpu);
	for (uint32 Index = 0; Index < 3; ++Index)
	{
		CommandList->SetGraphicsRootDescriptorTable(RootParam_DBufferA + Index, DecalRenderer.GetTarget(Index).GetSrv().Gpu);
	}
	CommandList->SetGraphicsRootShaderResourceView(RootParam_ReflectionCaptures, ReflectionCaptures.GetCaptureList());
	CommandList->SetGraphicsRootDescriptorTable(RootParam_CaptureAtlas, ReflectionCaptures.GetAtlasSrv().Gpu);
	CommandList->SetGraphicsRootDescriptorTable(RootParam_ScreenReflection, ScreenSpaceReflections.GetResultSrv().Gpu);

	// 머티리얼 상수는 패스 안에서 한 번만 업로드 (사전 패스는 거칠기만 읽는다)
	std::unordered_map<uint64, D3D12_GPU_VIRTUAL_ADDRESS> MaterialConstantCache;

	const std::vector<FMeshInstance>& Instances     = MeshInstances.GetInstances();
	const FMaterial*                  BoundMaterial = nullptr;
	bool                              bSkinnedBound = false;
	OutDrawCalls                                    = TerrainRenderer.GetDrawCalls(); // 지형 포함
	OutTriangles                                    = TerrainRenderer.GetTriangles();

	// Phase 0 = 오클루전 없음(바로 그림), 1/2 = 오클루전 단계 (정적 묶음은 간접 드로우, 스킨 묶음은 1단계에서 바로)
	// OutTriangles = 바로 그린 삼각형 (간접 드로우 정적 삼각형은 오클루전 통계가 센다)
	auto DrawBatches = [&](uint32 Phase) {
		const std::vector<FInstanceBatch>& Batches = MainBatches.GetBatches();
		for (uint32 BatchIndex = 0; BatchIndex < static_cast<uint32>(Batches.size()); ++BatchIndex)
		{
			const FInstanceBatch& Batch    = Batches[BatchIndex];
			const FMeshInstance&  Instance = Instances[Batch.Instance];
			const bool            bSkinned = Instance.IsSkinned();
			if (bSkinned && Phase == 2)
			{
				continue;
			}
			if (bSkinned != bSkinnedBound)
			{
				CommandList->SetPipelineState(bSkinned ? SkinnedPipeline.Get() : StaticPipeline.Get());
				bSkinnedBound = bSkinned;
				if (bSkinned && Phase == 1)
				{
					// 오클루전 1단계 목록은 정적 묶음만 채운다 → 스킨 묶음은 메인 묶음의 번호 목록으로 (스킨은 정적 뒤에 정렬됨)
					CommandList->SetGraphicsRootShaderResourceView(RootParam_InstanceIndices, MainBatches.GetIndexBuffer());
				}
			}
			if (Instance.Material != BoundMaterial) // 사전 패스도 거칠기(SSR)를 위해 금속/거칠기 텍스처를 읽는다
			{
				const uint64 Key   = Instance.MaterialHandle.ToId();
				auto         Found = MaterialConstantCache.find(Key);
				if (Found == MaterialConstantCache.end())
				{
					Found = MaterialConstantCache.emplace(Key, DynamicBuffer.AllocateConstants(Instance.Material->Constants).GpuAddress).first;
				}
				CommandList->SetGraphicsRootConstantBufferView(RootParam_Material, Found->second);
				CommandList->SetGraphicsRootDescriptorTable(RootParam_MaterialTexture, Instance.Material->TextureTable.Gpu);
				BoundMaterial = Instance.Material;
			}

			CommandList->SetGraphicsRoot32BitConstant(RootParam_DrawConstants, Batch.First, 0);
			if (bSkinned)
			{
				Instance.Mesh->DrawSkinned(CommandList, Batch.Count);
				OutTriangles += static_cast<uint64>(Instance.Mesh->GetIndexCount() / 3) * Batch.Count;
			}
			else if (Phase == 0)
			{
				Instance.Mesh->DrawInstanced(CommandList, Batch.Count, Instance.Lod);
				OutTriangles += static_cast<uint64>(Instance.Mesh->GetLod(Instance.Lod).IndexCount / 3) * Batch.Count;
			}
			else
			{
				Instance.Mesh->Bind(CommandList);
				OcclusionCuller.DrawIndirect(CommandList, BatchIndex, Phase);
			}
			++OutDrawCalls;
		}
	};

	if (!bOcclusion)
	{
		DrawBatches(0);
		return;
	}

	DrawBatches(1);

	// 이 패스가 처음 깊이를 쓰는 패스면: 1단계 깊이로 HZB → 1단계에서 가려진 것만 다시 검사 (새로 드러난 물체를 같은 프레임에 그린다)
	if (bBuildHzb)
	{
		BeginTimer(ERenderTimer::Hzb);
		OcclusionCuller.BuildHzbAndCullPhase2(*SceneColor, PerFrame.ViewProjection);
		EndTimer(ERenderTimer::Hzb);

		// 계산 PSO로 바뀌었으므로 렌더 타깃 복구 (그래픽스 루트 인자는 계산과 따로라 유지된다)
		if (bPrepassPass)
		{
			BindPrepassTargets();
		}
		else
		{
			const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = SceneColor->GetRtv();
			const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = SceneColor->GetDsv();
			CommandList->OMSetRenderTargets(1, &Rtv, FALSE, &Dsv);
		}
	}
	CommandList->SetPipelineState(StaticPipeline.Get());
	bSkinnedBound = false;
	CommandList->SetGraphicsRootShaderResourceView(RootParam_InstanceIndices, OcclusionCuller.GetIndices(2));
	DrawBatches(2);
}

void FSceneRenderer::SelectLods(const FCamera& Camera)
{
	if (!bEnableLod)
	{
		return; // 모두 LOD0 (Gather 기본값)
	}
	// 메인 카메라 화면 크기로 고르고 그림자 패스도 같은 LOD를 쓴다 (그림자와 본체 모양이 어긋나지 않게)
	const bool     bOrthographic  = Camera.IsOrthographic();
	const float    TanHalfFov     = FMath::Tan(FMath::DegreesToRadians(Camera.GetFovYDegrees()) * 0.5f);
	const FVector3 CameraPosition = Camera.GetPosition();
	for (FMeshInstance& Instance : MeshInstances.GetInstances())
	{
		if (Instance.IsSkinned() || Instance.bFixedLod || Instance.Mesh->GetLodCount() <= 1)
		{
			continue;
		}
		if (ForcedLod >= 0)
		{
			Instance.Lod = std::min(static_cast<uint32>(ForcedLod), Instance.Mesh->GetLodCount() - 1);
			continue;
		}
		const float Radius = Instance.WorldBounds.GetExtent().Length();
		const float ScreenSize =
			bOrthographic ? LodMath::ComputeOrthographicScreenSize(Radius, Camera.GetOrthoHeight())
			              : LodMath::ComputePerspectiveScreenSize(Radius, FVector3::Distance(Instance.WorldBounds.GetCenter(), CameraPosition), TanHalfFov);
		// 히스테리시스: 엔티티별 이전 LOD (처음이거나 엔티티가 바뀌었으면 없음)
		const uint32 Index = Instance.Entity.Index;
		if (Index >= LodHistory.size())
		{
			LodHistory.resize(Index + 1);
		}
		FLodHistory& History  = LodHistory[Index];
		const uint32 Previous = History.Generation == Instance.Entity.Generation ? History.Lod : ~0u;
		Instance.Lod = LodMath::SelectLodWithHysteresis(ScreenSize, Instance.Mesh->GetLodScreenSizes(), Instance.Mesh->GetLodCount(), LodScale, Previous,
		                                                LodHysteresis);
		History = { Instance.Entity.Generation, Instance.Lod };
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
