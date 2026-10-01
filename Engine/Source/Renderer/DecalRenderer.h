#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RenderTarget.h"

#include <memory>
#include <vector>

class FCamera;
class FD3D12RHI;
class FResourceManager;
class FScene;
class FScreenPassRootSignature;
class FShaderLibrary;
struct FFrustum;

// 박스 투영 데칼 (FDecalComponent) → DBuffer 3장 (Decal.hlsl, 식은 Renderer/DecalMath.h).
//   방식 결정 (Phase 33-4): 포워드 렌더러 + 깊이 사전 패스 구조이므로 "사전 패스 기반 DBuffer"(UE4 DBuffer 데칼 방식)를 택했다.
//     - 클러스터드 데칼은 메인 픽셀 셰이더가 데칼마다 다른 텍스처를 읽어야 해 바인드리스(동적 인덱스 디스크립터)가 필요하다.
//       DBuffer는 데칼마다 보통의 머티리얼 테이블로 상자 하나를 그리면 되고, 메인 패스는 고정된 3장만 읽는다.
//     - 사전 패스가 깊이·법선을 이미 만들므로 추가 깊이 패스가 없다. 비용 = 데칼 상자 래스터 + 전체 화면 RGBA8 3장.
//   흐름: 깊이 사전 패스 → Render(지우기 + 데칼 상자, SortOrder 오름차순 = 큰 값이 위) → 메인 패스가 t17~t19로 읽어
//     베이스색/노멀/거칠기·금속에 섞는다. 보이는 데칼이 없으면 지우지도 않고 false (메인 패스가 읽지 않음)
class FDecalRenderer
{
public:
	static constexpr DXGI_FORMAT FormatA = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // 베이스색
	static constexpr DXGI_FORMAT FormatB = DXGI_FORMAT_R8G8B8A8_UNORM;      // 법선 * 0.5 + 0.5
	static constexpr DXGI_FORMAT FormatC = DXGI_FORMAT_R8G8B8A8_UNORM;      // R 거칠기, G 금속

	~FDecalRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library, const FScreenPassRootSignature& InRoot);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	void EnsureTargets(uint32 Width, uint32 Height);

	// 씬 깊이(SceneDepth의 깊이, DEPTH_WRITE로 받는다) + 법선으로 데칼을 그린다. Camera = 깊이를 그린 카메라(지터 포함).
	// 데칼 머티리얼 경로는 여기서 해석한다. 그린 데칼이 있으면 true
	bool Render(FScene& Scene, FResourceManager& Resources, const FCamera& Camera, const FFrustum& Frustum, const FD3D12RenderTarget& SceneDepth,
	            const FD3D12RenderTarget& SceneNormal);

	const FD3D12RenderTarget& GetTarget(uint32 Index) const { return *Targets[Index]; }
	uint32                    GetDrawnCount() const { return DrawnCount; }

private:
	bool CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);

	FD3D12RHI*                      Rhi     = nullptr;
	FShaderLibrary*                 Library = nullptr;
	const FScreenPassRootSignature* Root    = nullptr; // 비소유: 씬 렌더러 소유
	FD3D12PipelineState             Pipeline;

	std::unique_ptr<FD3D12RenderTarget> Targets[3];
	uint32                              DrawnCount = 0;

	struct FVisibleDecal
	{
		int32  SortOrder = 0;
		uint32 Entity    = 0; // 같은 순서 안 안정 정렬
		uint32 Index     = 0;
	};
	std::vector<FVisibleDecal> Visible;
};
