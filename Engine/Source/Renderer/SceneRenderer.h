#pragma once

#include "Core/Math/Math.h"
#include "Renderer/Camera.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderLibrary.h"
#include "Renderer/PostProcess.h"
#include "Renderer/ShaderTypes.h"
#include "Renderer/ShadowRenderer.h"
#include "Renderer/SkinnedMeshPalette.h"
#include "Renderer/IblRenderer.h"
#include "Renderer/ParticleRenderer.h"
#include "Scene/ResourceHandles.h"

#include <memory>
#include <vector>

class FD3D12RHI;
struct FMaterial;
class FResourceManager;
class FScene;
class FStaticMesh;
struct FPixelArtComponent;

struct FSceneRenderStats
{
	uint32 TotalMeshes   = 0; // 씬의 정적 메시 컴포넌트 수
	uint32 VisibleMeshes = 0; // 컬링 통과
	uint32 DrawCalls     = 0;
	uint32 Particles     = 0; // 그린 파티클 입자 수
};

// 씬의 정적 메시를 수집 → 프러스텀 컬링 → 정렬 → HDR 버퍼에 드로우 → 포스트 프로세싱(톤매핑) → Output.
// 씬에 활성 FPixelArtComponent가 있으면 저해상도(출력 ÷ 도트 크기)로 렌더 → 포스트 → 픽셀 아트 합성(최근접 확대)으로 Output.
// 호출 순서: Rhi.BeginFrame() → Render(..., Output) → (오버레이/UI) → Rhi.EndFrame()
// Render가 끝나면 Output RTV가 깊이 없이 바인딩된 상태로 남는다 (에디터 오버레이가 그 위에 그린다).
class FSceneRenderer
{
public:
	bool Init(FD3D12RHI& InRhi, FResourceManager& InResources);
	void Shutdown();

	void Render(FScene& Scene, const FCamera& Camera, const FRenderOutput& Output);

	// HDR 씬 컬러 (Render 이후 PIXEL_SHADER_RESOURCE 상태). 출력과 같은 크기 (픽셀 아트 모드에서는 저해상도)
	const FD3D12RenderTarget* GetSceneColor() const { return SceneColor.get(); }

	FPostProcessSettings PostProcessSettings;
	FShadowSettings      ShadowSettings;
	FVector4             BackgroundColor = FVector4(0.12f, 0.2f, 0.36f, 1.0f); // HDR 선형 값
	bool                 bWireframe      = false; // 메시를 선으로 그린다 (에셋 미리보기용)
	bool                 bDrawSkybox     = true;  // false면 하늘 대신 BackgroundColor (썸네일용, 환경광은 그대로)

	// 핫 리로드: 셰이더를 라이브러리에서 다시 얻어 PSO를 재생성한다. 성공 시 교체(이전 PSO는 지연 해제),
	// 실패 시 기존 PSO를 유지하고 false. bForceRecompile이면 캐시·쿠킹 파일을 무시하고 컴파일한다.
	bool ReloadShaders(bool bForceRecompile = false);

	FShaderLibrary& GetShaderLibrary() { return ShaderLibrary; }

	// 이번 프레임 스킨 팔레트 (Render 이후 같은 프레임 안에서만 유효 — 에디터 오버레이용)
	const FSkinnedMeshPalette& GetSkinPalettes() const { return SkinPalettes; }

	// 컬링 프러스텀 고정 (컬링 동작 확인용). 켜면 이후 카메라를 움직여도 컬링은 고정 시점 기준
	void SetFreezeCulling(bool bFreeze);
	bool IsCullingFrozen() const { return bCullingFrozen; }

	const FSceneRenderStats& GetStats() const { return Stats; }

	// 간이 환경광 (하늘/지면 반구, HDR 선형). 이후 IBL이 대체한다
	FVector3 SkyColor         = FVector3(0.35f, 0.45f, 0.6f);
	FVector3 GroundColor      = FVector3(0.15f, 0.13f, 0.1f);
	float    AmbientIntensity = 1.0f;

private:
	struct FMeshDrawCommand
	{
		const FStaticMesh* Mesh     = nullptr;
		const FMaterial*   Material = nullptr;
		FMeshHandle        MeshHandle;
		FMaterialHandle    MaterialHandle;
		FMatrix4x4         World;
		float              DistanceSquared = 0.0f;
		D3D12_GPU_VIRTUAL_ADDRESS SkinPalette = 0; // 0이 아니면 스킨 메시 (World = 항등)
	};

	// 현재 라이브러리 셰이더로 메시 PSO 생성 (Init/ReloadShaders 공용). bWireframeFill이면 선 채우기 + 컬링 없음
	bool CreateMeshPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, bool bWireframeFill);
	// 스킨 메시 PSO (Mesh.hlsl VSSkinned + 스킨 입력 레이아웃)
	bool CreateSkinnedMeshPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, bool bWireframeFill);

	void               CollectDrawCommands(FScene& Scene, const FFrustum& Frustum, const FVector3& CameraPosition);
	FPerFrameConstants BuildPerFrameConstants(FScene& Scene, const FCamera& Camera) const;

	FD3D12RHI*        Rhi       = nullptr;
	FResourceManager* Resources = nullptr;

	FD3D12ShaderCompiler ShaderCompiler;
	FShaderLibrary       ShaderLibrary; // 쿠킹된 DXIL 우선, 없으면 컴파일
	FD3D12RootSignature  RootSignature;
	FD3D12PipelineState  PipelineState;
	FD3D12PipelineState  SkinnedPipelineState;
	FD3D12PipelineState  WireframePipelineState;
	FD3D12PipelineState  SkinnedWireframePipelineState;
	FSkinnedMeshPalette  SkinPalettes; // 프레임별 본 팔레트 (섀도우/메인 공유)
	FPostProcessor       PostProcessor;
	FShadowRenderer      ShadowRenderer;
	FIblRenderer         IblRenderer;
	FParticleRenderer    ParticleRenderer;

	std::unique_ptr<FD3D12RenderTarget> SceneColor; // HDR + 깊이, 출력 크기에 맞춰 재생성

	static constexpr DXGI_FORMAT SceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

	void EnsureSceneColor(uint32 Width, uint32 Height);
	void EnsureTarget(std::unique_ptr<FD3D12RenderTarget>& Target, uint32 Width, uint32 Height, const wchar_t* DebugName,
	                  const FRenderTargetDesc& Desc);
	// 섀도우 → HDR 씬 패스 (SceneColor를 Width x Height로 맞춘다)
	void RenderSceneColor(FScene& Scene, const FCamera& Camera, uint32 Width, uint32 Height);
	void DrawMeshes(FScene& Scene, const FCamera& Camera, const FPerFrameConstants& PerFrame);

	// 픽셀 아트: 저해상도 렌더용 카메라(여백만큼 넓힌 투영 + 도트 격자 스냅)와 합성 인자
	FCamera BuildPixelArtCamera(const FPixelArtComponent& PixelArt, const FCamera& Camera, const FRenderOutput& Output,
	                            uint32 SourceWidth, uint32 SourceHeight, FPixelArtCompositeParams& OutParams) const;

	std::unique_ptr<FD3D12RenderTarget> PixelArtColor; // 픽셀 아트: 저해상도 톤매핑 결과 (선형, 부동소수점)

	std::vector<FMeshDrawCommand> DrawCommands;
	FSceneRenderStats             Stats;

	FFrustum FrozenFrustum;
	bool     bCullingFrozen = false;
};
