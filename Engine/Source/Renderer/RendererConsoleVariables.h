#pragma once

#include "Core/Console/Console.h"

// 렌더러 콘솔 변수 (전역 — 모든 FSceneRenderer 공통). FSceneRenderer가 Render마다 읽어 반영한다 (ApplyConsoleVariables).
//   예전 명령줄 플래그는 각 변수의 별칭으로 그대로 동작한다 (--no-ssr = r.SSR 0 등), 새 방식은 --cvar r.SSR=0.
//   r.TAA/r.SSAO/r.SSR은 렌더러별 FPostProcessSettings 값과 AND (썸네일처럼 렌더러가 끈 것은 켜지지 않는다).
//   주의: 아래 전역 객체는 엔진 DLL 안에서만 직접 쓴다 (데이터는 DLL 밖으로 내보내지 않음) —
//   에디터/게임 모듈은 FConsoleManager::Get().FindVariable("r.SSR")로 찾는다.
namespace RendererCVars
{
	extern TAutoConsoleVariable<bool>  TemporalAA;       // r.TAA            (--no-taa)
	extern TAutoConsoleVariable<bool>  AmbientOcclusion; // r.SSAO           (--no-ssao)
	extern TAutoConsoleVariable<bool>  Reflections;      // r.SSR            (--no-ssr)
	extern TAutoConsoleVariable<bool>  DepthPrepass;     // r.DepthPrepass   (--no-depth-prepass)
	extern TAutoConsoleVariable<bool>  Occlusion;        // r.Occlusion      (--occlusion)
	extern TAutoConsoleVariable<bool>  SkinCulling;      // r.SkinCulling    (--no-skin-culling)
	extern TAutoConsoleVariable<bool>  ParticleCulling;  // r.ParticleCulling (--no-particle-culling)
	extern TAutoConsoleVariable<bool>  Lod;              // r.LOD            (--no-lod)
	extern TAutoConsoleVariable<int32> ForceLod;         // r.ForceLOD       (--force-lod N)
	extern TAutoConsoleVariable<float> LodHysteresis;    // r.LODHysteresis  (--lod-hysteresis X)
	extern TAutoConsoleVariable<bool>  Jitter;           // r.Jitter         (--jitter)
	extern TAutoConsoleVariable<int32> DebugView;        // r.DebugView      (--debug-view normal|velocity|depth|ao|ssr|rt-reflections|rt-shadows|rt-instances)
	extern TAutoConsoleVariable<bool>  ResourceAutoCollect; // r.ResourceAutoCollect (리소스 자동 수거, Phase 37)
	extern TAutoConsoleVariable<int32> AsyncLoading;     // r.AsyncLoading   (--sync-loading = 0, --async-loading = 1) — FResourceManager가 프레임마다 읽음
	// 텍스처 밉 스트리밍 (Phase 53, Renderer/TextureStreaming.h) — FResourceManager가 프레임마다 읽음
	extern TAutoConsoleVariable<bool>  Streaming;                   // r.Streaming (--no-texture-streaming / --texture-streaming)
	extern TAutoConsoleVariable<int32> StreamingPoolSizeMB;         // r.Streaming.PoolSizeMB (--streaming-pool-mb N, 0 = 자동)
	extern TAutoConsoleVariable<float> StreamingMaxUploadMBPerFrame; // r.Streaming.MaxUploadMBPerFrame
	extern TAutoConsoleVariable<float> StreamingDropDelay;          // r.Streaming.DropDelay (초)
	extern TAutoConsoleVariable<int32> StreamingMipMargin;          // r.Streaming.MipMargin
	extern TAutoConsoleVariable<bool>  StatStreaming;               // stat.Streaming (stat streaming)
	extern TAutoConsoleVariable<float> StreamingLogStats;           // r.Streaming.LogStats (초, 측정용 주기 로그)

