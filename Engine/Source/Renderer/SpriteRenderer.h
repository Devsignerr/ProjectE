#pragma once

#include "RHI/D3D12/D3D12PipelineState.h"
#include "Renderer/ShaderTypes.h"
#include "Renderer/SpriteDraw.h"
#include "Scene/ResourceHandles.h"

#include <span>
#include <vector>

class FCamera;
class FD3D12RHI;
class FResourceManager;
class FShaderLibrary;

// 2D 스프라이트 렌더러 코어 (Phase 56-4a/b). 컴포넌트를 모른다 — Prepare가 받은 항목(FSpriteDrawItem: 씬 수집 FSpriteSceneCollector + 앱 목록
// SetSpriteDrawList + r.Sprite.Benchmark)과 타일맵 청크(FSpriteChunkDraw — 정적 인스턴스 버퍼, SpriteTiles.h)만 그린다.
//
// 패스 위치 (FSceneRenderer::RenderSceneColor): 메인 불투명 → 안개 적용 → 물 수면 → [스프라이트] → 반투명 메시 → 파티클.
//   씬 컬러는 내부 해상도(TAAU면 출력 × 비율, 픽셀 아트면 저해상도)이고 투영 지터를 받는다 — 씬 컬러에 그리는 패스 규칙 그대로.
//   반투명 메시와는 서로 정렬하지 않는다: 스프라이트 패스 전체가 반투명 메시 앞(먼저)이다. 2D 게임은 스프라이트끼리의 레이어 순서가
//   중요하고 반투명 3D 메시와 겹치는 일은 드물며, Masked 스프라이트가 쓴 깊이에 반투명 메시·파티클이 가려지려면 먼저 그려야 한다.
// 깊이: 테스트 켬(LESS_EQUAL — 3D 불투명 물체가 가린다). 쓰기는 Masked만 (Alpha/Premultiplied/Additive는 쓰지 않음).
// 출력 알파 = 덮인 정도 (TAA 반응형 마스크 — 반투명 메시·파티클과 같음, Masked = 1: 움직임 벡터를 쓰지 않으므로 이력 의존을 줄인다).
// 정렬 (SpriteSorting::Sort): 레이어 → 레이어 안 순번 → 카메라 깊이(먼 것 먼저) → 제출 순서. 안정·결정적. 제출 순서 = 청크 → 씬 항목
//   (애니메이션 타일 → 스프라이트) → 앱 목록 → 시험이라 키가 모두 같으면 타일이 스프라이트 아래.
// 배치: 정렬된 순서에서 (블렌드, 조명) = 파이프라인이 같은 연속 구간마다 인스턴스 그리기 한 번 (정점 버퍼 없이 정점 6개 × 인스턴스).
//   텍스처·필터·색·UV·컷오프는 인스턴스 데이터(FSpriteInstanceGpu, 동적 업로드 버퍼 — 게임 스레드 Prepare에서 올림)라 묶음을 끊지 않는다.
//   텍스처 = 셰이더 가시 힙 칸 번호(바인드리스, 메시 루트 공간 3 표). 칸 번호는 매 Prepare에 ResolveTexture로 다시 구한다
//   (밉 스트리밍 SwapContents로 바뀔 수 있으므로 프레임을 넘겨 캐시하지 않는다).
// 바인딩: 메시 루트 시그니처를 그대로 쓴다 (FSceneRenderer::BindMeshPassRoot + 안개 b6/t23 — 반투명 메시와 같은 조명 바인딩) +
//   b0 루트 상수 = 구간 시작, t13 = 스프라이트 인스턴스 버퍼. 정적 샘플러는 메시 루트 것(선형 클램프 s1), Point는 셰이더 Load(밉 0).
// 타일맵 청크 구간: 청크마다 구간 하나 (정렬 키 = 타일맵 레이어/순번 + 청크 경계 가운데 깊이 — 항목과 섞여 정렬되지만 합쳐지지 않음).
//   b0 = SpriteChunkRunBit(최상위 비트), t13 = 청크 정적 버퍼(타일맵 로컬 인스턴스), t14 = 청크 머리 FSpriteChunkGpu(월드 0/2/3행 + 색 + 텍스처 칸,
//   동적 업로드 버퍼 — 매 Prepare에 텍스처 칸을 다시 구한다). 셰이더가 로컬 → 월드 변환·색 곱·텍스처 칸 교체 (Sprite.hlsl SpriteVS).
//   청크 버퍼는 COMMON 상태 정적 버퍼(정점 버퍼처럼 암시적 승격 — 그래프 선언 대상 아님), 수명은 수집기가 지연 해제로 관리.
// 조명 (bLit): 반투명 메시 EvaluateMeshLighting과 같은 식 — 방향광 + 캐스케이드 섀도맵, 클러스터 로컬 라이트(그림자 포함), 하늘 IBL/DDGI.
//   표면 = 거칠기 1 유전체, 법선 = 사각형 앞(로컬 +Y)을 카메라 쪽으로 (양면). 화면 버퍼(SSAO/SSR/데칼)는 쓰지 않는다. 안개는 EvaluateFog.
// 그림자 캐스팅(bCastShadows)은 후속.
// 렌더 스레드: Prepare(게임 스레드, BeginRender 안)가 목록 사본으로 정렬·업로드를 끝내고, 패스 람다는 FPreparedFrame 값만 쓴다.
class FSpriteRenderer
{
public:
	~FSpriteRenderer();

