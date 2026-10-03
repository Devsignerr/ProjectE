#include "Renderer/RendererConsoleVariables.h"

#include "Renderer/RayTracingMath.h"
#include "Renderer/TextureStreamingMath.h"

namespace RendererCVars
{
	TAutoConsoleVariable<bool> TemporalAA("r.TAA", true, "TAA (서브픽셀 지터 + 이력 누적). 픽셀 아트/와이어프레임/여러 뷰 렌더러는 자동으로 꺼짐",
	                                      EConsoleFlags::None, { .CommandLine = { { L"--no-taa", "0" } } });
	TAutoConsoleVariable<bool> AmbientOcclusion("r.SSAO", true, "SSAO (GTAO). 깊이 사전 패스가 있어야 한다", EConsoleFlags::None,
	                                            { .CommandLine = { { L"--no-ssao", "0" } } });
	TAutoConsoleVariable<bool> Reflections("r.SSR", true, "화면 공간 반사. 깊이 사전 패스·시간 이력이 있어야 한다", EConsoleFlags::None,
	                                       { .CommandLine = { { L"--no-ssr", "0" } } });
	TAutoConsoleVariable<bool> DepthPrepass("r.DepthPrepass", true,
	                                        "깊이 사전 패스 (깊이 + 화면 공간 법선 + 움직임 벡터). 끄면 SSAO/SSR/TAA 움직임이 틀어진다 (비교용)",
	                                        EConsoleFlags::None, { .CommandLine = { { L"--no-depth-prepass", "0" } } });
	TAutoConsoleVariable<bool> Occlusion("r.Occlusion", false, "HZB 오클루전 컬링 (메인 패스 정적 메시). 기본 끔 — LOD와 함께면 손해였음 (Phase 26 측정)",
	                                     EConsoleFlags::None, { .CommandLine = { { L"--occlusion", "1" } } });
	TAutoConsoleVariable<bool> SkinCulling("r.SkinCulling", true, "스킨 팔레트 가시성 컬링 (메인 프러스텀 ∪ 그림자 캐스터 볼륨 밖 스킨 메시 생략)",
	                                       EConsoleFlags::None, { .CommandLine = { { L"--no-skin-culling", "0" } } });
	TAutoConsoleVariable<bool> ParticleCulling("r.ParticleCulling", true, "화면 밖 파티클 이미터 컬링 (GPU 이미터는 계산도 미룸)", EConsoleFlags::None,
	                                           { .CommandLine = { { L"--no-particle-culling", "0" } } });
	TAutoConsoleVariable<bool> Lod("r.LOD", true, "메시 LOD (화면 크기 전환). 끄면 항상 LOD0", EConsoleFlags::None,
	                               { .CommandLine = { { L"--no-lod", "0" } } });
	TAutoConsoleVariable<int32> ForceLod("r.ForceLOD", -1, "0 이상이면 모든 정적 메시를 그 LOD로 (-1 = 끔, 확인용)", EConsoleFlags::None,
	                                     { .Range = std::pair(-1.0f, 7.0f), .CommandLine = { { L"--force-lod", "" } } });
	TAutoConsoleVariable<float> LodHysteresis("r.LODHysteresis", 0.1f, "LOD 전환 여유 (임계값 ±비율 띠 안에서는 이전 LOD 유지, 0 = 끔)",
	                                          EConsoleFlags::None, { .Range = std::pair(0.0f, 1.0f), .CommandLine = { { L"--lod-hysteresis", "" } } });
	TAutoConsoleVariable<bool> Jitter("r.Jitter", false, "TAA 없이도 서브픽셀 투영 지터 (확인용 — 혼자 켜면 화면이 떨린다)", EConsoleFlags::None,
	                                  { .CommandLine = { { L"--jitter", "1" } } });
	TAutoConsoleVariable<int32> DebugView("r.DebugView", 0,
	                                      "화면 공간 버퍼 확인 (톤매핑 결과 대신 출력에 그림). mip = 텍스처 밉 스트리밍 상주 밉 색칠 (메시 패스가 고정 PBR 베이스 컬러 기준으로 "
	                                      "빨강 = 필요한 밉이 없음, 초록 = 알맞음, 파랑 = 2밉 이상 여유, 회색 = 텍스처 없음/그래프 머티리얼). rt-* = 레이 트레이싱 반사/그림자 마스크/TLAS 인스턴스, "
	                                      "gi = 간접 확산광만 (메시 패스 — DDGI 볼륨 안은 프로브, 밖은 하늘 IBL)",
	                                      EConsoleFlags::None,
	                                      { .ValueNames  = { "none", "normal", "velocity", "depth", "ao", "ssr", "mip", "rt-reflections", "rt-shadows", "rt-instances", "gi" },
	                                        .Range       = std::pair(0.0f, 10.0f),
	                                        .CommandLine = { { L"--debug-view", "" } } });
	TAutoConsoleVariable<bool> ResourceAutoCollect("r.ResourceAutoCollect", true,
	                                               "맵 전환·서브 씬 내림·에디터 씬 열기 뒤 쓰지 않는 메시/텍스처/머티리얼/모델/파티클 자동 수거 (끄면 비교용으로 쌓임 — r.CollectResources는 계속 동작)");
	TAutoConsoleVariable<int32> AsyncLoading("r.AsyncLoading", -1,
	                                         "리소스 로딩 방식: -1 자동(대화형 = 비동기, 자동 검증 = 비동기 + 프레임마다 비우기), 0 동기, 1 비동기, "
	                                         "2 비동기 + 프레임마다 비우기 (EnableAsyncLoading을 부르지 않은 앱/테스트는 항상 동기)",
	                                         EConsoleFlags::None,
	                                         { .Range = std::pair(-1.0f, 2.0f), .CommandLine = { { L"--sync-loading", "0" }, { L"--async-loading", "1" } } });

