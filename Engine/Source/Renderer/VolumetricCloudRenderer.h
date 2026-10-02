#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/PersistentTexture.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/ShaderTypes.h"

#include <array>
#include <chrono>

class FCamera;
class FD3D12RHI;
class FD3D12RenderTarget;
class FScene;
class FShaderLibrary;
class FSkyAtmosphereRenderer;

// 볼류메트릭 구름 (Phase 49, FVolumetricCloudComponent — 식은 Renderer/CloudMath.h, 셰이더 VolumetricClouds.hlsl/CloudNoise.hlsl)
//   대기 컴포넌트가 있을 때만 (태양 투과율·하늘 조도·공중 원근을 대기에서 받는다)
//   노이즈: 시작(첫 사용) 때 계산 셰이더로 한 번 — 모양 128^3, 세부 32^3, 덮임 분포 256^2 (모두 주기 노이즈)
//   프레임: 추적(씬 ÷ r.VolumetricClouds.Divisor, 기본 4) → 시간 누적(재투영 + 분산 클리핑, 이력 핑퐁) → 합성(씬 컬러, 하늘·먼 기하 앞)
//     해상도 규칙(TAAU): 추적/이력은 씬(내부) 해상도 ÷ 나눗수 — 내부 크기가 바뀌면 버퍼를 다시 만들고 이력을 버린다
//     시간 안정성: 지터·행진 시작 노이즈는 시간 누적이 켜진 연속 프레임에만 (픽셀 아트·반사 캡처·미리보기는 결정적 표본 0.5)
//   IBL: 32^3 큐브 6면 저해상도 구름(지터 없음)을 대기 하늘 큐브에 합성 (bAffectEnvironmentLighting)
class FVolumetricCloudRenderer
{
public:
	~FVolumetricCloudRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	struct FPrepareInputs
	{
		const FCamera* Camera = nullptr;
		FMatrix4x4     UnjitteredViewProjection;
		FMatrix4x4     PrevViewProjection;
		uint32         Width  = 0; // 씬(내부) 해상도
		uint32         Height = 0;
		bool           bHistoryValid = false; // 씬 렌더러의 시간 이력 (연속 프레임·같은 크기·컷 아님)
		bool           bAllowTemporal = true; // 픽셀 아트·반사 캡처면 false
	};
	// 씬의 첫 구름 컴포넌트 + 대기 → 상수. 구름이 없거나 대기가 비활성이면 false
	bool Prepare(FScene& Scene, const FSkyAtmosphereRenderer& Atmosphere, const FPrepareInputs& Inputs);
	bool IsActive() const { return bActive; }
	bool IsChanging() const { return bActive && bWindMoving; }
	bool AffectsEnvironment() const { return bActive && bAffectEnvironment; }

	// 추적 + 누적 (+ IBL 큐브). FogConstants = 공중 원근 (FFogRenderer 상수, Prepare/PrepareVolumetric 뒤)
	void AddPasses(FRenderGraph& Graph, const FSkyAtmosphereRenderer& Atmosphere, D3D12_GPU_VIRTUAL_ADDRESS FogConstants, int32 Timer);
	// 씬 컬러에 합성 (메인 패스 뒤·안개 적용 전)
	void AddCompositePass(FRenderGraph& Graph, const FD3D12RenderTarget& SceneColor, FRGResourceRef ColorRef, FRGResourceRef DepthRef, int32 Timer);
	FRGResourceRef                GetCubeRef() const { return CubeRef; }
	const FD3D12DescriptorHandle& GetCubeSrv() const { return CloudCube.Srv; }

private:
	FCloudConstants Constants; // alignas(16) 상수를 첫 데이터 멤버로 (C4324)

	bool CreatePipelines(bool bForceRecompile);
	bool GenerateNoise();
	void EnsureTargets(uint32 Width, uint32 Height);
	void BindCompute(ID3D12GraphicsCommandList* List, D3D12_GPU_VIRTUAL_ADDRESS Address, const FSkyAtmosphereRenderer& Atmosphere,
	                 D3D12_GPU_VIRTUAL_ADDRESS FogConstants) const;

	FD3D12RHI*          Rhi     = nullptr;
	FShaderLibrary*     Library = nullptr;
	FD3D12RootSignature RootSignature;
	FD3D12RootSignature NoiseRoot;
	FD3D12PipelineState TracePipeline;
	FD3D12PipelineState ResolvePipeline;
	FD3D12PipelineState CubePipeline;
	FD3D12PipelineState CompositePipeline;

	FPersistentTexture ShapeNoise;
	FPersistentTexture DetailNoise;
	FPersistentTexture WeatherMap;
	bool               bNoiseReady = false;
	FPersistentTexture TraceColor;
	FPersistentTexture TraceDepth;
	std::array<FPersistentTexture, 2> HistoryColor;
	std::array<FPersistentTexture, 2> HistoryDepth;
	FPersistentTexture CloudCube;
	uint32             HistoryIndex = 0;     // 다음 추적이 쓸 이력 칸
	uint32             WrittenIndex = 0;     // 이번 렌더가 쓴 이력 칸 (합성이 읽는다)
	bool               bHasHistory  = false;
	FRGResourceRef     CubeRef;

	bool  bActive            = false;
	bool  bWindMoving        = false;
	bool  bAffectEnvironment = true;
	D3D12_GPU_VIRTUAL_ADDRESS ConstantsAddress = 0;
	uint32 FrameCounter = 0;
	std::chrono::steady_clock::time_point StartTime;
};
