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
#include "Renderer/PixelArtObjectSnap.h"
#include "Renderer/ScreenPass.h"
#include "Renderer/TemporalAA.h"
#include "Renderer/AmbientOcclusion.h"
#include "Renderer/DecalRenderer.h"
#include "Renderer/FogRenderer.h"
#include "Renderer/ReflectionCaptures.h"
#include "Renderer/ScreenSpaceReflections.h"
#include "Renderer/FoliageRenderer.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/TerrainRenderer.h"
#include "Scene/ResourceHandles.h"

#include <chrono>
#include <string>
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
	DepthPrepass, // 깊이 + 화면 공간 법선 + 움직임 벡터 (GPU: 오클루전이면 HZB·2단계 포함)
	TemporalAA,
	AmbientOcclusion, // SSAO 계산 + 블러 (반해상도)
	Decals,           // 데칼 → DBuffer
	VolumetricFog,    // 안개 상수 + 볼류메트릭 주입·적분 (계산)
	Fog,              // 안개 적용 (전체 화면)
	Reflections,      // SSR (Hi-Z + 추적)
	Translucent,      // 반투명/가산 메시 패스
	Count
};
const char* GetRenderTimerName(ERenderTimer Timer);

struct FSceneRenderStats
{
	uint32 TotalMeshes   = 0; // 씬의 정적 메시 컴포넌트 수
	uint32 VisibleMeshes = 0; // 컬링 통과
	uint32 DrawCalls     = 0; // 메인 패스
	uint32 TranslucentDrawCalls = 0; // 반투명 패스
	uint32 PrepassDrawCalls = 0; // 깊이 사전 패스
	uint32 Decals           = 0; // 그린 데칼 수
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
//
// 씬 패스 순서 (RenderSceneColor): 로컬 라이트/그림자 → 방향광 그림자 → 메인 묶음 컬링·정렬(+오클루전 1단계)
//   → [깊이 사전 패스] 씬 깊이 + 화면 공간 법선(SceneNormal) + 움직임 벡터(SceneVelocity) (오클루전이면 여기서 HZB + 2단계)
//   → [메인 패스] 하늘 + 불투명 메시 (깊이 같음 테스트, 깊이 쓰기 없음) → 안개 적용 → [반투명 패스] 반투명/가산 메시 (먼 것부터, 깊이 쓰기 없음)
//   → 파티클 → (포스트)
// 머티리얼 블렌드 모드 (Material.h): Opaque/Masked는 사전·메인·그림자 패스(Masked는 알파로 잘라냄), Translucent/Additive는 반투명 패스만
//   와이어프레임이거나 bDepthPrepass = false면 사전 패스 없이 예전처럼 메인 패스가 깊이를 쓴다 (법선/움직임 버퍼는 지운 값).
// 움직임 벡터: 현재 UV - 이전 UV (지터 없는 위치), 하늘 등 기하가 없는 픽셀은 0 → 쓰는 쪽이 깊이로 카메라 재투영 (ScreenSpace.hlsli)
// 시간 이력(이전 프레임 뷰-투영, 엔티티별 이전 월드/스킨 팔레트)은 렌더러가 한 프레임에 뷰 하나만 그리고 연속 프레임일 때만 유효하다
//   (에셋 미리보기·썸네일처럼 한 렌더러로 여러 씬을 그리면 엔티티 번호가 겹치므로 이력을 쓰지 않는다 → 지터/TAA 꺼짐)
class FSceneRenderer
{
public:
	bool Init(FD3D12RHI& InRhi, FResourceManager& InResources);
	void Shutdown();

	void Render(FScene& Scene, const FCamera& Camera, const FRenderOutput& Output);

