#include "Editor/AssetEditors/AssetEditorManager.h"

#include "Core/CommandLine.h"
#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/BehaviorTreeEditor.h"
#include "Editor/AssetEditors/DataAssetEditor.h"
#include "Editor/AssetEditors/DataStructEditor.h"
#include "Editor/AssetEditors/DataTableEditor.h"
#include "Editor/AssetEditors/MaterialEditor.h"
#include "Editor/AssetEditors/ModelEditors.h"
#include "Editor/AssetEditors/ParticleEditor.h"
#include "Editor/AssetEditors/PrefabEditor.h"
#include "Editor/AssetEditors/WidgetEditor.h"
#include "Editor/AssetEditors/StringTableEditor.h"
#include "Editor/AssetEditors/AnimGraphEditor.h"
#include "Scene/AnimGraph.h"
#include "Editor/AssetEditors/SequenceEditor.h"
#include "Scene/Sequence.h"
#include "Scene/Prefab.h"
#include "Editor/ContentBrowser/AssetFileOps.h"
#include "Scene/Particles.h"
#include "UI/UIAsset.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorGrid.h"
#include "RHI/D3D12/D3D12RHI.h"

#include <imgui.h>

#include <algorithm>
#include <cwctype>
#include <format>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	std::wstring ToLowerExtension(const std::filesystem::path& Path)
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Extension;
	}

	std::wstring MakeKey(const std::filesystem::path& Path)
	{
		std::error_code             ErrorCode;
		const std::filesystem::path Canonical = std::filesystem::weakly_canonical(Path, ErrorCode);
		std::wstring                Key       = (ErrorCode ? Path : Canonical).wstring();
		std::transform(Key.begin(), Key.end(), Key.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Key;
	}

	bool IsModelExtension(const std::wstring& Extension) { return Extension == L".glb" || Extension == L".gltf" || Extension == L".fbx"; }

	constexpr const wchar_t* BehaviorTreeExtension = L".ebt"; // FBehaviorTreeAsset::Extension

	std::unique_ptr<FAssetEditor> CreateEditor(const std::filesystem::path& Path, FResourceManager& Resources)
	{
		const std::wstring Extension = ToLowerExtension(Path);
		if (Extension == FMaterialAsset::Extension)
		{
			return std::make_unique<FMaterialEditor>(Path);
		}
		if (Extension == FParticleSystemAsset::Extension)
		{
			return std::make_unique<FParticleEditor>(Path);
		}
		if (Extension == FPrefabLibrary::Extension)
		{
			return std::make_unique<FPrefabEditor>(Path);
		}
		if (Extension == BehaviorTreeExtension)
		{
			return std::make_unique<FBehaviorTreeEditor>(Path);
		}
		if (Extension == FUIAsset::Extension)
		{
			return std::make_unique<FWidgetEditor>(Path);
		}
		if (Extension == FStringTable::Extension)
		{
			return std::make_unique<FStringTableEditor>(Path);
		}
		if (Extension == FAnimGraphAsset::Extension)
		{
			return std::make_unique<FAnimGraphEditor>(Path);
		}
		if (Extension == FSequenceAsset::Extension)
		{
			return std::make_unique<FSequenceEditor>(Path);
		}
		if (Extension == FDataTable::Extension)
		{
			return std::make_unique<FDataTableEditor>(Path);
		}
		if (Extension == FDataAsset::Extension)
		{
			return std::make_unique<FDataAssetEditor>(Path);
		}
		if (Extension == FDataStruct::Extension)
		{
			return std::make_unique<FDataStructEditor>(Path);
		}
		if (IsModelExtension(Extension))
		{
			// 애니메이션이 있는 모델은 애니메이션 편집기, 없으면 스태틱 메시 편집기
			if (FModelEditorBase::HasAnimations(Path, Resources))
			{
				return std::make_unique<FAnimationEditor>(Path);
			}
			return std::make_unique<FStaticMeshEditor>(Path);
		}
		return nullptr;
	}
} // namespace

FAssetEditorManager::FAssetEditorManager()  = default;
FAssetEditorManager::~FAssetEditorManager() = default;

void FAssetEditorManager::Shutdown(FEditorContext& Context)
{
	FAssetEditorEnvironment Env = MakeEnvironment(Context);
	for (FOpenEditor& Open : Editors)
	{
		Open.Editor->Close(Env);
	}
	Editors.clear();
	if (Grid)
	{
		Grid->Shutdown();
		Grid.reset();
	}
	if (bRendererReady)
	{
		UIRenderer.Shutdown();
		PreviewRenderer.Shutdown();
		bRendererReady = false;
	}
}

