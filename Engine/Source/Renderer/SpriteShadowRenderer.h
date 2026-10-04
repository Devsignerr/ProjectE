#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/ShaderTypes.h"
#include "Renderer/SpriteDraw.h"

#include <span>
#include <vector>

class FD3D12RHI;
class FResourceManager;
class FShaderLibrary;

// 2D 스프라이트·타일맵 그림자 캐스터 (Phase 56-4c). 씬 수집(FSpriteSceneCollector — bCastShadows + 캐스터 볼륨 판정)이 준 그림자 목록을
// 방향광 캐스케이드와 로컬 그림자 장에 깊이만 그린다 — 두 그림자 렌더러의 추가 캐스터 훅(FShadowCasterHook)으로 (지형과 같은 자리).
//
// 깊이: 모든 블렌드가 알파 컷오프로 clip (텍셀 알파 × 색 알파 < AlphaCutoff면 버림), 양면(컬링 없음), 방향광 = 깊이 클립 끔
//   (팬케이킹) + FShadowSettings 바이어스, 로컬 = FLocalShadowSettings 바이어스 (SetBias — 씬 렌더러가 프레임마다 설정 값을 넘기고 바뀌면 PSO를 다시 만든다.
//   기본값은 이전 고정값과 같다).
// 반투명 그림자 (r.Sprite.TranslucentShadows, 기본 끔): 알파·프리멀티플라이드 블렌드 캐스터는 컷오프 대신 그림자 맵 텍셀 고정 4x4 Bayer 디더로 clip →
//   덮는 텍셀 비율 = 텍셀 알파 × 색 알파, PCF가 평균해 옅은 그림자. 패턴이 그림자 맵에 고정이라 시간 안정(TAA 없는 2D도), 캐시 정적 판정 그대로.
//   한계: 필터 폭이 디더 4텍셀보다 좁으면(가까운 캐스케이드·로컬 그림자·픽셀 아트 확대) 점무늬, 겹친 반투명은 곱이 아니라 합집합에 가깝다.
// 방향광 그림자 캐시 (ShadowCacheMath.h): 캐스터를 정적/동적으로 나눈다.
//   정적 = 수집이 bShadowStatic으로 알린 것 (움직이지 않는 타일맵 청크 — 월드·색 알파·컷오프·내용 Revision이 r.Shadow.Cache.StaticFrames 수집 연속 같음,
//   스프라이트 — 그리는 값 해시가 연속 같음). 정적은 FShadowRenderer::ExtraCasters(지형과 함께 캐시에 그림)에, 상태 해시 GetStaticStateHash(장 프러스텀과
//   겹치는 정적 청크 + 정적 항목 묶음(경계 합집합 하나 — 장마다 O(1))의 인스턴스 해시 합 — 텍스처 칸·컷오프·월드 위치 포함, 색 RGB 제외)를
//   ExtraCasterState에 섞는다 (정적 캐스터가 없으면 0 = 섞지 않음 →
//   스프라이트 그림자가 없는 씬은 캐시 키·화면이 이전과 같다).
//   동적 = 그 밖(움직이는 스프라이트, 플립북, 애니메이션 타일) — FShadowRenderer::ExtraDynamicCasters로 매 프레임 그린다 (캐시에 넣지 않음).
//   동적 구간이 닿는 캐스케이드만(GetDynamicCasterBounds) 그리고, 다음 프레임 그 경계의 텍셀 사각형만 캐시에서 되살린다(장 전체 복사 대신 —
//   ShadowCacheMath "장 되살리기"). 동적 항목은 구간 하나라 경계는 동적 항목 전체의 합(넓게 흩어진 애니메이션 타일이면 사각형도 넓다).
//   로컬 그림자는 캐시가 없으므로 둘 다 (ESet::All).
// 데이터: Prepare(게임 스레드, 그림자 패스 등록 전)가 항목 인스턴스(FSpriteInstanceGpu, 월드 공간 — 정적 구간 | 동적 구간)와 청크 머리를 동적 업로드
//   버퍼에 쓰고 구간 목록을 만든다. 텍스처 칸은 매 Prepare에 ResolveTexture로 다시 구한다 (프레임을 넘겨 캐시하지 않음 — 준비 전 기본 텍스처면 칸이 바뀌어
//   정적 해시가 바뀌므로 로드가 끝난 프레임에 캐시를 다시 그린다). 청크 인스턴스는 수집기의 정적 버퍼(로컬 공간)를 그대로 읽는다.
// RenderShadow(렌더 스레드 가능 — 그래프 패스 람다 안 훅)는 Prepare가 만든 구간만 읽는다 (씬을 읽지 않음). 장 프러스텀과 겹치지 않는 구간은 건너뛴다
//   (항목 구간은 구간 전체 경계 — 개별 컬링은 GPU 클립에 맡김).
// 바인딩: 자체 루트 시그니처 (b0 상수 = 광원 뷰-투영 + 구간 정보, t0 인스턴스, t1 청크 머리, 공간 1 무제한 표 = 셰이더 가시 힙 전체, s0 선형 클램프).
// 한계: 레이 트레이싱 그림자(r.RayTracing.Shadows)·RT 반사·DDGI에는 스프라이트가 없다 (TLAS 밖 — 래스터 그림자만).
class FSpriteShadowRenderer
{
public:
	enum class ESet : uint8
	{
		Static,  // 정적 캐스터만 (방향광 캐시에 그림)
		Dynamic, // 동적 캐스터만 (방향광 매 프레임)
		All,     // 둘 다 (로컬 그림자)
	};