	// HDR 씬 컬러 (Render 이후 PIXEL_SHADER_RESOURCE 상태). 출력과 같은 크기 (픽셀 아트 모드에서는 저해상도)
	// 깊이는 지터가 들어간 투영으로 그려진다 (TAA 켬일 때) — 오버레이가 깊이 테스트에 써도 서브픽셀 차이뿐
	const FD3D12RenderTarget* GetSceneColor() const { return SceneColor.get(); }
	// 화면 공간 법선 (R10G10B10A2, ScreenSpace.hlsli) / 움직임 벡터 (R16G16_FLOAT). 씬 컬러와 같은 크기, PIXEL_SHADER_RESOURCE
	const FD3D12RenderTarget* GetSceneNormal() const { return SceneNormal.get(); }
	const FD3D12RenderTarget* GetSceneVelocity() const { return SceneVelocity.get(); }
	// 이번 프레임 이전 프레임 이력(뷰-투영, 엔티티 이전 월드)을 쓸 수 있었는지 (Render 이후)
	bool IsTemporalHistoryValid() const { return bTemporalHistoryValid; }
	// 이번 프레임 투영 지터 (NDC, 없으면 0)
	const FVector2& GetJitterNdc() const { return CurrentJitterNdc; }

	// 다음 Render에서 씬의 반사 캡처를 모두 굽는다 (에디터 도구 메뉴, --bake-captures). 파일은 몇 프레임 뒤(GPU 완료) 저장
	void RequestReflectionCaptureBake() { bBakeCapturesRequested = true; }

	FPostProcessSettings PostProcessSettings;
	FShadowSettings      ShadowSettings;
	FLocalShadowSettings LocalShadowSettings; // 점광원/스포트라이트 그림자
	FVector4             BackgroundColor = FVector4(0.12f, 0.2f, 0.36f, 1.0f); // HDR 선형 값
	bool                 bWireframe      = false; // 메시를 선으로 그린다 (에셋 미리보기용)
	bool                 bDrawSkybox     = true;  // false면 하늘 대신 BackgroundColor (썸네일용, 환경광은 그대로)
	// ---- 아래 디버그/비교 토글(LodScale 제외)은 Render마다 콘솔 변수에서 다시 읽는다 (RendererConsoleVariables.h — 바꾸려면 r.* 변수, 예전 --no-* 플래그는 별칭)
	bool                 bEnableLod      = true;  // 메시 LOD (화면 크기 전환). 끄면 항상 LOD0 (--no-lod)
	float                LodScale        = 1.0f;  // 화면 크기 배율: 크면 고품질 LOD를 더 멀리까지
	int32                ForcedLod       = -1;    // 0 이상이면 모든 정적 메시를 그 LOD로 (확인용, --force-lod N)
	float                LodHysteresis   = 0.1f;  // LOD 전환 여유 (임계값 ±비율 띠 안에서는 이전 LOD 유지, 0 = 끔, --lod-hysteresis X)
	// HZB 오클루전 컬링 (메인 패스 정적 메시, --occlusion). 기본 끔: LOD를 켠 예제 씬들에서는 HZB·간접 드로우 비용(GPU ~0.1ms)이
	// 아낀 정점 비용보다 커서 손해였다 (LOD 없이 정점이 많은 씬에서는 이득 — Phase 26 측정)
	bool                 bEnableOcclusion = false;
	// 스킨 팔레트 가시성 컬링: 메인 프러스텀 ∪ 그림자 캐스터 볼륨 밖 스킨 메시는 팔레트/드로우 생략. 끄면 모두 계산 (--no-skin-culling)
	bool                 bSkinVisibilityCulling = true;
	// 깊이 사전 패스 (깊이 + 화면 공간 법선 + 움직임 벡터, 메인 패스는 깊이 같음 테스트). 끄면 법선/움직임 버퍼가 비어 있다 (--no-depth-prepass)
	bool                 bDepthPrepass = true;
	// 화면 공간 버퍼 확인 (톤매핑 결과 대신 출력에 그림): 0 없음, 1 법선, 2 움직임 벡터, 3 깊이, 4 SSAO (--debug-view normal|velocity|depth|ao)
	uint32               DebugView = 0;
	// 서브픽셀 투영 지터 (Halton 2,3 8개). TAA가 켜질 때만 켠다 — 혼자 켜면 화면이 떨린다 (--jitter: 확인용 강제)
	bool                 bTemporalJitter = false;

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
	// 마지막으로 실행한 렌더 그래프 요약 (패스·제거·비동기·전이 수 — 통계 창, r.RenderGraph.Dump는 전체 덤프)
	const FRGStats&          GetGraphStats() const { return LastGraphStats; }
	FTerrainRenderer&        GetTerrainRenderer() { return TerrainRenderer; }
	FFoliageRenderer&        GetFoliageRenderer() { return FoliageRenderer; }

