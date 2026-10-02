#include "Core/Testing/TestFramework.h"
#include "Renderer/RenderGraph/RenderGraphCompiler.h"

#include <vector>

// 렌더 그래프 컴파일러 규칙 (RenderGraphCompiler.h 머리 주석): 컬링, 상태 전이(읽기 합치기/서브리소스/묶음), UAV 배리어, 비동기 포크/조인, 수명
namespace
{
	FRGCompileResource MakeResource(const char* Name, bool bImported, ERGAccess Initial, ERGAccess Final = ERGAccess::None, uint32 Mips = 1)
	{
		FRGCompileResource Resource;
		Resource.Name         = Name;
		Resource.bImported    = bImported;
		Resource.InitialState = Initial;
		Resource.FinalState   = Final;
		Resource.MipCount     = Mips;
		return Resource;
	}

	FRGCompilePass MakePass(const char* Name, std::vector<FRGCompileAccess> Accesses, ERGQueue Queue = ERGQueue::Graphics)
	{
		FRGCompilePass Pass;
		Pass.Name     = Name;
		Pass.Queue    = Queue;
		Pass.Accesses = std::move(Accesses);
		return Pass;
	}

	FRGCompileAccess Access(uint32 Resource, ERGAccess State, FRGSubresourceRange Range = FRGSubresourceRange::All(), bool bOverwrite = false)
	{
		FRGCompileAccess Result;
		Result.Resource   = Resource;
		Result.Access     = State;
		Result.Range      = Range;
		Result.bOverwrite = bOverwrite;
		return Result;
	}

	bool HasTransition(const std::vector<FRGBarrier>& Barriers, uint32 Resource, uint32 Subresource, ERGAccess Before, ERGAccess After)
	{
		for (const FRGBarrier& Barrier : Barriers)
		{
			if (!Barrier.bUav && Barrier.Resource == Resource && Barrier.Subresource == Subresource && Barrier.Before == Before && Barrier.After == After)
			{
				return true;
			}
		}
		return false;
	}

	uint32 CountUav(const std::vector<FRGBarrier>& Barriers)
	{
		uint32 Count = 0;
		for (const FRGBarrier& Barrier : Barriers)
		{
			Count += Barrier.bUav ? 1u : 0u;
		}
		return Count;
	}
} // namespace

E_TEST(RenderGraph_CullsPassesWithoutExternalOutput)
{
	const std::vector<FRGCompileResource> Resources = {
		MakeResource("Unused", false, ERGAccess::Common),
		MakeResource("Temp", false, ERGAccess::Common),
		MakeResource("Output", true, ERGAccess::RenderTarget, ERGAccess::RenderTarget),
	};
	const std::vector<FRGCompilePass> Passes = {
		MakePass("WritesUnused", { Access(0, ERGAccess::RenderTarget) }),
		MakePass("WritesTemp", { Access(1, ERGAccess::RenderTarget) }),
		MakePass("Composite", { Access(1, ERGAccess::SrvPixel), Access(2, ERGAccess::RenderTarget) }),
	};
	const FRGCompileResult Result = RenderGraphCompiler::Compile(Resources, Passes);
	E_EXPECT_TRUE(Result.Errors.empty());
	E_EXPECT_TRUE(Result.Passes[0].bCulled);
	E_EXPECT_FALSE(Result.Passes[1].bCulled);
	E_EXPECT_FALSE(Result.Passes[2].bCulled);
	E_EXPECT_EQ(Result.CulledPassCount, 1u);
	E_EXPECT_EQ(Result.Steps.size(), static_cast<size_t>(2));

	// 컬링을 끄면 모두 실행
	FRGCompileOptions Options;
	Options.bCullPasses = false;
	const FRGCompileResult NoCull = RenderGraphCompiler::Compile(Resources, Passes, Options);
	E_EXPECT_EQ(NoCull.CulledPassCount, 0u);
}

E_TEST(RenderGraph_NeverCullAndOverwriteBreakDependency)
{
	const std::vector<FRGCompileResource> Resources = {
		MakeResource("Temp", false, ERGAccess::Common),
		MakeResource("Output", true, ERGAccess::RenderTarget, ERGAccess::RenderTarget),
	};
	FRGCompilePass SideEffect = MakePass("Readback", { Access(0, ERGAccess::CopySource) });
	SideEffect.bNeverCull     = true;
	const std::vector<FRGCompilePass> Passes = {
		MakePass("FirstWrite", { Access(0, ERGAccess::RenderTarget) }),
		MakePass("Overwrite", { Access(0, ERGAccess::RenderTarget, FRGSubresourceRange::All(), true) }),
		MakePass("Composite", { Access(0, ERGAccess::SrvPixel), Access(1, ERGAccess::RenderTarget) }),
		SideEffect,
	};
	const FRGCompileResult Result = RenderGraphCompiler::Compile(Resources, Passes);
	E_EXPECT_TRUE(Result.Passes[0].bCulled); // 덮어쓰기가 이전 내용을 읽지 않으므로 첫 쓰기는 쓸모없다
	E_EXPECT_FALSE(Result.Passes[1].bCulled);
	E_EXPECT_FALSE(Result.Passes[3].bCulled); // 부수 효과
}

