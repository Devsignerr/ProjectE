#pragma once

#include "Core/Math/Math.h"
#include "Renderer/Camera.h"
#include "RHI/D3D12/D3D12GpuTimer.h"
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
#include "Renderer/MeshInstancing.h"
#include "Renderer/OcclusionCuller.h"
#include "Renderer/LocalLightRenderer.h"
#include "Renderer/ParticleRenderer.h"
#include "Scene/ResourceHandles.h"

#include <chrono>
#include <memory>
#include <vector>

class FD3D12RHI;
struct FMaterial;
class FResourceManager;
class FScene;
class FStaticMesh;
struct FPixelArtComponent;

// 렌더 구간 (CPU/GPU 시간 측정 칸). GPU는 기록 구간이 GPU 작업을 담는 칸(Total/LocalLights/Shadow/MainDraw/Particles/PostProcess)만 의미가 있다
enum class ERenderTimer : uint32
{
	Total,       // Render 전체
	Gather,      // 스킨 팔레트 + 메시 인스턴스 수집/업로드
	LocalLights, // 점광원/스포트 수집 + 로컬 그림자 + 클러스터 컬링
	Shadow,      // 방향광 캐스케이드 그림자
	MainCull,    // 메인 패스 수집/컬링
	MainSort,    // 메인 패스 정렬
	Occlusion,   // 오클루전 1단계 (항목 업로드 + 이전 프레임 HZB 컬링)
	MainDraw,    // 메인 패스 기록 (GPU: 하늘 + 메시, 오클루전이면 HZB·2단계 포함)
	Hzb,         // 오클루전: HZB 만들기 + 2단계 컬링 (MainDraw 안)
	Particles,
	PostProcess,
	Count
};
const char* GetRenderTimerName(ERenderTimer Timer);

struct FSceneRenderStats
{
	uint32 TotalMeshes   = 0; // 씬의 정적 메시 컴포넌트 수
	uint32 VisibleMeshes = 0; // 컬링 통과
	uint32 DrawCalls     = 0; // 메인 패스
	uint32 ShadowDrawCalls = 0; // 방향광 + 로컬 그림자 패스
	uint64 Triangles       = 0; // 메인 패스에서 그린 삼각형
	uint64 ShadowTriangles = 0; // 그림자 패스에서 그린 삼각형
	uint32 Particles     = 0; // 그린 파티클 입자 수
	uint32 ParticleEmittersCulled = 0; // 화면 밖이라 그리지 않은 이미터 (GPU 이미터는 계산도 미룸)
	uint32 LocalLights   = 0; // 클러스터에 올린 점광원/스포트라이트 수
	uint32 LocalShadowSlices = 0; // 이번 프레임 그린 로컬 그림자 장 수 (스포트 1, 점광원 6)
	uint32 SkinnedDrawn  = 0; // 팔레트를 계산한 스킨 메시 (메인 프러스텀 ∪ 그림자 캐스터 볼륨)
	uint32 SkinnedCulled = 0; // 가시성 판정에서 빠진 스킨 메시
	uint64 UploadBytes   = 0; // 씬 렌더러가 이번 프레임 동적 업로드 버퍼에 쓴 양
	// 오클루전 컬링 (GPU 리드백 — 몇 프레임 늦은 값): 검사한 정적 인스턴스, 1단계/2단계에서 그린 수
	uint32 OcclusionTested = 0;
	uint32 OcclusionPhase1 = 0;
	uint32 OcclusionPhase2 = 0;

	float CpuMs[static_cast<uint32>(ERenderTimer::Count)] = {}; // 이번 프레임 CPU 기록 시간
	float GpuMs[static_cast<uint32>(ERenderTimer::Count)] = {}; // GPU 시간 (타임스탬프, 몇 프레임 늦은 값)
	float FrameIntervalMs = 0.0f; // 직전 Render와의 간격 (= 프레임 시간)

