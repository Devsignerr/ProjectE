// 레이 트레이싱 (Phase 50) RHI 테스트:
//   - 셰이더 모델/라이브러리 컴파일 (DXC만): cs_6_5 RayQuery, lib_6_3 DXR 라이브러리, 매니페스트 "ShaderModel"/"Library"
//   - GPU: 삼각형 BLAS(압축 크기 질의) → TLAS(인스턴스 이동·마스크) → 계산 셰이더 RayQuery 추적 → 되읽기 (DXR 1.1 미지원이면 건너뜀)
#include "Core/Testing/TestFramework.h"
#include "RHI/D3D12/D3D12CommandQueue.h"
#include "RHI/D3D12/D3D12Device.h"
#include "RHI/D3D12/D3D12PipelineState.h"
#include "RHI/D3D12/D3D12RootSignature.h"
#include "RHI/D3D12/D3D12ShaderCompiler.h"
#include "RHI/ShaderManifest.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace
{
	// 셰이더 소스 (가상 진입 파일)
	const char* RayQueryTestSource = R"(
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<float4> Results : register(u0);
cbuffer TestConstants : register(b0) { uint RayMask; };

[numthreads(8, 1, 1)]
void CSMain(uint3 Id : SV_DispatchThreadID)
{
	// 광선 i: 원점 (x, y, 0)에서 +Z로. 삼각형은 인스턴스 이동 (0, 0, 10)
	static const float2 Origins[8] = { float2(0.0f, 0.0f), float2(0.5f, -0.5f), float2(-0.5f, -0.5f), float2(0.0f, 0.8f),
	                                   float2(2.0f, 0.0f), float2(0.0f, -2.0f), float2(0.9f, 0.9f), float2(-0.1f, 0.1f) };
	RayDesc Ray;
	Ray.Origin    = float3(Origins[Id.x], 0.0f);
	Ray.Direction = float3(0.0f, 0.0f, 1.0f);
	Ray.TMin      = 0.0f;
	Ray.TMax      = 100.0f;
	RayQuery<RAY_FLAG_NONE> Query;
	Query.TraceRayInline(Scene, RAY_FLAG_NONE, RayMask, Ray);
	Query.Proceed();
	if (Query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
	{
		Results[Id.x] = float4(Query.CommittedRayT(), (float)Query.CommittedInstanceID(), Query.CommittedTriangleBarycentrics());
	}
	else
	{
		Results[Id.x] = float4(-1.0f, 0.0f, 0.0f, 0.0f);
	}
}
)";

	const char* LibraryTestSource = R"(
RaytracingAccelerationStructure Scene : register(t0);
RWTexture2D<float4> Output : register(u0);
struct FPayload { float4 Color; };
[shader("raygeneration")] void RayGen()
{
	RayDesc Ray;
	Ray.Origin = float3(0, 0, 0); Ray.Direction = float3(0, 0, 1); Ray.TMin = 0; Ray.TMax = 100;
	FPayload Payload; Payload.Color = 0;
	TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 2, 0, Ray, Payload);
	Output[DispatchRaysIndex().xy] = Payload.Color;
}
[shader("miss")] void Miss(inout FPayload Payload) { Payload.Color = float4(0, 0, 1, 1); }
[shader("closesthit")] void ClosestHit(inout FPayload Payload, BuiltInTriangleIntersectionAttributes Attributes)
{
	Payload.Color = float4(Attributes.barycentrics, RayTCurrent(), 1);
}
)";

	uint64 CountDebugIssues(ID3D12Device* Device)
	{
		ComPtr<ID3D12InfoQueue> InfoQueue;
		if (FAILED(Device->QueryInterface(IID_PPV_ARGS(&InfoQueue))))
		{
			return 0;
		}
		uint64 Issues = 0;
		for (uint64 Index = 0; Index < InfoQueue->GetNumStoredMessages(); ++Index)
		{
			SIZE_T Length = 0;
			InfoQueue->GetMessage(Index, nullptr, &Length);
			std::vector<uint8> Storage(Length);
			auto* Message = reinterpret_cast<D3D12_MESSAGE*>(Storage.data());
			if (SUCCEEDED(InfoQueue->GetMessage(Index, Message, &Length)) && Message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
			{
				E_LOG(LogD3D12, Error, "테스트 중 디버그 레이어 메시지: {}", Message->pDescription);
				++Issues;
			}
		}
		return Issues;
	}

	ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* Device, uint64 Size, D3D12_HEAP_TYPE Heap, D3D12_RESOURCE_FLAGS Flags, D3D12_RESOURCE_STATES State)
	{
		const D3D12_HEAP_PROPERTIES Properties = MakeHeapProperties(Heap);
		const D3D12_RESOURCE_DESC   Desc       = MakeBufferDesc(Size, Flags);
		ComPtr<ID3D12Resource>      Buffer;
		Device->CreateCommittedResource(&Properties, D3D12_HEAP_FLAG_NONE, &Desc, State, nullptr, IID_PPV_ARGS(&Buffer));
		return Buffer;
	}

	ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* Device, const void* Data, uint64 Size)
	{
		ComPtr<ID3D12Resource> Buffer = CreateBuffer(Device, Size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
		void*                  Mapped = nullptr;
		if (Buffer && SUCCEEDED(Buffer->Map(0, nullptr, &Mapped)))
		{
			std::memcpy(Mapped, Data, Size);
			Buffer->Unmap(0, nullptr);
		}
		return Buffer;
	}

	FShaderCompileDesc MakeVirtualDesc(const wchar_t* Name, const char* Source, const wchar_t* Entry, EShaderStage Stage, const wchar_t* Model)
	{
		FShaderCompileDesc Desc;
		Desc.FileName    = Name;
		Desc.EntryPoint  = Entry;
		Desc.Stage       = Stage;
		Desc.ShaderModel = Model;
		Desc.VirtualFiles.push_back({ Name, Source });
		return Desc;
	}
} // namespace