	TAutoConsoleVariable<bool> Streaming("r.Streaming", true,
	                                     "텍스처 밉 스트리밍: 머티리얼 텍스처를 화면에 필요한 밉까지만 VRAM에 둔다 (끄면 새 텍스처는 전체 밉, 줄어든 텍스처는 전체로 되돌림). "
	                                     "비동기 로딩을 켠 앱만 (테스트·도구는 항상 전체)",
	                                     EConsoleFlags::None, { .CommandLine = { { L"--no-texture-streaming", "0" }, { L"--texture-streaming", "1" } } });
	TAutoConsoleVariable<int32> StreamingPoolSizeMB("r.Streaming.PoolSizeMB", 0,
	                                                "텍스처 스트리밍 예산(MB): 스트리밍 텍스처 상주 합이 넘으면 우선순위가 낮은 텍스처부터 밉을 줄인다. "
	                                                "0 = 자동 (VRAM 예산의 40%, 256MB~4GB)",
	                                                EConsoleFlags::None, { .Range = std::pair(0.0f, 65536.0f), .CommandLine = { { L"--streaming-pool-mb", "" } } });
	TAutoConsoleVariable<float> StreamingMaxUploadMBPerFrame("r.Streaming.MaxUploadMBPerFrame", 32.0f,
	                                                         "텍스처 스트리밍 프레임당 새 요청 상한(MB, 비동기만 — 끊김 방지). 요청 하나는 상한보다 커도 보낸다",
	                                                         EConsoleFlags::None, { .Range = std::pair(1.0f, 1024.0f) });
	TAutoConsoleVariable<float> StreamingDropDelay("r.Streaming.DropDelay", 2.0f,
	                                               "텍스처 스트리밍: 덜 세밀한 밉으로 내리기 전 기다리는 시간(초). 예산 초과면 기다리지 않는다",
	                                               EConsoleFlags::None, { .Range = std::pair(0.0f, 60.0f) });
	TAutoConsoleVariable<int32> StreamingMipMargin("r.Streaming.MipMargin", TextureStreamingMath::DefaultMipMargin,
	                                               "텍스처 스트리밍 필요 밉 여유(밉 수): 계산한 필요 밉보다 이만큼 더 세밀하게 둔다 (이방성·UV 밀도 분포)",
	                                               EConsoleFlags::None, { .Range = std::pair(0.0f, 4.0f) });
	TAutoConsoleVariable<bool> StatStreaming("stat.Streaming", false, "화면 통계: 텍스처 밉 스트리밍 (stat streaming으로 켜고 끔)");
	TAutoConsoleVariable<float> StreamingLogStats("r.Streaming.LogStats", 0.0f,
	                                              "텍스처 스트리밍 + 리소스 메모리 통계를 이 간격(초)마다 로그로 (측정용, 0 = 끔)", EConsoleFlags::None,
	                                              { .Range = std::pair(0.0f, 3600.0f) });