	// 렌더 그래프 (Phase 47)
	extern TAutoConsoleVariable<bool> RenderGraphCull;          // r.RenderGraph.Cull          (안 쓰는 패스 제거, 끄면 모두 실행 — 비교용)
	extern TAutoConsoleVariable<bool> RenderGraphAsyncCompute;  // r.RenderGraph.AsyncCompute  (계산 큐 패스를 비동기 계산 큐에서, --no-async-compute)
	extern TAutoConsoleVariable<bool> RenderGraphAsyncFog;      // r.RenderGraph.AsyncFog      (볼류메트릭 안개 주입/적분을 계산 큐 후보로)
	extern TAutoConsoleVariable<bool> RenderGraphAsyncParticles; // r.RenderGraph.AsyncParticles (GPU 파티클 계산을 계산 큐 후보로)
	// TAAU / 동적 해상도 (Phase 48) — 화면 비율을 쓰는 렌더러(FSceneRenderer::bAllowScreenPercentage)만
	extern TAutoConsoleVariable<float> ScreenPercentage;        // r.ScreenPercentage          (--screen-percentage N)
	extern TAutoConsoleVariable<float> UpscaleMipBiasOffset;    // r.Upscale.MipBiasOffset
	extern TAutoConsoleVariable<bool>  DynamicResolution;       // r.DynamicResolution         (--dynamic-resolution)
	extern TAutoConsoleVariable<float> DynamicResolutionTargetMs; // r.DynamicResolution.TargetMs (--dynamic-resolution-target ms)
	extern TAutoConsoleVariable<float> DynamicResolutionMin;    // r.DynamicResolution.MinPercentage
	extern TAutoConsoleVariable<float> DynamicResolutionMax;    // r.DynamicResolution.MaxPercentage
	// 레이 트레이싱 (Phase 50) — 레이 트레이싱을 허용한 렌더러(FSceneRenderer::bAllowRayTracing: 에디터 뷰포트·런타임)만, DXR 1.1 미지원이면 항상 끔.
	//   켬/끔 변수(-1)는 프로젝트 설정 "Rendering"(FRenderingSettings)을 따른다
	extern TAutoConsoleVariable<int32> RayTracing;                  // r.RayTracing                  (--raytracing / --no-raytracing)
	extern TAutoConsoleVariable<int32> RayTracingShadows;           // r.RayTracing.Shadows          (--rt-shadows / --no-rt-shadows)
	extern TAutoConsoleVariable<int32> RayTracingReflections;       // r.RayTracing.Reflections      (--rt-reflections / --no-rt-reflections)
	extern TAutoConsoleVariable<bool>  RayTracingSkinned;           // r.RayTracing.Skinned
	extern TAutoConsoleVariable<float> RayTracingSkinnedDistance;   // r.RayTracing.Skinned.MaxDistance
	extern TAutoConsoleVariable<bool>  RayTracingFoliage;           // r.RayTracing.Foliage
	extern TAutoConsoleVariable<bool>  RayTracingTerrain;           // r.RayTracing.Terrain
	extern TAutoConsoleVariable<bool>  RayTracingCompaction;        // r.RayTracing.Compaction
	extern TAutoConsoleVariable<bool>  RayTracingGraphMaterials;    // r.RayTracing.GraphMaterials
	extern TAutoConsoleVariable<int32> RayTracingMaxBuilds;         // r.RayTracing.MaxBuildsPerFrame
	extern TAutoConsoleVariable<float> RayTracingShadowSunAngle;    // r.RayTracing.Shadows.SunAngle
	extern TAutoConsoleVariable<float> RayTracingShadowBias;        // r.RayTracing.Shadows.NormalBias
	extern TAutoConsoleVariable<float> RayTracingShadowHistory;     // r.RayTracing.Shadows.HistoryWeight
	extern TAutoConsoleVariable<float> RayTracingReflectionRoughness; // r.RayTracing.Reflections.MaxRoughness
	extern TAutoConsoleVariable<int32> RayTracingReflectionLights;  // r.RayTracing.Reflections.MaxLocalLights
	extern TAutoConsoleVariable<bool>  RayTracingReflectionShadows; // r.RayTracing.Reflections.Shadows
	extern TAutoConsoleVariable<int32> RayTracingDebugMode;         // r.RayTracing.DebugMode
	// r.RayTracing.Stats 명령이 불린 횟수 (렌더러마다 바뀌면 다음 프레임 레이 트레이싱 통계를 로그로)
	uint32 GetRayTracingStatsSerial();

	// 하늘·대기·구름·물 (Phase 49)
	extern TAutoConsoleVariable<bool>  SkyAtmosphere;           // r.SkyAtmosphere              (대기 컴포넌트 무시 = 0, 비교용)
	extern TAutoConsoleVariable<int32> SkyAtmosphereIblSamples; // r.SkyAtmosphere.IblSamples   (실시간 IBL 적분 표본 수)
	extern TAutoConsoleVariable<bool>  VolumetricClouds;        // r.VolumetricClouds           (--no-clouds)
	extern TAutoConsoleVariable<int32> VolumetricCloudsDivisor; // r.VolumetricClouds.Divisor   (추적 해상도 = 씬 ÷ 이 값)
	extern TAutoConsoleVariable<int32> VolumetricCloudsSteps;   // r.VolumetricClouds.Steps
	extern TAutoConsoleVariable<bool>  VolumetricCloudsTemporal; // r.VolumetricClouds.Temporal (시간 누적)
	extern TAutoConsoleVariable<bool>  Water;                   // r.Water                      (--no-water)
	extern TAutoConsoleVariable<bool>  WaterScreenReflections;  // r.Water.SSR
	extern TAutoConsoleVariable<int32> HdrOutput;               // r.HDR.Output (0 끔, 1 자동, 2 HDR10, 3 scRGB — --hdr-output)
	extern TAutoConsoleVariable<float> HdrPaperWhite;           // r.HDR.PaperWhite (nits)
	extern TAutoConsoleVariable<float> HdrMaxNits;              // r.HDR.MaxNits (0 = 디스플레이)
	// r.RenderGraph.Dump 명령이 불린 횟수 (렌더러마다 바뀌면 다음 그래프를 로그로 덤프)
	uint32 GetRenderGraphDumpSerial();
} // namespace RendererCVars