	// 간이 환경광 (하늘/지면 반구, HDR 선형). 이후 IBL이 대체한다
	FVector3 SkyColor         = FVector3(0.35f, 0.45f, 0.6f);
	FVector3 GroundColor      = FVector3(0.15f, 0.13f, 0.1f);
	float    AmbientIntensity = 1.0f;

private:
	// 메시 패스 PSO 종류. 패스마다 머티리얼 변형(MaterialRender::MakeVariant: Masked|Additive / 양면 / 스킨) 8개.
	// 정점 셰이더는 모두 같은 바이트코드(VSMain/VSSkinned) → 사전 패스와 메인 패스 깊이가 비트 단위로 같다
	enum class EMeshPass : uint8
	{
		Main,           // 깊이 LESS + 쓰기 (사전 패스 없음)
		MainDepthEqual, // 사전 패스 뒤: 깊이 EQUAL, 쓰기 없음
		Wireframe,      // 선 채우기, 컬링 없음 (변형은 스킨만 — 머티리얼 무관)
		Prepass,        // PSPrepass: 깊이 + 법선 + 움직임 벡터 (MRT 2개)
		Translucent,    // 반투명/가산 (변형 bit0 = 가산): 깊이 LESS, 쓰기 없음, 블렌드
		Count
	};
	// 패스가 실제로 쓰는 변형인가 (Wireframe은 Masked/양면 비트 없음)
	static bool IsMeshPipelineUsed(EMeshPass Pass, uint32 Variant);
	// 현재 라이브러리 셰이더로 메시 PSO 생성 (Init/ReloadShaders 공용)
	bool CreateMeshPipeline(FD3D12PipelineState& OutPipeline, EMeshPass Pass, uint32 Variant);
	static void GetMeshShaderDescs(EMeshPass Pass, uint32 Variant, FShaderCompileDesc& OutVertex, FShaderCompileDesc& OutPixel);
	FD3D12PipelineState& GetMeshPipeline(EMeshPass Pass, uint32 Variant)
	{
		if (Pass == EMeshPass::Wireframe)
		{
			Variant &= MaterialRender::VariantSkinned;
		}
		return MeshPipelines[static_cast<uint32>(Pass)][Variant];
	}

	FPerFrameConstants BuildPerFrameConstants(FScene& Scene, const FCamera& Camera) const;

	FD3D12RHI*        Rhi       = nullptr;
	FResourceManager* Resources = nullptr;
	uint32            ResourceRootProviderId = 0; // FResourceManager::AddRootProvider (지형/폴리지 캐시)

