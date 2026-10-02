#pragma once

#include "Core/Math/Math.h"
#include "RHI/D3D12/D3D12DescriptorAllocator.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/ShaderTypes.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

class FD3D12RHI;
class FD3D12RenderTarget;
class FScene;
class FShaderLibrary;

// 반사 캡처 큐브 저장 (.ecapture, 굽기 결과 = 엔진 바이너리. 원본 에셋이 아니므로 FAssetCache를 거치지 않는다)
//   머리: "ECAP", Version, Size, MipCount, DXGI 포맷 → 면 6개 × 밉(큰 것부터), 행 빈틈없이 RGBA16F
struct FReflectionCaptureFile
{
	static constexpr uint32 Version = 1;

	uint32                Size     = 0;
	uint32                MipCount = 0;
	std::vector<uint8>    Data; // 면 0 밉 0, 면 0 밉 1, ..., 면 5 밉 N-1

	static size_t GetFaceMipBytes(uint32 Size, uint32 Mip) { const size_t S = std::max<size_t>(1, Size >> Mip); return S * S * 8; }
	size_t        GetExpectedBytes() const;
	bool          Save(const std::filesystem::path& Path) const;
	bool          Load(const std::filesystem::path& Path);
};

// 반사 캡처 관리 (ReflectionMath.h): 큐브 배열 아틀라스(최대 8개, 128px, 프리필터 6밉, 셰이더 t21) + 프레임 캡처 목록(t20).
//   Gather(매 프레임): 씬의 캡처 → 구운 파일을 아틀라스 칸에 올림(경로별 캐시, 처음 한 번 동기 업로드) → 우선순위 정렬 목록 업로드.
//   굽기(FSceneRenderer::BakeReflectionCaptures가 면 6개를 그린 뒤): 원시 큐브 → 하늘과 같은 GGX 프리필터(Ibl.hlsl PrefilterCS)로
//   아틀라스 칸에 직접 쓰고, 리드백해 몇 프레임 뒤(GPU 완료) .ecapture로 저장한다
class FReflectionCaptures
{
public:
	~FReflectionCaptures();

	bool Init(FD3D12RHI& InRhi, FShaderLibrary& Library);
	void Shutdown();
	bool ReloadShaders(bool bForceRecompile);

	// 씬 캡처 목록을 만들고 GPU에 올린다. 반환: 목록 개수 (0이면 메인 패스가 읽지 않음)
	uint32                    Gather(FScene& Scene);
	D3D12_GPU_VIRTUAL_ADDRESS GetCaptureList() const { return CaptureListAddress; }
	const FD3D12DescriptorHandle& GetAtlasSrv() const { return AtlasSrv; }

	// ---- 굽기 (씬 렌더러)
	// 면 Face 그림: SceneColor(HDR, CaptureSize 정사각)를 원시 큐브 면으로 복사하는 그래프 패스 (면 그래프 끝에 등록)
	void AddCopyFacePass(FRenderGraph& Graph, const FD3D12RenderTarget& SceneColor, FRGResourceRef SceneColorRef, uint32 Face);
	// 원시 큐브 → 프리필터 → Path에 해당하는 아틀라스 칸(서브리소스: 칸의 6장 × 모든 밉) + 리드백 저장 예약 (그래프 패스 2개)
	void AddFinishBakePasses(FRenderGraph& Graph, const std::string& AssetPath);
	// 리드백 완료된 굽기를 파일로 쓴다 (매 프레임)
	void ProcessPendingSaves(bool bForce = false); // bForce: GPU 완료가 보장될 때 (종료)
	bool HasPendingSaves() const { return !PendingSaves.empty(); }

private:
	struct FSlot
	{
		std::string AssetPath;
		uint64      LastUsedFrame = 0;
		bool        bLoaded       = false;
	};
	int32 FindOrAssignSlot(const std::string& AssetPath);
	bool  UploadSlot(uint32 Slot, const FReflectionCaptureFile& File);
	bool  CreatePipeline(FD3D12PipelineState& OutPipeline, bool bForceRecompile);

	FD3D12RHI*      Rhi     = nullptr;
	FShaderLibrary* Library = nullptr;

	ComPtr<ID3D12Resource> Atlas;   // Texture2DArray 6 × MaxCaptures, 밉 CaptureMipCount (평소 PIXEL_SHADER_RESOURCE)
	FD3D12DescriptorHandle AtlasSrv; // TEXTURECUBEARRAY
	ComPtr<ID3D12Resource> RawCube; // 굽기 중 면 6개 (밉 1)
	FD3D12DescriptorHandle RawCubeSrv;
	D3D12_RESOURCE_STATES  RawCubeState = D3D12_RESOURCE_STATE_COPY_DEST;

	FD3D12RootSignature PrefilterRoot; // Ibl.hlsl 굽기 레이아웃 (상수 4개, t0, u0, u1)
	FD3D12PipelineState PrefilterPipeline;

	std::vector<FSlot>                     Slots;
	std::vector<FReflectionCaptureGpuData> FrameCaptures;
	D3D12_GPU_VIRTUAL_ADDRESS              CaptureListAddress = 0;
	uint64                                 GatherCount        = 0;

	struct FPendingSave
	{
		std::string                                     AssetPath;
		ComPtr<ID3D12Resource>                          Readback;
		std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints;
		uint64                                          ReadyFrame = 0; // 이 Rhi 프레임 번호부터 읽어도 된다
	};
	std::vector<FPendingSave> PendingSaves;
};
