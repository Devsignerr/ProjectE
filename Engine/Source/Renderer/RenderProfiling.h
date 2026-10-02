#pragma once

#include "Core/CoreTypes.h"

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

// 씬 렌더러 측정 구간(ERenderTimer) → Tracy CPU 존 + GPU 존(TracyD3D12). E_TRACY가 꺼져 있으면 빈 함수.
//   GPU 컨텍스트는 그래픽스 큐 하나에 프로세스 전역 하나 (렌더러 여러 개가 공유 — Tracy GPU 컨텍스트 수 제한 255).
//   AddRef/Release: FSceneRenderer Init/Shutdown (Release는 GPU Flush 뒤 — 마지막이면 컨텍스트 파괴).
//   BeginFrame: Render마다 부르지만 Rhi 프레임 번호가 바뀔 때 한 번만 수집 + 새 페이로드 (지난 프레임 명령이 제출된 뒤라야 맞다).
//   구간은 메인 스레드에서 순서대로 (같은 칸을 겹쳐 열지 않는다). Slot = ERenderTimer 번호, Name은 정적 문자열
namespace RenderProfiling
{
	inline constexpr uint32 MaxSlots = 32;

#if E_TRACY
	void AddRef();
	void Release();
	void BeginFrame(ID3D12Device* Device, ID3D12CommandQueue* Queue, uint64 FrameNumber);
	void BeginZone(ID3D12GraphicsCommandList* CommandList, uint32 Slot, const char* Name, bool bGpu);
	void EndZone(uint32 Slot);
#else
	inline void AddRef() {}
	inline void Release() {}
	inline void BeginFrame(ID3D12Device*, ID3D12CommandQueue*, uint64) {}
	inline void BeginZone(ID3D12GraphicsCommandList*, uint32, const char*, bool) {}
	inline void EndZone(uint32) {}
#endif
} // namespace RenderProfiling
