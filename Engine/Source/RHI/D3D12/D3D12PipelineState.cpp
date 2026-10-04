#include "RHI/D3D12/D3D12PipelineState.h"

#include "RHI/D3D12/D3D12PipelineCache.h"

#include "Core/CommandLine.h"
#include "Core/StringConv.h"

#include <mutex>
#include <string>

namespace
{
	std::mutex          GDeferredMutex;           // 지연 PSO 생성·교환 (Get()의 첫 호출은 렌더 스레드일 수 있다)
	int32               GDeferredScopeDepth = 0;  // FDeferredCreationScope 중첩 (메인 스레드)
	int32               GDeferredDisabled   = -1; // -1 미확인, 1 = --no-deferred-pso
	std::atomic<uint32> GDeferredRecorded{ 0 };
	std::atomic<uint32> GDeferredResolved{ 0 };

	void CopyBytecode(const D3D12_SHADER_BYTECODE& Code, std::vector<uint8>& Out)
	{
		const uint8* Bytes = static_cast<const uint8*>(Code.pShaderBytecode);
		Out.assign(Bytes, Bytes + (Bytes != nullptr ? Code.BytecodeLength : 0));
	}
	D3D12_SHADER_BYTECODE ToBytecode(const std::vector<uint8>& Bytes)
	{
		return Bytes.empty() ? D3D12_SHADER_BYTECODE{} : D3D12_SHADER_BYTECODE{ Bytes.data(), Bytes.size() };
	}
} // namespace

// 지연 생성용 사본: 포인터 필드(셰이더·입력 레이아웃·의미 이름·이름)는 이 구조체 안 사본을 가리킨다
struct FD3D12PipelineState::FDeferred
{
	ID3D12Device*                         Device   = nullptr;
	bool                                  bCompute = false;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC    Graphics{};
	D3D12_COMPUTE_PIPELINE_STATE_DESC     Compute{};
	ComPtr<ID3D12RootSignature>           RootSignature; // 만들 때까지 살려 둔다
	std::vector<uint8>                    Shader0;       // VS 또는 CS
	std::vector<uint8>                    Shader1;       // PS
	std::vector<D3D12_INPUT_ELEMENT_DESC> Elements;
	std::vector<std::string>              SemanticNames;
	std::wstring                          Name;
};

FD3D12PipelineState::FDeferredCreationScope::FDeferredCreationScope()
{
	++GDeferredScopeDepth;
}

FD3D12PipelineState::FDeferredCreationScope::~FDeferredCreationScope()
{
	--GDeferredScopeDepth;
}

bool FD3D12PipelineState::IsDeferredCreationActive()
{
	if (GDeferredDisabled < 0)
	{
		GDeferredDisabled = FCommandLine::FromProcess().HasFlag(L"--no-deferred-pso") ? 1 : 0;
	}
	return GDeferredScopeDepth > 0 && GDeferredDisabled == 0;
}

uint32 FD3D12PipelineState::GetDeferredResolvedCount()
{
	return GDeferredResolved.load();
}

uint32 FD3D12PipelineState::GetDeferredRecordedCount()
{
	return GDeferredRecorded.load();
}

FD3D12PipelineState::FD3D12PipelineState() = default;

FD3D12PipelineState::~FD3D12PipelineState()
{
	Shutdown();
}

bool FD3D12PipelineState::IsInitialized() const
{
	return ResolvedPipeline.load(std::memory_order_acquire) != nullptr || bDeferredPending.load(std::memory_order_acquire);
}

bool FD3D12PipelineState::IsDeferredPending() const
{
	return bDeferredPending.load(std::memory_order_acquire);
}

