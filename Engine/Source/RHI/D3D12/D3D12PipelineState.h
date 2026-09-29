#pragma once

#include "RHI/D3D12/D3D12Common.h"

#include <vector>

// 렌더 타깃 블렌드 모드
enum class EBlendMode : uint8
{
	Opaque,   // 블렌딩 없음
	Alpha,    // Src * SrcAlpha + Dest * (1 - SrcAlpha)
	Additive, // Src + Dest (HDR 누적, 블룸 업샘플 등)
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

class FD3D12PipelineState
{
public:
	~FD3D12PipelineState();

	bool InitGraphics(ID3D12Device* Device, const FGraphicsPipelineDesc& Desc, const wchar_t* DebugName);
	bool InitCompute(ID3D12Device* Device, ID3D12RootSignature* RootSignature, const D3D12_SHADER_BYTECODE& ComputeShader,
	                 const wchar_t* DebugName);
	void Shutdown();

	// 핫 리로드용: 두 PSO 내용 교환 / 내부 오브젝트 소유권 넘기기 (지연 해제에 전달)
	void                        Swap(FD3D12PipelineState& Other) { PipelineState.Swap(Other.PipelineState); }
	ComPtr<ID3D12PipelineState> Detach() { return std::move(PipelineState); }

	ID3D12PipelineState* Get() const { return PipelineState.Get(); }

private:
	ComPtr<ID3D12PipelineState> PipelineState;
};