	TAutoConsoleVariable<bool> RenderGraphCull("r.RenderGraph.Cull", true, "렌더 그래프: 결과를 아무도 읽지 않는 패스 제거 (끄면 모두 실행 — 비교용)");
	// 기본 끔 (Phase 47 측정, RTX 3060 Laptop): 데모 씬에서는 프레임당 큐 제출이 늘어 CPU +0.3~0.5ms, Demo_Showcase GPU 프레임 +0.15ms(손해),
	// GPU 입자 15만 개 장면에서만 GPU 프레임 -1% 안팎 — 계산 작업이 작아 겹쳐도 이득이 제출 비용보다 작다
	TAutoConsoleVariable<bool> RenderGraphAsyncCompute("r.RenderGraph.AsyncCompute", false,
	                                                   "렌더 그래프: 계산 큐 후보 패스(볼류메트릭 안개, GPU 파티클)를 비동기 계산 큐에서 그래픽스와 겹쳐 실행 "
	                                                   "(끄면 그래픽스 큐에서 순서대로). 기본 끔 — 측정에서 이득이 제출 비용보다 작았다",
	                                                   EConsoleFlags::None, { .CommandLine = { { L"--no-async-compute", "0" }, { L"--async-compute", "1" } } });
	TAutoConsoleVariable<bool> RenderGraphAsyncFog("r.RenderGraph.AsyncFog", true, "볼류메트릭 안개 주입/적분을 비동기 계산 큐 후보로 (r.RenderGraph.AsyncCompute와 AND)");
	TAutoConsoleVariable<bool> RenderGraphAsyncParticles("r.RenderGraph.AsyncParticles", true,
	                                                     "GPU 파티클 계산을 비동기 계산 큐 후보로 (r.RenderGraph.AsyncCompute와 AND)");

	TAutoConsoleVariable<float> ScreenPercentage("r.ScreenPercentage", 100.0f,
	                                             "화면 비율(%): 씬을 출력 × 비율 해상도로 그리고 TAA 단계에서 출력 해상도로 시간 업샘플(TAAU). 100 = 네이티브. "
	                                             "프리셋 품질 77 / 균형 67 / 성능 50. 픽셀 아트·미리보기 렌더러에는 적용 안 됨",
	                                             EConsoleFlags::None, { .Range = std::pair(25.0f, 100.0f), .CommandLine = { { L"--screen-percentage", "" } } });
	TAutoConsoleVariable<float> UpscaleMipBiasOffset("r.Upscale.MipBiasOffset", -0.3f,
	                                                 "TAAU 텍스처 밉 바이어스 보정: 바이어스 = log2(내부/출력) + 이 값 (음수 = 더 선명, 지글거리면 0 쪽으로). 네이티브는 항상 0");
	TAutoConsoleVariable<bool> DynamicResolution("r.DynamicResolution", false,
	                                             "동적 해상도: GPU 씬 렌더 시간이 목표(r.DynamicResolution.TargetMs)에 맞게 화면 비율을 Min~Max 사이 5% 단계로 조절 "
	                                             "(r.ScreenPercentage 대신). 기본 끔",
	                                             EConsoleFlags::None, { .CommandLine = { { L"--dynamic-resolution", "1" } } });
	TAutoConsoleVariable<float> DynamicResolutionTargetMs("r.DynamicResolution.TargetMs", 16.6f, "동적 해상도 목표 GPU 씬 렌더 시간(ms)", EConsoleFlags::None,
	                                                      { .Range = std::pair(1.0f, 100.0f), .CommandLine = { { L"--dynamic-resolution-target", "" } } });
	TAutoConsoleVariable<float> DynamicResolutionMin("r.DynamicResolution.MinPercentage", 50.0f, "동적 해상도 최소 화면 비율(%)", EConsoleFlags::None,
	                                                 { .Range = std::pair(25.0f, 100.0f) });
	TAutoConsoleVariable<float> DynamicResolutionMax("r.DynamicResolution.MaxPercentage", 100.0f, "동적 해상도 최대 화면 비율(%)", EConsoleFlags::None,
	                                                 { .Range = std::pair(25.0f, 100.0f) });

