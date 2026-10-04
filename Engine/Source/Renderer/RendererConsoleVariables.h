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
	extern TAutoConsoleVariable<bool>  SkinCache;        // r.SkinCache      (--skin-cache / --no-skin-cache, Renderer/SkinCache.h)
	extern TAutoConsoleVariable<bool>  ParticleCulling;  // r.ParticleCulling (--no-particle-culling)
	extern TAutoConsoleVariable<bool>  Lod;              // r.LOD            (--no-lod)
	extern TAutoConsoleVariable<bool>  SkinnedLod;       // r.LOD.Skinned    (--no-skinned-lod)
	extern TAutoConsoleVariable<float> SkinnedLodScale;  // r.LOD.SkinnedScale
	extern TAutoConsoleVariable<int32> ForceLod;         // r.ForceLOD       (--force-lod N)
	extern TAutoConsoleVariable<float> LodHysteresis;    // r.LODHysteresis  (--lod-hysteresis X)
	extern TAutoConsoleVariable<bool>  TaaReconstruct;   // r.TAA.Reconstruct
	extern TAutoConsoleVariable<float> TaaStaticWeight;  // r.TAA.StaticWeight
	extern TAutoConsoleVariable<float> TaaFlickerReduction; // r.TAA.FlickerReduction
	extern TAutoConsoleVariable<float> LodErrorPixels;   // r.LOD.ErrorPixels
	// 메인/깊이 사전 패스 인스턴스 거리·화면 크기 컬링 (같은 묶음 목록 — 사전 패스와 메인이 같은 집합)
	extern TAutoConsoleVariable<float> MinScreenSize;    // r.MinScreenSize  (경계 구 지름 / 화면 높이가 이보다 작으면 안 그림, 0 = 끔)
	extern TAutoConsoleVariable<float> MaxDrawDistance;  // r.MaxDrawDistance (cm, 0 = 끔)
	// 방향광 그림자 캐시·LOD (Renderer/ShadowCacheMath.h)
	extern TAutoConsoleVariable<bool>  ShadowCache;             // r.Shadow.Cache (--no-shadow-cache)
	extern TAutoConsoleVariable<int32> ShadowCacheStaticFrames; // r.Shadow.Cache.StaticFrames
	extern TAutoConsoleVariable<bool>  SpriteShadows;           // r.Sprite.Shadows (--no-sprite-shadows, 2D 그림자 캐스팅)
	extern TAutoConsoleVariable<bool>  SpriteTranslucentShadows; // r.Sprite.TranslucentShadows (알파 블렌드 2D 그림자 디더, 기본 끔)
	extern TAutoConsoleVariable<float> ShadowCacheQuantize;     // r.Shadow.Cache.Quantize (카메라 이동 중 캐시 재사용 — 캐스케이드 중심 격자)
	extern TAutoConsoleVariable<int32> ShadowCacheQuantizeFirst; // r.Shadow.Cache.QuantizeFirstCascade
	extern TAutoConsoleVariable<float> ShadowLodBias;           // r.Shadow.LodBias
	extern TAutoConsoleVariable<float> ShadowMinCasterTexels;   // r.Shadow.MinCasterTexels
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
	extern TAutoConsoleVariable<bool> RenderThread;             // r.RenderThread (게임 갱신과 명령 기록·제출을 겹침 — RenderThread.h, 런타임만, --render-thread)
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
	extern TAutoConsoleVariable<float> RayTracingSkinnedRefitDistance; // r.RayTracing.Skinned.RefitDistance
	extern TAutoConsoleVariable<int32> RayTracingSkinnedRefitInterval; // r.RayTracing.Skinned.RefitInterval
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
	// RT 앰비언트 오클루전 (RTAO — 근거리 간접 가림, SSAO 대신)
	extern TAutoConsoleVariable<int32> RayTracingAmbientOcclusion;  // r.RayTracing.AO               (-1 자동 = DDGI 활성 프레임, --rtao / --no-rtao)
	extern TAutoConsoleVariable<float> RayTracingAoRadius;          // r.RayTracing.AO.Radius        (--rtao-radius cm)
	extern TAutoConsoleVariable<int32> RayTracingAoRays;            // r.RayTracing.AO.Rays          (--rtao-rays N)
	extern TAutoConsoleVariable<float> RayTracingAoFalloff;         // r.RayTracing.AO.FalloffPower
	extern TAutoConsoleVariable<int32> RayTracingAoDivisor;         // r.RayTracing.AO.ResolutionDivisor (1 씬, 2 반해상도)
	extern TAutoConsoleVariable<float> RayTracingAoIntensity;       // r.RayTracing.AO.Intensity
	extern TAutoConsoleVariable<float> RayTracingAoHistory;         // r.RayTracing.AO.HistoryWeight
	extern TAutoConsoleVariable<int32> RayTracingAoReference;       // r.RayTracing.AO.Reference     (--rtao-reference N: 경로 추적 간접 확산 기준)
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

	// 동적 GI — DDGI 프로브 볼륨 (Phase 51)
	extern TAutoConsoleVariable<bool>  Ddgi;                  // r.DDGI (--no-ddgi)
	extern TAutoConsoleVariable<int32> DdgiProbeBudget;       // r.DDGI.ProbeBudget (프레임당 갱신 프로브 전체 상한)
	extern TAutoConsoleVariable<float> DdgiBounceIntensity;   // r.DDGI.BounceIntensity (다중 반사 배율)
	extern TAutoConsoleVariable<float> DdgiChangeThreshold;   // r.DDGI.ChangeThreshold (급변 판정)
	extern TAutoConsoleVariable<int32> DdgiMaxLocalLights;    // r.DDGI.MaxLocalLights (히트 로컬 라이트 상한)
	extern TAutoConsoleVariable<int32> DdgiShowProbes;        // r.DDGI.ShowProbes (-1 컴포넌트 값, 0~3)
	extern TAutoConsoleVariable<int32> DdgiBoostFrames;       // r.DDGI.LightChangeBoostFrames (조명 변화 가속 프레임)
	extern TAutoConsoleVariable<float> DdgiBoostHysteresis;   // r.DDGI.LightChangeHysteresis (가속 중 히스테리시스 상한)
	extern TAutoConsoleVariable<int32> DdgiSettleFrames;      // r.DDGI.SettleFrames (가속 뒤·이력 처음 정착 프레임)
	extern TAutoConsoleVariable<float> DdgiSettleHysteresis;  // r.DDGI.SettleHysteresis (정착 중 히스테리시스 상한)
	// r.DDGI.Stats 명령이 불린 횟수
	uint32 GetDdgiStatsSerial();
} // namespace RendererCVars
