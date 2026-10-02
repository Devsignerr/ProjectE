#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/AtmosphereMath.h"
#include "Renderer/PersistentTexture.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/ShaderTypes.h"

#include <array>

class FCamera;
class FD3D12RHI;
class FIblRenderer;
class FScene;
class FShaderLibrary;
struct FSkyAtmosphereComponent;

// 물리 기반 대기 (Phase 49, 식은 Renderer/AtmosphereMath.h, 셰이더 Atmosphere.hlsli/SkyAtmosphere.hlsl).
//   Prepare (씬 렌더마다, CPU): 씬의 첫 FSkyAtmosphereComponent + 첫 방향광(태양) → 상수, 태양 투과율(방향광 색), 공중 원근 매개변수
//   AddLutPasses: 투과율/다중 산란 LUT(매질이 바뀔 때만) + 하늘 뷰 LUT(매 렌더) — 계산 패스
//   메인 패스: DeclareSkyReads로 LUT 읽기 선언 → 람다에서 RenderSky (하늘 상자 대신, 깊이 1 LESS_EQUAL)
//   AddEnvironmentPasses (IBL 실시간 갱신): 하늘 큐브(128, 대기 + 구름) → 밉 → 조도/프리필터를 3단계로 나눠 프레임마다 한 단계
//     (단계 0 하늘 큐브 + 밉 + 조도, 1 프리필터 밉 0~2, 2 프리필터 밉 3~5). 결과는 이중 버퍼(조도·프리필터 2벌 + 조명 표 2개)의
//     뒤쪽에 쓰고 다 끝난 다음 프레임 Prepare에서 앞뒤를 바꿔 FIblRenderer 조명 표를 덮는다 (메시/지형은 GetLightingTable 그대로).
//     적분은 Ibl.hlsl IrradianceCS/PrefilterCS/DownsampleCS를 그대로 쓴다 (필터드 중요도 샘플링, 표본 수만 r.SkyAtmosphere.IblSamples)
//   갱신 조건: 태양 방향(0.05도)·매질·카메라 고도(50m)·구름(움직이면 계속)이 바뀌었을 때. 반사 캡처 굽는 중에는 갱신하지 않는다
class FSkyAtmosphereRenderer
{
public:
	~FSkyAtmosphereRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 씬의 대기 컴포넌트·태양 → 이번 렌더 상수. 대기가 없으면 false (나머지 함수는 아무것도 하지 않음)
	bool Prepare(FScene& Scene, const FCamera& Camera, bool bEnabled = true);
	bool IsActive() const { return bActive; }

	// 방향광 색에 곱할 대기 투과율 (bAffectSunLight가 아니거나 비활성이면 1). 지평선 아래로 원반이 지는 만큼 0으로
	FVector3 GetSunLightTransmittance() const { return bActive ? SunLightTransmittance : FVector3::OneVector; }
	// 씬 렌더러의 방향광 상수에 대기 적용: 태양 투과율을 곱하거나, 태양이 완전히 지고 달빛이 켜졌으면 달 방향·달빛으로 바꾼다
	void ApplyToDirectionalLight(struct FDirectionalLightConstants& Light) const;
	// 공중 원근 (Fog.hlsli 상수로 들어간다). 비활성이거나 배율 0이면 nullptr
	const FAtmosphereMath::FAerialParams* GetAerialParams() const { return bActive && bAerialEnabled ? &AerialParams : nullptr; }
	const FAtmosphereMath::FParams&       GetParams() const { return Params; }
	D3D12_GPU_VIRTUAL_ADDRESS             GetConstantsAddress() const { return ConstantsAddress; }
	const FAtmosphereConstants&           GetConstants() const { return Constants; }

	// LUT 계산 패스 (Prepare 뒤). Timer = ERenderTimer 번호
	void AddLutPasses(FRenderGraph& Graph, ERGQueue Queue, int32 Timer);
	// 이 렌더 그래프의 LUT 참조 (AddLutPasses 뒤 — 읽는 패스 선언용)
	FRGResourceRef GetTransmittanceRef() const { return TransmittanceRef; }
	FRGResourceRef GetMultiScatteringRef() const { return MultiScatteringRef; }
	FRGResourceRef GetSkyViewRef() const { return SkyViewRef; }
	const FD3D12DescriptorHandle& GetTransmittanceSrv() const { return TransmittanceLut.Srv; }
	const FD3D12DescriptorHandle& GetMultiScatteringSrv() const { return MultiScatteringLut.Srv; }
	const FD3D12DescriptorHandle& GetSkyViewSrv() const { return SkyViewLut.Srv; }
	// 하늘 패스가 읽는 LUT 선언 (픽셀 셰이더)
	void DeclareSkyReads(FRenderGraph::FPassBuilder& Pass) const;
	// 메인 패스 람다 안: 하늘 그리기 (씬 컬러 + 깊이가 바인딩된 상태). 루트 시그니처를 바꾼다
	void RenderSky(ID3D12GraphicsCommandList* CommandList) const;