	float GetCpuMs(ERenderTimer Timer) const { return CpuMs[static_cast<uint32>(Timer)]; }
	float GetGpuMs(ERenderTimer Timer) const { return GpuMs[static_cast<uint32>(Timer)]; }
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
	FLocalShadowSettings LocalShadowSettings; // 점광원/스포트라이트 그림자
	FVector4             BackgroundColor = FVector4(0.12f, 0.2f, 0.36f, 1.0f); // HDR 선형 값
	bool                 bWireframe      = false; // 메시를 선으로 그린다 (에셋 미리보기용)
	bool                 bDrawSkybox     = true;  // false면 하늘 대신 BackgroundColor (썸네일용, 환경광은 그대로)
	bool                 bEnableLod      = true;  // 메시 LOD (화면 크기 전환). 끄면 항상 LOD0 (--no-lod)
	float                LodScale        = 1.0f;  // 화면 크기 배율: 크면 고품질 LOD를 더 멀리까지
	int32                ForcedLod       = -1;    // 0 이상이면 모든 정적 메시를 그 LOD로 (확인용, --force-lod N)
	float                LodHysteresis   = 0.1f;  // LOD 전환 여유 (임계값 ±비율 띠 안에서는 이전 LOD 유지, 0 = 끔, --lod-hysteresis X)
	// HZB 오클루전 컬링 (메인 패스 정적 메시, --occlusion). 기본 끔: LOD를 켠 예제 씬들에서는 HZB·간접 드로우 비용(GPU ~0.1ms)이
	// 아낀 정점 비용보다 커서 손해였다 (LOD 없이 정점이 많은 씬에서는 이득 — Phase 26 측정)
	bool                 bEnableOcclusion = false;
	// 스킨 팔레트 가시성 컬링: 메인 프러스텀 ∪ 그림자 캐스터 볼륨 밖 스킨 메시는 팔레트/드로우 생략. 끄면 모두 계산 (--no-skin-culling)
	bool                 bSkinVisibilityCulling = true;

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
	// 현재 라이브러리 셰이더로 메시 PSO 생성 (Init/ReloadShaders 공용). bWireframeFill이면 선 채우기 + 컬링 없음
	bool CreateMeshPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, bool bWireframeFill);
	// 스킨 메시 PSO (Mesh.hlsl VSSkinned + 스킨 입력 레이아웃)
	bool CreateSkinnedMeshPipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile, bool bWireframeFill);

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
	FLocalLightRenderer  LocalLightRenderer; // 점광원/스포트라이트 + 클러스터 컬링
	FOcclusionCuller     OcclusionCuller;    // HZB 오클루전 (메인 패스 정적 메시)

	std::unique_ptr<FD3D12RenderTarget> SceneColor; // HDR + 깊이, 출력 크기에 맞춰 재생성

	static constexpr DXGI_FORMAT SceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

	void EnsureSceneColor(uint32 Width, uint32 Height);
	void EnsureTarget(std::unique_ptr<FD3D12RenderTarget>& Target, uint32 Width, uint32 Height, const wchar_t* DebugName,
	                  const FRenderTargetDesc& Desc);
	void RenderFrame(FScene& Scene, const FCamera& Camera, const FRenderOutput& Output);
	// 섀도우 → HDR 씬 패스 (SceneColor를 Width x Height로 맞춘다)
	void RenderSceneColor(FScene& Scene, const FCamera& Camera, uint32 Width, uint32 Height);
	// 인스턴스마다 메인 카메라 화면 크기로 LOD 선택 (그림자 패스도 같은 값)
	void SelectLods(const FCamera& Camera);
	// 메인 패스: 인스턴스 목록 프러스텀 컬링 → 묶음 → 인스턴싱 드로우
	void DrawMeshes(const FCamera& Camera, const FPerFrameConstants& PerFrame);

	// 픽셀 아트: 저해상도 렌더용 카메라(여백만큼 넓힌 투영 + 도트 격자 스냅)와 합성 인자
	FCamera BuildPixelArtCamera(const FPixelArtComponent& PixelArt, const FCamera& Camera, const FRenderOutput& Output,
	                            uint32 SourceWidth, uint32 SourceHeight, FPixelArtCompositeParams& OutParams) const;

	std::unique_ptr<FD3D12RenderTarget> PixelArtColor; // 픽셀 아트: 저해상도 톤매핑 결과 (선형, 부동소수점)

	FMeshInstanceList MeshInstances; // 프레임 메시 인스턴스 (모든 패스 공유)
	// LOD 히스테리시스용 엔티티별 이전 LOD (엔티티 인덱스 칸, 세대로 검증 — 렌더러(= 카메라)마다 따로)
	struct FLodHistory
	{
		uint32 Generation = 0;
		uint32 Lod        = ~0u;
	};
	std::vector<FLodHistory> LodHistory;
	FMeshPassBatches  MainBatches;
	FSceneRenderStats Stats;

	FFrustum FrozenFrustum;
	bool     bCullingFrozen = false;

	// ---- 측정 (통계 패널 + --perf-capture)
	using FClock = std::chrono::steady_clock;
	void BeginTimer(ERenderTimer Timer);
	void EndTimer(ERenderTimer Timer);
	void BeginCpuTimer(ERenderTimer Timer); // GPU 작업이 없는 구간 (같은 칸 CPU 시간에 더한다)
	void EndCpuTimer(ERenderTimer Timer);
	void AccumulatePerfCapture();
	void LogPerfCapture() const;

	FD3D12GpuTimer     GpuTimer;
	FClock::time_point TimerStarts[static_cast<uint32>(ERenderTimer::Count)];
	FClock::time_point LastRenderTime;
	bool               bHasLastRenderTime = false;

	// --perf-capture [--perf-warmup N]: 워밍업 뒤 프레임 평균을 종료 때 로그로 남긴다 (단계별 성능 비교용)
	struct FPerfCapture
	{
		bool   bEnabled      = false;
		uint32 WarmupFrames  = 120;
		uint32 SeenFrames    = 0;
		uint32 Frames        = 0;
		double CpuMs[static_cast<uint32>(ERenderTimer::Count)] = {};
		double GpuMs[static_cast<uint32>(ERenderTimer::Count)] = {};
		double FrameMs         = 0.0;
		double DrawCalls       = 0.0;
		double ShadowDrawCalls = 0.0;
		double Triangles       = 0.0;
		double ShadowTriangles = 0.0;
		double VisibleMeshes   = 0.0;
		double OcclusionTested = 0.0;
		double OcclusionDrawn  = 0.0; // 1단계 + 2단계
		double OcclusionPhase2 = 0.0; // 2단계에서 그린 수 (이전 프레임 HZB만 썼다면 한 프레임 늦게 나왔을 물체)
		uint32 TotalMeshes     = 0;
		double SkinnedDrawn    = 0.0;
		double SkinnedCulled   = 0.0;
		double UploadBytes     = 0.0;
	};
	FPerfCapture PerfCapture;
};