	FD3D12ShaderCompiler ShaderCompiler;
	FShaderLibrary       ShaderLibrary; // 쿠킹된 DXIL 우선, 없으면 컴파일
	FD3D12RootSignature  RootSignature;
	FD3D12PipelineState  MeshPipelines[static_cast<uint32>(EMeshPass::Count)][MaterialRender::VariantCount]; // [패스][머티리얼 변형]
	FSkinnedMeshPalette  SkinPalettes; // 프레임별 본 팔레트 (섀도우/메인 공유)
	FPostProcessor       PostProcessor;
	FShadowRenderer      ShadowRenderer;
	FIblRenderer         IblRenderer;
	FParticleRenderer    ParticleRenderer;
	FLocalLightRenderer  LocalLightRenderer; // 점광원/스포트라이트 + 클러스터 컬링
	FOcclusionCuller     OcclusionCuller;    // HZB 오클루전 (메인 패스 정적 메시)
	FScreenPassRootSignature ScreenPassRoot; // 화면 공간 패스 공용 (TAA/SSAO/안개/SSR)
	FTemporalAA          TemporalAA;
	FAmbientOcclusion    AmbientOcclusion;
	FDecalRenderer       DecalRenderer;
	FFogRenderer         FogRenderer;
	FScreenSpaceReflections ScreenSpaceReflections;
	FReflectionCaptures  ReflectionCaptures;
	bool                 bBakeCapturesRequested = false;
	bool                 bRenderingCaptures     = false; // 굽는 중: 캡처/SSR 없이 하늘만 반사
	// 콘솔 변수 → 위 디버그 토글 + 아래 r.TAA/r.SSAO/r.SSR (FPostProcessSettings와 AND). Init과 Render 시작에서
	void                 ApplyConsoleVariables();
	bool                 bConsoleTemporalAA       = true;
	bool                 bConsoleAmbientOcclusion = true;
	bool                 bConsoleReflections      = true;
	// 하늘광 환경맵이 바뀌면 FAssetCache로 읽어 IBL을 다시 만든다 (Phase 33-7)
	void                 UpdateEnvironment(FScene& Scene);
	std::string          AppliedEnvironmentMap;
	float                AppliedEnvironmentRotation = 0.0f;
	// 씬의 반사 캡처마다 큐브 면 6개를 그려 프리필터 → 아틀라스 + .ecapture 저장 예약 (Render 안에서, 프레임 명령 목록에 기록)
	void                 BakeReflectionCaptures(FScene& Scene);
	bool                 bTaaRanLastFrame = false;
	FMatrix4x4           CurrentReprojection; // 이번 프레임 카메라 재투영 (현재 클립 → 이전 클립, 지터 없음)
	const FScene*        PrevScene = nullptr;  // 이전 프레임에 그린 씬 (바뀌면 이력 무효)
	FTerrainRenderer     TerrainRenderer;    // 지형 (Phase 34)
	FFoliageRenderer     FoliageRenderer;    // 풀·나무 → 메시 인스턴스 목록 (Phase 34-3)

	std::unique_ptr<FD3D12RenderTarget> SceneColor;    // HDR + 깊이, 출력 크기에 맞춰 재생성
	std::unique_ptr<FD3D12RenderTarget> SceneNormal;   // 화면 공간 법선 (깊이 사전 패스)
	std::unique_ptr<FD3D12RenderTarget> SceneVelocity; // 움직임 벡터 (깊이 사전 패스)

	static constexpr DXGI_FORMAT SceneColorFormat    = DXGI_FORMAT_R16G16B16A16_FLOAT;
	static constexpr DXGI_FORMAT SceneNormalFormat   = DXGI_FORMAT_R10G10B10A2_UNORM;
	static constexpr DXGI_FORMAT SceneVelocityFormat = DXGI_FORMAT_R16G16_FLOAT;

	// 씬 타깃의 그래프 참조 (RenderSceneColor가 가져온다)
	struct FSceneGraphRefs
	{
		FRGResourceRef Color;
		FRGResourceRef Depth;
		FRGResourceRef Normal;
		FRGResourceRef Velocity;
	};
	// 메시 패스의 오클루전 단계: All = 오클루전 없음, Phase1/Phase2 = 그 단계만, Both = 1 → 2를 한 패스에서 (HZB를 만들지 않는 패스)
	enum class EMeshPhase : uint8
	{
		All,
		Phase1,
		Phase2,
		Both,
	};

