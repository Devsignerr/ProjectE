#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/ShaderTypes.h"
#include "Scene/ResourceHandles.h"

#include <memory>
#include <vector>

class FCamera;
class FD3D12RHI;
class FResourceManager;
class FScene;
class FShaderLibrary;
struct FParticleEmitter;
struct FParticleGpuBuffer;

// 파티클 패스 (FSceneRenderer). 조명/그림자 없음.
//   Simulate: GPU 이미터의 쌓인 계산 요청을 계산 셰이더로 처리 (HDR 패스 전에 호출)
//   Render: 이미터 × 렌더러(스프라이트/메시/리본)를 카메라에서 먼 순으로 그린다. CPU 반투명은 입자도 뒤→앞 정렬.
//   CPU 입자는 프레임마다 동적 업로드 버퍼에 올리고, GPU 입자는 계산 결과 버퍼를 그대로 읽는다.
class FParticleRenderer
{
public:
	~FParticleRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources, DXGI_FORMAT ColorFormat, DXGI_FORMAT DepthFormat);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	void Simulate(FScene& Scene);
	// 렌더 타깃(HDR + 깊이)이 바인딩된 상태에서 호출. 반환: 그린 입자 수 (GPU는 추정치)
	uint32 Render(FScene& Scene, const FCamera& Camera);

	// 모듈 설정 → GPU 프로그램 (ParticleSimulate.hlsl 형식). 테스트용으로 공개
	static void BuildGpuProgram(const FParticleEmitter& Emitter, std::vector<FVector4>& OutProgram, FParticleSimConstants& OutConstants);

private:
	enum EPipelineKind : uint32
	{
		Pipeline_Sprite = 0,
		Pipeline_Mesh,
		Pipeline_Ribbon,
		Pipeline_Count
	};
	struct FPipelineSet
	{
		FD3D12PipelineState Graphics[Pipeline_Count][2]; // [종류][0 = 반투명, 1 = 가산]
		FD3D12PipelineState Simulate;
	};
	bool CreatePipelines(FPipelineSet& Out, bool bForceRecompile);

	FD3D12RHI*        Rhi           = nullptr;
	FShaderLibrary*   ShaderLibrary = nullptr;
	FResourceManager* Resources     = nullptr;
	DXGI_FORMAT       ColorFormat   = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT       DepthFormat   = DXGI_FORMAT_UNKNOWN;

	FD3D12RootSignature RootSignature;
	FD3D12RootSignature ComputeRootSignature;
	FPipelineSet        Pipelines;
	FTextureHandle      DefaultTexture; // 부드러운 원 (렌더러에 텍스처가 없을 때)

	// 이 렌더러가 만든 GPU 버퍼 (종료 시 먼저 해제하고 이미터 쪽 소유권만 남긴다)
	std::vector<std::weak_ptr<FParticleGpuBuffer>> GpuBuffers;

	std::vector<FParticleGpuData>      UploadScratch;
	std::vector<uint32>                OrderScratch;
	std::vector<FParticleRibbonVertex> RibbonScratch;
	std::vector<FVector4>              ProgramScratch;
	bool                               bWarnedBufferFull = false;
	bool                               bWarnedGpuRibbon  = false;
};
