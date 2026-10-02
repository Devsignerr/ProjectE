#pragma once

#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "Renderer/Material.h"

#include <memory>
#include <unordered_map>
#include <vector>

class FD3D12DynamicUploadBuffer;
class FD3D12RHI;
class FShaderLibrary;

// 머티리얼 렌더 도우미 (Phase 49 사이드 — 그래프 머티리얼 셰이더 변형/상수/깊이 PSO).
//   그래프 머티리얼 셰이더 변형 = 엔진 셰이더(Mesh.hlsl/Shadow.hlsl 픽셀 엔트리) + 디파인 E_MATERIAL_GRAPH + 가상 포함 파일
//   MaterialGraph.generated.hlsli(= FMaterialShader::Hlsl). FShaderLibrary 캐시 키·쿠킹 파일명에 생성 소스 해시가 들어가므로
//   같은 그래프(같은 해시)는 머티리얼이 달라도 바이트코드/PSO를 공유하고, ProjectECook이 미리 쿠킹하면 패키지에서 DXC 없이 돈다.
//   정점 셰이더는 머티리얼과 무관(기본 바이트코드) — 사전 패스와 메인이 같은 VS여야 깊이 EQUAL이 맞는다.
namespace MaterialRender
{
	// 머티리얼 시간 (초, 프로세스 시작 기준 — 1시간마다 되감김). 그래프 Time/Panner 노드
	float GetMaterialTime();

	// 머티리얼 상수 업로드 (b2): 고정 PBR = FMaterialConstants, 그래프 = FMaterialGraphHeader + GraphConstants
	D3D12_GPU_VIRTUAL_ADDRESS UploadMaterialConstants(FD3D12DynamicUploadBuffer& DynamicBuffer, const FMaterial& Material);

	// 그래프 머티리얼 픽셀 셰이더 변형 (File/Entry는 엔진 셰이더, 디파인 + 가상 파일 추가)
	FShaderCompileDesc MakeGraphShaderDesc(const wchar_t* FileName, const wchar_t* EntryPoint, EShaderStage Stage, const FMaterialShader& Shader);
	// 그래프 머티리얼이 쓸 수 있는 모든 픽셀 셰이더 변형 (메시 패스 엔트리 6개 + Masked 그림자 1개) — 쿠킹/미리 컴파일용
	std::vector<FShaderCompileDesc> GetGraphShaderDescs(const FMaterialShader& Shader);
} // namespace MaterialRender

// 깊이 패스(그림자) 그래프 머티리얼 Masked PSO 캐시: 셰이더 해시 × (정적/스킨). 기본 설정(루트 시그니처·바이어스·깊이 클립)이 바뀌면 Reset
class FMaterialDepthPipelines
{
public:
	~FMaterialDepthPipelines();

	void Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary, const wchar_t* InDebugName);
	// BaseDesc: 셰이더·입력 레이아웃을 뺀 PSO 설정. 바뀌면 캐시를 비운다(지연 해제)
	void SetBaseDesc(const FGraphicsPipelineDesc& Desc);
	// 실패하면 nullptr (그 해시는 다시 시도하지 않는다 — Reset까지)
	ID3D12PipelineState* Get(const FMaterialShader& Shader, bool bSkinned);
	void                 Reset();
	void                 Shutdown();

private:
	struct FEntry
	{
		FD3D12PipelineState Pipelines[2];
		bool                bTried[2]  = {};
		bool                bFailed[2] = {};
	};
	struct FState
	{
		FShaderLibrary*                                     ShaderLibrary = nullptr;
		const wchar_t*                                      DebugName     = L"MaterialDepthPipeline";
		FGraphicsPipelineDesc                               BaseDesc;
		std::unordered_map<uint64, std::unique_ptr<FEntry>> Entries;
	};

	// 16바이트 (포인터 2개): 이 객체를 품은 렌더러의 정렬/채움이 바뀌지 않게 상태는 힙에 둔다 (C4324)
	FD3D12RHI*              Rhi = nullptr;
	std::unique_ptr<FState> State;
};