	// MeshRootSignature: FSceneRenderer 메시 루트 시그니처 (수명은 씬 렌더러)
	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources, ID3D12RootSignature* InMeshRootSignature,
	          DXGI_FORMAT InColorFormat, DXGI_FORMAT InDepthFormat);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 그리기 목록 (다음 Set/Clear까지 유지 — 프레임마다 다시 넘기는 것이 보통)
	void SetDrawList(std::vector<FSpriteDrawItem> Items) { DrawList = std::move(Items); }
	void ClearDrawList() { DrawList.clear(); }
	const std::vector<FSpriteDrawItem>& GetDrawList() const { return DrawList; }

	struct FPreparedRun
	{
		ID3D12PipelineState*      Pipeline    = nullptr;
		uint32                    First       = 0; // b0 루트 상수: 항목 구간 = 업로드 버퍼 안 시작 / 청크 구간 = SpriteChunkRunBit
		uint32                    Count       = 0;
		D3D12_GPU_VIRTUAL_ADDRESS ChunkBuffer = 0; // 청크 구간: t13 = 청크 정적 버퍼 (0 = 항목 구간 — 프레임 업로드 버퍼)
		D3D12_GPU_VIRTUAL_ADDRESS ChunkHeader = 0; // 청크 구간: t14 = FSpriteChunkGpu (동적 업로드 버퍼)
	};
	struct FPreparedFrame
	{
		D3D12_GPU_VIRTUAL_ADDRESS Instances = 0; // 동적 업로드 버퍼 (항목 인스턴스, 그리기 순서)
		std::vector<FPreparedRun> Runs;
		uint32                    SpriteCount = 0; // 항목 수 (스프라이트 + 애니메이션 타일 + 앱 목록 + 시험)
		uint32                    ChunkCount  = 0; // 청크 구간 수
		uint32                    TileCount   = 0; // 청크 인스턴스 합
		bool IsEmpty() const { return Runs.empty(); }
	};
	// 게임 스레드: 씬 항목(SceneItems — FSpriteSceneCollector) + 앱 목록(SetDrawList) + r.Sprite.Benchmark 시험 스프라이트 + 타일맵 청크 → 정렬 → 인스턴스 업로드 → 구간.
	// 정렬 키의 제출 순서 = 청크 → 씬 항목 → 앱 목록 → 시험 (완전히 같은 키면 이 순서로 그린다 — 타일맵이 같은 레이어 스프라이트 아래)
	void Prepare(const FCamera& Camera, std::span<const FSpriteDrawItem> SceneItems, std::span<const FSpriteChunkDraw> Chunks, FPreparedFrame& Out);
	// 렌더 스레드 가능: 메시 루트 + 공용 인자(BindMeshPassRoot, 안개)가 묶이고 씬 컬러 + 깊이가 바인딩된 뒤.
	// 인자 번호 = 메시 루트 시그니처의 b0 루트 상수 / t13 / t14 (항목 구간의 t14는 셰이더가 읽지 않지만 유효 주소로 채운다, 청크 구간 = 청크 머리)
	static void RecordDraws(ID3D12GraphicsCommandList* CommandList, const FPreparedFrame& Prepared, uint32 DrawConstantsParam, uint32 InstancesParam,
	                        uint32 InstanceIndicesParam);

private:
	bool CreatePipelines(FD3D12PipelineState (&Out)[SpriteBatching::PipelineKeyCount], bool bForceRecompile);

	// ---- 성능 측정 경로 (r.Sprite.Benchmark N): 씬과 별개인 시험 항목 (텍스처는 만든 쪽 소유 + 아이콘은 루트 제공자로)
	void AppendTestSprites(const FCamera& Camera, int32 Count, std::vector<FSpriteDrawItem>& Out);
	std::vector<FTextureHandle> TestTextures; // [0] 생성 체커(직선 알파), [1] 같은 것 프리멀티플라이드, 뒤 = 프로젝트 아이콘 PNG (있으면)
	uint32                      TestRootProviderId = 0;

	FD3D12RHI*           Rhi               = nullptr;
	FShaderLibrary*      ShaderLibrary     = nullptr;
	FResourceManager*    Resources         = nullptr;
	ID3D12RootSignature* MeshRootSignature = nullptr;
	DXGI_FORMAT          ColorFormat       = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT          DepthFormat       = DXGI_FORMAT_UNKNOWN;
	FD3D12PipelineState  Pipelines[SpriteBatching::PipelineKeyCount]; // [블렌드 × 2 + 조명]

	std::vector<FSpriteDrawItem>    DrawList;
	std::vector<FSpriteDrawItem>    FrameItems; // 이번 Prepare 대상 (목록 + 시험)
	std::vector<SpriteSorting::FKey> SortKeys;
	std::vector<uint32>              SortOrder;
	std::vector<FSpriteInstanceGpu>  InstanceScratch;  // 제출 순서 (병렬로 채움)
	std::vector<uint8>               ItemPipelineKeys; // 제출 순서
	std::vector<uint8>               PipelineKeys;     // 그리기 순서
	std::vector<int32>               ChunkIndices;     // 그리기 순서 (청크 번호, -1 = 항목)
	std::vector<SpriteBatching::FRun> Runs;
	bool                             bWarnedBufferFull = false;
};
