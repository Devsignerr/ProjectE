#include "Renderer/RendererConsoleVariables.h"

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
	TAutoConsoleVariable<int32> DebugView("r.DebugView", 0, "화면 공간 버퍼 확인 (톤매핑 결과 대신 출력에 그림)", EConsoleFlags::None,
	                                      { .ValueNames  = { "none", "normal", "velocity", "depth", "ao", "ssr", "rt-reflections", "rt-shadows", "rt-instances" },
	                                        .Range       = std::pair(0.0f, 8.0f),
	                                        .CommandLine = { { L"--debug-view", "" } } });
	TAutoConsoleVariable<bool> ResourceAutoCollect("r.ResourceAutoCollect", true,
	                                               "맵 전환·서브 씬 내림·에디터 씬 열기 뒤 쓰지 않는 메시/텍스처/머티리얼/모델/파티클 자동 수거 (끄면 비교용으로 쌓임 — r.CollectResources는 계속 동작)");
	TAutoConsoleVariable<int32> AsyncLoading("r.AsyncLoading", -1,
	                                         "리소스 로딩 방식: -1 자동(대화형 = 비동기, 자동 검증 = 비동기 + 프레임마다 비우기), 0 동기, 1 비동기, "
	                                         "2 비동기 + 프레임마다 비우기 (EnableAsyncLoading을 부르지 않은 앱/테스트는 항상 동기)",
	                                         EConsoleFlags::None,
	                                         { .Range = std::pair(-1.0f, 2.0f), .CommandLine = { { L"--sync-loading", "0" }, { L"--async-loading", "1" } } });

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
	FAutoConsoleCommand RenderGraphDump("r.RenderGraph.Dump", "다음 프레임 각 씬 렌더러의 렌더 그래프(패스 순서·큐·제거된 패스·전이 수·포크/조인·리소스 수명)를 로그로",
	                                    [](const std::vector<std::string>&, const FConsoleOutput& Output) {
		                                    ++GRenderGraphDumpSerial;
		                                    Output.Print("다음 프레임 렌더 그래프를 로그로 덤프합니다");
	                                    });
} // namespace RendererCVars