	~FSpriteShadowRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);
	// 게임 스레드 (Prepare 전): 그림자 설정 바이어스 (방향광 FShadowSettings, 로컬 FLocalShadowSettings). 바뀌면 PSO를 다시 만든다
	void SetBias(int32 DirectionalDepthBias, float DirectionalSlopeBias, int32 LocalDepthBias, float LocalSlopeBias);

	// 게임 스레드: 그림자 목록 (수집기 GetShadowItems/GetShadowChunks — 비면 이번 프레임 캐스터 없음)
	// bTranslucentDither = r.Sprite.TranslucentShadows: 알파·프리멀티플라이드 블렌드는 컷오프 대신 디더 (SpriteShadow.hlsl)
	void Prepare(std::span<const FSpriteDrawItem> Items, std::span<const FSpriteChunkDraw> Chunks, bool bTranslucentDither = false);
	// 동적 구간 중 장 프러스텀과 겹치는 것이 있나 + 그 구간들의 월드 경계 합 (RenderShadow(ESet::Dynamic)가 그 장에 그리는 것과 같은 판정 —
	// FShadowRenderer 캐스케이드별 캐시 되살리기: 다음 프레임 이 경계의 텍셀 사각형만 캐시에서 다시 쓴다)
	bool GetDynamicCasterBounds(const FFrustum& Frustum, FBox& OutBounds) const;
	// 장 프러스텀 안 정적 캐스터의 상태 해시 (없으면 0). 게임 스레드 (FShadowRenderer::PrepareBatches)
	uint64 GetStaticStateHash(const FFrustum& Frustum) const;
	// 렌더 스레드 가능: 장 DSV·뷰포트가 묶인 상태에서 (FShadowCasterHook). 자기 루트 시그니처/PSO를 묶는다
	void RenderShadow(ID3D12GraphicsCommandList* CommandList, const FMatrix4x4& ViewProjection, const FFrustum& Frustum, bool bLocalLight, ESet Set) const;

	// 통계 (마지막 Prepare)
	uint32 GetCasterCount() const { return CasterCount; }       // 항목 + 청크 타일
	uint32 GetStaticCasterCount() const { return StaticCount; } // 그중 정적

private:
	bool CreatePipelines(FD3D12PipelineState& OutDirectional, FD3D12PipelineState& OutLocal, bool bForceRecompile);

	struct FRun
	{
		D3D12_GPU_VIRTUAL_ADDRESS Instances = 0;
		D3D12_GPU_VIRTUAL_ADDRESS Header    = 0; // 청크 구간: 청크 머리 (항목 구간은 Instances — 셰이더가 읽지 않는 유효 주소)
		uint32                    Count     = 0;
		uint32                    RunInfo   = 0; // b0 RunInfo (청크 비트)
		FBox                      Bounds;
		bool                      bStatic   = false;
	};
	struct FStaticCaster
	{
		FBox   Bounds;
		uint64 Hash = 0;
	};

	FD3D12RHI*          Rhi           = nullptr;
	FShaderLibrary*     ShaderLibrary = nullptr;
	FResourceManager*   Resources     = nullptr;
	FD3D12RootSignature RootSignature;
	FD3D12PipelineState DirectionalPipeline;
	FD3D12PipelineState LocalPipeline;
	int32               DirectionalDepthBias = 0; // PSO에 고정된 바이어스 (Init에서 설정 기본값)
	float               DirectionalSlopeBias = 0.0f;
	int32               LocalDepthBias       = 0;
	float               LocalSlopeBias       = 0.0f;

	std::vector<FRun>               Runs;
	std::vector<FStaticCaster>      StaticCasters;     // 정적 청크 (장 프러스텀마다 개별 판정)
	uint64                          StaticItemSum = 0; // 정적 항목 인스턴스 해시 합 — 항목은 하나로 묶어 판정 (항목이 수만 개여도 장마다 O(1),
	uint32                          StaticItemCount = 0; //   한 정적 항목이 바뀌면 그 묶음 경계와 겹치는 장이 모두 다시 그림 — 보수적, 정적 변경은 드묾)
	FBox                            StaticItemBounds;
	std::vector<FSpriteInstanceGpu> Scratch;
	std::vector<uint64>             ScratchHashes; // 정적 항목 인스턴스 해시 (동적 = 0)
	uint32                          CasterCount = 0;
	uint32                          StaticCount = 0;
};
