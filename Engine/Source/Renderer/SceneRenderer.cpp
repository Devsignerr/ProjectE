#include "Renderer/SceneRenderer.h"

#include "RHI/D3D12/D3D12RHI.h"
#include "Renderer/Camera.h"
#include "Renderer/Material.h"
#include "Renderer/PixelArtMath.h"
#include "Renderer/ResourceManager.h"
#include "Renderer/StaticMesh.h"
#include "Scene/Scene.h"

#include <algorithm>
#include <cstring>
#include <tuple>
#include <unordered_map>

E_DECLARE_LOG_CATEGORY(LogRenderer)

namespace
{
	// Mesh.hlsl 루트 시그니처 레이아웃
	enum ERootParameter : uint32
	{
		RootParam_PerObject       = 0, // b0
		RootParam_PerFrame        = 1, // b1
		RootParam_Material        = 2, // b2
		RootParam_MaterialTexture = 3, // t0~t4 (머티리얼 텍스처 테이블)
		RootParam_Shadow          = 4, // b3 (캐스케이드 상수)
		RootParam_ShadowMap       = 5, // t8 (섀도우 맵 배열)
		RootParam_Ibl             = 6, // t5~t7
		RootParam_SkinPalette     = 7, // b4 (스킨 메시 본 팔레트, 정점 셰이더)
		RootParam_Cluster         = 8, // b5 (클러스터 상수)
		RootParam_LocalLights     = 9, // t9 (라이트 목록, 루트 SRV)
		RootParam_ClusterData     = 10, // t10 (클러스터별 라이트 인덱스, 루트 SRV)
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

