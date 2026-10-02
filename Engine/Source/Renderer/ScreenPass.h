#pragma once

#include "Core/CoreTypes.h"
#include "RHI/D3D12/D3D12Common.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"

#include <initializer_list>

class FD3D12RHI;
class FShaderLibrary;
struct FRenderOutput;

// 화면 공간 패스(TAA, SSAO, 안개 적용, SSR 등) 공용 루트 시그니처 + 그리기 도우미.
//   b0 = 루트 CBV (동적 업로드 버퍼 상수), t0~t7 = SRV 1칸짜리 표 8개(셰이더 가시 힙, 매 패스 따로 지정), u0~u1 = UAV 표 2개
//   s0 = 선형 클램프, s1 = 점 클램프, s2 = 비교(LESS_EQUAL, 그림자 맵), s3 = 선형 반복. 그래픽스/계산 공용(가시성 ALL)
//   그래픽스 패스는 전체 화면 삼각형(Fullscreen.hlsli VSMain 규약: 각 셰이더 파일에 VSMain 진입점)
class FScreenPassRootSignature
{
public:
	static constexpr uint32 SrvCount = 8;
	static constexpr uint32 UavCount = 2;
	enum ERootParameter : uint32
	{
		Root_Constants = 0,
		Root_Srv0      = 1,                  // t0 (+ i)
		Root_Uav0      = Root_Srv0 + SrvCount, // u0 (+ i)
	};

	bool Init(ID3D12Device* Device);
	void Shutdown() { RootSignature.Shutdown(); }
	ID3D12RootSignature* Get() const { return RootSignature.Get(); }

	// 전체 화면 그래픽스 PSO (VSMain + PixelEntry). Formats 개수만큼 렌더 타깃, 깊이 없음
	bool CreateGraphicsPipeline(FD3D12PipelineState& OutPipeline, ID3D12Device* Device, FShaderLibrary& Library, const wchar_t* File,
	                            const wchar_t* PixelEntry, std::initializer_list<DXGI_FORMAT> Formats, EBlendMode BlendMode,
	                            bool bForceRecompile, const wchar_t* DebugName) const;
	// 전체 화면 깊이 출력 PSO (VSMain + PixelEntry가 SV_Depth를 쓴다): 렌더 타깃 없음, 깊이 항상 통과 + 쓰기
	bool CreateDepthOutputPipeline(FD3D12PipelineState& OutPipeline, ID3D12Device* Device, FShaderLibrary& Library, const wchar_t* File,
	                               const wchar_t* PixelEntry, DXGI_FORMAT DepthFormat, bool bForceRecompile, const wchar_t* DebugName) const;
	bool CreateComputePipeline(FD3D12PipelineState& OutPipeline, ID3D12Device* Device, FShaderLibrary& Library, const wchar_t* File,
	                           const wchar_t* Entry, bool bForceRecompile, const wchar_t* DebugName) const;

private:
	FD3D12RootSignature RootSignature;
};

// 그래픽스 패스 하나: 루트 시그니처/PSO/상수/SRV 바인딩 → 전체 화면 삼각형. Srvs[i] = t(i) (빈 핸들은 건너뜀)
void DrawScreenPass(ID3D12GraphicsCommandList* CommandList, const FScreenPassRootSignature& Root, const FD3D12PipelineState& Pipeline,
                    D3D12_GPU_VIRTUAL_ADDRESS Constants, std::initializer_list<FD3D12DescriptorHandle> Srvs, uint32 Width, uint32 Height);
void SetScreenPassViewport(ID3D12GraphicsCommandList* CommandList, uint32 Width, uint32 Height);
