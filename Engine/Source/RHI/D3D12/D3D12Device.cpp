#include "RHI/D3D12/D3D12Device.h"

#include "Core/StringConv.h"
#include "RHI/D3D12/D3D12MipGenerator.h"

#include <dxgidebug.h>

FD3D12Device::FD3D12Device()  = default;
FD3D12Device::~FD3D12Device() = default;

FD3D12MipGenerator* FD3D12Device::GetMipGenerator()
{
	if (MipGenerator == nullptr && !bMipGeneratorFailed && Device != nullptr)
	{
		auto Generator = std::make_unique<FD3D12MipGenerator>();
		if (Generator->Init(Device.Get()))
		{
			MipGenerator = std::move(Generator);
		}
		else
		{
			bMipGeneratorFailed = true;
			E_LOG(LogD3D12, Warning, "밉맵 생성기 초기화 실패 — 이후 텍스처는 밉 없이 생성됩니다");
		}
	}
	return MipGenerator.get();
}

bool FD3D12Device::Init(bool bEnableDebugLayer)
{
	UINT FactoryFlags = 0;

	if (bEnableDebugLayer)
	{
		ComPtr<ID3D12Debug> DebugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&DebugController))))
		{
			DebugController->EnableDebugLayer();
			FactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
			bDebugLayerEnabled = true;
			E_LOG(LogD3D12, Display, "D3D12 디버그 레이어 활성화");
		}
		else
		{
			E_LOG(LogD3D12, Warning, "D3D12 디버그 레이어를 사용할 수 없습니다 (Windows 선택적 기능 '그래픽 도구' 설치 필요)");
		}
	}

	E_D3D_VERIFY(CreateDXGIFactory2(FactoryFlags, IID_PPV_ARGS(&Factory)));

	if (!SelectAdapter())
	{
		return false;
	}

	E_D3D_VERIFY(D3D12CreateDevice(Adapter.Get(), MinFeatureLevel, IID_PPV_ARGS(&Device)));
	Device->SetName(L"MainDevice");

	if (bDebugLayerEnabled)
	{
		// 심각한 메시지에서 디버거 중단
		ComPtr<ID3D12InfoQueue> InfoQueue;
		if (SUCCEEDED(Device.As(&InfoQueue)))
		{
			// 디버거가 붙어 있을 때만 중단 (자동 검증 실행에서는 로그로 남기고 계속)
			const BOOL bBreak = IsDebuggerPresent() ? TRUE : FALSE;
			InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, bBreak);
			InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, bBreak);
		}

		// 디버그 레이어 메시지를 엔진 로그로 (Windows 11 SDK의 ID3D12InfoQueue1)
		ComPtr<ID3D12InfoQueue1> InfoQueue1;
		if (SUCCEEDED(Device.As(&InfoQueue1)))
		{
			DWORD CallbackCookie = 0;
			InfoQueue1->RegisterMessageCallback(
				[](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY Severity, D3D12_MESSAGE_ID Id, LPCSTR Description, void*) {
					if (Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
					{
						E_LOG(LogD3D12, Error, "[디버그 레이어] ({}) {}", static_cast<int32>(Id), Description);
					}
					else if (Severity == D3D12_MESSAGE_SEVERITY_WARNING)
					{
						E_LOG(LogD3D12, Warning, "[디버그 레이어] ({}) {}", static_cast<int32>(Id), Description);
					}
				},
				D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &CallbackCookie);
		}
	}

	for (uint32 Type = 0; Type < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++Type)
	{
		DescriptorSizes[Type] = Device->GetDescriptorHandleIncrementSize(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(Type));
	}

	BOOL bAllowTearing = FALSE;
	if (FAILED(Factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &bAllowTearing, sizeof(bAllowTearing))))
	{
		bAllowTearing = FALSE;
	}
	bTearingSupported = (bAllowTearing == TRUE);

	E_LOG(LogD3D12, Display, "D3D12 디바이스 생성 완료 (테어링 지원: {})", bTearingSupported);
	return true;
}

void FD3D12Device::Shutdown()
{
	// 디바이스보다 먼저 파생 오브젝트 해제
	MipGenerator.reset();
	bMipGeneratorFailed = false;

	Device.Reset();
	Adapter.Reset();
	Factory.Reset();

	if (bDebugLayerEnabled)
	{
		// 해제되지 않은 D3D/DXGI 오브젝트를 디버거 출력 창에 보고
		ComPtr<IDXGIDebug1> DxgiDebug;
		if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&DxgiDebug))))
		{
			E_LOG(LogD3D12, Display, "라이브 D3D 오브젝트 보고 (디버거 출력 창 확인)");
			DxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL,
			                             static_cast<DXGI_DEBUG_RLO_FLAGS>(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
		}
	}
}

bool FD3D12Device::SelectAdapter()
{
	ComPtr<IDXGIAdapter4> Candidate;
	for (UINT Index = 0;
	     Factory->EnumAdapterByGpuPreference(Index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&Candidate)) != DXGI_ERROR_NOT_FOUND;
	     ++Index)
	{
		DXGI_ADAPTER_DESC3 AdapterDesc{};
		Candidate->GetDesc3(&AdapterDesc);

		if (AdapterDesc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE)
		{
			continue;
		}

		// 디바이스 생성 가능 여부만 확인 (ppDevice = nullptr)
		if (SUCCEEDED(D3D12CreateDevice(Candidate.Get(), MinFeatureLevel, __uuidof(ID3D12Device), nullptr)))
		{
			Adapter = Candidate;
			E_LOG(LogD3D12, Display, "그래픽 어댑터: {} (전용 VRAM {} MB)",
			      FStringConv::ToUtf8(AdapterDesc.Description), AdapterDesc.DedicatedVideoMemory / (1024 * 1024));
			return true;
		}
	}

	E_LOG(LogD3D12, Error, "D3D12를 지원하는 그래픽 어댑터를 찾지 못했습니다");
	return false;
}