ID3D12PipelineState* FD3D12PipelineState::ResolveDeferred() const
{
	if (!bDeferredPending.load(std::memory_order_acquire))
	{
		return nullptr; // 초기화되지 않은 PSO
	}
	std::lock_guard Lock(GDeferredMutex);
	if (ID3D12PipelineState* Resolved = ResolvedPipeline.load(std::memory_order_acquire))
	{
		return Resolved; // 다른 스레드가 먼저 만들었다
	}
	if (!Deferred)
	{
		return nullptr;
	}
	FDeferred&    Pending = *Deferred;
	const HRESULT Result  = Pending.bCompute ? FD3D12PipelineCache::Get().CreateCompute(Pending.Device, Pending.Compute, PipelineState)
	                                         : FD3D12PipelineCache::Get().CreateGraphics(Pending.Device, Pending.Graphics, PipelineState);
	E_CHECKF(SUCCEEDED(Result) && PipelineState, "지연 PSO 생성 실패: {} ({})", FStringConv::ToUtf8(Pending.Name), HResultToString(Result));
	PipelineState->SetName(Pending.Name.c_str());
	ResolvedPipeline.store(PipelineState.Get(), std::memory_order_release);
	Deferred.reset();
	bDeferredPending.store(false, std::memory_order_release);
	GDeferredResolved.fetch_add(1);
	return PipelineState.Get();
}

void FD3D12PipelineState::Swap(FD3D12PipelineState& Other)
{
	std::lock_guard Lock(GDeferredMutex);
	PipelineState.Swap(Other.PipelineState);
	Deferred.swap(Other.Deferred);
	ResolvedPipeline.store(PipelineState.Get(), std::memory_order_release);
	Other.ResolvedPipeline.store(Other.PipelineState.Get(), std::memory_order_release);
	bDeferredPending.store(Deferred != nullptr, std::memory_order_release);
	Other.bDeferredPending.store(Other.Deferred != nullptr, std::memory_order_release);
}

ComPtr<ID3D12PipelineState> FD3D12PipelineState::Detach()
{
	std::lock_guard Lock(GDeferredMutex);
	Deferred.reset();
	bDeferredPending.store(false, std::memory_order_release);
	ResolvedPipeline.store(nullptr, std::memory_order_release);
	return std::move(PipelineState);
}

