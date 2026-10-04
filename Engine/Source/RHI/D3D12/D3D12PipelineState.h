#pragma once

#include "RHI/D3D12/D3D12Common.h"

#include <atomic>
#include <memory>
#include <vector>

// 렌더 타깃 블렌드 모드
enum class EBlendMode : uint8
{
	Opaque,   // 블렌딩 없음
	Alpha,    // Src * SrcAlpha + Dest * (1 - SrcAlpha)
	Additive, // Src + Dest (HDR 누적, 블룸 업샘플 등)
	Premultiplied, // 색 = Src + Dest * (1 - SrcAlpha), 알파 = Dest 유지 (안개 적용: 씬 컬러 알파 = TAA 반응형 마스크를 건드리지 않는다)
	Remaining, // 색 = Src * SrcAlpha + Dest * (1 - SrcAlpha), 알파 = Dest * (1 - SrcAlpha) (데칼 DBuffer: 알파 = 남은 원래 표면 비중)
	PremultipliedOver, // 색 = Src + Dest * (1 - SrcAlpha), 알파 = SrcAlpha + Dest * (1 - SrcAlpha) (프리멀티플라이드 스프라이트: 알파 = 덮인 정도 누적)
};

// 그래픽스 파이프라인 설정. 자주 쓰는 값이 기본값이며 필요한 항목만 바꾼다.
struct FGraphicsPipelineDesc
{
	ID3D12RootSignature*  RootSignature = nullptr;
	D3D12_SHADER_BYTECODE VertexShader{};
	D3D12_SHADER_BYTECODE PixelShader{};

	std::vector<D3D12_INPUT_ELEMENT_DESC> InputLayout;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE         PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	uint32      NumRenderTargets       = 1;
	DXGI_FORMAT RenderTargetFormats[8] = { DXGI_FORMAT_R8G8B8A8_UNORM };
	DXGI_FORMAT DepthStencilFormat     = DXGI_FORMAT_UNKNOWN;

	// 래스터라이저. 왼손 좌표계 기본: 시계 방향(CW)이 앞면
	D3D12_CULL_MODE CullMode              = D3D12_CULL_MODE_BACK;
	D3D12_FILL_MODE FillMode              = D3D12_FILL_MODE_SOLID;
	bool            bFrontCounterClockwise = false;
	// 깊이 바이어스 (섀도우 맵 등). 기본은 D3D12 기본값(0)
	int32 DepthBias            = 0;
	float SlopeScaledDepthBias = 0.0f;
	float DepthBiasClamp       = 0.0f;
	bool  bDepthClip           = true; // false면 근평면 뒤 캐스터도 그린다 (섀도우 팬케이킹)

	// 깊이
	bool                  bDepthEnable = false;
	bool                  bDepthWrite  = true;
	D3D12_COMPARISON_FUNC DepthFunc    = D3D12_COMPARISON_FUNC_LESS;

	// 블렌딩
	EBlendMode BlendMode = EBlendMode::Opaque;
	// 하위 호환: true이고 BlendMode가 Opaque면 Alpha로 취급 (새 코드는 BlendMode 사용)
	bool bAlphaBlend = false;

	EBlendMode GetEffectiveBlendMode() const { return (bAlphaBlend && BlendMode == EBlendMode::Opaque) ? EBlendMode::Alpha : BlendMode; }
};

// PSO 하나. 지연 생성(2026-10-05, 2D 시작 시간): FDeferredCreationScope 안에서 Init*을 부르면 설명(셰이더 바이트코드·입력 레이아웃 사본 +
// 루트 시그니처 참조)만 기억하고 실제 Create*PipelineState(PSO 캐시 요청)는 처음 Get()할 때 한다 — 쓰지 않는 패스(2D 게임의 3D 메시·
// 지형·물·구름 변형 등)의 PSO는 만들지 않고, 만든 것만 PSO 레시피(사용자/--record-pso)에 남는다. 범위 밖 Init*은 예전처럼 즉시 만든다
// (핫 리로드: 실패하면 기존 PSO 유지가 즉시 판정에 기댄다).
//   Get()은 어느 스레드에서 불러도 된다(렌더 스레드 기록 중 첫 사용 — 생성은 전역 잠금 안, 이미 만든 뒤는 원자 읽기 하나).
//   지연 생성 실패는 Fatal (예전에는 Init이 false → 앱 초기화 실패였던 경우). 존재 확인은 Get() 대신 IsInitialized()(만들지 않음).
//   Init*/Swap/Detach/Shutdown은 예전처럼 그 PSO를 쓰는 기록이 없을 때만 (메인 스레드).
class FD3D12PipelineState
{
public:
	FD3D12PipelineState();
	~FD3D12PipelineState();
	FD3D12PipelineState(const FD3D12PipelineState&)            = delete;
	FD3D12PipelineState& operator=(const FD3D12PipelineState&) = delete;

	bool InitGraphics(ID3D12Device* Device, const FGraphicsPipelineDesc& Desc, const wchar_t* DebugName);
	bool InitCompute(ID3D12Device* Device, ID3D12RootSignature* RootSignature, const D3D12_SHADER_BYTECODE& ComputeShader,
	                 const wchar_t* DebugName);
	void Shutdown();

	// 핫 리로드용: 두 PSO 내용 교환 / 내부 오브젝트 소유권 넘기기 (지연 해제에 전달 — 아직 만들지 않은 지연 PSO면 빈 포인터)
	void                        Swap(FD3D12PipelineState& Other);
	ComPtr<ID3D12PipelineState> Detach();

	// 지연 PSO면 여기서 만든다
	ID3D12PipelineState* Get() const
	{
		ID3D12PipelineState* Resolved = ResolvedPipeline.load(std::memory_order_acquire);
		return Resolved != nullptr ? Resolved : ResolveDeferred();
	}
	// 만들었거나 지연 생성 대기 중인가 (만들지 않는다)
	bool IsInitialized() const;
	bool IsDeferredPending() const;

	// 이 범위(중첩 가능, 메인 스레드)에서 부른 Init*은 지연 생성. --no-deferred-pso면 범위가 있어도 즉시
	struct FDeferredCreationScope
	{
		FDeferredCreationScope();
		~FDeferredCreationScope();
		FDeferredCreationScope(const FDeferredCreationScope&)            = delete;
		FDeferredCreationScope& operator=(const FDeferredCreationScope&) = delete;
	};
	static bool IsDeferredCreationActive();
	static uint32 GetDeferredResolvedCount(); // 통계: 지연 생성으로 실제 만든 수
	static uint32 GetDeferredRecordedCount(); // 통계: 지연으로 기록한 수 (만들지 않은 것 포함)

private:
	struct FDeferred;

	ID3D12PipelineState* ResolveDeferred() const;

	mutable ComPtr<ID3D12PipelineState>              PipelineState;
	mutable std::atomic<ID3D12PipelineState*>        ResolvedPipeline{ nullptr };
	mutable std::atomic<bool>                        bDeferredPending{ false }; // Deferred가 있다 (빈 PSO의 Get()이 잠금을 잡지 않게)
	mutable std::unique_ptr<FDeferred>               Deferred;
};