E_TEST(RayTracing_ShaderModelAndLibraryCompile)
{
	// 대상 프로필 규칙 (모델이 비면 단계 기본)
	E_EXPECT_TRUE(GetShaderTargetProfile(EShaderStage::Compute, L"") == L"cs_6_0");
	E_EXPECT_TRUE(GetShaderTargetProfile(EShaderStage::Pixel, L"6_5") == L"ps_6_5");
	E_EXPECT_TRUE(GetShaderTargetProfile(EShaderStage::Library, L"") == L"lib_6_3");
	E_EXPECT_TRUE(GetShaderTargetProfile(EShaderStage::Library, L"6_5") == L"lib_6_5");

	// 매니페스트: "ShaderModel"과 "Library" 단계, 쿠킹 파일명 _sm<모델>
	FShaderManifest Manifest;
	std::string     Error;
	E_EXPECT_TRUE(Manifest.ParseJson(R"({ "Shaders": [ { "File": "A.hlsl", "Entry": "PSTrace", "Stage": "Pixel", "ShaderModel": "6_5" },
	                                                     { "File": "B.hlsl", "Entry": "Lib", "Stage": "Library" } ] })",
	                                 Error));
	E_EXPECT_EQ(Manifest.Entries.size(), static_cast<size_t>(2));
	E_EXPECT_TRUE(Manifest.Entries[0].ShaderModel == L"6_5" && Manifest.Entries[1].Stage == EShaderStage::Library);
	E_EXPECT_TRUE(GetCookedShaderFileName(Manifest.Entries[0].ToCompileDesc(), false) == L"A_PSTrace_Pixel_sm6_5.dxil");
	E_EXPECT_TRUE(GetCookedShaderFileName(Manifest.Entries[1].ToCompileDesc(), false) == L"B_Lib_Library.dxil");

	FD3D12ShaderCompiler Compiler;
	if (!Compiler.Init() || !Compiler.IsAvailable())
	{
		E_LOG(LogD3D12, Warning, "DXC가 없어 레이 트레이싱 셰이더 컴파일 테스트를 건너뜁니다");
		return;
	}
	E_EXPECT_TRUE(Compiler.Compile(MakeVirtualDesc(L"RayQueryTest.hlsl", RayQueryTestSource, L"CSMain", EShaderStage::Compute, L"6_5")) != nullptr);
	E_EXPECT_TRUE(Compiler.Compile(MakeVirtualDesc(L"RayLibraryTest.hlsl", LibraryTestSource, L"Lib", EShaderStage::Library, L"")) != nullptr);
	// RayQuery는 6.5 미만에서 거부 (모델이 실제로 적용되는지)
	E_EXPECT_TRUE(Compiler.Compile(MakeVirtualDesc(L"RayQueryOld.hlsl", RayQueryTestSource, L"CSMain", EShaderStage::Compute, L"")) == nullptr);
}

