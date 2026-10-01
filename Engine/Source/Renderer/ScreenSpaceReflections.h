#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"
#include "RHI/D3D12/D3D12RootSignature.h"

#include <memory>
#include <vector>

class FD3D12RHI;
class FScreenPassRootSignature;
class FShaderLibrary;

// SSR 입력 (FSceneRenderer)
struct FScreenSpaceReflectionInputs
{
	const FD3D12RenderTarget* SceneColor  = nullptr; // 깊이(사전 패스) + 이전 프레임 색 (메인 패스 전이라 지난 프레임 내용)
	const FD3D12RenderTarget* SceneNormal = nullptr;
	const FD3D12RenderTarget* DecalNormal   = nullptr; // DBufferB (항상 바인딩 — bDecals가 거짓이면 읽지 않음)
	const FD3D12RenderTarget* DecalMaterial = nullptr; // DBufferC
	bool                      bDecals       = false;   // 이번 프레임 데칼을 그렸음 → 추적/흐림이 데칼 법선·거칠기를 쓴다 (사전 패스 버퍼에는 데칼이 없다)
	FMatrix4x4                Projection;   // 지터 포함
	FMatrix4x4                View;
	FMatrix4x4                Reprojection; // 현재 클립 → 이전 클립
	float                     NearZ         = 10.0f;
	bool                      bOrthographic = false;
	float                     MaxDistance   = 2000.0f;
	float                     Thickness     = 40.0f;
	float                     MaxRoughness  = 0.6f;
	float                     MaxBlurRadius = 16.0f; // 거칠기 흐림 반경 상한 (픽셀)
	const FD3D12RenderTarget* Velocity      = nullptr; // 표면 움직임 벡터 (반사가 맞지 않은 픽셀의 누적)
	const FD3D12RenderTarget* PrevColor     = nullptr; // 반사 색 원본: 지난 프레임 TAA 결과 (지터·계단이 없어 반사가 덜 지글거림). 없으면 SceneColor
	bool                      bHistoryValid = false;   // 씬 렌더러의 시간 이력 유효 (카메라 컷/크기 변경이면 false)
};

// 화면 공간 반사 (ScreenSpaceReflections.hlsl Hi-Z + SsrTrace.hlsl, 식은 Renderer/ReflectionMath.h).
//   사전 패스 뒤·메인 패스 전: 사전 패스 깊이로 최소 깊이 밉 체인 → 픽셀마다 거울 반사 광선을 계층 추적 → (색, 신뢰도).
//   표면 법선/거칠기 = 사전 패스 버퍼 + 데칼 DBuffer(B/C, 메인 패스 ApplyDecals와 같은 식) — 데칼 패스 뒤에 돌아야 한다.
//   색은 이전 프레임 씬 컬러를 재투영해 읽으므로 이력이 없는 프레임(첫 프레임/크기 변경/카메라 컷)은 끈다.
//   거울 방향 한 번 추적(결정적) → SsrResolve.hlsl PSBlur 거칠기 원뿔 반경 원판 흐림 → PSResolve 반사 움직임 재투영 누적(이력 2장 핑퐁).
//   확률 반사(픽셀마다 GGX 방향 하나 + 누적)는 TV 노이즈처럼 지글거려 쓰지 않는다.
//   메인 패스가 t22로 읽어 거칠기 페이드 후 캡처/하늘 IBL 반사 대신 섞는다. 결과 타깃은 항상 있다 (끄면 읽지 않음)
class FScreenSpaceReflections
{
public:
	static constexpr DXGI_FORMAT ResultFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	static constexpr DXGI_FORMAT MotionFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // xy = 반사 움직임 벡터 (가상 점 기준), z = 흐림 반경 (픽셀)

	~FScreenSpaceReflections();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library, const FScreenPassRootSignature& InRoot);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	void EnsureTargets(uint32 Width, uint32 Height);
	void Render(const FScreenSpaceReflectionInputs& Inputs);

	// 이번 프레임 결과: 누적했으면 이력 타깃, 아니면 추적 결과
	const FD3D12DescriptorHandle& GetResultSrv() const { return Output != nullptr ? Output->GetSrv() : Result->GetSrv(); }

private:
	bool CreatePipelines(FD3D12PipelineState& OutCopy, FD3D12PipelineState& OutDownsample, FD3D12PipelineState& OutTrace,
	                     FD3D12PipelineState& OutBlur, FD3D12PipelineState& OutResolve, bool bForceRecompile);
	void ReleaseHiz();

	FD3D12RHI*                      Rhi     = nullptr;
	FShaderLibrary*                 Library = nullptr;
	const FScreenPassRootSignature* Root    = nullptr; // 비소유: 씬 렌더러 소유
	FD3D12RootSignature             HizRoot;           // b0 상수 4개, t0 표, u0 표, u1 표
	FD3D12PipelineState             HizCopyPipeline;
	FD3D12PipelineState             HizDownsamplePipeline;
	FD3D12PipelineState             TracePipeline;
	FD3D12PipelineState             BlurPipeline;
	FD3D12PipelineState             ResolvePipeline;

	std::unique_ptr<FD3D12RenderTarget> Result;
	std::unique_ptr<FD3D12RenderTarget> ReflectMotion; // 추적 2번째 출력
	std::unique_ptr<FD3D12RenderTarget> Blurred;        // 거칠기 흐림 결과
	std::unique_ptr<FD3D12RenderTarget> History[2];           // 누적 결과 핑퐁
	const FD3D12RenderTarget*           Output         = nullptr; // 이번 프레임 누적 결과 (없으면 Result)
	uint32                              HistoryIndex   = 0;
	uint64                              LastResolveFrame = 0;  // 마지막 누적의 Rhi 프레임 번호 (연속일 때만 이력 사용, 0 = 없음)
	ComPtr<ID3D12Resource>              Hiz; // R32_FLOAT 밉 체인 (평소 PIXEL_SHADER_RESOURCE)
	FD3D12DescriptorHandle              HizSrv;
	std::vector<FD3D12DescriptorHandle> HizUavs;
	uint32                              HizWidth    = 0;
	uint32                              HizHeight   = 0;
	uint32                              HizMipCount = 0;
};
