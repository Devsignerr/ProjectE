#include "Renderer/RenderProfiling.h"

#if E_TRACY

#include "Core/Profiling.h"

#include <d3d12.h>

#pragma warning(push, 0)
#include <tracy/TracyD3D12.hpp>
#pragma warning(pop)

#include <array>
#include <optional>

namespace
{
	struct FRenderProfilingState
	{
		uint32           RefCount     = 0;
		TracyD3D12Ctx    GpuContext   = nullptr;
		ID3D12CommandQueue* Queue     = nullptr; // 컨텍스트를 만든 큐 (다른 큐에서 불리면 GPU 존 없음)
		uint64           LastFrame    = ~0ull;

		// 칸별 위치 (이름은 처음 연 이름 — ERenderTimer 이름은 정적 문자열)
		std::array<FProfileSourceLocation, RenderProfiling::MaxSlots>    CpuLocations{};
		std::array<tracy::SourceLocationData, RenderProfiling::MaxSlots> GpuLocations{};
		std::array<bool, RenderProfiling::MaxSlots>                      bLocationReady{};

		std::array<FProfileZone, RenderProfiling::MaxSlots>                         CpuZones{};
		std::array<std::optional<tracy::D3D12ZoneScope>, RenderProfiling::MaxSlots> GpuZones;
	};

	FRenderProfilingState& GetState()
	{
		static FRenderProfilingState State;
		return State;
	}

	void DestroyContext(FRenderProfilingState& State)
	{
		for (std::optional<tracy::D3D12ZoneScope>& Zone : State.GpuZones)
		{
			Zone.reset();
		}
		if (State.GpuContext != nullptr)
		{
			TracyD3D12Destroy(State.GpuContext);
			State.GpuContext = nullptr;
			State.Queue      = nullptr;
		}
		State.LastFrame = ~0ull;
	}
} // namespace

namespace RenderProfiling
{
	void AddRef()
	{
		++GetState().RefCount;
	}

	void Release()
	{
		FRenderProfilingState& State = GetState();
		if (State.RefCount > 0 && --State.RefCount == 0)
		{
			DestroyContext(State); // 호출자가 GPU Flush를 마친 뒤 (컨텍스트 소멸자가 마지막 페이로드 펜스를 기다린다)
		}
	}

	void BeginFrame(ID3D12Device* Device, ID3D12CommandQueue* Queue, uint64 FrameNumber)
	{
		FRenderProfilingState& State = GetState();
		if (!Profiling::IsStarted() || State.RefCount == 0 || Device == nullptr || Queue == nullptr)
		{
			return;
		}
		if (State.GpuContext == nullptr)
		{
			State.GpuContext = TracyD3D12Context(Device, Queue);
			State.Queue      = Queue;
			static constexpr char ContextName[] = "D3D12 Graphics";
			TracyD3D12ContextName(State.GpuContext, ContextName, static_cast<uint16_t>(sizeof(ContextName) - 1));
		}
		if (State.Queue != Queue || State.LastFrame == FrameNumber)
		{
			return;
		}
		// 지난 프레임까지 기록한 쿼리를 한 페이로드로 묶고(지난 명령 목록은 이미 제출됨) 끝난 페이로드를 읽는다
		State.LastFrame = FrameNumber;
		TracyD3D12NewFrame(State.GpuContext);
		TracyD3D12Collect(State.GpuContext);
	}

	void BeginZone(ID3D12GraphicsCommandList* CommandList, uint32 Slot, const char* Name, bool bGpu)
	{
		FRenderProfilingState& State = GetState();
		if (Slot >= MaxSlots || !Profiling::IsStarted())
		{
			return;
		}
		if (!State.bLocationReady[Slot])
		{
			State.CpuLocations[Slot]   = { Name, "FSceneRenderer", __FILE__, Slot, 0 };
			State.GpuLocations[Slot]   = { Name, "FSceneRenderer", __FILE__, Slot, 0 };
			State.bLocationReady[Slot] = true;
		}
		EndZone(Slot); // 닫히지 않은 칸 (예외 경로) 정리
		State.CpuZones[Slot] = Profiling::BeginZone(&State.CpuLocations[Slot]);
		if (bGpu && State.GpuContext != nullptr && CommandList != nullptr)
		{
			State.GpuZones[Slot].emplace(State.GpuContext, CommandList, &State.GpuLocations[Slot], true);
		}
	}

	void EndZone(uint32 Slot)
	{
		FRenderProfilingState& State = GetState();
		if (Slot >= MaxSlots)
		{
			return;
		}
		State.GpuZones[Slot].reset();
		Profiling::EndZone(State.CpuZones[Slot]);
		State.CpuZones[Slot] = {};
	}
} // namespace RenderProfiling

#endif
