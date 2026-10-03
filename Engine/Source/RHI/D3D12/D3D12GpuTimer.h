#pragma once

#include "RHI/D3D12/D3D12Common.h"

// GPU 구간 시간 측정 (타임스탬프 쿼리).
//   프레임 슬롯(RHI 백버퍼 칸)마다 쿼리 범위와 리드백 영역을 따로 둔다. 슬롯 s를 다시 기록할 때는 RHI BeginFrame이
//   그 슬롯의 펜스를 이미 기다렸으므로, 같은 슬롯의 지난 결과를 그때 읽는다 → 결과는 슬롯 수만큼 늦게 나온다.
//   한 프레임에 BeginFrame이 두 번 불리면(같은 렌더러로 여러 뷰를 그림) 두 번째부터는 기록하지 않는다.
class FD3D12GpuTimer
{
public:
	static constexpr uint32 MaxScopes = 32; // ERenderTimer 칸 수 이상 (Phase 50에서 24 → 32: 대기·구름·물 + 레이 트레이싱 3칸)
	static constexpr uint32 MaxSlots  = 4;

	~FD3D12GpuTimer();

	bool Init(ID3D12Device* Device, ID3D12CommandQueue* Queue, uint32 InSlotCount, const wchar_t* DebugName);
	void Shutdown();

	// 이번 프레임 기록 시작 (Slot = RHI 프레임 슬롯, FrameNumber = RHI 프레임 번호). 같은 슬롯의 지난 결과를 읽어 둔다.
	// false면 이번 호출은 기록하지 않는다 (BeginScope/EndScope/EndFrame은 그대로 불러도 된다)
	bool BeginFrame(uint32 Slot, uint64 FrameNumber);
	void BeginScope(ID3D12GraphicsCommandList* CommandList, uint32 Scope);
	void EndScope(ID3D12GraphicsCommandList* CommandList, uint32 Scope);
	// 이번 프레임에 쓴 구간의 쿼리를 리드백 버퍼로 옮긴다
	void EndFrame(ID3D12GraphicsCommandList* CommandList);

	// 가장 최근에 읽은 결과 (ms). 측정하지 않은 구간은 0
	float GetScopeMs(uint32 Scope) const { return Scope < MaxScopes ? ResultsMs[Scope] : 0.0f; }
	bool  HasResults() const { return bHasResults; }

private:
	void ReadSlot(uint32 Slot);

	ComPtr<ID3D12QueryHeap> QueryHeap;
	ComPtr<ID3D12Resource>  Readback;
	double                  MsPerTick    = 0.0;
	uint32                  SlotCount    = 0;
	uint32                  CurrentSlot  = 0;
	uint64                  LastFrame    = ~0ull;
	bool                    bRecording   = false;
	bool                    bHasResults  = false;
	uint32                  SlotScopeMask[MaxSlots] = {}; // 슬롯별로 기록을 끝낸(End까지) 구간 비트
	uint32                  OpenScopeMask = 0;            // 이번 프레임 Begin만 한 구간
	float                   ResultsMs[MaxScopes] = {};
};