E_TEST(RayTracing_GpuInlineRayQuery)
{
	FD3D12Device Device;
	if (!Device.Init(/*bEnableDebugLayer*/ true))
	{
		E_LOG(LogD3D12, Warning, "D3D12 장치가 없어 레이 트레이싱 GPU 테스트를 건너뜁니다");
		return;
	}
	if (!Device.SupportsRayTracing())
	{
		E_LOG(LogD3D12, Warning, "DXR 1.1 미지원 GPU — 레이 트레이싱 GPU 테스트를 건너뜁니다");
		Device.Shutdown();
		return;
	}
	FD3D12ShaderCompiler Compiler;
	if (!Compiler.Init() || !Compiler.IsAvailable())
	{
		E_LOG(LogD3D12, Warning, "DXC가 없어 레이 트레이싱 GPU 테스트를 건너뜁니다");
		Device.Shutdown();
		return;
	}
	ID3D12Device*  D3DDevice = Device.GetDevice();
	ID3D12Device5* Device5   = Device.GetDevice5();
	{
		FD3D12CommandQueue Direct;
		E_EXPECT_TRUE(Direct.Init(D3DDevice, D3D12_COMMAND_LIST_TYPE_DIRECT));

		// 삼각형 하나 (z = 0, 인스턴스가 +10 이동)
		const float  Vertices[9] = { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		const uint32 Indices[3]  = { 0, 1, 2 };
		ComPtr<ID3D12Resource> VertexBuffer = CreateUploadBuffer(D3DDevice, Vertices, sizeof(Vertices));
		ComPtr<ID3D12Resource> IndexBuffer  = CreateUploadBuffer(D3DDevice, Indices, sizeof(Indices));

		D3D12_RAYTRACING_GEOMETRY_DESC Geometry{};
		Geometry.Type                                 = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		Geometry.Flags                                = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
		Geometry.Triangles.VertexBuffer.StartAddress  = VertexBuffer->GetGPUVirtualAddress();
		Geometry.Triangles.VertexBuffer.StrideInBytes = 12;
		Geometry.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
		Geometry.Triangles.VertexCount                = 3;
		Geometry.Triangles.IndexBuffer                = IndexBuffer->GetGPUVirtualAddress();
		Geometry.Triangles.IndexCount                 = 3;
		Geometry.Triangles.IndexFormat                = DXGI_FORMAT_R32_UINT;
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Bottom{};
		Bottom.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
		Bottom.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
		Bottom.NumDescs       = 1;
		Bottom.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
		Bottom.pGeometryDescs = &Geometry;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO BottomInfo{};
		Device5->GetRaytracingAccelerationStructurePrebuildInfo(&Bottom, &BottomInfo);
		E_EXPECT_TRUE(BottomInfo.ResultDataMaxSizeInBytes > 0);

		D3D12_RAYTRACING_INSTANCE_DESC Instance{};
		Instance.Transform[0][0] = Instance.Transform[1][1] = Instance.Transform[2][2] = 1.0f;
		Instance.Transform[2][3] = 10.0f;
		Instance.InstanceID      = 7;
		Instance.InstanceMask    = 0x01;
		Instance.Flags           = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
		ComPtr<ID3D12Resource> Blas = CreateBuffer(D3DDevice, BottomInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		                                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
		Instance.AccelerationStructure = Blas->GetGPUVirtualAddress();
		ComPtr<ID3D12Resource> InstanceBuffer = CreateUploadBuffer(D3DDevice, &Instance, sizeof(Instance));

		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Top{};
		Top.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		Top.NumDescs      = 1;
		Top.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
		Top.InstanceDescs = InstanceBuffer->GetGPUVirtualAddress();
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO TopInfo{};
		Device5->GetRaytracingAccelerationStructurePrebuildInfo(&Top, &TopInfo);
		ComPtr<ID3D12Resource> Tlas = CreateBuffer(D3DDevice, TopInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		                                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
		const uint64 ScratchSize = std::max(BottomInfo.ScratchDataSizeInBytes, TopInfo.ScratchDataSizeInBytes);
		ComPtr<ID3D12Resource> Scratch   = CreateBuffer(D3DDevice, ScratchSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		                                                D3D12_RESOURCE_STATE_COMMON);
		ComPtr<ID3D12Resource> Postbuild = CreateBuffer(D3DDevice, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		ComPtr<ID3D12Resource> Results   = CreateBuffer(D3DDevice, 8 * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
		ComPtr<ID3D12Resource> Readback  = CreateBuffer(D3DDevice, 8 * 16 * 2 + 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

		// 계산 셰이더 RayQuery
		const ComPtr<IDxcBlob> Shader = Compiler.Compile(MakeVirtualDesc(L"RayQueryTest.hlsl", RayQueryTestSource, L"CSMain", EShaderStage::Compute, L"6_5"));
		FD3D12RootSignature    Root;
		Root.AddShaderResourceView(0);
		Root.AddUnorderedAccessView(0);
		Root.AddConstants(1, 0);
		E_EXPECT_TRUE(Shader != nullptr && Root.Finalize(D3DDevice, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"RayQueryTestRoot"));
		FD3D12PipelineState Pipeline;
		E_EXPECT_TRUE(Shader != nullptr && Pipeline.InitCompute(D3DDevice, Root.Get(), FD3D12ShaderCompiler::ToBytecode(Shader.Get()), L"RayQueryTest"));

		const auto Dispatch = [&](uint32 Mask, uint64 ReadbackOffset) {
			return Direct.ExecuteImmediate(D3DDevice, [&](ID3D12GraphicsCommandList* List) {
				List->SetComputeRootSignature(Root.Get());
				List->SetPipelineState(Pipeline.Get());
				List->SetComputeRootShaderResourceView(0, Tlas->GetGPUVirtualAddress());
				List->SetComputeRootUnorderedAccessView(1, Results->GetGPUVirtualAddress());
				List->SetComputeRoot32BitConstant(2, Mask, 0);
				List->Dispatch(1, 1, 1);
				const D3D12_RESOURCE_BARRIER ToCopy = MakeTransitionBarrier(Results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
				List->ResourceBarrier(1, &ToCopy);
				List->CopyBufferRegion(Readback.Get(), ReadbackOffset, Results.Get(), 0, 8 * 16);
			});
		};
		E_EXPECT_TRUE(Direct.ExecuteImmediate(D3DDevice, [&](ID3D12GraphicsCommandList* List) {
			ComPtr<ID3D12GraphicsCommandList4> List4;
			E_EXPECT_TRUE(SUCCEEDED(List->QueryInterface(IID_PPV_ARGS(&List4))));
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC Build{};
			Build.Inputs                           = Bottom;
			Build.DestAccelerationStructureData    = Blas->GetGPUVirtualAddress();
			Build.ScratchAccelerationStructureData = Scratch->GetGPUVirtualAddress();
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC PostbuildDesc{};
			PostbuildDesc.InfoType   = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
			PostbuildDesc.DestBuffer = Postbuild->GetGPUVirtualAddress();
			List4->BuildRaytracingAccelerationStructure(&Build, 1, &PostbuildDesc);
			const D3D12_RESOURCE_BARRIER Uavs[2] = { MakeUavBarrier(Blas.Get()), MakeUavBarrier(Scratch.Get()) };
			List->ResourceBarrier(2, Uavs);
			Build.Inputs                           = Top;
			Build.DestAccelerationStructureData    = Tlas->GetGPUVirtualAddress();
			List4->BuildRaytracingAccelerationStructure(&Build, 0, nullptr);
			const D3D12_RESOURCE_BARRIER TlasUav = MakeUavBarrier(Tlas.Get());
			List->ResourceBarrier(1, &TlasUav);
			const D3D12_RESOURCE_BARRIER ToCopy = MakeTransitionBarrier(Postbuild.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			List->ResourceBarrier(1, &ToCopy);
			List->CopyBufferRegion(Readback.Get(), 8 * 16 * 2, Postbuild.Get(), 0, 8);
		}));
		E_EXPECT_TRUE(Dispatch(0x01, 0));    // 인스턴스 마스크 맞음
		E_EXPECT_TRUE(Dispatch(0x02, 8 * 16)); // 마스크가 달라 모두 빗나감
		Direct.Flush();

		uint8* Mapped = nullptr;
		E_EXPECT_TRUE(SUCCEEDED(Readback->Map(0, nullptr, reinterpret_cast<void**>(&Mapped))));
		if (Mapped != nullptr)
		{
			const float* Hits   = reinterpret_cast<const float*>(Mapped);
			const float* Masked = reinterpret_cast<const float*>(Mapped + 8 * 16);
			// 광선 0~3, 7은 삼각형 안 (거리 10, InstanceID 7), 4~6은 밖
			const bool bExpectedHit[8] = { true, true, true, true, false, false, false, true };
			for (uint32 Ray = 0; Ray < 8; ++Ray)
			{
				if (bExpectedHit[Ray])
				{
					E_EXPECT_NEAR(Hits[Ray * 4 + 0], 10.0f, 1.0e-4f);
					E_EXPECT_NEAR(Hits[Ray * 4 + 1], 7.0f, 1.0e-6f);
					E_EXPECT_TRUE(Hits[Ray * 4 + 2] >= 0.0f && Hits[Ray * 4 + 3] >= 0.0f && Hits[Ray * 4 + 2] + Hits[Ray * 4 + 3] <= 1.0f + 1.0e-5f);
				}
				else
				{
					E_EXPECT_EQ(Hits[Ray * 4 + 0], -1.0f);
				}
				E_EXPECT_EQ(Masked[Ray * 4 + 0], -1.0f);
			}
			// 광선 0 (0, 0): 무게중심 = 정점 1·2 비중 — P = w0·V0 + b.x·V1 + b.y·V2 → (0,0) = (-1+2bx+by... ) 해: bx = 0.25, by = 0.5
			E_EXPECT_NEAR(Hits[2], 0.25f, 1.0e-4f);
			E_EXPECT_NEAR(Hits[3], 0.5f, 1.0e-4f);
			uint64 CompactedSize = 0;
			std::memcpy(&CompactedSize, Mapped + 8 * 16 * 2, sizeof(CompactedSize));
			E_EXPECT_TRUE(CompactedSize > 0 && CompactedSize <= BottomInfo.ResultDataMaxSizeInBytes);
			Readback->Unmap(0, nullptr);
		}
		E_EXPECT_EQ(CountDebugIssues(D3DDevice), 0ull);
		Pipeline.Shutdown();
		Root.Shutdown();
		Direct.Shutdown();
	}
	Compiler.Shutdown();
	Device.Shutdown();
}
