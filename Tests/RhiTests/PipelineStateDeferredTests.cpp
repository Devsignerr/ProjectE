// PSO 지연 생성 (2026-10-05, D3D12PipelineState.h): FDeferredCreationScope 안의 Init*은 처음 Get()까지 만들지 않는다.
//   만든 뒤에는 같은 객체, 다른 스레드의 첫 Get()도 같은 PSO, 교환·떼어 내기는 만들지 않은 상태도 옮긴다 (GPU 장치 없으면 건너뜀)
#include "Core/Testing/TestFramework.h"
#include "RHI/D3D12/D3D12Device.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"

#include <thread>

namespace
{
	const char* DeferredTestSource = R"(
RWStructuredBuffer<uint> Output : register(u0);
[numthreads(1, 1, 1)]
void CSMain(uint3 Id : SV_DispatchThreadID) { Output[Id.x] = Id.x + 1; }
)";
} // namespace

E_TEST(PipelineState_DeferredCreation)
{
	FD3D12Device Device;
	if (!Device.Init(/*bEnableDebugLayer*/ false))
	{
		E_LOG(LogD3D12, Warning, "D3D12 장치가 없어 PSO 지연 생성 테스트를 건너뜁니다");
		return;
	}
	FD3D12ShaderCompiler Compiler;
	if (!Compiler.Init() || !Compiler.IsAvailable())
	{
		E_LOG(LogD3D12, Warning, "DXC가 없어 PSO 지연 생성 테스트를 건너뜁니다");
		Device.Shutdown();
		return;
	}
	{
		FShaderCompileDesc Desc;
		Desc.FileName   = L"DeferredPsoTest.hlsl";
		Desc.EntryPoint = L"CSMain";
		Desc.Stage      = EShaderStage::Compute;
		Desc.VirtualFiles.push_back({ Desc.FileName, DeferredTestSource });
		const ComPtr<IDxcBlob> Shader = Compiler.Compile(Desc);
		FD3D12RootSignature    Root;
		Root.AddUnorderedAccessView(0);
		E_EXPECT_TRUE(Shader != nullptr && Root.Finalize(Device.GetDevice(), D3D12_ROOT_SIGNATURE_FLAG_NONE, L"DeferredPsoTestRoot"));

		// 범위 밖: 즉시
		FD3D12PipelineState Immediate;
		E_EXPECT_TRUE(Immediate.InitCompute(Device.GetDevice(), Root.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()), L"Immediate"));
		E_EXPECT_FALSE(Immediate.IsDeferredPending());
		E_EXPECT_TRUE(Immediate.IsInitialized());

		// 범위 안: 기록만 → 다른 스레드의 첫 Get()이 만든다 → 이후 같은 객체
		FD3D12PipelineState Deferred;
		FD3D12PipelineState Unused;
		{
			const FD3D12PipelineState::FDeferredCreationScope Scope;
			E_EXPECT_TRUE(FD3D12PipelineState::IsDeferredCreationActive());
			E_EXPECT_TRUE(Deferred.InitCompute(Device.GetDevice(), Root.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()), L"Deferred"));
			E_EXPECT_TRUE(Unused.InitCompute(Device.GetDevice(), Root.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()), L"Unused"));
		}
		E_EXPECT_FALSE(FD3D12PipelineState::IsDeferredCreationActive());
		E_EXPECT_TRUE(Deferred.IsDeferredPending());
		E_EXPECT_TRUE(Deferred.IsInitialized());
		ID3D12PipelineState* FromThread = nullptr;
		std::thread          Worker([&]() { FromThread = Deferred.Get(); });
		Worker.join();
		E_EXPECT_TRUE(FromThread != nullptr);
		E_EXPECT_FALSE(Deferred.IsDeferredPending());
		E_EXPECT_TRUE(Deferred.Get() == FromThread);

		// 교환: 만들지 않은 지연 상태도 함께 옮긴다. 떼어 내면 만들지 않은 것은 빈 포인터
		FD3D12PipelineState Other;
		Other.Swap(Unused);
		E_EXPECT_FALSE(Unused.IsInitialized());
		E_EXPECT_TRUE(Other.IsDeferredPending());
		E_EXPECT_TRUE(Other.Detach() == nullptr);
		E_EXPECT_FALSE(Other.IsInitialized());
		E_EXPECT_TRUE(Other.Get() == nullptr);

		// 초기화하지 않은 PSO의 Get()은 만들지 않는다
		FD3D12PipelineState Empty;
		E_EXPECT_TRUE(Empty.Get() == nullptr);
	}
	Device.Shutdown();
}
