#include "Renderer/SceneRenderer.h"

#include "Core/CommandLine.h"
#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/Camera.h"
#include "Renderer/LodMath.h"
#include "Renderer/Material.h"
#include "Renderer/PixelArtMath.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
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

	if (!CreateMeshPipeline(PipelineState, false, false) || !CreateSkinnedMeshPipeline(SkinnedPipelineState, false, false) ||
	    !CreateMeshPipeline(WireframePipelineState, false, true) || !CreateSkinnedMeshPipeline(SkinnedWireframePipelineState, false, true))
	{
		return false;
	}
	if (!PostProcessor.Init(*Rhi, ShaderLibrary))
	{
		return false;
	}
	if (!ShadowRenderer.Init(*Rhi, ShaderLibrary) || !IblRenderer.Init(*Rhi, ShaderLibrary) || !LocalLightRenderer.Init(*Rhi, ShaderLibrary) ||
	    !OcclusionCuller.Init(*Rhi, ShaderLibrary))
	{
		return false;
	}
	if (!ParticleRenderer.Init(*Rhi, ShaderLibrary, *Resources, SceneColorFormat, FD3D12RHI::DepthBufferFormat))
	{
		return false;
	}

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
	if (CommandLine.HasFlag(L"--no-lod"))
	{
		bEnableLod = false; // 측정/비교용
	}
	if (const std::wstring ForceLod = CommandLine.GetValue(L"--force-lod"); !ForceLod.empty())
	{
		ForcedLod = std::stoi(ForceLod); // LOD 모양 확인용
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
	E_LOG(LogRenderer, Display, "[성능] {} 프레임 평균 ({}): 프레임 {:.3f} ms, 드로우 {:.1f} (그림자 {:.1f}), 삼각형 {:.0f} (그림자 {:.0f}), 메시 {:.1f}/{}",
	      Capture.Frames, Config, Capture.FrameMs / Count, Capture.DrawCalls / Count, Capture.ShadowDrawCalls / Count, Capture.Triangles / Count,
	      Capture.ShadowTriangles / Count, Capture.VisibleMeshes / Count, Capture.TotalMeshes);
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

bool FSceneRenderer::CreateMeshPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, bool bWireframeFill)
{
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Mesh.hlsl";
	VertexDesc.EntryPoint = L"VSMain";
	VertexDesc.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc PixelDesc = VertexDesc;
	PixelDesc.EntryPoint         = L"PSMain";
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
	PsoDesc.InputLayout            = FStaticMesh::GetInputLayout();
	PsoDesc.RenderTargetFormats[0] = SceneColorFormat;
	PsoDesc.DepthStencilFormat     = FD3D12RHI::DepthBufferFormat;
	PsoDesc.bDepthEnable           = true;
	if (bWireframeFill)
	{
		PsoDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
		PsoDesc.CullMode = D3D12_CULL_MODE_NONE;
	}
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), PsoDesc, bWireframeFill ? L"MeshWireframePipeline" : L"MeshPipeline");
}

bool FSceneRenderer::CreateSkinnedMeshPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, bool bWireframeFill)
{
	FShaderCompileDesc VertexDesc;
	VertexDesc.FileName   = L"Mesh.hlsl";
	VertexDesc.EntryPoint = L"VSSkinned";
	VertexDesc.Stage      = EShaderStage::Vertex;
	FShaderCompileDesc PixelDesc = VertexDesc;
	PixelDesc.EntryPoint         = L"PSMain";
	PixelDesc.Stage              = EShaderStage::Pixel;

	if (bForceRecompile && !ShaderLibrary.CookShader(VertexDesc))
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
	PsoDesc.InputLayout            = FStaticMesh::GetSkinnedInputLayout();
	PsoDesc.RenderTargetFormats[0] = SceneColorFormat;
	PsoDesc.DepthStencilFormat     = FD3D12RHI::DepthBufferFormat;
	PsoDesc.bDepthEnable           = true;
	if (bWireframeFill)
	{
		PsoDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
		PsoDesc.CullMode = D3D12_CULL_MODE_NONE;
	}
	return OutPipeline.InitGraphics(Rhi->GetDevice().GetDevice(), PsoDesc,
	                                bWireframeFill ? L"SkinnedMeshWireframePipeline" : L"SkinnedMeshPipeline");
}

