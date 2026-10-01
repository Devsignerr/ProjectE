#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
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
// 화면 밖 컬링 (이미터 단위, 메인 카메라 프러스텀 — 경계 식은 Renderer/ParticleBounds.h):
//   CPU 이미터: 실제 입자 AABB가 밖이면 그리기만 생략 (시뮬레이션은 Scene의 FParticleSystem이 계속 돌린다).
//   GPU 이미터: 고정 경계(에셋 이미터 설정) 또는 모듈 추정 경계(월드 공간이면 최근 최대 수명 동안의 이미터 위치 자취 포함)가 밖이면
//     그리기와 계산 디스패치를 모두 미룬다. 규칙: 계산 요청(Scene이 프레임마다 쌓음)을 풀의 대기열로 옮겨 두고, 숨어 있는 동안
//     "뒤에서부터 합이 최대 수명 이상이 되는 요청"보다 오래된 것은 버린다 — 그 사이 만들어졌을 입자는 지금쯤 모두 죽었고,
//     숨기 전부터 살던 입자도 남긴 요청을 다시 돌리면 최대 수명 이상 나이를 먹어 죽으므로 결과가 계속 계산한 것과 같다
//     (요청마다 시각을 거꾸로 계산해 Time 입력도 같다). 대기열이 64개를 넘으면 이웃끼리 합쳐 절반으로 (근사: 합친 구간의 생성이
//     한 번에 몰린다). 다시 보이는 프레임에 대기열을 모두 디스패치해 따라잡는다 (숨은 시간이 길수록 최대 64번 + 수명 분량).
class FParticleRenderer
{
public:
	~FParticleRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, FResourceManager& InResources, DXGI_FORMAT ColorFormat, DXGI_FORMAT DepthFormat);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	void Simulate(FScene& Scene, const FFrustum& CullFrustum);
	// 렌더 타깃(HDR + 깊이)이 바인딩된 상태에서 호출. 반환: 그린 입자 수 (GPU는 추정치)
	uint32 Render(FScene& Scene, const FCamera& Camera, const FFrustum& CullFrustum);

	bool bEnableCulling = true; // 화면 밖 이미터 컬링 (끄면 모두 계산·그리기, --no-particle-culling)

	// 안개 (FFogRenderer): Render 전에 이번 프레임 상수(b2)와 볼류메트릭 결과(t2)를 넘긴다. 정점마다 Fog.hlsli EvaluateFog
	void SetFog(D3D12_GPU_VIRTUAL_ADDRESS InFogConstants, const FD3D12DescriptorHandle& InFogVolume)
	{
		FogConstants = InFogConstants;
		FogVolume    = InFogVolume;
	}

	// 지난 Render에서 화면 밖이라 그리지 않은 이미터 수, 지난 Simulate에서 계산을 미룬 GPU 이미터 수 (통계)
	uint32 GetCulledEmitterCount() const { return CulledEmitters; }
	uint32 GetCulledGpuEmitterCount() const { return CulledGpuEmitters; }

	// 모듈 설정 → GPU 프로그램 (ParticleSimulate.hlsl 형식). 테스트용으로 공개
	static void BuildGpuProgram(const FParticleEmitter& Emitter, std::vector<FVector4>& OutProgram, FParticleSimConstants& OutConstants);

private:
	D3D12_GPU_VIRTUAL_ADDRESS FogConstants = 0;
	FD3D12DescriptorHandle    FogVolume;

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
	uint32                             CulledEmitters    = 0;
	uint32                             CulledGpuEmitters = 0;
	bool                               bWarnedGpuRibbon  = false;
};