bool FD3D12PipelineState::InitGraphics(ID3D12Device* Device, const FGraphicsPipelineDesc& Desc, const wchar_t* DebugName)
{
	E_CHECKF(!IsInitialized(), "파이프라인 스테이트가 이미 생성되어 있습니다");
	E_CHECKF(Desc.RootSignature != nullptr, "루트 시그니처가 필요합니다");
	E_CHECKF(Desc.VertexShader.pShaderBytecode != nullptr, "정점 셰이더가 필요합니다");
	E_CHECKF(Desc.NumRenderTargets <= 8, "렌더 타깃은 최대 8개입니다");

	D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc{};
	PsoDesc.pRootSignature = Desc.RootSignature;
	PsoDesc.VS             = Desc.VertexShader;
	PsoDesc.PS             = Desc.PixelShader;

	// 블렌드
	PsoDesc.BlendState.AlphaToCoverageEnable  = FALSE;
	PsoDesc.BlendState.IndependentBlendEnable = FALSE;
	const EBlendMode BlendMode = Desc.GetEffectiveBlendMode();
	for (D3D12_RENDER_TARGET_BLEND_DESC& Target : PsoDesc.BlendState.RenderTarget)
	{
		Target.BlendEnable   = BlendMode != EBlendMode::Opaque ? TRUE : FALSE;
		Target.LogicOpEnable = FALSE;
		if (BlendMode == EBlendMode::Additive)
		{
			Target.SrcBlend       = D3D12_BLEND_ONE;
			Target.DestBlend      = D3D12_BLEND_ONE;
			Target.SrcBlendAlpha  = D3D12_BLEND_ONE;
			Target.DestBlendAlpha = D3D12_BLEND_ONE;
		}
		else if (BlendMode == EBlendMode::Premultiplied)
		{
			Target.SrcBlend       = D3D12_BLEND_ONE;
			Target.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
			Target.SrcBlendAlpha  = D3D12_BLEND_ZERO;
			Target.DestBlendAlpha = D3D12_BLEND_ONE;
		}
		else if (BlendMode == EBlendMode::PremultipliedOver)
		{
			Target.SrcBlend       = D3D12_BLEND_ONE;
			Target.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
			Target.SrcBlendAlpha  = D3D12_BLEND_ONE;
			Target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		}
		else if (BlendMode == EBlendMode::Remaining)
		{
			Target.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
			Target.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
			Target.SrcBlendAlpha  = D3D12_BLEND_ZERO;
			Target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		}
		else
		{
			Target.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
			Target.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
			Target.SrcBlendAlpha  = D3D12_BLEND_ONE;
			Target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		}
		Target.BlendOp               = D3D12_BLEND_OP_ADD;
		Target.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
		Target.LogicOp               = D3D12_LOGIC_OP_NOOP;
		Target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}
	PsoDesc.SampleMask = UINT_MAX;

	// 래스터라이저
	PsoDesc.RasterizerState.FillMode              = Desc.FillMode;
	PsoDesc.RasterizerState.CullMode              = Desc.CullMode;
	PsoDesc.RasterizerState.FrontCounterClockwise = Desc.bFrontCounterClockwise ? TRUE : FALSE;
	PsoDesc.RasterizerState.DepthBias             = Desc.DepthBias;
	PsoDesc.RasterizerState.DepthBiasClamp        = Desc.DepthBiasClamp;
	PsoDesc.RasterizerState.SlopeScaledDepthBias  = Desc.SlopeScaledDepthBias;
	PsoDesc.RasterizerState.DepthClipEnable       = Desc.bDepthClip ? TRUE : FALSE;
	PsoDesc.RasterizerState.MultisampleEnable     = FALSE;
	PsoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
	PsoDesc.RasterizerState.ForcedSampleCount     = 0;
	PsoDesc.RasterizerState.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	// 깊이/스텐실
	PsoDesc.DepthStencilState.DepthEnable      = Desc.bDepthEnable ? TRUE : FALSE;
	PsoDesc.DepthStencilState.DepthWriteMask   = Desc.bDepthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
	PsoDesc.DepthStencilState.DepthFunc        = Desc.DepthFunc;
	PsoDesc.DepthStencilState.StencilEnable    = FALSE;
	PsoDesc.DepthStencilState.StencilReadMask  = D3D12_DEFAULT_STENCIL_READ_MASK;
	PsoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
	const D3D12_DEPTH_STENCILOP_DESC DefaultStencilOp{ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
	                                                   D3D12_COMPARISON_FUNC_ALWAYS };
	PsoDesc.DepthStencilState.FrontFace = DefaultStencilOp;
	PsoDesc.DepthStencilState.BackFace  = DefaultStencilOp;

	// 입력 조립
	PsoDesc.InputLayout.pInputElementDescs = Desc.InputLayout.empty() ? nullptr : Desc.InputLayout.data();
	PsoDesc.InputLayout.NumElements        = static_cast<UINT>(Desc.InputLayout.size());
	PsoDesc.IBStripCutValue                = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
	PsoDesc.PrimitiveTopologyType          = Desc.PrimitiveTopologyType;

	// 출력
	PsoDesc.NumRenderTargets = Desc.NumRenderTargets;
	for (uint32 Index = 0; Index < Desc.NumRenderTargets; ++Index)
	{
		PsoDesc.RTVFormats[Index] = Desc.RenderTargetFormats[Index];
	}
	PsoDesc.DSVFormat  = Desc.DepthStencilFormat;
	PsoDesc.SampleDesc = { 1, 0 };
	PsoDesc.NodeMask   = 0;
	PsoDesc.Flags      = D3D12_PIPELINE_STATE_FLAG_NONE;

	if (IsDeferredCreationActive())
	{
		// 지연: 포인터 필드를 사본으로 바꿔 둔다 (호출자의 셰이더 블롭·입력 레이아웃은 Init 뒤 사라질 수 있다)
		auto Pending           = std::make_unique<FDeferred>();
		Pending->Device        = Device;
		Pending->Graphics      = PsoDesc;
		Pending->RootSignature = Desc.RootSignature;
		CopyBytecode(PsoDesc.VS, Pending->Shader0);
		CopyBytecode(PsoDesc.PS, Pending->Shader1);
		Pending->Graphics.VS = ToBytecode(Pending->Shader0);
		Pending->Graphics.PS = ToBytecode(Pending->Shader1);
		Pending->Elements    = Desc.InputLayout;
		Pending->SemanticNames.reserve(Pending->Elements.size());
		for (const D3D12_INPUT_ELEMENT_DESC& Element : Pending->Elements)
		{
			Pending->SemanticNames.emplace_back(Element.SemanticName != nullptr ? Element.SemanticName : "");
		}
		for (size_t Index = 0; Index < Pending->Elements.size(); ++Index)
		{
			Pending->Elements[Index].SemanticName = Pending->SemanticNames[Index].c_str();
		}
		Pending->Graphics.InputLayout.pInputElementDescs = Pending->Elements.empty() ? nullptr : Pending->Elements.data();
		Pending->Graphics.InputLayout.NumElements        = static_cast<UINT>(Pending->Elements.size());
		Pending->Graphics.pRootSignature                 = Pending->RootSignature.Get();
		Pending->Name                                    = DebugName != nullptr ? DebugName : L"";
		Deferred                                         = std::move(Pending);
		bDeferredPending.store(true, std::memory_order_release);
		GDeferredRecorded.fetch_add(1);
		return true;
	}
	E_D3D_VERIFY(FD3D12PipelineCache::Get().CreateGraphics(Device, PsoDesc, PipelineState)); // PSO 캐시 (워밍/드라이버 캐시, 꺼져 있으면 바로 생성)
	PipelineState->SetName(DebugName);
	ResolvedPipeline.store(PipelineState.Get(), std::memory_order_release);
	return true;
}