	// ---- 레이 트레이싱 (Phase 50)
	TAutoConsoleVariable<int32> RayTracing("r.RayTracing", -1,
	                                       "레이 트레이싱 전체 (-1 프로젝트 설정 Rendering, 0 끔, 1 켬). DXR 1.1(인라인 RayQuery) 미지원 GPU는 항상 끔. "
	                                       "에디터 뷰포트·런타임 렌더러만 (미리보기·썸네일 제외)",
	                                       EConsoleFlags::None,
	                                       { .Range = std::pair(-1.0f, 1.0f), .CommandLine = { { L"--raytracing", "1" }, { L"--no-raytracing", "0" } } });
	TAutoConsoleVariable<int32> RayTracingShadows("r.RayTracing.Shadows", -1,
	                                              "RT 방향광 그림자 (-1 프로젝트 설정, 0 섀도맵, 1 RT): 불투명 표면의 방향광 그림자를 화면 픽셀마다 광선 하나 + "
	                                              "교차 표본 공간 필터 + 시간 누적으로. 반투명·볼류메트릭 안개는 계속 섀도맵",
	                                              EConsoleFlags::None,
	                                              { .Range = std::pair(-1.0f, 1.0f), .CommandLine = { { L"--rt-shadows", "1" }, { L"--no-rt-shadows", "0" } } });
	TAutoConsoleVariable<int32> RayTracingReflections("r.RayTracing.Reflections", -1,
	                                                  "RT 반사 (-1 프로젝트 설정, 0 SSR, 1 RT): SSR 추적 대신 TLAS로 거울 방향 광선 하나 → 히트 머티리얼·조명. "
	                                                  "흐림/누적은 SSR과 같은 경로. 거칠기 한계 위는 캡처/하늘",
	                                                  EConsoleFlags::None,
	                                                  { .Range       = std::pair(-1.0f, 1.0f),
	                                                    .CommandLine = { { L"--rt-reflections", "1" }, { L"--no-rt-reflections", "0" } } });
	TAutoConsoleVariable<bool> RayTracingSkinned("r.RayTracing.Skinned", true,
	                                             "스킨 메시를 TLAS에 (계산 셰이더 스키닝 + BLAS 갱신, 프레임마다 — 비용은 r.RayTracing.Stats)");
	TAutoConsoleVariable<float> RayTracingSkinnedDistance("r.RayTracing.Skinned.MaxDistance", 5000.0f,
	                                                      "이 거리(cm)보다 먼 스킨 메시는 TLAS에서 뺀다 (갱신 비용 상한)", EConsoleFlags::None,
	                                                      { .Range = std::pair(0.0f, 100000.0f) });
	TAutoConsoleVariable<bool> RayTracingFoliage("r.RayTracing.Foliage", true, "폴리지 인스턴스를 TLAS에 (끄면 RT 그림자/반사에 풀·나무 없음)");
	TAutoConsoleVariable<bool> RayTracingTerrain("r.RayTracing.Terrain", true,
	                                             "지형을 TLAS에 (높이장 타일 BLAS — 간격은 정점 13만 개 상한, 편집된 타일만 다시 빌드, 히트 표면은 레이어 0 근사)");
	TAutoConsoleVariable<bool> RayTracingCompaction("r.RayTracing.Compaction", true, "정적 BLAS 압축 (빌드 몇 프레임 뒤 압축 크기로 복사 — 메모리 절약)");
	TAutoConsoleVariable<bool> RayTracingGraphMaterials("r.RayTracing.GraphMaterials", true,
	                                                  "그래프 머티리얼 히트를 생성 함수로 평가 (씬의 그래프 셰이더 집합마다 RT 셰이더 변형 컴파일 — 끄면 회색 근사)");
	TAutoConsoleVariable<int32> RayTracingMaxBuilds("r.RayTracing.MaxBuildsPerFrame", 32,
	                                                "프레임당 새 BLAS 빌드 상한 (씬 로드 끊김 방지 — 넘친 메시는 다음 프레임부터 RT에 보임)", EConsoleFlags::None,
	                                                { .Range = std::pair(1.0f, 4096.0f) });
	TAutoConsoleVariable<float> RayTracingShadowSunAngle("r.RayTracing.Shadows.SunAngle", 0.5f,
	                                                     "RT 그림자 태양 원반 지름(도): 반그림자 폭. 0 = 단단한 그림자 (실제 태양 ≈ 0.53)", EConsoleFlags::None,
	                                                     { .Range = std::pair(0.0f, 10.0f) });
	TAutoConsoleVariable<float> RayTracingShadowBias("r.RayTracing.Shadows.NormalBias", 1.0f,
	                                                 "RT 그림자/반사 1차 표면 법선 바이어스 배율 (여드름이 보이면 키운다, 접촉 그림자가 떠 보이면 줄인다)",
	                                                 EConsoleFlags::None, { .Range = std::pair(0.0f, 10.0f) });
	TAutoConsoleVariable<float> RayTracingShadowHistory("r.RayTracing.Shadows.HistoryWeight", 0.2f, "RT 그림자 시간 누적: 이번 프레임 비중 (1 = 누적 없음)",
	                                                    EConsoleFlags::None, { .Range = std::pair(0.01f, 1.0f) });
	TAutoConsoleVariable<float> RayTracingReflectionRoughness("r.RayTracing.Reflections.MaxRoughness", -1.0f,
	                                                          "RT 반사 거칠기 한계 (-1 = 후처리 설정 SsrMaxRoughness와 같게). 이보다 거친 면은 캡처/하늘",
	                                                          EConsoleFlags::None, { .Range = std::pair(-1.0f, 1.0f) });
	TAutoConsoleVariable<int32> RayTracingReflectionLights("r.RayTracing.Reflections.MaxLocalLights", 16,
	                                                       "RT 반사 히트가 계산하는 로컬 라이트 상한 (카메라 가까운 순, 그림자 없음 — 비용 상한)", EConsoleFlags::None,
	                                                       { .Range = std::pair(0.0f, 256.0f) });
	TAutoConsoleVariable<bool> RayTracingReflectionShadows("r.RayTracing.Reflections.Shadows", true, "RT 반사 히트의 방향광 그림자 광선 (끄면 반사 속 그림자 없음)");
	TAutoConsoleVariable<int32> RayTracingDebugMode("r.RayTracing.DebugMode", 0, "--debug-view rt-instances 내용: 0 인스턴스 색, 1 히트 알베도, 2 히트 조명",
	                                                EConsoleFlags::None, { .Range = std::pair(0.0f, 2.0f) });

