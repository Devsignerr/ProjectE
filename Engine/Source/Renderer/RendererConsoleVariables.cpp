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
	                                      { .ValueNames  = { "none", "normal", "velocity", "depth", "ao", "ssr" },
	                                        .Range       = std::pair(0.0f, 5.0f),
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

	namespace
	{
		uint32 GRenderGraphDumpSerial = 0;
	}
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