bool FD3D12PipelineState::InitCompute(ID3D12Device* Device, ID3D12RootSignature* RootSignature,
                                      const D3D12_SHADER_BYTECODE& ComputeShader, const wchar_t* DebugName)
{
	E_CHECKF(!IsInitialized(), "파이프라인 스테이트가 이미 생성되어 있습니다");
	E_CHECKF(RootSignature != nullptr, "루트 시그니처가 필요합니다");
	E_CHECKF(ComputeShader.pShaderBytecode != nullptr, "컴퓨트 셰이더가 필요합니다");

	D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc{};
	PsoDesc.pRootSignature = RootSignature;
	PsoDesc.CS             = ComputeShader;
	PsoDesc.NodeMask       = 0;
	PsoDesc.Flags          = D3D12_PIPELINE_STATE_FLAG_NONE;

	if (IsDeferredCreationActive())
	{
		auto Pending           = std::make_unique<FDeferred>();
		Pending->Device        = Device;
		Pending->bCompute      = true;
		Pending->Compute       = PsoDesc;
		Pending->RootSignature = RootSignature;
		CopyBytecode(ComputeShader, Pending->Shader0);
		Pending->Compute.CS             = ToBytecode(Pending->Shader0);
		Pending->Compute.pRootSignature = Pending->RootSignature.Get();
		Pending->Name                   = DebugName != nullptr ? DebugName : L"";
		Deferred                        = std::move(Pending);
		bDeferredPending.store(true, std::memory_order_release);
		GDeferredRecorded.fetch_add(1);
		return true;
	}
	E_D3D_VERIFY(FD3D12PipelineCache::Get().CreateCompute(Device, PsoDesc, PipelineState));
	PipelineState->SetName(DebugName);
	ResolvedPipeline.store(PipelineState.Get(), std::memory_order_release);
	return true;
}

void FD3D12PipelineState::Shutdown()
{
	if (!IsInitialized() && !PipelineState)
	{
		return; // 빈 PSO (정적 객체 소멸 순서와 무관하게 잠금을 잡지 않는다)
	}
	std::lock_guard Lock(GDeferredMutex);
	Deferred.reset();
	bDeferredPending.store(false, std::memory_order_release);
	ResolvedPipeline.store(nullptr, std::memory_order_release);
	PipelineState.Reset();
}
