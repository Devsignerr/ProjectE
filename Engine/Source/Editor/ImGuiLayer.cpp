#include "Editor/ImGuiLayer.h"

// ImGuizmo.h는 imgui.h 다음, Windows.h(열거형 이름 충돌)보다 먼저 포함
#include <imgui.h>
#include <ImGuizmo.h>

#include "Core/Log.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"
#include "Core/Window.h"
#include "Editor/EditorTheme.h"
#include "RHI/D3D12/D3D12RHI.h"

#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <shellapi.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

E_DECLARE_LOG_CATEGORY(LogEditor)
E_DEFINE_LOG_CATEGORY(LogEditor, Log)

FImGuiLayer::~FImGuiLayer()
{
	Shutdown();
}

bool FImGuiLayer::Init(FWindow& InWindow, FD3D12RHI& InRhi, const std::filesystem::path& InIniFilePath)
{
	E_CHECKF(!bInitialized, "ImGui 레이어가 이미 초기화되어 있습니다");
	Rhi    = &InRhi;
	Window = &InWindow;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& IO = ImGui::GetIO();
	IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
	IO.ConfigWindowsMoveFromTitleBarOnly = true;

	// 레이아웃 저장 파일
	std::error_code ErrorCode;
	std::filesystem::create_directories(InIniFilePath.parent_path(), ErrorCode);
	IniFilePath    = FStringConv::ToUtf8(InIniFilePath.wstring());
	IO.IniFilename = IniFilePath.c_str();

	// DPI 스케일 + 테마(언리얼 5 풍) + 한글 글꼴(맑은 고딕, 없으면 기본) + 아이콘 글꼴
	DpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(Window->GetHandle());
	FEditorTheme::Apply(DpiScale);
	FEditorTheme::LoadFonts(DpiScale);

	if (!ImGui_ImplWin32_Init(Window->GetHandle()))
	{
		E_LOG(LogEditor, Error, "ImGui Win32 백엔드 초기화 실패");
		return false;
	}

	// D3D12 백엔드: 폰트/텍스처 SRV는 엔진의 셰이더 가시 할당자에서 할당
	ImGui_ImplDX12_InitInfo InitInfo{};
	InitInfo.Device            = Rhi->GetDevice().GetDevice();
	InitInfo.CommandQueue      = Rhi->GetGraphicsQueue().GetQueue();
	InitInfo.NumFramesInFlight = static_cast<int>(FD3D12RHI::FrameCount);
	InitInfo.RTVFormat         = FD3D12SwapChain::BackBufferFormat; // UI는 UNORM 뷰에 그린다
	InitInfo.DSVFormat         = DXGI_FORMAT_UNKNOWN;
	InitInfo.SrvDescriptorHeap = Rhi->GetSrvAllocator().GetHeap();
	InitInfo.UserData          = &Rhi->GetSrvAllocator();
	InitInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* Info, D3D12_CPU_DESCRIPTOR_HANDLE* OutCpu, D3D12_GPU_DESCRIPTOR_HANDLE* OutGpu) {
		const FD3D12DescriptorHandle Handle = static_cast<FD3D12DescriptorAllocator*>(Info->UserData)->Allocate();
		*OutCpu = Handle.Cpu;
		*OutGpu = Handle.Gpu;
	};
	InitInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* Info, D3D12_CPU_DESCRIPTOR_HANDLE Cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
		static_cast<FD3D12DescriptorAllocator*>(Info->UserData)->FreeByCpuHandle(Cpu);
	};
	if (!ImGui_ImplDX12_Init(&InitInfo))
	{
		E_LOG(LogEditor, Error, "ImGui D3D12 백엔드 초기화 실패");
		return false;
	}

	// 창 메시지 전달 + 윈도우 탐색기에서 끌어 놓은 파일 받기 (WM_DROPFILES)
	DragAcceptFiles(Window->GetHandle(), TRUE);
	Window->SetMessageHook([this](HWND Hwnd, uint32 Message, uint64 WParam, int64 LParam) {
		if (Message == WM_DROPFILES)
		{
			const HDROP Drop  = reinterpret_cast<HDROP>(WParam);
			const UINT  Count = DragQueryFileW(Drop, 0xFFFFFFFF, nullptr, 0);
			for (UINT Index = 0; Index < Count; ++Index)
			{
				const UINT   Length = DragQueryFileW(Drop, Index, nullptr, 0);
				std::wstring Path(Length, L' ');
				DragQueryFileW(Drop, Index, Path.data(), Length + 1);
				DroppedFiles.emplace_back(Path);
			}
			DragFinish(Drop);
			return true;
		}
		return ImGui_ImplWin32_WndProcHandler(Hwnd, Message, WParam, LParam) != 0;
	});

	bInitialized = true;
	E_LOG(LogEditor, Display, "ImGui 초기화 완료 (버전 {}, DPI 스케일 {:.2f})", IMGUI_VERSION, DpiScale);
	return true;
}

void FImGuiLayer::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}

	// 백엔드가 SRV를 반환하기 전에 GPU 작업 완료 보장
	Rhi->GetGraphicsQueue().Flush();

	Window->SetMessageHook(nullptr);
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	bInitialized = false;
	Rhi          = nullptr;
	Window       = nullptr;
}

void FImGuiLayer::BeginFrame()
{
	E_CHECKF(bInitialized && !bFrameBegun, "ImGui 프레임 순서 오류");

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGuizmo::BeginFrame(); // 매 프레임 필수 — 없으면 기즈모 상호작용이 동작하지 않는다

	// 메인 창 전체를 도크스페이스로 (중앙은 뷰포트가 차지하도록 패스스루)
	DockSpaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

	bFrameBegun = true;
}

void FImGuiLayer::EndFrame(ID3D12GraphicsCommandList* CommandList)
{
	E_CHECKF(bFrameBegun, "BeginFrame 없이 EndFrame이 호출되었습니다");

	ImGui::Render();
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), CommandList);

	bFrameBegun = false;
}

bool FImGuiLayer::WantCaptureMouse() const
{
	return bInitialized && ImGui::GetIO().WantCaptureMouse;
}

bool FImGuiLayer::WantCaptureKeyboard() const
{
	return bInitialized && ImGui::GetIO().WantCaptureKeyboard;
}