	const uint32 PerObjectIndex = RootSignature.AddConstantBufferView(0);
	const uint32 PerFrameIndex  = RootSignature.AddConstantBufferView(1);
	const uint32 MaterialIndex  = RootSignature.AddConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 TextureIndex   = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MaterialSlot_Count, 0) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(PerObjectIndex == RootParam_PerObject && PerFrameIndex == RootParam_PerFrame &&
	        MaterialIndex == RootParam_Material && TextureIndex == RootParam_MaterialTexture);
	const uint32 ShadowIndex    = RootSignature.AddConstantBufferView(3, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 ShadowMapIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(ShadowIndex == RootParam_Shadow && ShadowMapIndex == RootParam_ShadowMap);
	const uint32 IblIndex = RootSignature.AddDescriptorTable(
		{ FD3D12RootSignature::MakeRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 5) }, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(IblIndex == RootParam_Ibl);
	const uint32 SkinPaletteIndex = RootSignature.AddConstantBufferView(4, 0, D3D12_SHADER_VISIBILITY_VERTEX);
	E_CHECK(SkinPaletteIndex == RootParam_SkinPalette);
	const uint32 ClusterIndex     = RootSignature.AddConstantBufferView(5, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 LocalLightsIndex = RootSignature.AddShaderResourceView(9, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	const uint32 ClusterDataIndex = RootSignature.AddShaderResourceView(10, 0, D3D12_SHADER_VISIBILITY_PIXEL);
	E_CHECK(ClusterIndex == RootParam_Cluster && LocalLightsIndex == RootParam_LocalLights && ClusterDataIndex == RootParam_ClusterData);
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
	if (!ShadowRenderer.Init(*Rhi, ShaderLibrary) || !IblRenderer.Init(*Rhi, ShaderLibrary) || !LocalLightRenderer.Init(*Rhi, ShaderLibrary))
	{
		return false;
	}
	if (!ParticleRenderer.Init(*Rhi, ShaderLibrary, *Resources, SceneColorFormat, FD3D12RHI::DepthBufferFormat))
	{
		return false;
	}

	E_LOG(LogRenderer, Display, "씬 렌더러 초기화 완료 (HDR {}, 톤매핑)", "R16G16B16A16_FLOAT");
	return true;
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
	SceneColor.reset();
	PixelArtColor.reset();
	PostProcessor.Shutdown();
	ShadowRenderer.Shutdown();
	IblRenderer.Shutdown();
	ParticleRenderer.Shutdown();
	LocalLightRenderer.Shutdown();
	PipelineState.Shutdown();
	SkinnedPipelineState.Shutdown();
	WireframePipelineState.Shutdown();
	SkinnedWireframePipelineState.Shutdown();
	RootSignature.Shutdown();
	ShaderLibrary.Shutdown();
	ShaderCompiler.Shutdown();
	DrawCommands.clear();
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

	const FPixelArtComponent* PixelArt = FindPixelArtSettings(Scene);
	if (PixelArt == nullptr)
	{
		RenderSceneColor(Scene, Camera, Output.Width, Output.Height);
		PostProcessor.Render(CommandList, SceneColor->GetSrv(), Output, PostProcessSettings);
		return;
	}

	// 픽셀 아트: 저해상도 씬 → 저해상도 포스트(톤매핑) → 합성 확대
	const uint32 PixelSize    = FPixelArtMath::ClampPixelSize(PixelArt->PixelSize);
	const uint32 SourceWidth  = FPixelArtMath::GetSourceDimension(Output.Width, PixelSize);
	const uint32 SourceHeight = FPixelArtMath::GetSourceDimension(Output.Height, PixelSize);

	FPixelArtCompositeParams Params;
	const FCamera            SourceCamera = BuildPixelArtCamera(*PixelArt, Camera, Output, SourceWidth, SourceHeight, Params);
	RenderSceneColor(Scene, SourceCamera, SourceWidth, SourceHeight);

	EnsureTarget(PixelArtColor, SourceWidth, SourceHeight, L"PixelArtColor", FRenderTargetDesc::MakeHdr(false));
	PixelArtColor->Begin(CommandList, nullptr); // 톤매핑이 전체를 덮어쓴다
	PostProcessor.Render(CommandList, SceneColor->GetSrv(), PixelArtColor->GetOutput(), PostProcessSettings);
	PixelArtColor->End(CommandList);

	PostProcessor.RenderPixelArtComposite(CommandList, *PixelArtColor, *SceneColor, Output, Params);
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

	// 스킨 메시 본 팔레트 (섀도우/메인 패스 공유)
	SkinPalettes.Build(Scene, *Resources, Rhi->GetDynamicBuffer());

	// GPU 파티클 계산 (그리기 전에)
	ParticleRenderer.Simulate(Scene);

	// 점광원/스포트라이트 목록 + 클러스터 컬링 (화면 크기 = 이번 씬 타깃)
	LocalLightRenderer.Prepare(Scene, Camera, Width, Height);
	Stats.LocalLights = LocalLightRenderer.GetLightCount();

	// 0) 방향광 섀도우 패스
	ShadowRenderer.Render(Scene, *Resources, Camera, PerFrame.DirectionalLight.Direction, ShadowSettings, &SkinPalettes);

	// 1) HDR 씬 패스
	EnsureSceneColor(Width, Height);
	SceneColor->Begin(CommandList, &BackgroundColor.X);
	if (bDrawSkybox)
	{
		IblRenderer.RenderSkybox(Camera, PerFrame.AmbientIntensity);
	}
	DrawMeshes(Scene, Camera, PerFrame);
	Stats.Particles = ParticleRenderer.Render(Scene, Camera);
	SceneColor->End(CommandList);
}

void FSceneRenderer::DrawMeshes(FScene& Scene, const FCamera& Camera, const FPerFrameConstants& PerFrame)
{
	const FMatrix4x4 ViewProjection = Camera.GetViewProjectionMatrix();
	if (!bCullingFrozen)
	{
		FrozenFrustum = FFrustum::FromViewProjection(ViewProjection);
	}

	CollectDrawCommands(Scene, FrozenFrustum, Camera.GetPosition());

	// 정렬: (정적/스킨 PSO) → 머티리얼 → 메시 → 가까운 순 (상태 변경 최소화 + 초기 깊이 기각)
	std::sort(DrawCommands.begin(), DrawCommands.end(), [](const FMeshDrawCommand& A, const FMeshDrawCommand& B) {
		const bool bSkinnedA = A.SkinPalette != 0;
		const bool bSkinnedB = B.SkinPalette != 0;
		return std::tie(bSkinnedA, A.MaterialHandle.Index, A.MeshHandle.Index, A.DistanceSquared) <
		       std::tie(bSkinnedB, B.MaterialHandle.Index, B.MeshHandle.Index, B.DistanceSquared);
	});

	FD3D12DynamicUploadBuffer& DynamicBuffer = Rhi->GetDynamicBuffer();
	ID3D12GraphicsCommandList* CommandList   = Rhi->GetCommandList();

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

	// 머티리얼 상수는 프레임 내에서 한 번만 업로드
	std::unordered_map<uint64, D3D12_GPU_VIRTUAL_ADDRESS> MaterialConstantCache;

	const FMaterial* BoundMaterial = nullptr;
	bool             bSkinnedBound = false;
	Stats.DrawCalls                = 0;

	for (const FMeshDrawCommand& Command : DrawCommands)
	{
		const bool bSkinned = Command.SkinPalette != 0;
		if (bSkinned != bSkinnedBound)
		{
			CommandList->SetPipelineState(bSkinned ? SkinnedPipeline.Get() : StaticPipeline.Get());
			bSkinnedBound = bSkinned;
		}
		if (Command.Material != BoundMaterial)
		{
			const uint64 Key   = Command.MaterialHandle.ToId();
			auto         Found = MaterialConstantCache.find(Key);
			if (Found == MaterialConstantCache.end())
			{
				Found = MaterialConstantCache.emplace(Key, DynamicBuffer.AllocateConstants(Command.Material->Constants).GpuAddress).first;
			}
			CommandList->SetGraphicsRootConstantBufferView(RootParam_Material, Found->second);
			CommandList->SetGraphicsRootDescriptorTable(RootParam_MaterialTexture, Command.Material->TextureTable.Gpu);
			BoundMaterial = Command.Material;
		}

		FPerObjectConstants PerObject;
		PerObject.World                 = Command.World;
		PerObject.WorldInverseTranspose = Command.World.GetInverse().GetTransposed();
		CommandList->SetGraphicsRootConstantBufferView(RootParam_PerObject, DynamicBuffer.AllocateConstants(PerObject).GpuAddress);

		if (bSkinned)
		{
			CommandList->SetGraphicsRootConstantBufferView(RootParam_SkinPalette, Command.SkinPalette);
			Command.Mesh->DrawSkinned(CommandList);
		}
		else
		{
			Command.Mesh->Draw(CommandList);
		}
		++Stats.DrawCalls;
	}
}

void FSceneRenderer::CollectDrawCommands(FScene& Scene, const FFrustum& Frustum, const FVector3& CameraPosition)
{
	DrawCommands.clear();
	Stats.TotalMeshes   = 0;
	Stats.VisibleMeshes = 0;

	Scene.GetRegistry().View<FTransformComponent, FStaticMeshComponent>().Each(
		[&](FEntity Entity, FTransformComponent& Transform, FStaticMeshComponent& MeshComponent) {
			++Stats.TotalMeshes;
			if (!MeshComponent.bVisible)
			{
				return;
			}

			const FStaticMesh* Mesh = Resources->GetMesh(MeshComponent.Mesh);
			if (Mesh == nullptr)
			{
				return;
			}

			// 월드 AABB로 프러스텀 컬링 (스킨 메시는 팔레트 기준 경계)
			const FSkinnedDrawInfo* Skinned     = SkinPalettes.Find(Entity);
			const FBox              WorldBounds = Skinned ? Skinned->WorldBounds : Mesh->GetLocalBounds().TransformBy(Transform.WorldMatrix);
			if (!Frustum.Intersects(WorldBounds))
			{
				return;
			}
			++Stats.VisibleMeshes;

			FMeshDrawCommand& Command = DrawCommands.emplace_back();
			Command.Mesh              = Mesh;
			Command.MeshHandle        = MeshComponent.Mesh;
			Command.Material          = &Resources->ResolveMaterial(MeshComponent.Material);
			Command.MaterialHandle    = MeshComponent.Material.IsValid() ? MeshComponent.Material : Resources->GetDefaultMaterial();
			Command.World             = Skinned ? FMatrix4x4::Identity : Transform.WorldMatrix;
			Command.DistanceSquared   = FVector3::DistanceSquared(WorldBounds.GetCenter(), CameraPosition);
			Command.SkinPalette       = Skinned ? Skinned->Palette : 0;
		});
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