bool FAssetEditorManager::CanOpen(const std::filesystem::path& Path)
{
	const std::wstring Extension = ToLowerExtension(Path);
	return Extension == FMaterialAsset::Extension || Extension == FParticleSystemAsset::Extension || Extension == FPrefabLibrary::Extension ||
	       Extension == BehaviorTreeExtension || Extension == FUIAsset::Extension || IsModelExtension(Extension) ||
	       Extension == FStringTable::Extension || Extension == FAnimGraphAsset::Extension || Extension == FSequenceAsset::Extension ||
	       Extension == FDataTable::Extension || Extension == FDataAsset::Extension || Extension == FDataStruct::Extension;
}

bool FAssetEditorManager::EnsureRenderer(FEditorContext& Context)
{
	if (bRendererReady)
	{
		return true;
	}
	if (bRendererFailed)
	{
		return false;
	}
	if (!PreviewRenderer.Init(*Context.Rhi, *Context.Resources))
	{
		E_LOG(LogEditor, Error, "에셋 미리보기 렌더러 초기화 실패");
		bRendererFailed = true;
		return false;
	}
	PreviewRenderer.BackgroundColor = FVector4(0.08f, 0.09f, 0.11f, 1.0f);
	Grid                            = std::make_unique<FEditorGrid>();
	if (!Grid->Init(*Context.Rhi, PreviewRenderer.GetShaderLibrary()))
	{
		Grid.reset();
	}
	if (!UIRenderer.Init(*Context.Rhi, PreviewRenderer.GetShaderLibrary(), *Context.Resources, FD3D12RHI::RenderTargetFormat))
	{
		E_LOG(LogEditor, Warning, "UI 미리보기 렌더러 초기화 실패 (UI 디자이너 미리보기 없음)");
	}
	bRendererReady = true;
	return true;
}

FAssetEditorEnvironment FAssetEditorManager::MakeEnvironment(FEditorContext& Context)
{
	FAssetEditorEnvironment Env;
	Env.Editor          = &Context;
	Env.Rhi             = Context.Rhi;
	Env.Resources       = Context.Resources;
	Env.PreviewRenderer = bRendererReady ? &PreviewRenderer : nullptr;
	Env.Grid            = Grid.get();
	Env.UIRenderer      = bRendererReady && UIRenderer.IsInitialized() ? &UIRenderer : nullptr;
	return Env;
}

bool FAssetEditorManager::Open(FEditorContext& Context, const std::filesystem::path& Path)
{
	const std::wstring Key = MakeKey(Path);
	for (FOpenEditor& Open : Editors)
	{
		if (Open.Key == Key)
		{
			Open.bRequestFocus = true;
			return true;
		}
	}

	std::unique_ptr<FAssetEditor> Editor = CreateEditor(Path, *Context.Resources);
	if (!Editor)
	{
		E_LOG(LogEditor, Warning, "편집기가 없는 에셋 형식입니다: {}", FStringConv::ToUtf8(Path.filename().wstring()));
		return false;
	}
	if (!EnsureRenderer(Context))
	{
		return false;
	}
	FAssetEditorEnvironment Env = MakeEnvironment(Context);
	if (!Editor->Open(Env))
	{
		E_LOG(LogEditor, Error, "에셋을 열지 못했습니다: {}", FStringConv::ToUtf8(Path.wstring()));
		return false;
	}
	E_LOG(LogEditor, Display, "{} 편집기 열기: {}", Editor->GetTypeName(), Editor->GetDisplayName());
	Editors.push_back(FOpenEditor{ std::move(Editor), Key, true });
	return true;
}

void FAssetEditorManager::Update(FEditorContext& Context, float DeltaSeconds)
{
	if (Editors.empty())
	{
		return;
	}
	FAssetEditorEnvironment Env = MakeEnvironment(Context);
	for (FOpenEditor& Open : Editors)
	{
		Open.Editor->Update(Env, DeltaSeconds);
	}
}

