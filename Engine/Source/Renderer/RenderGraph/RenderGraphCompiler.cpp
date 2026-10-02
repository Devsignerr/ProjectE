#include "Renderer/RenderGraph/RenderGraphCompiler.h"

#include "Core/Assert.h"

#include <algorithm>
#include <format>
#include <iterator>

bool RGAccess::IsValidCombination(ERGAccess Access)
{
	const uint32 Value = Bits(Access);
	if (Value == 0)
	{
		return false;
	}
	const uint32 Writes  = Value & WriteMask;
	const uint32 Singles = Value & SingleMask;
	if (Writes != 0)
	{
		return Value == Writes && (Writes & (Writes - 1)) == 0; // 쓰기 하나 단독
	}
	if (Singles != 0)
	{
		return Value == Singles && (Singles & (Singles - 1)) == 0;
	}
	return true; // 읽기 조합
}

const char* RGAccess::ToString(ERGAccess Access)
{
	switch (Access)
	{
	case ERGAccess::None:         return "None";
	case ERGAccess::RenderTarget: return "RenderTarget";
	case ERGAccess::DepthWrite:   return "DepthWrite";
	case ERGAccess::DepthRead:    return "DepthRead";
	case ERGAccess::SrvPixel:     return "SrvPixel";
	case ERGAccess::SrvNonPixel:  return "SrvNonPixel";
	case ERGAccess::Uav:          return "Uav";
	case ERGAccess::CopySource:   return "CopySource";
	case ERGAccess::CopyDest:     return "CopyDest";
	case ERGAccess::IndirectArgs: return "IndirectArgs";
	case ERGAccess::Present:      return "Present";
	case ERGAccess::Common:       return "Common";
	case ERGAccess::SrvAll:       return "SrvPixel|SrvNonPixel";
	default:                      break;
	}
	if (Access == (ERGAccess::DepthRead | ERGAccess::SrvPixel))
	{
		return "DepthRead|SrvPixel";
	}
	if (Access == (ERGAccess::DepthRead | ERGAccess::SrvAll))
	{
		return "DepthRead|SrvPixel|SrvNonPixel";
	}
	if (Access == (ERGAccess::SrvAll | ERGAccess::CopySource))
	{
		return "SrvPixel|SrvNonPixel|CopySource";
	}
	if (Access == (ERGAccess::SrvNonPixel | ERGAccess::IndirectArgs))
	{
		return "SrvNonPixel|IndirectArgs";
	}
	return "(조합)";
}

namespace
{
	// 패스의 서브리소스 하나에 대한 (합친) 접근
	struct FSubAccess
	{
		uint32    Global     = 0; // 리소스 시작 + 서브리소스 번호
		uint32    Resource   = 0;
		uint32    Sub        = 0;
		ERGAccess Access     = ERGAccess::None;
		bool      bOverwrite = true;
	};

	bool IsWrite(ERGAccess Access) { return RGAccess::HasWrite(Access); }

	// 읽기 상태 Current가 읽기 요구 Required를 이미 포함하는가
	bool CoversRead(ERGAccess Current, ERGAccess Required)
	{
		const uint32 Cur = RGAccess::Bits(Current);
		const uint32 Req = RGAccess::Bits(Required);
		return Cur != 0 && (Cur & ~RGAccess::ReadMask) == 0 && (Cur & Req) == Req;
	}
} // namespace