E_TEST(RenderGraph_TransitionsAndFinalState)
{
	const std::vector<FRGCompileResource> Resources = {
		MakeResource("Color", true, ERGAccess::SrvPixel, ERGAccess::SrvPixel),
		MakeResource("Depth", true, ERGAccess::DepthWrite, ERGAccess::DepthWrite),
		MakeResource("Output", true, ERGAccess::RenderTarget, ERGAccess::RenderTarget),
	};
	const std::vector<FRGCompilePass> Passes = {
		MakePass("Main", { Access(0, ERGAccess::RenderTarget), Access(1, ERGAccess::DepthWrite) }),
		MakePass("Post", { Access(0, ERGAccess::SrvPixel), Access(1, ERGAccess::SrvPixel), Access(2, ERGAccess::RenderTarget) }),
	};
	const FRGCompileResult Result = RenderGraphCompiler::Compile(Resources, Passes);
	E_EXPECT_TRUE(Result.Errors.empty());
	E_EXPECT_EQ(Result.Passes[0].Barriers.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(HasTransition(Result.Passes[0].Barriers, 0, FRGBarrier::AllSubresources, ERGAccess::SrvPixel, ERGAccess::RenderTarget));
	E_EXPECT_TRUE(HasTransition(Result.Passes[1].Barriers, 0, FRGBarrier::AllSubresources, ERGAccess::RenderTarget, ERGAccess::SrvPixel));
	E_EXPECT_TRUE(HasTransition(Result.Passes[1].Barriers, 1, FRGBarrier::AllSubresources, ERGAccess::DepthWrite, ERGAccess::SrvPixel));
	// 끝: 깊이는 DepthWrite로 되돌린다 (색은 이미 SrvPixel)
	E_EXPECT_EQ(Result.FinalBarriers.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(HasTransition(Result.FinalBarriers, 1, FRGBarrier::AllSubresources, ERGAccess::SrvPixel, ERGAccess::DepthWrite));
	E_EXPECT_EQ(Result.TransitionCount, 4u);
	E_EXPECT_EQ(Result.BarrierBatchCount, 3u);
	E_EXPECT_TRUE(Result.FinalStates[1][0] == ERGAccess::DepthWrite);
}

E_TEST(RenderGraph_MergesFollowingReads)
{
	const std::vector<FRGCompileResource> Resources = {
		MakeResource("Shadow", true, ERGAccess::SrvPixel, ERGAccess::None),
		MakeResource("Output", true, ERGAccess::RenderTarget, ERGAccess::RenderTarget),
	};
	const std::vector<FRGCompilePass> Passes = {
		MakePass("ShadowDepth", { Access(0, ERGAccess::DepthWrite) }),
		MakePass("FogCompute", { Access(0, ERGAccess::SrvNonPixel), Access(1, ERGAccess::Uav) }),
		MakePass("Main", { Access(0, ERGAccess::SrvPixel), Access(1, ERGAccess::RenderTarget) }),
	};
	const FRGCompileResult Result = RenderGraphCompiler::Compile(Resources, Passes);
	E_EXPECT_TRUE(Result.Errors.empty());
	// 다음 쓰기 전까지 이어지는 읽기를 합쳐 한 번만 전이
	E_EXPECT_TRUE(HasTransition(Result.Passes[1].Barriers, 0, FRGBarrier::AllSubresources, ERGAccess::DepthWrite, ERGAccess::SrvAll));
	for (const FRGBarrier& Barrier : Result.Passes[2].Barriers)
	{
		E_EXPECT_TRUE(Barrier.Resource != 0);
	}
	// FinalState None: 마지막 상태 유지 + 알림
	E_EXPECT_TRUE(Result.FinalStates[0][0] == ERGAccess::SrvAll);
}

E_TEST(RenderGraph_SubresourceMipChain)
{
	// Hi-Z: 밉 0 복사(UAV) → 밉 i = 밉 i-1 읽기(SrvNonPixel) + 밉 i 쓰기(UAV) → 전체 읽기
	constexpr uint32                      Mips      = 4;
	const std::vector<FRGCompileResource> Resources = {
		MakeResource("Hiz", true, ERGAccess::SrvPixel, ERGAccess::SrvPixel, Mips),
		MakeResource("Output", true, ERGAccess::RenderTarget, ERGAccess::RenderTarget),
	};
	std::vector<FRGCompilePass> Passes;
	Passes.push_back(MakePass("HizMip0", { Access(0, ERGAccess::Uav, FRGSubresourceRange::Mip(0), true) }));
	for (uint32 Mip = 1; Mip < Mips; ++Mip)
	{
		Passes.push_back(MakePass("HizDown", { Access(0, ERGAccess::SrvNonPixel, FRGSubresourceRange::Mip(Mip - 1)),
		                                       Access(0, ERGAccess::Uav, FRGSubresourceRange::Mip(Mip), true) }));
	}
	Passes.push_back(MakePass("Trace", { Access(0, ERGAccess::SrvPixel), Access(1, ERGAccess::RenderTarget) }));
	const FRGCompileResult Result = RenderGraphCompiler::Compile(Resources, Passes);
	E_EXPECT_TRUE(Result.Errors.empty());
	E_EXPECT_EQ(Result.CulledPassCount, 0u);
	E_EXPECT_EQ(Result.Passes[0].Barriers.size(), static_cast<size_t>(1));
	E_EXPECT_TRUE(HasTransition(Result.Passes[0].Barriers, 0, 0, ERGAccess::SrvPixel, ERGAccess::Uav));
	// 밉 1 패스: 밉 0 Uav → 읽기, 밉 1 SrvPixel → Uav (서브리소스별)
	E_EXPECT_TRUE(HasTransition(Result.Passes[1].Barriers, 0, 1, ERGAccess::SrvPixel, ERGAccess::Uav));
	bool bMip0ToRead = false;
	for (const FRGBarrier& Barrier : Result.Passes[1].Barriers)
	{
		bMip0ToRead = bMip0ToRead || (Barrier.Subresource == 0 && Barrier.Before == ERGAccess::Uav && RGAccess::IsReadOnly(Barrier.After));
	}
	E_EXPECT_TRUE(bMip0ToRead);
	// 추적: 밉 0~2는 읽기 상태(SrvNonPixel → 합친 상태)에서, 밉 3은 Uav에서 → 서브리소스별 배리어 (ALL 아님)
	const std::vector<FRGBarrier>& TraceBarriers = Result.Passes[Mips].Barriers;
	E_EXPECT_TRUE(HasTransition(TraceBarriers, 0, Mips - 1, ERGAccess::Uav, ERGAccess::SrvPixel));
	for (const FRGBarrier& Barrier : TraceBarriers)
	{
		E_EXPECT_TRUE(Barrier.Subresource != FRGBarrier::AllSubresources);
	}
	// 끝 상태 SrvPixel: 합친 읽기 상태였던 밉 0~2만 되돌린다 (밉 3은 이미 SrvPixel)
	E_EXPECT_EQ(Result.FinalBarriers.size(), static_cast<size_t>(Mips - 1));
	E_EXPECT_TRUE(HasTransition(Result.FinalBarriers, 0, 0, ERGAccess::SrvAll, ERGAccess::SrvPixel));
}

E_TEST(RenderGraph_CoalescesAllSubresourcesAndUavBarrier)
{
	const std::vector<FRGCompileResource> Resources = {
		MakeResource("Array", true, ERGAccess::SrvPixel, ERGAccess::SrvPixel, 3),
		MakeResource("Buffer", true, ERGAccess::Uav, ERGAccess::Uav),
	};
	const std::vector<FRGCompilePass> Passes = {
		MakePass("WriteAll", { Access(0, ERGAccess::Uav), Access(1, ERGAccess::Uav) }),
		MakePass("Accumulate", { Access(1, ERGAccess::Uav) }),
	};
	const FRGCompileResult Result = RenderGraphCompiler::Compile(Resources, Passes);
	E_EXPECT_TRUE(Result.Errors.empty());
	E_EXPECT_TRUE(HasTransition(Result.Passes[0].Barriers, 0, FRGBarrier::AllSubresources, ERGAccess::SrvPixel, ERGAccess::Uav));
	E_EXPECT_EQ(CountUav(Result.Passes[0].Barriers), 0u); // 시작 상태가 이미 Uav여도 이전 패스가 없으면 UAV 배리어 없음
	E_EXPECT_EQ(CountUav(Result.Passes[1].Barriers), 1u);
	E_EXPECT_EQ(Result.UavBarrierCount, 1u);
	E_EXPECT_TRUE(HasTransition(Result.FinalBarriers, 0, FRGBarrier::AllSubresources, ERGAccess::Uav, ERGAccess::SrvPixel));
}

E_TEST(RenderGraph_RejectsInvalidCombination)
{
	const std::vector<FRGCompileResource> Resources = { MakeResource("Color", true, ERGAccess::SrvPixel, ERGAccess::SrvPixel) };
	const std::vector<FRGCompilePass>     Passes    = { MakePass("Feedback", { Access(0, ERGAccess::RenderTarget), Access(0, ERGAccess::SrvPixel) }) };
	E_EXPECT_FALSE(RenderGraphCompiler::Compile(Resources, Passes).Errors.empty());
	E_EXPECT_TRUE(RGAccess::IsValidCombination(ERGAccess::DepthRead | ERGAccess::SrvPixel));
	E_EXPECT_FALSE(RGAccess::IsValidCombination(ERGAccess::Uav | ERGAccess::SrvNonPixel));
}

namespace
{
	// 그림자(그래픽스) → 볼류메트릭 안개(계산) → 사전 패스·메인(그래픽스, 그림자 읽기) → 안개 적용(볼륨 읽기)
	void MakeAsyncGraph(std::vector<FRGCompileResource>& Resources, std::vector<FRGCompilePass>& Passes)
	{
		Resources = {
			MakeResource("ShadowMap", true, ERGAccess::SrvPixel, ERGAccess::SrvPixel),
			MakeResource("FogVolume", true, ERGAccess::SrvAll, ERGAccess::None),
			MakeResource("SceneColor", true, ERGAccess::SrvPixel, ERGAccess::SrvPixel),
			MakeResource("SceneDepth", true, ERGAccess::DepthWrite, ERGAccess::DepthWrite),
		};
		Passes = {
			MakePass("Shadow", { Access(0, ERGAccess::DepthWrite) }),
			MakePass("Prepass", { Access(3, ERGAccess::DepthWrite) }),
			MakePass("FogInject", { Access(0, ERGAccess::SrvNonPixel), Access(1, ERGAccess::Uav) }, ERGQueue::AsyncCompute),
			MakePass("Main", { Access(0, ERGAccess::SrvPixel), Access(2, ERGAccess::RenderTarget), Access(3, ERGAccess::DepthWrite) }),
			MakePass("FogApply", { Access(1, ERGAccess::SrvPixel), Access(2, ERGAccess::RenderTarget), Access(3, ERGAccess::SrvPixel) }),
		};
	}
} // namespace

E_TEST(RenderGraph_AsyncComputeForkJoin)
{
	std::vector<FRGCompileResource> Resources;
	std::vector<FRGCompilePass>     Passes;
	MakeAsyncGraph(Resources, Passes);
	const FRGCompileResult Result = RenderGraphCompiler::Compile(Resources, Passes);
	E_EXPECT_TRUE(Result.Errors.empty());
	E_EXPECT_EQ(Result.Batches.size(), static_cast<size_t>(1));
	const FRGAsyncBatch& Batch = Result.Batches[0];
	E_EXPECT_EQ(Batch.ForkAfterPass, 0);  // 그림자가 그림자 맵을 쓰므로 그 뒤
	E_EXPECT_EQ(Batch.JoinBeforePass, 4); // 안개 적용이 볼륨을 읽는다 (사전 패스·메인은 겹쳐 실행)
	E_EXPECT_TRUE(Result.Passes[2].bAsync);
	// 타임라인: 그림자, [묶음], 사전 패스, 메인, 안개 적용
	E_EXPECT_EQ(Result.Steps.size(), static_cast<size_t>(5));
	E_EXPECT_TRUE(Result.Steps[1].bAsyncBatch);
	E_EXPECT_EQ(Result.Steps[2].Index, 1u);
	// 포크 배리어(그래픽스): 그림자 맵 DepthWrite → 계산·메인 읽기 합침, 볼륨 → Uav
	E_EXPECT_TRUE(HasTransition(Batch.ForkBarriers, 0, FRGBarrier::AllSubresources, ERGAccess::DepthWrite, ERGAccess::SrvAll));
	E_EXPECT_TRUE(HasTransition(Batch.ForkBarriers, 1, FRGBarrier::AllSubresources, ERGAccess::SrvAll, ERGAccess::Uav));
	E_EXPECT_TRUE(Result.Passes[2].Barriers.empty()); // 계산 큐는 전이 없음
	// 메인은 그림자 맵 전이 없이 읽는다
	for (const FRGBarrier& Barrier : Result.Passes[3].Barriers)
	{
		E_EXPECT_TRUE(Barrier.Resource != 0);
	}
	// 조인 뒤 그래픽스 큐에서 볼륨 Uav → SrvPixel
	E_EXPECT_TRUE(HasTransition(Result.Passes[4].Barriers, 1, FRGBarrier::AllSubresources, ERGAccess::Uav, ERGAccess::SrvPixel));
	// 수명: 볼륨은 묶음(칸 1)에서 시작해 안개 적용(칸 4)까지
	E_EXPECT_EQ(Result.Lifetimes[1].FirstStep, 1);
	E_EXPECT_EQ(Result.Lifetimes[1].LastStep, 4);
	E_EXPECT_EQ(Result.Lifetimes[1].FirstPass, 2);
}

E_TEST(RenderGraph_AsyncComputeDisabledRunsInOrder)
{
	std::vector<FRGCompileResource> Resources;
	std::vector<FRGCompilePass>     Passes;
	MakeAsyncGraph(Resources, Passes);
	FRGCompileOptions Options;
	Options.bAsyncCompute         = false;
	const FRGCompileResult Result = RenderGraphCompiler::Compile(Resources, Passes, Options);
	E_EXPECT_TRUE(Result.Errors.empty());
	E_EXPECT_TRUE(Result.Batches.empty());
	E_EXPECT_FALSE(Result.Passes[2].bAsync);
	E_EXPECT_EQ(Result.Steps.size(), static_cast<size_t>(5));
	E_EXPECT_TRUE(HasTransition(Result.Passes[2].Barriers, 0, FRGBarrier::AllSubresources, ERGAccess::DepthWrite, ERGAccess::SrvAll));
}

E_TEST(RenderGraph_AsyncComputeRejectsGraphicsOnlyState)
{
	const std::vector<FRGCompileResource> Resources = { MakeResource("Target", true, ERGAccess::SrvPixel, ERGAccess::SrvPixel) };
	const std::vector<FRGCompilePass>     Passes    = { MakePass("Bad", { Access(0, ERGAccess::SrvPixel) }, ERGQueue::AsyncCompute) };
	E_EXPECT_FALSE(RenderGraphCompiler::Compile(Resources, Passes).Errors.empty());
}

E_TEST(RenderGraph_AsyncBatchInternalTransitionsAreComputeLegal)
{
	// 계산 묶음 안: 주입(Uav) → 적분(주입 읽기 + 결과 쓰기). 주입 볼륨의 Uav → 읽기 전이는 계산 큐 합법 상태로만 (뒤의 픽셀 읽기는 조인 뒤 그래픽스)
	const std::vector<FRGCompileResource> Resources = {
		MakeResource("Inject", true, ERGAccess::SrvNonPixel, ERGAccess::None),
		MakeResource("Result", true, ERGAccess::SrvAll, ERGAccess::None),
		MakeResource("Output", true, ERGAccess::RenderTarget, ERGAccess::RenderTarget),
	};
	const std::vector<FRGCompilePass> Passes = {
		MakePass("Inject", { Access(0, ERGAccess::Uav) }, ERGQueue::AsyncCompute),
		MakePass("Integrate", { Access(0, ERGAccess::SrvNonPixel), Access(1, ERGAccess::Uav) }, ERGQueue::AsyncCompute),
		MakePass("Apply", { Access(0, ERGAccess::SrvPixel), Access(1, ERGAccess::SrvPixel), Access(2, ERGAccess::RenderTarget) }),
	};
	const FRGCompileResult Result = RenderGraphCompiler::Compile(Resources, Passes);
	E_EXPECT_TRUE(Result.Errors.empty());
	E_EXPECT_EQ(Result.Batches.size(), static_cast<size_t>(1));
	E_EXPECT_EQ(Result.Batches[0].ForkAfterPass, -1); // 앞선 그래픽스 의존 없음 → 그래프 처음
	E_EXPECT_EQ(Result.Batches[0].JoinBeforePass, 2);
	E_EXPECT_TRUE(HasTransition(Result.Passes[1].Barriers, 0, FRGBarrier::AllSubresources, ERGAccess::Uav, ERGAccess::SrvNonPixel));
	E_EXPECT_TRUE(HasTransition(Result.Passes[2].Barriers, 0, FRGBarrier::AllSubresources, ERGAccess::SrvNonPixel, ERGAccess::SrvPixel));
	E_EXPECT_TRUE(HasTransition(Result.Passes[2].Barriers, 1, FRGBarrier::AllSubresources, ERGAccess::Uav, ERGAccess::SrvPixel));
}