	void EnsureSceneColor(uint32 Width, uint32 Height);
	void EnsureTarget(std::unique_ptr<FD3D12RenderTarget>& Target, uint32 Width, uint32 Height, const wchar_t* DebugName,
	                  const FRenderTargetDesc& Desc);
	// 렌더 그래프: 측정 훅 연결 / 컴파일(CVar 옵션) + 실행 + 통계·덤프
	void SetupGraph(FRenderGraph& Graph);
	void ExecuteGraph(FRenderGraph& Graph);
	void BeginGraphTimer(ERenderTimer Timer, ID3D12GraphicsCommandList* List, bool bCompute);
	void EndGraphTimer(ERenderTimer Timer, ID3D12GraphicsCommandList* List, bool bCompute);
	// 그래프 실행 뒤 하위 렌더러 통계 모으기 (그림자 드로우·오클루전·파티클 등)
	void FinalizeFrameStats();
	// 씬 → (TAA) → 포스트/픽셀 아트 합성 → Output 패스 등록
	void RenderFrame(FRenderGraph& Graph, FScene& Scene, const FCamera& Camera, const FRenderOutput& Output);
	// 섀도우 → HDR 씬 패스 등록 (SceneColor를 Width x Height로 맞춘다). CPU 준비(수집·컬링·상수 업로드)는 여기서 바로, GPU 기록은 그래프 실행 때
	// bAllowJitter = false면 bTemporalJitter여도 지터 없음 (픽셀 아트)
	void RenderSceneColor(FRenderGraph& Graph, FScene& Scene, const FCamera& Camera, uint32 Width, uint32 Height, bool bAllowJitter,
	                      FSceneGraphRefs& OutRefs);
	// 인스턴스마다 메인 카메라 화면 크기로 LOD 선택 (그림자 패스도 같은 값)
	void SelectLods(const FCamera& Camera);
	// 메인 묶음: 인스턴스 목록 프러스텀 컬링 → 묶음·정렬 (+ 오클루전 1단계 준비). 사전 패스와 메인 패스가 같은 묶음을 그린다
	void PrepareMainBatches(const FCamera& Camera, bool bOcclusion);
	// 메인 묶음 기록 (지형 포함 — 2단계 패스 제외). 드로우/삼각형 수를 더한다
	void RecordMeshBatches(ID3D12GraphicsCommandList* CommandList, EMeshPass Pass, D3D12_GPU_VIRTUAL_ADDRESS PerFrameAddress,
	                       D3D12_GPU_VIRTUAL_ADDRESS ShadowAddress, EMeshPhase Phase, uint32& InOutDrawCalls, uint64& InOutTriangles);
	// 반투명 묶음 기록 (안개 적용 뒤, 파티클 전 — 씬 컬러 + 깊이가 바인딩된 상태)
	void DrawTranslucentBatches(ID3D12GraphicsCommandList* CommandList, D3D12_GPU_VIRTUAL_ADDRESS PerFrameAddress, D3D12_GPU_VIRTUAL_ADDRESS ShadowAddress,
	                            D3D12_GPU_VIRTUAL_ADDRESS FogConstants, uint32& OutDrawCalls, uint64& OutTriangles);
	// 메시 루트 시그니처 + 패스 공용 루트 인자 (프레임/그림자/IBL/로컬 라이트/인스턴스/화면 버퍼)
	void BindMeshPassRoot(ID3D12GraphicsCommandList* CommandList, D3D12_GPU_VIRTUAL_ADDRESS PerFrameAddress, D3D12_GPU_VIRTUAL_ADDRESS ShadowAddress,
	                      D3D12_GPU_VIRTUAL_ADDRESS InstanceIndices);
	// DebugView가 켜져 있으면 화면 공간 버퍼를 Output에 덮어 그리는 패스
	void AddDebugViewPass(FRenderGraph& Graph, const FPostProcessGraphOutput& Output, const FSceneGraphRefs& Refs);
	// 깊이 사전 패스 렌더 타깃 바인딩 (법선 + 움직임 벡터 MRT + 씬 깊이) + 뷰포트
	void BindPrepassTargets(ID3D12GraphicsCommandList* CommandList);