	// IBL 실시간 갱신 패스 (하늘 뷰 LUT 뒤). CloudCube = 구름 저해상도 큐브(rgb 산란, a 투과율, 없으면 무효 참조), CloudCubeSrv 같음.
	// bAllowUpdate = false면 이번 렌더는 건너뜀 (반사 캡처 굽는 중·미리보기)
	void AddEnvironmentPasses(FRenderGraph& Graph, FRGResourceRef CloudCube, const FD3D12DescriptorHandle& CloudCubeSrv, bool bCloudsChanging,
	                          bool bAllowUpdate, int32 Timer);
	// 대기 IBL을 FIblRenderer에 덮는다 (Prepare 뒤, 메시 패스 전). 비활성이면 덮기 해제
	void ApplyEnvironmentOverride(FIblRenderer& Ibl) const;
	// 다음 갱신을 강제 (매질/구름 설정 변경 등)
	void InvalidateEnvironment() { bEnvironmentDirty = true; }

private:
	FAtmosphereConstants Constants; // alignas(16) 상수를 첫 데이터 멤버로 (C4324 방지)

	bool CreatePipelines(bool bForceRecompile);
	void BuildConstants(const FSkyAtmosphereComponent& Component, const FCamera& Camera, const FVector3& SunDirection, const FVector3& SunIlluminance);
	void EnsureResources();
	void AllocateTables();

	FD3D12RHI*      Rhi     = nullptr;
	FShaderLibrary* Library = nullptr;

	FD3D12RootSignature RootSignature; // b0 상수, b1 패스 상수 4개, t0~t3 표, u0~u1 표, s0 선형 클램프
	FD3D12RootSignature IblRoot;       // Ibl.hlsl 적분 (b0 상수 4개, t0, u0, u2, s0, s1)
	FD3D12PipelineState TransmittancePipeline;
	FD3D12PipelineState MultiScatteringPipeline;
	FD3D12PipelineState SkyViewPipeline;
	FD3D12PipelineState SkyCubePipeline;
	FD3D12PipelineState SkyPipeline; // 그래픽스 (하늘 패스)
	FD3D12PipelineState DownsamplePipeline;
	FD3D12PipelineState IrradiancePipeline;
	FD3D12PipelineState PrefilterPipeline;

	FPersistentTexture TransmittanceLut;
	FPersistentTexture MultiScatteringLut;
	FPersistentTexture SkyViewLut;
	FPersistentTexture SkyCube;                 // IBL 원본 (128, 전체 밉)
	std::array<FPersistentTexture, 2> Irradiance; // 이중 버퍼
	std::array<FPersistentTexture, 2> Prefilter;
	FPersistentTexture DummyCloudCube;          // 구름 없을 때 t3 (1x1 큐브, 투과 1)
	std::array<FD3D12DescriptorHandle, 2> LightingTables; // 조도 / 프리필터 / BRDF (FIblRenderer 조명 표와 같은 배치)
	ID3D12Resource* TableBrdfSource = nullptr;  // 조명 표에 기록한 BRDF LUT (FIblRenderer 소유)

	// 이번 렌더
	bool                            bActive          = false;
	bool                            bAerialEnabled   = false;
	FAtmosphereMath::FParams        Params;
	D3D12_GPU_VIRTUAL_ADDRESS       ConstantsAddress = 0;
	FVector3                        SunLightTransmittance = FVector3::OneVector;
	bool                            bUseMoonLight = false; // 이번 렌더 방향광 = 달
	FAtmosphereMath::FAerialParams  AerialParams;
	bool                            bRealtimeEnvironment = true;
	FRGResourceRef                  TransmittanceRef;
	FRGResourceRef                  MultiScatteringRef;
	FRGResourceRef                  SkyViewRef;

	// LUT 갱신 판정 (매질이 바뀌었을 때만 투과율/다중 산란)
	bool                     bLutsValid = false;
	FAtmosphereMath::FParams LutParams;

	// 공중 원근 다중 산란 (CPU, 태양 각·고도가 바뀔 때만 다시)
	float    CachedMsSunCos   = -2.0f;
	float    CachedMsHeight   = -1.0f;
	FVector3 CachedMultiScattering;
	FAtmosphereMath::FParams CachedMsParams;

	// IBL 갱신 상태
	int32    EnvironmentStage   = -1;   // -1 쉼, 0~2 진행 중 단계
	uint32   BackIndex          = 1;    // 쓰는 쪽 (앞 = 1 - BackIndex)
	bool     bHasFront          = false; // 앞쪽에 한 번이라도 결과가 있다
	bool     bPublishPending    = false; // 다음 Prepare에서 앞뒤를 바꾼다
	bool     bEnvironmentDirty  = true;
	FVector3 PublishedSunDirection = FVector3::ZeroVector; // 마지막으로 갱신을 시작한 기준
	float    PublishedHeight    = -1.0f;
	FAtmosphereMath::FParams PublishedParams;
	FVector3 PublishedSunIlluminance;
};