FRGCompileResult RenderGraphCompiler::Compile(const std::vector<FRGCompileResource>& Resources, const std::vector<FRGCompilePass>& Passes,
                                              const FRGCompileOptions& Options)
{
	FRGCompileResult Result;
	const uint32     ResourceCount = static_cast<uint32>(Resources.size());
	const uint32     PassCount     = static_cast<uint32>(Passes.size());
	Result.Passes.resize(PassCount);
	Result.Lifetimes.resize(ResourceCount);

	// 리소스별 서브리소스 시작 번호 (전역 번호 = 시작 + 서브리소스)
	std::vector<uint32> SubBase(ResourceCount + 1, 0);
	for (uint32 Index = 0; Index < ResourceCount; ++Index)
	{
		SubBase[Index + 1] = SubBase[Index] + std::max(1u, Resources[Index].GetSubresourceCount());
	}
	const uint32 GlobalCount = SubBase[ResourceCount];

	// 1) 접근을 서브리소스 단위로 펼치고 같은 패스·같은 서브리소스는 합친다
	std::vector<std::vector<FSubAccess>> PassSubs(PassCount);
	for (uint32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
	{
		const FRGCompilePass&    Pass = Passes[PassIndex];
		std::vector<FSubAccess>& Subs = PassSubs[PassIndex];
		for (const FRGCompileAccess& Access : Pass.Accesses)
		{
			if (Access.Resource >= ResourceCount || Access.Access == ERGAccess::None)
			{
				Result.Errors.push_back(std::format("패스 '{}': 잘못된 리소스/접근", Pass.Name));
				continue;
			}
			const FRGCompileResource& Resource  = Resources[Access.Resource];
			const uint32              MipCount  = std::max(1u, Resource.MipCount);
			const uint32              Slices    = std::max(1u, Resource.ArraySize);
			const uint32              MipBegin  = std::min(Access.Range.FirstMip, MipCount);
			const uint32              MipEnd    = Access.Range.MipCount == FRGSubresourceRange::Remaining ? MipCount
			                                                                                              : std::min(MipCount, MipBegin + Access.Range.MipCount);
			const uint32              SliceBegin = std::min(Access.Range.FirstSlice, Slices);
			const uint32              SliceEnd   = Access.Range.SliceCount == FRGSubresourceRange::Remaining
			                                           ? Slices
			                                           : std::min(Slices, SliceBegin + Access.Range.SliceCount);
			if (MipBegin >= MipEnd || SliceBegin >= SliceEnd)
			{
				Result.Errors.push_back(std::format("패스 '{}': 리소스 '{}' 서브리소스 범위가 비었습니다", Pass.Name, Resource.Name));
				continue;
			}
			for (uint32 Slice = SliceBegin; Slice < SliceEnd; ++Slice)
			{
				for (uint32 Mip = MipBegin; Mip < MipEnd; ++Mip)
				{
					const uint32 Sub    = Mip + Slice * MipCount;
					const uint32 Global = SubBase[Access.Resource] + Sub;
					auto Found = std::find_if(Subs.begin(), Subs.end(), [Global](const FSubAccess& Entry) { return Entry.Global == Global; });
					if (Found == Subs.end())
					{
						Subs.push_back({ Global, Access.Resource, Sub, Access.Access, Access.bOverwrite });
					}
					else
					{
						Found->Access |= Access.Access;
						Found->bOverwrite = Found->bOverwrite && Access.bOverwrite;
					}
				}
			}
		}
		for (const FSubAccess& Sub : Subs)
		{
			if (!RGAccess::IsValidCombination(Sub.Access))
			{
				Result.Errors.push_back(std::format("패스 '{}': 리소스 '{}' 서브리소스 {}에 함께 쓸 수 없는 접근 ({:#x})", Pass.Name,
				                                    Resources[Sub.Resource].Name, Sub.Sub, RGAccess::Bits(Sub.Access)));
			}
			if (Pass.Queue == ERGQueue::AsyncCompute && !RGAccess::IsComputeLegal(Sub.Access))
			{
				Result.Errors.push_back(std::format("패스 '{}': 계산 큐 패스가 그래픽스 전용 상태({})를 요구합니다", Pass.Name, RGAccess::ToString(Sub.Access)));
			}
		}
		std::sort(Subs.begin(), Subs.end(), [](const FSubAccess& A, const FSubAccess& B) { return A.Global < B.Global; });
	}

	// 2) 컬링: 뿌리에서 읽기 의존을 거꾸로 따라간다
	std::vector<bool> Alive(PassCount, !Options.bCullPasses);
	if (Options.bCullPasses)
	{
		for (uint32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
		{
			if (Passes[PassIndex].bNeverCull)
			{
				Alive[PassIndex] = true;
				continue;
			}
			for (const FSubAccess& Sub : PassSubs[PassIndex])
			{
				if (IsWrite(Sub.Access) && Resources[Sub.Resource].bImported)
				{
					Alive[PassIndex] = true;
					break;
				}
			}
		}
		for (int32 PassIndex = static_cast<int32>(PassCount) - 1; PassIndex >= 0; --PassIndex)
		{
			if (!Alive[PassIndex])
			{
				continue;
			}
			for (const FSubAccess& Sub : PassSubs[PassIndex])
			{
				if (IsWrite(Sub.Access) && Sub.bOverwrite)
				{
					continue; // 이전 내용을 읽지 않는 쓰기
				}
				for (int32 Producer = PassIndex - 1; Producer >= 0; --Producer)
				{
					const std::vector<FSubAccess>& ProducerSubs = PassSubs[Producer];
					auto Found = std::lower_bound(ProducerSubs.begin(), ProducerSubs.end(), Sub.Global,
					                              [](const FSubAccess& Entry, uint32 Global) { return Entry.Global < Global; });
					if (Found != ProducerSubs.end() && Found->Global == Sub.Global && IsWrite(Found->Access))
					{
						Alive[Producer] = true;
						break;
					}
				}
			}
		}
	}
	for (uint32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
	{
		Result.Passes[PassIndex].bCulled = !Alive[PassIndex];
		Result.Passes[PassIndex].bAsync  = Alive[PassIndex] && Options.bAsyncCompute && Passes[PassIndex].Queue == ERGQueue::AsyncCompute;
		Result.CulledPassCount += Alive[PassIndex] ? 0u : 1u;
	}

	// 3) 비동기 묶음 + 포크/조인
	{
		FRGAsyncBatch* Current = nullptr;
		for (uint32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
		{
			if (!Alive[PassIndex])
			{
				continue;
			}
			if (Result.Passes[PassIndex].bAsync)
			{
				if (Current == nullptr)
				{
					Current = &Result.Batches.emplace_back();
				}
				Current->Passes.push_back(PassIndex);
			}
			else
			{
				Current = nullptr;
			}
		}
	}
	const auto Overlaps = [&](const std::vector<FSubAccess>& Subs, const std::vector<uint32>& Sorted) {
		for (const FSubAccess& Sub : Subs)
		{
			if (std::binary_search(Sorted.begin(), Sorted.end(), Sub.Global))
			{
				return true;
			}
		}
		return false;
	};
	int32 PreviousFork = -1;
	for (FRGAsyncBatch& Batch : Result.Batches)
	{
		std::vector<uint32> BatchReads;  // 읽기만 하는 서브리소스
		std::vector<uint32> BatchWrites; // 쓰는 서브리소스
		for (const uint32 PassIndex : Batch.Passes)
		{
			for (const FSubAccess& Sub : PassSubs[PassIndex])
			{
				(IsWrite(Sub.Access) ? BatchWrites : BatchReads).push_back(Sub.Global);
			}
		}
		std::sort(BatchWrites.begin(), BatchWrites.end());
		BatchWrites.erase(std::unique(BatchWrites.begin(), BatchWrites.end()), BatchWrites.end());
		std::sort(BatchReads.begin(), BatchReads.end());
		BatchReads.erase(std::unique(BatchReads.begin(), BatchReads.end()), BatchReads.end());
		std::vector<uint32> BatchAll;
		std::set_union(BatchReads.begin(), BatchReads.end(), BatchWrites.begin(), BatchWrites.end(), std::back_inserter(BatchAll));

		const auto Conflicts = [&](uint32 PassIndex) {
			std::vector<FSubAccess> Writes;
			std::vector<FSubAccess> Reads;
			for (const FSubAccess& Sub : PassSubs[PassIndex])
			{
				(IsWrite(Sub.Access) ? Writes : Reads).push_back(Sub);
			}
			return Overlaps(Writes, BatchAll) || Overlaps(Reads, BatchWrites);
		};
		Batch.ForkAfterPass = -1;
		for (int32 PassIndex = static_cast<int32>(Batch.Passes.front()) - 1; PassIndex >= 0; --PassIndex)
		{
			if (Alive[PassIndex] && !Result.Passes[PassIndex].bAsync && Conflicts(static_cast<uint32>(PassIndex)))
			{
				Batch.ForkAfterPass = PassIndex;
				break;
			}
		}
		Batch.ForkAfterPass = std::max(Batch.ForkAfterPass, PreviousFork); // 묶음끼리 등록 순서 유지
		PreviousFork        = Batch.ForkAfterPass;
		Batch.JoinBeforePass = -1;
		for (uint32 PassIndex = Batch.Passes.back() + 1; PassIndex < PassCount; ++PassIndex)
		{
			if (Alive[PassIndex] && !Result.Passes[PassIndex].bAsync && Conflicts(PassIndex))
			{
				Batch.JoinBeforePass = static_cast<int32>(PassIndex);
				break;
			}
		}
	}

	// 3.5) 묶음 합치기: 뒤 묶음의 포크가 앞 묶음의 조인보다 먼저면 앞 묶음의 포크를 뒤 묶음 포크까지 늦춰 한 번에 제출한다
	//      (늦춘 구간의 그래픽스 패스는 앞 묶음과 충돌하지 않는다 — 포크/조인 정의). 조인은 둘 중 이른 쪽. 큐 제출·펜스 수를 줄인다
	if (Options.bMergeAsyncBatches)
	{
		for (size_t Index = 0; Index + 1 < Result.Batches.size();)
		{
			FRGAsyncBatch& First  = Result.Batches[Index];
			FRGAsyncBatch& Second = Result.Batches[Index + 1];
			if (First.JoinBeforePass >= 0 && First.JoinBeforePass <= Second.ForkAfterPass)
			{
				++Index;
				continue;
			}
			First.ForkAfterPass = Second.ForkAfterPass;
			if (First.JoinBeforePass < 0 || (Second.JoinBeforePass >= 0 && Second.JoinBeforePass < First.JoinBeforePass))
			{
				First.JoinBeforePass = Second.JoinBeforePass;
			}
			First.Passes.insert(First.Passes.end(), Second.Passes.begin(), Second.Passes.end());
			Result.Batches.erase(Result.Batches.begin() + static_cast<std::ptrdiff_t>(Index + 1));
		}
	}

	// 4) 실행 타임라인: 그래픽스 패스 순서 + 포크 위치에 묶음
	{
		uint32     NextBatch = 0;
		const auto EmitBatchesAfter = [&](int32 Fork) {
			while (NextBatch < Result.Batches.size() && Result.Batches[NextBatch].ForkAfterPass == Fork)
			{
				Result.Steps.push_back({ true, NextBatch++ });
			}
		};
		EmitBatchesAfter(-1);
		for (uint32 PassIndex = 0; PassIndex < PassCount; ++PassIndex)
		{
			if (!Alive[PassIndex] || Result.Passes[PassIndex].bAsync)
			{
				continue;
			}
			Result.Steps.push_back({ false, PassIndex });
			EmitBatchesAfter(static_cast<int32>(PassIndex));
		}
		E_CHECKF(NextBatch == Result.Batches.size(), "렌더 그래프: 비동기 묶음 포크 위치 오류");
	}

	// 타임라인 순서의 패스 목록 + 서브리소스별 접근 사건 (look-ahead용)
	std::vector<uint32> Timeline;       // 패스 번호
	std::vector<int32>  TimelineStep;   // 패스가 속한 타임라인 칸
	for (uint32 StepIndex = 0; StepIndex < Result.Steps.size(); ++StepIndex)
	{
		const FRGStep& Step = Result.Steps[StepIndex];
		if (Step.bAsyncBatch)
		{
			for (const uint32 PassIndex : Result.Batches[Step.Index].Passes)
			{
				Timeline.push_back(PassIndex);
				TimelineStep.push_back(static_cast<int32>(StepIndex));
			}
		}
		else
		{
			Timeline.push_back(Step.Index);
			TimelineStep.push_back(static_cast<int32>(StepIndex));
		}
	}
	struct FEvent
	{
		uint32    Position = 0; // Timeline 번호
		ERGAccess Access   = ERGAccess::None;
	};
	std::vector<std::vector<FEvent>> Events(GlobalCount);
	for (uint32 Position = 0; Position < Timeline.size(); ++Position)
	{
		for (const FSubAccess& Sub : PassSubs[Timeline[Position]])
		{
			Events[Sub.Global].push_back({ Position, Sub.Access });
		}
	}
	// Position의 접근부터 이어지는 읽기 상태 합 (쓰기/단독 상태를 만나면 멈춤). bStoppedByWrite = 묶음 안에서 쓰기로 멈췄는가
	const auto MergeReads = [&](uint32 Global, uint32 Position, uint32 EndPosition, bool& bStoppedBeforeEnd) {
		ERGAccess Merged   = ERGAccess::None;
		bStoppedBeforeEnd  = false;
		for (const FEvent& Event : Events[Global])
		{
			if (Event.Position < Position)
			{
				continue;
			}
			if (!RGAccess::IsReadOnly(Event.Access))
			{
				bStoppedBeforeEnd = Event.Position < EndPosition;
				break;
			}
			Merged |= Event.Access;
		}
		return Merged;
	};

	// 5) 상태 시뮬레이션
	std::vector<ERGAccess> State(GlobalCount, ERGAccess::Common);
	std::vector<int32>     LastUavPass(GlobalCount, -1); // 마지막으로 UAV로 접근한 패스
	std::vector<bool>      LastUavAsync(GlobalCount, false);
	for (uint32 ResourceIndex = 0; ResourceIndex < ResourceCount; ++ResourceIndex)
	{
		const FRGCompileResource& Resource = Resources[ResourceIndex];
		const uint32              Count    = SubBase[ResourceIndex + 1] - SubBase[ResourceIndex];
		for (uint32 Sub = 0; Sub < Count; ++Sub)
		{
			State[SubBase[ResourceIndex] + Sub] = Sub < Resource.InitialStates.size() ? Resource.InitialStates[Sub] : Resource.InitialState;
		}
	}
	const auto ResourceOf = [&](uint32 Global) {
		return static_cast<uint32>(std::upper_bound(SubBase.begin(), SubBase.end(), Global) - SubBase.begin()) - 1;
	};
	// 서브리소스 전이 목록을 리소스 단위로 묶어 배리어로 (모든 서브리소스가 같은 전/후 상태면 ALL_SUBRESOURCES 하나)
	struct FSubTransition
	{
		uint32    Global = 0;
		ERGAccess Before = ERGAccess::None;
		ERGAccess After  = ERGAccess::None;
	};
	const auto EmitTransitions = [&](std::vector<FSubTransition>& Transitions, std::vector<FRGBarrier>& Out) {
		std::sort(Transitions.begin(), Transitions.end(), [](const FSubTransition& A, const FSubTransition& B) { return A.Global < B.Global; });
		size_t Begin = 0;
		while (Begin < Transitions.size())
		{
			const uint32 Resource = ResourceOf(Transitions[Begin].Global);
			size_t       End      = Begin;
			bool         bUniform = true;
			while (End < Transitions.size() && ResourceOf(Transitions[End].Global) == Resource)
			{
				bUniform = bUniform && Transitions[End].Before == Transitions[Begin].Before && Transitions[End].After == Transitions[Begin].After;
				++End;
			}
			const uint32 SubCount = SubBase[Resource + 1] - SubBase[Resource];
			if (bUniform && End - Begin == SubCount)
			{
				Out.push_back({ false, Resource, FRGBarrier::AllSubresources, Transitions[Begin].Before, Transitions[Begin].After });
			}
			else
			{
				for (size_t Index = Begin; Index < End; ++Index)
				{
					Out.push_back({ false, Resource, Transitions[Index].Global - SubBase[Resource], Transitions[Index].Before, Transitions[Index].After });
				}
			}
			Begin = End;
		}
	};

	std::vector<FSubTransition> Pending;
	std::vector<uint32>         PendingUav; // 리소스 번호
	uint32                      Position = 0;
	for (uint32 StepIndex = 0; StepIndex < Result.Steps.size(); ++StepIndex)
	{
		const FRGStep& Step = Result.Steps[StepIndex];
		if (Step.bAsyncBatch)
		{
			// 포크 배리어: 묶음이 처음 요구하는 상태로 그래픽스 큐에서 (묶음 안 첫 접근 위치부터 합친 읽기)
			FRGAsyncBatch& Batch      = Result.Batches[Step.Index];
			const uint32   BatchBegin = Position;
			const uint32   BatchEnd   = Position + static_cast<uint32>(Batch.Passes.size());
			Pending.clear();
			std::vector<uint32> Seen;
			for (uint32 Local = BatchBegin; Local < BatchEnd; ++Local)
			{
				for (const FSubAccess& Sub : PassSubs[Timeline[Local]])
				{
					if (std::find(Seen.begin(), Seen.end(), Sub.Global) != Seen.end())
					{
						continue;
					}
					Seen.push_back(Sub.Global);
					ERGAccess Target = Sub.Access;
					if (RGAccess::IsReadOnly(Sub.Access))
					{
						bool            bStoppedInBatch = false;
						const ERGAccess Merged          = MergeReads(Sub.Global, Local, BatchEnd, bStoppedInBatch);
						// 묶음 안에서 다시 전이할 상태는 계산 큐가 바꿀 수 있어야 한다
						Target = bStoppedInBatch ? static_cast<ERGAccess>(RGAccess::Bits(Merged) & RGAccess::ComputeLegalMask) : Merged;
						if (CoversRead(State[Sub.Global], Sub.Access) &&
						    (!bStoppedInBatch || RGAccess::IsComputeLegal(State[Sub.Global])))
						{
							continue; // 이미 읽을 수 있는 상태
						}
					}
					else if (State[Sub.Global] == Target)
					{
						continue; // 같은 쓰기 상태 (큐 사이 펜스가 UAV 배리어 역할)
					}
					Pending.push_back({ Sub.Global, State[Sub.Global], Target });
					State[Sub.Global] = Target;
				}
			}
			EmitTransitions(Pending, Batch.ForkBarriers);
			Result.TransitionCount += static_cast<uint32>(Batch.ForkBarriers.size());
			Result.BarrierBatchCount += Batch.ForkBarriers.empty() ? 0u : 1u;
		}

		const uint32 PassesInStep = Step.bAsyncBatch ? static_cast<uint32>(Result.Batches[Step.Index].Passes.size()) : 1u;
		for (uint32 Local = 0; Local < PassesInStep; ++Local, ++Position)
		{
			const uint32     PassIndex = Timeline[Position];
			FRGCompiledPass& Compiled  = Result.Passes[PassIndex];
			const bool       bCompute  = Compiled.bAsync;
			Pending.clear();
			PendingUav.clear();
			for (const FSubAccess& Sub : PassSubs[PassIndex])
			{
				const ERGAccess Current = State[Sub.Global];
				if (IsWrite(Sub.Access) || (RGAccess::Bits(Sub.Access) & RGAccess::SingleMask) != 0)
				{
					if (Current == Sub.Access)
					{
						if (Sub.Access == ERGAccess::Uav && LastUavPass[Sub.Global] >= 0 && LastUavPass[Sub.Global] != static_cast<int32>(PassIndex) &&
						    LastUavAsync[Sub.Global] == bCompute)
						{
							const uint32 Resource = Sub.Resource;
							if (std::find(PendingUav.begin(), PendingUav.end(), Resource) == PendingUav.end())
							{
								PendingUav.push_back(Resource);
							}
						}
					}
					else
					{
						Pending.push_back({ Sub.Global, Current, Sub.Access });
						State[Sub.Global] = Sub.Access;
					}
				}
				else if (!CoversRead(Current, Sub.Access))
				{
					bool      bStopped = false;
					ERGAccess Target   = MergeReads(Sub.Global, Position, static_cast<uint32>(Timeline.size()), bStopped);
					if (bCompute)
					{
						Target = static_cast<ERGAccess>(RGAccess::Bits(Target) & RGAccess::ComputeLegalMask);
					}
					Pending.push_back({ Sub.Global, Current, Target });
					State[Sub.Global] = Target;
				}
				if (Sub.Access == ERGAccess::Uav)
				{
					LastUavPass[Sub.Global]  = static_cast<int32>(PassIndex);
					LastUavAsync[Sub.Global] = bCompute;
				}
			}
			if (bCompute)
			{
				for (const FSubTransition& Transition : Pending)
				{
					if (!RGAccess::IsComputeLegal(Transition.Before) || !RGAccess::IsComputeLegal(Transition.After))
					{
						Result.Errors.push_back(std::format("패스 '{}': 계산 큐에서 할 수 없는 전이 {} → {} (리소스 '{}')", Passes[PassIndex].Name,
						                                    RGAccess::ToString(Transition.Before), RGAccess::ToString(Transition.After),
						                                    Resources[ResourceOf(Transition.Global)].Name));
					}
				}
			}
			EmitTransitions(Pending, Compiled.Barriers);
			Result.TransitionCount += static_cast<uint32>(Compiled.Barriers.size());
			for (const uint32 Resource : PendingUav)
			{
				Compiled.Barriers.push_back({ true, Resource, FRGBarrier::AllSubresources, ERGAccess::Uav, ERGAccess::Uav });
				++Result.UavBarrierCount;
			}
			Result.BarrierBatchCount += Compiled.Barriers.empty() ? 0u : 1u;

			// 수명
			for (const FSubAccess& Sub : PassSubs[PassIndex])
			{
				FRGLifetime& Lifetime = Result.Lifetimes[Sub.Resource];
				if (Lifetime.FirstStep < 0)
				{
					Lifetime.FirstStep = TimelineStep[Position];
					Lifetime.FirstPass = static_cast<int32>(PassIndex);
				}
				Lifetime.LastStep = TimelineStep[Position];
				Lifetime.LastPass = static_cast<int32>(PassIndex);
			}
		}
	}

	// 6) 끝: 가져온 리소스를 끝 상태로
	Pending.clear();
	for (uint32 ResourceIndex = 0; ResourceIndex < ResourceCount; ++ResourceIndex)
	{
		const FRGCompileResource& Resource = Resources[ResourceIndex];
		if (!Resource.bImported || (Resource.FinalState == ERGAccess::None && !Resource.bUniformFinal))
		{
			continue;
		}
		const ERGAccess Final = Resource.FinalState != ERGAccess::None ? Resource.FinalState : State[SubBase[ResourceIndex]];
		for (uint32 Global = SubBase[ResourceIndex]; Global < SubBase[ResourceIndex + 1]; ++Global)
		{
			if (State[Global] != Final)
			{
				Pending.push_back({ Global, State[Global], Final });
				State[Global] = Final;
			}
		}
	}
	EmitTransitions(Pending, Result.FinalBarriers);
	Result.TransitionCount += static_cast<uint32>(Result.FinalBarriers.size());
	Result.BarrierBatchCount += Result.FinalBarriers.empty() ? 0u : 1u;

	Result.FinalStates.resize(ResourceCount);
	for (uint32 ResourceIndex = 0; ResourceIndex < ResourceCount; ++ResourceIndex)
	{
		Result.FinalStates[ResourceIndex].assign(State.begin() + SubBase[ResourceIndex], State.begin() + SubBase[ResourceIndex + 1]);
	}
	return Result;
}
