#pragma once

#include "Core/CoreTypes.h"
#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12Buffer.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"

#include <vector>

class FCamera;
class FD3D12RHI;
class FShaderLibrary;
struct FRenderOutput;

// 에디터 뷰포트의 내비메시/이동 경로 디버그 표시 (NavMeshDebug.hlsl).
// 씬 깊이로 가려지며(쓰기 없음) 알파 블렌드로 겹쳐 그린다. 에디터 그리드와 같은 단계(씬 렌더 뒤, 선택 아웃라인 앞).
// 내비메시 면/테두리는 SetNavMeshTriangles에서 정적 GPU 버퍼로 한 번 올리고, 경로 선만 매 프레임 동적 업로드 버퍼로 올린다
class FNavMeshDebugRenderer
{
public:
	static constexpr uint32 MaxPathVertices = 4096; // 프레임당 경로 선 정점 상한 (동적 업로드 버퍼 공유 — 넘치면 자른다)

	~FNavMeshDebugRenderer();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& InShaderLibrary);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 내비메시 면 (엔진 좌표 삼각형 목록, FNavMesh::GetDebugTriangles). 바뀔 때만 부른다 (동기 업로드). 빈 목록 = 지움
	void SetNavMeshTriangles(const std::vector<FVector3>& Triangles);
	bool HasNavMesh() const { return FaceVertexCount > 0; }

	// 이번 프레임에 그릴 경로 (선분 목록으로 누적, Render 후 비운다)
	void AddPath(const std::vector<FVector3>& Points);

	// Output: 렌더 타깃 상태의 출력. SceneDepthDsv: 같은 크기의 씬 깊이 (DEPTH_WRITE, 읽기 전용으로 사용)
	void Render(const FCamera& Camera, const FRenderOutput& Output, D3D12_CPU_DESCRIPTOR_HANDLE SceneDepthDsv, bool bDrawNavMesh);

private:
	struct FDebugVertex
	{
		FVector3 Position;
		uint32   Color = 0; // R8G8B8A8 (메모리 순서 R, G, B, A)
	};
	static_assert(sizeof(FDebugVertex) == 16);

	bool CreatePipelines(FD3D12PipelineState& OutFaces, FD3D12PipelineState& OutLines, bool bForceRecompile);

	FD3D12RHI*                Rhi           = nullptr; // 비소유
	FShaderLibrary*           ShaderLibrary = nullptr; // 비소유
	FD3D12RootSignature       RootSignature;
	FD3D12PipelineState       FacePipeline;
	FD3D12PipelineState       LinePipeline;
	FD3D12Buffer              FaceBuffer;
	FD3D12Buffer              EdgeBuffer;
	uint32                    FaceVertexCount = 0;
	uint32                    EdgeVertexCount = 0;
	std::vector<FDebugVertex> PathVertices;
};