bool FSceneRenderer::ReloadShaders(bool bForceRecompile)
{
	E_CHECKF(Rhi != nullptr, "씬 렌더러가 초기화되지 않았습니다");

	FD3D12PipelineState NewPipeline;
	if (!CreateMeshPipeline(NewPipeline, bForceRecompile, false))
	{
		E_LOG(LogRenderer, Error, "셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
		return false;
	}

	// 이전 PSO는 진행 중인 프레임이 참조할 수 있으므로 지연 해제
	PipelineState.Swap(NewPipeline);
	Rhi->DeferRelease(NewPipeline.Detach());

	FD3D12PipelineState NewSkinnedPipeline;
	if (CreateSkinnedMeshPipeline(NewSkinnedPipeline, bForceRecompile, false))
	{
		SkinnedPipelineState.Swap(NewSkinnedPipeline);
		Rhi->DeferRelease(NewSkinnedPipeline.Detach());
	}
	else
	{
		E_LOG(LogRenderer, Error, "스킨 메시 셰이더 다시 로드 실패: 기존 파이프라인을 유지합니다");
	}

	// 와이어프레임 PSO는 셰이더가 같으므로 위에서 이미 쿠킹됨 (강제 재컴파일 불필요)
	FD3D12PipelineState NewWireframePipeline;
	FD3D12PipelineState NewSkinnedWireframePipeline;
	if (CreateMeshPipeline(NewWireframePipeline, false, true) && CreateSkinnedMeshPipeline(NewSkinnedWireframePipeline, false, true))
	{
		WireframePipelineState.Swap(NewWireframePipeline);
		Rhi->DeferRelease(NewWireframePipeline.Detach());
		SkinnedWireframePipelineState.Swap(NewSkinnedWireframePipeline);
		Rhi->DeferRelease(NewSkinnedWireframePipeline.Detach());
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
	if (!OcclusionCuller.ReloadShaders(bForceRecompile))
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
	PixelArtColor.reset();
	PostProcessor.Shutdown();
	ShadowRenderer.Shutdown();
	IblRenderer.Shutdown();
	ParticleRenderer.Shutdown();
	LocalLightRenderer.Shutdown();
	OcclusionCuller.Shutdown();
	PipelineState.Shutdown();
	SkinnedPipelineState.Shutdown();
	WireframePipelineState.Shutdown();
	SkinnedWireframePipelineState.Shutdown();
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
	EnsureTarget(SceneColor, Width, Height, L"SceneColorHDR", SceneDesc);
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
		RenderSceneColor(Scene, Camera, Output.Width, Output.Height);
		BeginTimer(ERenderTimer::PostProcess);
		PostProcessor.Render(CommandList, SceneColor->GetSrv(), Output, PostProcessSettings);
		EndTimer(ERenderTimer::PostProcess);
		return;
	}

	// 픽셀 아트: 저해상도 씬 → 저해상도 포스트(톤매핑) → 합성 확대
	const uint32 PixelSize    = FPixelArtMath::ClampPixelSize(PixelArt->PixelSize);
	const uint32 SourceWidth  = FPixelArtMath::GetSourceDimension(Output.Width, PixelSize);
	const uint32 SourceHeight = FPixelArtMath::GetSourceDimension(Output.Height, PixelSize);

	FPixelArtCompositeParams Params;
	const FCamera            SourceCamera = BuildPixelArtCamera(*PixelArt, Camera, Output, SourceWidth, SourceHeight, Params);
	RenderSceneColor(Scene, SourceCamera, SourceWidth, SourceHeight);

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

void FSceneRenderer::RenderSceneColor(FScene& Scene, const FCamera& Camera, uint32 Width, uint32 Height)
{
	ID3D12GraphicsCommandList* CommandList = Rhi->GetCommandList();

	const FPerFrameConstants PerFrame = BuildPerFrameConstants(Scene, Camera);

	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	const uint64               UploadStart   = DynamicBuffer.GetUsed();

	// 컬링 프러스텀 (고정 중이면 고정 시점)
	if (!bCullingFrozen)
	{
		FrozenFrustum = FFrustum::FromViewProjection(Camera.GetViewProjectionMatrix());
	}

	// 그림자 캐스터 볼륨 먼저 (CPU만): 점광원/스포트라이트 수집 + 그림자 장 배정, 방향광 캐스케이드
	BeginCpuTimer(ERenderTimer::LocalLights);
	LocalLightRenderer.PrepareLights(Scene, Camera, LocalShadowSettings);
	EndCpuTimer(ERenderTimer::LocalLights);
	BeginCpuTimer(ERenderTimer::Shadow);
	ShadowRenderer.PrepareCascades(Camera, PerFrame.DirectionalLight.Direction, ShadowSettings);
	EndCpuTimer(ERenderTimer::Shadow);

	// 스킨 메시 본 팔레트(메인 프러스텀 ∪ 그림자 캐스터 볼륨에 드는 것만) + 프레임 메시 인스턴스 목록 (섀도우/로컬 그림자/메인 패스 공유)
	BeginTimer(ERenderTimer::Gather);
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
	MeshInstances.Upload(DynamicBuffer);
	SelectLods(Camera);
	Stats.TotalMeshes   = MeshInstances.GetComponentCount();
	Stats.SkinnedDrawn  = static_cast<uint32>(SkinPalettes.GetCount());
	Stats.SkinnedCulled = SkinPalettes.GetCulledCount();
	EndTimer(ERenderTimer::Gather);

	// GPU 파티클 계산 (그리기 전에)
	ParticleRenderer.Simulate(Scene);

	// 점광원/스포트라이트 그림자 + 클러스터 컬링 (화면 크기 = 이번 씬 타깃)
	BeginTimer(ERenderTimer::LocalLights);
	LocalLightRenderer.Render(MeshInstances, SkinPalettes.GetGpuData(), Camera, Width, Height, LocalShadowSettings);
	EndTimer(ERenderTimer::LocalLights);
	Stats.LocalLights       = LocalLightRenderer.GetLightCount();
	Stats.LocalShadowSlices = LocalLightRenderer.GetShadowSliceCount();

	// 0) 방향광 섀도우 패스
	BeginTimer(ERenderTimer::Shadow);
	ShadowRenderer.Render(MeshInstances, SkinPalettes.GetGpuData());
	EndTimer(ERenderTimer::Shadow);
	Stats.ShadowDrawCalls = ShadowRenderer.GetDrawCalls() + LocalLightRenderer.GetShadowDrawCalls();
	Stats.ShadowTriangles = ShadowRenderer.GetTriangles() + LocalLightRenderer.GetShadowTriangles();

	// 1) HDR 씬 패스
	EnsureSceneColor(Width, Height);
	SceneColor->Begin(CommandList, &BackgroundColor.X);
	DrawMeshes(Camera, PerFrame);
	BeginTimer(ERenderTimer::Particles);
	Stats.Particles = ParticleRenderer.Render(Scene, Camera);
	EndTimer(ERenderTimer::Particles);
	SceneColor->End(CommandList);
	Stats.UploadBytes = DynamicBuffer.GetUsed() - UploadStart;
}

void FSceneRenderer::DrawMeshes(const FCamera& Camera, const FPerFrameConstants& PerFrame)
{
	const FMatrix4x4 ViewProjection = Camera.GetViewProjectionMatrix();

	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	ID3D12GraphicsCommandList* CommandList   = Rhi->GetCommandList();

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

	// 오클루전 1단계: 이전 프레임 HZB로 정적 인스턴스 판정 (와이어프레임은 깊이가 성겨 끔)
	const bool bOcclusion = bEnableOcclusion && !bWireframe && SceneColor->GetDesc().bWithDepth;
	if (bOcclusion)
	{
		BeginTimer(ERenderTimer::Occlusion);
		OcclusionCuller.CullPhase1(MeshInstances, MainBatches, SceneColor->GetWidth(), SceneColor->GetHeight());
		EndTimer(ERenderTimer::Occlusion);
	}

	BeginTimer(ERenderTimer::MainDraw);
	if (bDrawSkybox)
	{
		IblRenderer.RenderSkybox(Camera, PerFrame.AmbientIntensity);
	}

	const FD3D12DynamicAllocation PerFrameAllocation = DynamicBuffer.AllocateConstants(PerFrame);
	const FD3D12DynamicAllocation ShadowAllocation   = DynamicBuffer.AllocateConstants(ShadowRenderer.GetConstants());

	FD3D12PipelineState& StaticPipeline  = bWireframe ? WireframePipelineState : PipelineState;
	FD3D12PipelineState& SkinnedPipeline = bWireframe ? SkinnedWireframePipelineState : SkinnedPipelineState;

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

	// 머티리얼 상수는 프레임 내에서 한 번만 업로드
	std::unordered_map<uint64, D3D12_GPU_VIRTUAL_ADDRESS> MaterialConstantCache;

	const FMaterial* BoundMaterial = nullptr;
	bool             bSkinnedBound = false;
	Stats.DrawCalls                = 0;
	Stats.Triangles                = 0;
	uint64 SkinnedTriangles        = 0;

	// Phase 0 = 오클루전 없음(바로 그림), 1/2 = 오클루전 단계 (정적 묶음은 간접 드로우, 스킨 묶음은 1단계에서 바로)
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
			if (Instance.Material != BoundMaterial)
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

			if (bSkinned)
			{
				CommandList->SetGraphicsRoot32BitConstant(RootParam_DrawConstants, Batch.First, 0);
				Instance.Mesh->DrawSkinned(CommandList, Batch.Count);
				SkinnedTriangles += static_cast<uint64>(Instance.Mesh->GetIndexCount() / 3) * Batch.Count;
			}
			else
			{
				CommandList->SetGraphicsRoot32BitConstant(RootParam_DrawConstants, Batch.First, 0);
				if (Phase == 0)
				{
					Instance.Mesh->DrawInstanced(CommandList, Batch.Count, Instance.Lod);
					Stats.Triangles += static_cast<uint64>(Instance.Mesh->GetLod(Instance.Lod).IndexCount / 3) * Batch.Count;
				}
				else
				{
					Instance.Mesh->Bind(CommandList);
					OcclusionCuller.DrawIndirect(CommandList, BatchIndex, Phase);
				}
			}
			++Stats.DrawCalls;
		}
	};

	if (!bOcclusion)
	{
		DrawBatches(0);
		Stats.Triangles += SkinnedTriangles;
		Stats.OcclusionTested = Stats.OcclusionPhase1 = Stats.OcclusionPhase2 = 0;
		EndTimer(ERenderTimer::MainDraw);
		return;
	}

	DrawBatches(1);

	// 1단계 깊이로 HZB → 1단계에서 가려진 것만 다시 검사 (새로 드러난 물체를 같은 프레임에 그린다)
	BeginTimer(ERenderTimer::Hzb);
	OcclusionCuller.BuildHzbAndCullPhase2(*SceneColor, ViewProjection);
	EndTimer(ERenderTimer::Hzb);

	// 계산 PSO로 바뀌었으므로 그래픽스 상태 복구 (그래픽스 루트 인자는 계산과 따로라 유지된다)
	const D3D12_CPU_DESCRIPTOR_HANDLE Rtv = SceneColor->GetRtv();
	const D3D12_CPU_DESCRIPTOR_HANDLE Dsv = SceneColor->GetDsv();
	CommandList->OMSetRenderTargets(1, &Rtv, FALSE, &Dsv);
	CommandList->SetPipelineState(StaticPipeline.Get());
	bSkinnedBound = false;
	CommandList->SetGraphicsRootShaderResourceView(RootParam_InstanceIndices, OcclusionCuller.GetIndices(2));
	DrawBatches(2);
	OcclusionCuller.FinishFrame();

	Stats.Triangles       = OcclusionCuller.GetDrawnTriangles() + SkinnedTriangles;
	Stats.OcclusionTested = OcclusionCuller.GetTestedInstances();
	Stats.OcclusionPhase1 = OcclusionCuller.GetPhase1Instances();
	Stats.OcclusionPhase2 = OcclusionCuller.GetPhase2Instances();
	EndTimer(ERenderTimer::MainDraw);
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
		if (Instance.IsSkinned() || Instance.Mesh->GetLodCount() <= 1)
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
		Instance.Lod = LodMath::SelectLod(ScreenSize, Instance.Mesh->GetLodScreenSizes(), Instance.Mesh->GetLodCount(), LodScale);
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
