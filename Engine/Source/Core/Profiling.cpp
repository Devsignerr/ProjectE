#include "Core/Profiling.h"

#if E_TRACY

#include "Core/Log.h"

#pragma warning(push, 0)
#include <tracy/TracyC.h>
#pragma warning(pop)

#include <atomic>
#include <cstddef>

static_assert(sizeof(FProfileSourceLocation) == sizeof(___tracy_source_location_data));
static_assert(offsetof(FProfileSourceLocation, Name) == offsetof(___tracy_source_location_data, name));
static_assert(offsetof(FProfileSourceLocation, Function) == offsetof(___tracy_source_location_data, function));
static_assert(offsetof(FProfileSourceLocation, File) == offsetof(___tracy_source_location_data, file));
static_assert(offsetof(FProfileSourceLocation, Line) == offsetof(___tracy_source_location_data, line));
static_assert(offsetof(FProfileSourceLocation, Color) == offsetof(___tracy_source_location_data, color));
static_assert(sizeof(FProfileZone) == sizeof(TracyCZoneCtx));

namespace
{
	// 메인 스레드가 Startup/Shutdown. 다른 스레드의 존은 시작된 동안만 Tracy로 간다
	std::atomic<bool> GProfilerStarted = false;
} // namespace

namespace Profiling
{
	void Startup()
	{
		if (GProfilerStarted)
		{
			return;
		}
		___tracy_startup_profiler();
		GProfilerStarted = true;
		___tracy_set_thread_name("Main");
		E_LOG(LogCore, Display, "Tracy 프로파일러 시작 (뷰어 0.11.1로 127.0.0.1에 연결하면 기록)");
	}

	void Shutdown()
	{
		if (!GProfilerStarted)
		{
			return;
		}
		GProfilerStarted = false;
		___tracy_shutdown_profiler();
	}

	bool IsStarted()
	{
		return GProfilerStarted;
	}

	bool IsConnected()
	{
		return GProfilerStarted && ___tracy_connected() != 0;
	}

	FProfileZone BeginZone(const FProfileSourceLocation* Location)
	{
		if (!GProfilerStarted)
		{
			return {};
		}
		const TracyCZoneCtx Context = ___tracy_emit_zone_begin(reinterpret_cast<const ___tracy_source_location_data*>(Location), 1);
		return { Context.id, Context.active };
	}

	void EndZone(FProfileZone Zone)
	{
		if (!GProfilerStarted || Zone.Active == 0)
		{
			return;
		}
		___tracy_emit_zone_end(TracyCZoneCtx{ Zone.Id, Zone.Active });
	}

	void FrameMark()
	{
		if (GProfilerStarted)
		{
			___tracy_emit_frame_mark(nullptr);
		}
	}

	void Plot(const char* Name, double Value)
	{
		if (GProfilerStarted)
		{
			___tracy_emit_plot(Name, Value);
		}
	}

	void SetThreadName(const char* Name)
	{
		if (GProfilerStarted)
		{
			___tracy_set_thread_name(Name);
		}
	}
} // namespace Profiling

#endif