void FAssetEditorManager::HandleShortcuts(FAssetEditorEnvironment& Env, FAssetEditor& Editor)
{
	if (ImGui::GetIO().WantTextInput)
	{
		return;
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
	{
		Editor.Save(Env);
	}
	if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
	{
		Editor.Redo(Env);
	}
	else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
	{
		Editor.Undo(Env);
	}
}

void FAssetEditorManager::Draw(FEditorContext& Context)
{
	bEditorFocused = false;
	if (Editors.empty())
	{
		return;
	}
	FAssetEditorEnvironment Env = MakeEnvironment(Context);

	std::vector<size_t> ToClose;
	for (size_t Index = 0; Index < Editors.size(); ++Index)
	{
		FOpenEditor&  Open   = Editors[Index];
		FAssetEditor& Editor = *Open.Editor;

		// 제목의 변경 표시가 바뀌어도 창 ID(도킹 위치)는 경로로 고정
		const std::string Title = std::format("{}: {}###AssetEditor_{}", Editor.GetTypeName(), Editor.GetDisplayName(), FStringConv::ToUtf8(Open.Key));
		if (Open.bRequestFocus)
		{
			ImGui::SetNextWindowFocus();
			Open.bRequestFocus = false;
		}
		// 처음 열 때 메인 창의 70% 크기로 가운데
		const ImGuiViewport* MainViewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowSize(ImVec2(MainViewport->WorkSize.x * 0.7f, MainViewport->WorkSize.y * 0.75f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowPos(ImVec2(MainViewport->WorkPos.x + MainViewport->WorkSize.x * 0.5f, MainViewport->WorkPos.y + MainViewport->WorkSize.y * 0.5f),
		                        ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
		// 자동 검증: --asset-window x,y,w,h (편집 창 위치/크기, 뷰포트를 함께 찍을 때)
		static const std::wstring WindowArg = FCommandLine::FromProcess().GetValue(L"--asset-window");
		if (float Rect[4] = {}; !WindowArg.empty() && swscanf_s(WindowArg.c_str(), L"%f,%f,%f,%f", &Rect[0], &Rect[1], &Rect[2], &Rect[3]) == 4)
		{
			ImGui::SetNextWindowPos(ImVec2(MainViewport->WorkPos.x + Rect[0], MainViewport->WorkPos.y + Rect[1]), ImGuiCond_Appearing);
			ImGui::SetNextWindowSize(ImVec2(Rect[2], Rect[3]), ImGuiCond_Appearing);
		}
		ImGui::SetNextWindowBgAlpha(1.0f);

		bool             bOpen = true;
		ImGuiWindowFlags Flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		if (Editor.IsDirty())
		{
			Flags |= ImGuiWindowFlags_UnsavedDocument;
		}
		if (ImGui::Begin(Title.c_str(), &bOpen, Flags))
		{
			ImGui::PushID(static_cast<int>(Index));
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
			{
				bEditorFocused = true;
				HandleShortcuts(Env, Editor);
			}
			Editor.Draw(Env);
			ImGui::PopID();
		}
		ImGui::End();
		Editor.CommitPendingEdit(ImGui::IsAnyItemActive());

		if (!bOpen)
		{
			if (Editor.IsDirty())
			{
				PendingCloseKey = Open.Key;
				ImGui::OpenPopup("저장하지 않은 변경##AssetEditorClose");
			}
			else
			{
				ToClose.push_back(Index);
			}
		}
	}
	for (auto It = ToClose.rbegin(); It != ToClose.rend(); ++It)
	{
		CloseEditor(Context, *It);
	}
	DrawCloseConfirm(Context);
}

void FAssetEditorManager::DrawCloseConfirm(FEditorContext& Context)
{
	if (!ImGui::BeginPopupModal("저장하지 않은 변경##AssetEditorClose", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}
	const auto Found = std::find_if(Editors.begin(), Editors.end(), [this](const FOpenEditor& Open) { return Open.Key == PendingCloseKey; });
	if (Found == Editors.end())
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	FAssetEditorEnvironment Env = MakeEnvironment(Context);
	ImGui::Text("'%s'에 저장하지 않은 변경이 있습니다.", Found->Editor->GetDisplayName().c_str());
	ImGui::TextDisabled("저장하지 않고 닫으면 파일에 저장된 상태로 돌아갑니다.");
	ImGui::Spacing();
	const size_t Index   = static_cast<size_t>(Found - Editors.begin());
	bool         bClosed = false;
	if (ImGui::Button("저장 후 닫기"))
	{
		if (Found->Editor->Save(Env))
		{
			bClosed = true;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("저장 안 함"))
	{
		bClosed = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("취소"))
	{
		PendingCloseKey.clear();
		ImGui::CloseCurrentPopup();
	}
	if (bClosed)
	{
		PendingCloseKey.clear();
		ImGui::CloseCurrentPopup();
		CloseEditor(Context, Index);
	}
	ImGui::EndPopup();
}

void FAssetEditorManager::CloseEditor(FEditorContext& Context, size_t Index)
{
	FAssetEditorEnvironment Env = MakeEnvironment(Context);
	Editors[Index].Editor->Close(Env);
	Editors.erase(Editors.begin() + static_cast<std::ptrdiff_t>(Index));
}

void FAssetEditorManager::RenderPreviews(FEditorContext& Context)
{
	if (!bRendererReady)
	{
		return;
	}
	FAssetEditorEnvironment Env = MakeEnvironment(Context);
	for (FOpenEditor& Open : Editors)
	{
		Open.Editor->RenderPreview(Env);
	}
}

bool FAssetEditorManager::ReloadShaders(const std::vector<std::filesystem::path>* ChangedFiles)
{
	if (!bRendererReady)
	{
		return true;
	}
	// 전용 렌더러는 셰이더 라이브러리(메모리 캐시)도 따로 가지므로 같은 파일을 무효화한다
	FShaderLibrary& Library = PreviewRenderer.GetShaderLibrary();
	if (ChangedFiles == nullptr)
	{
		Library.InvalidateAll();
	}
	else
	{
		for (const std::filesystem::path& File : *ChangedFiles)
		{
			Library.Invalidate(File);
		}
	}
	const bool bForce = ChangedFiles == nullptr;
	bool       bOk    = PreviewRenderer.ReloadShaders(bForce);
	if (Grid)
	{
		bOk = Grid->ReloadShaders(bForce) && bOk;
	}
	bOk = UIRenderer.ReloadShaders(bForce) && bOk;
	return bOk;
}

uint32 FAssetEditorManager::VerifyCloseWithoutSave(FEditorContext& Context)
{
	FAssetEditorEnvironment Env    = MakeEnvironment(Context);
	uint32                  Closed = 0;
	while (!Editors.empty())
	{
		FAssetEditor& Editor = *Editors.back().Editor;
		const bool    bEdited = Editor.HasEditableState() && Editor.ApplyTestEdit(Env);
		E_LOG(LogEditor, Display, "자동 검증: {} 편집 {} → 저장하지 않고 닫기", Editor.GetDisplayName(), bEdited ? "적용" : "없음");
		CloseEditor(Context, Editors.size() - 1);
		++Closed;
	}
	return Closed;
}

bool FAssetEditorManager::CloseEditorsFor(FEditorContext& Context, const std::vector<std::filesystem::path>& Paths)
{
	std::vector<size_t> Matches;
	for (size_t Index = 0; Index < Editors.size(); ++Index)
	{
		for (const std::filesystem::path& Path : Paths)
		{
			if (FAssetFileOps::IsSameOrUnder(Editors[Index].Editor->GetPath(), Path))
			{
				if (Editors[Index].Editor->IsDirty())
				{
					Editors[Index].bRequestFocus = true;
					return false;
				}
				Matches.push_back(Index);
				break;
			}
		}
	}
	for (auto It = Matches.rbegin(); It != Matches.rend(); ++It)
	{
		CloseEditor(Context, *It);
	}
	return true;
}

void FAssetEditorManager::SwapScenePreviews(FEditorContext& Context)
{
	FAssetEditorEnvironment Env = MakeEnvironment(Context);
	for (FOpenEditor& Open : Editors)
	{
		Open.Editor->SwapScenePreview(Env);
	}
}

void FAssetEditorManager::CollectResourceRoots(FResourceRoots& Roots)
{
	for (FOpenEditor& Open : Editors)
	{
		Open.Editor->CollectResourceRoots(Roots);
	}
}

void FAssetEditorManager::EndScenePreviews(FEditorContext& Context)
{
	FAssetEditorEnvironment Env = MakeEnvironment(Context);
	for (FOpenEditor& Open : Editors)
	{
		Open.Editor->EndScenePreview(Env);
	}
}

void FAssetEditorManager::OnModelReimported(FEditorContext& Context, const std::filesystem::path& Path)
{
	FAssetEditorEnvironment Env = MakeEnvironment(Context);
	const std::wstring      Key = MakeKey(Path);
	for (FOpenEditor& Open : Editors)
	{
		if (Open.Key == Key)
		{
			if (FModelEditorBase* ModelEditor = dynamic_cast<FModelEditorBase*>(Open.Editor.get()))
			{
				ModelEditor->RebuildPreview(Env);
			}
		}
	}
}