	// ---- 렌더 그래프 (Phase 47)
	FRGResourcePool GraphPool;          // 그래프 내부 텍스처 풀 (크기·형식 키)
	FRGStats        LastGraphStats;
	uint32          SeenDumpSerial      = 0;     // r.RenderGraph.Dump 요청 번호 (마지막으로 덤프한)
	bool            bPendingSnapRestore = false; // 픽셀 아트 물체 스냅: 그래프 실행 뒤 되돌린다
	bool            bFrameOcclusion     = false; // 이번 씬 렌더가 오클루전을 썼는가 (통계)
	uint64          FrameMainTriangles        = 0;
	uint64          FrameTranslucentTriangles = 0;
	FD3D12GpuTimer  ComputeGpuTimer;    // 비동기 계산 큐 패스 구간 (같은 ERenderTimer 칸 — Stats.GpuMs는 그래픽스 + 계산)
	// 엔티티별 이전 프레임 월드로 인스턴스 PrevWorld 채우기 (Upload 전). bValid = false면 이력을 쓰지 않고 현재로
	void ApplyMotionHistory(bool bValid);

	// ---- 시간 이력 (지터/움직임 벡터/TAA)
	uint64     CurrentFrameNumber = ~0ull; // Rhi 프레임 번호
	uint32     ViewsThisFrame     = 0;     // 이번 Rhi 프레임에 Render가 불린 횟수
	uint32     ViewsLastFrame     = 0;     // 바로 앞 Rhi 프레임의 횟수 (연속이 아니면 0)
	bool       bTemporalHistoryValid = false;
	bool       bHasPrevView          = false;
	FMatrix4x4 PrevUnjitteredViewProjection;
	FVector3   PrevCameraPosition;
	FVector3   PrevCameraForward = FVector3::ForwardVector;
	uint32     PrevTargetWidth   = 0;
	uint32     PrevTargetHeight  = 0;
	uint64     TemporalFrameIndex = 0; // 지터 수열 번호
	FVector2   CurrentJitterNdc;
	uint64     SceneFrameCount = 0;     // RenderSceneColor 호출 번호 (엔티티 이력 연속성 확인)
	struct FMotionHistory
	{
		uint32     Generation = 0;
		uint64     Frame      = 0; // 기록한 SceneFrameCount
		FMatrix4x4 World;
	};
	std::vector<FMotionHistory> MotionHistory; // 엔티티 인덱스 칸

	// 픽셀 아트: 저해상도 렌더용 카메라(여백만큼 넓힌 투영 + 도트 격자 스냅)와 합성 인자
	FCamera BuildPixelArtCamera(const FPixelArtComponent& PixelArt, const FCamera& Camera, const FRenderOutput& Output,
	                            uint32 SourceWidth, uint32 SourceHeight, FPixelArtCompositeParams& OutParams) const;

	FPixelArtObjectSnap                 PixelArtObjectSnap; // 픽셀 아트: 움직인 물체 도트 스냅 (씬 렌더 동안만 적용, 저해상도 톤매핑 결과는 그래프 풀)
	uint32                              AoResolutionDivisor = 2;     // 이번 씬 렌더의 SSAO 해상도 (픽셀 아트 렌더 동안만 1)
	bool                                bAoGridNoise        = false; // 픽셀 아트: SSAO 노이즈를 월드 도트 격자에 고정
	int32                               AoGridOrigin[2]     = {};

	FMeshInstanceList MeshInstances; // 프레임 메시 인스턴스 (모든 패스 공유)
	// LOD 히스테리시스용 엔티티별 이전 LOD (엔티티 인덱스 칸, 세대로 검증 — 렌더러(= 카메라)마다 따로)
	struct FLodHistory
	{
		uint32 Generation = 0;
		uint32 Lod        = ~0u;
	};
	std::vector<FLodHistory> LodHistory;
	FMeshPassBatches  MainBatches;
	FMeshPassBatches  TranslucentBatches; // 반투명/가산 (먼 것부터, PrepareMainBatches가 함께 만든다)
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
		double PrepassDrawCalls = 0.0;
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