	TAutoConsoleVariable<bool> SkyAtmosphere("r.SkyAtmosphere", true, "물리 기반 대기 (SkyAtmosphereComponent): 0이면 컴포넌트가 있어도 예전 하늘(하늘광 환경맵/절차적)로 그린다 (비교용)");
	TAutoConsoleVariable<int32> SkyAtmosphereIblSamples("r.SkyAtmosphere.IblSamples", 64,
	                                                    "대기 실시간 IBL 적분 표본 수 (조도/프리필터, 필터드 중요도 샘플링 — 굽기 IBL은 256)", EConsoleFlags::None,
	                                                    { .Range = std::pair(8.0f, 1024.0f) });
	TAutoConsoleVariable<bool> VolumetricClouds("r.VolumetricClouds", true, "볼류메트릭 구름 (VolumetricCloudComponent)", EConsoleFlags::None,
	                                            { .CommandLine = { { L"--no-clouds", "0" } } });
	TAutoConsoleVariable<int32> VolumetricCloudsDivisor("r.VolumetricClouds.Divisor", 4,
	                                                    "구름 추적 해상도 = 씬(내부) 해상도 ÷ 이 값 (가로·세로 각각). 시간 누적 후 업샘플 합성", EConsoleFlags::None,
	                                                    { .Range = std::pair(1.0f, 8.0f) });
	TAutoConsoleVariable<int32> VolumetricCloudsSteps("r.VolumetricClouds.Steps", 48, "구름 레이마칭 최대 단계 수", EConsoleFlags::None,
	                                                  { .Range = std::pair(8.0f, 256.0f) });
	TAutoConsoleVariable<bool> VolumetricCloudsTemporal("r.VolumetricClouds.Temporal", true, "구름 시간 누적 (끄면 매 프레임 지터 없이 추적 — 비교용)");
	TAutoConsoleVariable<bool> Water("r.Water", true, "소규모 물 (WaterBodyComponent) 패스", EConsoleFlags::None, { .CommandLine = { { L"--no-water", "0" } } });
	TAutoConsoleVariable<bool> WaterScreenReflections("r.Water.SSR", true, "물 반사: 화면 공간 추적 (끄면 반사 캡처/하늘만)");
	TAutoConsoleVariable<int32> HdrOutput("r.HDR.Output", 0,
	                                      "HDR 디스플레이 출력: 0 끔(SDR), 1 자동(디스플레이가 HDR이면 HDR10), 2 HDR10(PQ/BT.2020), 3 scRGB(FP16). "
	                                      "지원하지 않으면 SDR 유지. 앱(런타임·에디터)이 프레임마다 반영",
	                                      EConsoleFlags::None,
	                                      { .ValueNames = { "off", "auto", "hdr10", "scrgb" }, .Range = std::pair(0.0f, 3.0f), .CommandLine = { { L"--hdr-output", "" } } });
	TAutoConsoleVariable<float> HdrPaperWhite("r.HDR.PaperWhite", 200.0f, "HDR 출력 종이 흰색(SDR 흰색·UI) 밝기 nits", EConsoleFlags::None,
	                                          { .Range = std::pair(80.0f, 1000.0f) });
	TAutoConsoleVariable<float> HdrMaxNits("r.HDR.MaxNits", 0.0f, "HDR 출력 최대 밝기 nits (톤매핑 하이라이트 상한, 0 = 디스플레이 값)", EConsoleFlags::None,
	                                       { .Range = std::pair(0.0f, 10000.0f) });
	namespace
	{
		uint32 GRenderGraphDumpSerial = 0;
		uint32 GRayTracingStatsSerial = 0;
	}
	uint32 GetRayTracingStatsSerial()
	{
		return GRayTracingStatsSerial;
	}
	FAutoConsoleCommand RayTracingStats("r.RayTracing.Stats", "다음 프레임 레이 트레이싱 통계(BLAS 수·메모리, TLAS 인스턴스, 빌드/갱신 수, GPU 시간)를 로그로",
	                                    [](const std::vector<std::string>&, const FConsoleOutput& Output) {
		                                    ++GRayTracingStatsSerial;
		                                    Output.Print("다음 프레임 레이 트레이싱 통계를 로그로 남깁니다");
	                                    });
	uint32 GetRenderGraphDumpSerial()
	{
		return GRenderGraphDumpSerial;
	}
	// ---- 동적 GI — DDGI (Phase 51)
	TAutoConsoleVariable<bool> Ddgi("r.DDGI", true,
	                                "동적 GI (IrradianceVolumeComponent 프로브 볼륨을 레이 트레이싱으로 갱신 → 간접 확산광). 볼륨이 있는 씬만, "
	                                "RT가 켜진 렌더러만 (r.RayTracing). 끄면 하늘 IBL 조도 (비교용)",
	                                EConsoleFlags::None, { .CommandLine = { { L"--no-ddgi", "0" }, { L"--ddgi", "1" } } });
	TAutoConsoleVariable<int32> DdgiProbeBudget("r.DDGI.ProbeBudget", 1024,
	                                            "DDGI 프레임당 갱신 프로브 전체 상한 (볼륨 프로브 수 비율로 나눔, 0 = 무제한). 줄이면 싸지만 조명 변화를 늦게 따라감. "
	                                            "기본 1024 (1440p 측정: 6720 프로브 Demo_Apartment 추적 0.26 + 누적 0.34ms, 7프레임에 한 바퀴)",
	                                            EConsoleFlags::None, { .Range = std::pair(0.0f, 16384.0f), .CommandLine = { { L"--ddgi-budget", "" } } });
	TAutoConsoleVariable<float> DdgiBounceIntensity("r.DDGI.BounceIntensity", 1.0f,
	                                                "DDGI 다중 반사: 프로브 광선 히트의 간접 확산(이전 프레임 프로브 조도) 배율 (0 = 한 번 반사만)", EConsoleFlags::None,
	                                                { .Range = std::pair(0.0f, 2.0f) });
	TAutoConsoleVariable<float> DdgiChangeThreshold("r.DDGI.ChangeThreshold", 1.0f,
	                                                "DDGI 텍셀 급변 감지: 새 값이 이전·새 값 크기의 이 배 넘게 바뀌면 그 텍셀만 히스테리시스를 낮춘다. 1 이상 = 끔(기본). "
	                                                "프로브 광선 잡음(작은 밝은 면을 드물게 맞힘)에 걸려 프로브 단위로 깜빡였다 — 조명 변화는 결정적인 "
	                                                "r.DDGI.LightChangeBoostFrames(방향광·하늘 변화 감지)가 맡는다",
	                                                EConsoleFlags::None, { .Range = std::pair(0.0f, 10.0f) });
	TAutoConsoleVariable<int32> DdgiMaxLocalLights("r.DDGI.MaxLocalLights", 16, "DDGI 프로브 광선 히트가 계산하는 로컬 라이트 상한 (카메라 가까운 순, 그림자 없음)",
	                                               EConsoleFlags::None, { .Range = std::pair(0.0f, 256.0f) });
	TAutoConsoleVariable<int32> DdgiShowProbes("r.DDGI.ShowProbes", -1,
	                                           "DDGI 프로브 구 표시: -1 컴포넌트 DebugProbes 값, 0 끔, 1 조도, 2 거리, 3 상태 (초록 활성 / 빨강 벽 속 / 파랑 아직 없음)",
	                                           EConsoleFlags::None, { .Range = std::pair(-1.0f, 3.0f), .CommandLine = { { L"--ddgi-probes", "" } } });
	TAutoConsoleVariable<int32> DdgiBoostFrames("r.DDGI.LightChangeBoostFrames", 30,
	                                            "DDGI 조명 변화 가속: 방향광(2도·10%)·하늘 배율(10%)이 크게 바뀌면 이 프레임 수 동안 히스테리시스를 낮춘다 "
	                                            "(다중 반사 수렴이 느린 실내용, 0 = 끔)",
	                                            EConsoleFlags::None, { .Range = std::pair(0.0f, 600.0f) });
	TAutoConsoleVariable<float> DdgiBoostHysteresis("r.DDGI.LightChangeHysteresis", 0.7f, "DDGI 조명 변화 가속 중 히스테리시스 상한 (작을수록 빠르고 잡음)",
	                                                EConsoleFlags::None, { .Range = std::pair(0.0f, 0.995f) });
	TAutoConsoleVariable<int32> DdgiSettleFrames("r.DDGI.SettleFrames", 300,
	                                             "DDGI 정착 구간: 조명 변화 가속이 끝난 뒤와 이력을 처음 채울 때 이 프레임 수 동안 히스테리시스 상한을 "
	                                             "r.DDGI.SettleHysteresis로 (볼륨 Hysteresis를 높여 광선 잡음을 줄여도 다중 반사가 빨리 차게, 0 = 끔)",
	                                             EConsoleFlags::None, { .Range = std::pair(0.0f, 6000.0f) });
	TAutoConsoleVariable<float> DdgiSettleHysteresis("r.DDGI.SettleHysteresis", 0.97f, "DDGI 정착 구간 히스테리시스 상한 (볼륨 값이 이보다 낮으면 영향 없음)",
	                                                 EConsoleFlags::None, { .Range = std::pair(0.0f, 0.995f) });
	// ---- RT 앰비언트 오클루전 (RTAO — 근거리 간접 가림, Demo_GI 그림자 품질)
	TAutoConsoleVariable<int32> RayTracingAmbientOcclusion("r.RayTracing.AO", -1,
	                                                       "RT 앰비언트 오클루전 (SSAO 대신 TLAS 짧은 광선 — 간접광에만 곱함): -1 자동(DDGI 볼륨이 활성인 프레임만), "
	                                                       "0 끔(SSAO), 1 켬(RT가 켜진 렌더러 항상)",
	                                                       EConsoleFlags::None,
	                                                       { .Range = std::pair(-1.0f, 1.0f), .CommandLine = { { L"--rtao", "1" }, { L"--no-rtao", "0" } } });
	TAutoConsoleVariable<float> RayTracingAoRadius("r.RayTracing.AO.Radius", RayTracingMath::DefaultAoRadius,
	                                               "RTAO 광선 길이 = 가림 반경(cm). DDGI 프로브 간격(보통 100cm)보다 작은 가림을 담당 — 간격의 1~2배 (경로 추적 기준과 비교해 정함)", EConsoleFlags::None,
	                                               { .Range = std::pair(1.0f, 1000.0f), .CommandLine = { { L"--rtao-radius", "" } } });
	TAutoConsoleVariable<int32> RayTracingAoRays("r.RayTracing.AO.Rays", static_cast<int32>(RayTracingMath::DefaultAoRaysPerPixel),
	                                             "RTAO 픽셀당 광선 수 (4x4 교차 표본 × 이 수 = 방향 수)", EConsoleFlags::None,
	                                             { .Range = std::pair(1.0f, 4.0f), .CommandLine = { { L"--rtao-rays", "" } } });
	TAutoConsoleVariable<float> RayTracingAoFalloff("r.RayTracing.AO.FalloffPower", RayTracingMath::DefaultAoFalloffPower,
	                                                "RTAO 거리 감쇠 지수: 가림 = (1 - 거리/반경)^지수 (클수록 접촉부만)", EConsoleFlags::None,
	                                                { .Range = std::pair(0.1f, 8.0f) });
	TAutoConsoleVariable<int32> RayTracingAoDivisor("r.RayTracing.AO.ResolutionDivisor", 2,
	                                                "RTAO 해상도 나눔: 1 = 씬 해상도, 2 = 반해상도(기본 — 1440p 비용 약 1/4, 메인 패스가 깊이 가중 업샘플. 1이 정지 화면에서 더 안정)",
	                                                EConsoleFlags::None, { .Range = std::pair(1.0f, 2.0f) });
	TAutoConsoleVariable<float> RayTracingAoIntensity("r.RayTracing.AO.Intensity", 1.0f, "RTAO 세기: 가시도^세기", EConsoleFlags::None,
	                                                  { .Range = std::pair(0.0f, 4.0f) });
	TAutoConsoleVariable<float> RayTracingAoHistory("r.RayTracing.AO.HistoryWeight", 0.1f, "RTAO 시간 누적: 이번 프레임 비중 (1 = 누적 없음, 기본 0.1 — 반해상도 가림 경계의 정지 화면 출렁임 억제)",
	                                                EConsoleFlags::None, { .Range = std::pair(0.01f, 1.0f) });
	TAutoConsoleVariable<int32> RayTracingAoReference("r.RayTracing.AO.Reference", 0,
	                                                  "RTAO 고비용 기준 (비교·튜닝용, 정지 카메라): 픽셀당 이 수의 경로(4번 반사, 그림자 광선, 마지막 정점만 DDGI)를 추적해 프레임마다 평균 → "
	                                                  "메인 패스 간접 확산 = 모은 값 (DDGI × 밝기 비). 0 = 끔",
	                                                  EConsoleFlags::None, { .Range = std::pair(0.0f, 256.0f), .CommandLine = { { L"--rtao-reference", "" } } });
	namespace
	{
		uint32 GDdgiStatsSerial = 0;
	}
	uint32 GetDdgiStatsSerial()
	{
		return GDdgiStatsSerial;
	}
	FAutoConsoleCommand DdgiStats("r.DDGI.Stats", "다음 프레임 DDGI 통계(볼륨·프로브·갱신 수, 광선, 아틀라스 메모리, GPU 시간)를 로그로",
	                              [](const std::vector<std::string>&, const FConsoleOutput& Output) {
		                              ++GDdgiStatsSerial;
		                              Output.Print("다음 프레임 DDGI 통계를 로그로 남깁니다");
	                              });

	FAutoConsoleCommand RenderGraphDump("r.RenderGraph.Dump", "다음 프레임 각 씬 렌더러의 렌더 그래프(패스 순서·큐·제거된 패스·전이 수·포크/조인·리소스 수명)를 로그로",
	                                    [](const std::vector<std::string>&, const FConsoleOutput& Output) {
		                                    ++GRenderGraphDumpSerial;
		                                    Output.Print("다음 프레임 렌더 그래프를 로그로 덤프합니다");
	                                    });
} // namespace RendererCVars
