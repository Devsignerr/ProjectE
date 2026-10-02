#include "Editor/Panels/ContentBrowserPanel.h"

#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Platform/WindowsHeaders.h"
#include "Core/StringConv.h"
#include "Editor/AssetEditors/AssetEditorManager.h"
#include "Editor/AssetEditors/BehaviorTreeEditor.h"
#include "Editor/AssetEditors/DataValueWidgets.h"
#include "Editor/ContentBrowser/AssetFileOps.h"
#include "Editor/ContentBrowser/AssetReferenceUpdater.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"
#include "Editor/ContentBrowser/ThumbnailCache.h"
#include "Editor/EditorActions.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Renderer/MaterialAsset.h"
#include "Renderer/ModelImportSettings.h"
#include "Renderer/ModelLoader.h"
#include "Scene/AnimGraph.h"
#include "Scene/Building/BuildingScene.h"
#include "Scene/DataLibrary.h"
#include "Scene/ModelMetadata.h"
#include "Scene/Sequence.h"
#include "Scene/Particles.h"
#include "Core/Settings/ProjectSettings.h"
#include "UI/Localization.h"
#include "UI/UIAsset.h"
#include "UI/UIComponent.h"
#include "Scene/Scene.h"

#include <imgui_internal.h> // ImHashStr

#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <format>

E_DECLARE_LOG_CATEGORY(LogEditor)

namespace
{
	std::string ToLower(std::string Text)
	{
		for (char& Char : Text)
		{
			Char = static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));
		}
		return Text;
	}

	bool IsModelExtension(const std::string& Extension) { return Extension == ".glb" || Extension == ".gltf" || Extension == ".fbx"; }
	bool IsSceneExtension(const std::string& Extension) { return Extension == ".escene"; }
	// 모델과 함께 다니는 사이드카 (임포트 설정 .eimport, 노티파이/소켓 .emeta). 원본 경로 + 확장자
	std::vector<std::filesystem::path> GetModelSidecars(const std::filesystem::path& Model)
	{
		return { FModelImportSettings::GetSidecarPath(Model), FModelMetadata::GetSidecarPath(Model) };
	}

	bool IsSidecarExtension(const std::string& Extension) { return Extension == ".eimport" || Extension == ".emeta"; }

	bool IsParticleExtension(const std::string& Extension) { return Extension == ".eparticle"; }
	bool IsPrefabExtension(const std::string& Extension) { return Extension == ".eprefab"; }
	bool IsUIExtension(const std::string& Extension) { return Extension == ".eui"; }
	bool IsBuildingExtension(const std::string& Extension) { return Extension == ".ebuilding"; }

	// 종류 필터 (0 = 전체). FEditorTheme::GetAssetStyle의 Label과 같은 이름
	constexpr const char* GTypeFilters[] = { "전체", "모델", "머티리얼", "텍스처", "파티클", "프리팹", "씬", "스크립트", "오디오" };

	// 폭에 맞게 말줄임 ("긴이름…")
	std::string Ellipsize(const std::string& Text, float MaxWidth)
	{
		if (ImGui::CalcTextSize(Text.c_str()).x <= MaxWidth)
		{
			return Text;
		}
		std::string Result = Text;
		while (!Result.empty())
		{
			// UTF-8 한 글자씩 제거
			size_t Cut = Result.size() - 1;
			while (Cut > 0 && (static_cast<unsigned char>(Result[Cut]) & 0xC0) == 0x80)
			{
				--Cut;
			}
			Result.erase(Cut);
			if (ImGui::CalcTextSize((Result + "…").c_str()).x <= MaxWidth)
			{
				return Result + "…";
			}
		}
		return "…";
	}

	std::string ToContentRelative(const std::filesystem::path& Path, const std::filesystem::path& Root)
	{
		std::error_code             ErrorCode;
		const std::filesystem::path Relative = std::filesystem::relative(Path, Root, ErrorCode);
		return FStringConv::ToUtf8((ErrorCode || Relative.empty() ? Path : Relative).generic_wstring());
	}

	ImGuiID MakeEntryId(const std::filesystem::path& Path)
	{
		const std::string Key = FStringConv::ToUtf8(Path.lexically_normal().generic_wstring());
		return ImHashStr(Key.c_str());
	}

	std::filesystem::path GetProjectFileOrEmpty()
	{
		return FPaths::HasProject() ? FPaths::GetProjectFile() : std::filesystem::path();
	}

	void ShowInExplorer(const std::filesystem::path& Path)
	{
		const std::wstring Arguments = L"/select,\"" + std::filesystem::absolute(Path).wstring() + L"\"";
		ShellExecuteW(nullptr, L"open", L"explorer.exe", Arguments.c_str(), nullptr, SW_SHOWNORMAL);
	}

	void OpenFolderInExplorer(const std::filesystem::path& Directory)
	{
		ShellExecuteW(nullptr, L"open", std::filesystem::absolute(Directory).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
} // namespace

FContentBrowserPanel::FContentBrowserPanel()
	: Selection(std::make_unique<ImGuiSelectionBasicStorage>())
	, Thumbnails(std::make_unique<FThumbnailCache>())
{
	// 선택은 경로 해시로 저장 (새로 고침 후에도 유지). 다중 선택 인덱스 = Visible 인덱스
	Selection->UserData                = this;
	Selection->AdapterIndexToStorageId = [](ImGuiSelectionBasicStorage* Storage, int Index) {
		const FContentBrowserPanel* Self = static_cast<const FContentBrowserPanel*>(Storage->UserData);
		return Self->Entries[Self->Visible[static_cast<size_t>(Index)]].Id;
	};
}

FContentBrowserPanel::~FContentBrowserPanel() = default;

void FContentBrowserPanel::Shutdown(FEditorContext& Context)
{
	Thumbnails->Shutdown(Context);
}

bool FContentBrowserPanel::ReloadShaders(const std::vector<std::filesystem::path>* ChangedFiles)
{
	return Thumbnails->ReloadShaders(ChangedFiles);
}

void FContentBrowserPanel::RenderThumbnails(FEditorContext& Context)
{
	if (bOpen)
	{
		Thumbnails->RenderPending(Context, 2);
	}
}

void FContentBrowserPanel::Notify(FEditorContext& Context, const std::string& Message, bool bError) const
{
	E_LOG(LogEditor, Display, "콘텐츠: {}", Message);
	if (Context.Notify)
	{
		Context.Notify(Message, bError);
	}
}

// ---------------------------------------------------------------- 목록

void FContentBrowserPanel::Refresh(const std::filesystem::path& InRoot)
{
	bNeedsRefresh = false;
	LastRefresh   = std::chrono::steady_clock::now();
	Root          = InRoot;
	Entries.clear();

	std::error_code ErrorCode;
	if (!std::filesystem::exists(CurrentDirectory, ErrorCode) || !FAssetFileOps::IsSameOrUnder(CurrentDirectory, Root))
	{
		CurrentDirectory = Root;
	}
	for (const std::filesystem::directory_entry& DirectoryEntry : std::filesystem::directory_iterator(CurrentDirectory, ErrorCode))
	{
		// 모델 사이드카(.eimport/.emeta)는 모델과 함께 다루므로 목록에서 숨긴다
		if (IsSidecarExtension(ToLower(FStringConv::ToUtf8(DirectoryEntry.path().extension().wstring()))))
		{
			continue;
		}
		FEntry Entry;
		Entry.Path        = DirectoryEntry.path();
		Entry.DisplayName = FStringConv::ToUtf8(Entry.Path.filename().wstring());
		Entry.Extension   = ToLower(FStringConv::ToUtf8(Entry.Path.extension().wstring()));
		Entry.bDirectory  = DirectoryEntry.is_directory(ErrorCode);
		Entry.SizeInBytes = Entry.bDirectory ? 0 : static_cast<uint64>(DirectoryEntry.file_size(ErrorCode));
		Entry.Id          = MakeEntryId(Entry.Path);
		Entries.push_back(std::move(Entry));
	}
	// 폴더 먼저, 이름순
	std::sort(Entries.begin(), Entries.end(), [](const FEntry& A, const FEntry& B) {
		if (A.bDirectory != B.bDirectory)
		{
			return A.bDirectory;
		}
		return ToLower(A.DisplayName) < ToLower(B.DisplayName);
	});

	// 폴더 트리 (콘텐츠 폴더 전체)
	const auto Build = [](auto& Self, const std::filesystem::path& Directory) -> FFolderNode {
		FFolderNode Node;
		Node.Path = Directory;
		Node.Name = FStringConv::ToUtf8(Directory.filename().wstring());
		std::error_code LocalError;
		for (const std::filesystem::directory_entry& Child : std::filesystem::directory_iterator(Directory, LocalError))
		{
			if (Child.is_directory(LocalError))
			{
				Node.Children.push_back(Self(Self, Child.path()));
			}
		}
		std::sort(Node.Children.begin(), Node.Children.end(), [](const FFolderNode& A, const FFolderNode& B) { return ToLower(A.Name) < ToLower(B.Name); });
		return Node;
	};
	FolderTree      = Build(Build, Root);
	FolderTree.Name = "Content";
}

void FContentBrowserPanel::Navigate(const std::filesystem::path& Directory)
{
	CurrentDirectory = Directory;
	bNeedsRefresh    = true;
	Selection->Clear();
}

bool FContentBrowserPanel::PassesFilter(const FEntry& Entry) const
{
	if (SearchText[0] != '\0' && ToLower(Entry.DisplayName).find(ToLower(SearchText)) == std::string::npos)
	{
		return false;
	}
	if (TypeFilter == 0)
	{
		return true;
	}
	if (Entry.bDirectory)
	{
		return false;
	}
	return std::string_view(FEditorTheme::GetAssetStyle(Entry.Extension, false).Label) == GTypeFilters[TypeFilter];
}

std::vector<std::filesystem::path> FContentBrowserPanel::GetSelectedPaths() const
{
	std::vector<std::filesystem::path> Paths;
	for (const FEntry& Entry : Entries)
	{
		if (Selection->Contains(Entry.Id))
		{
			Paths.push_back(Entry.Path);
		}
	}
	return Paths;
}

// ---------------------------------------------------------------- 그리기

void FContentBrowserPanel::Draw(FEditorContext& Context)
{
	bFocused = false;
	if (!bOpen)
	{
		return;
	}
	if (CurrentDirectory.empty())
	{
		CurrentDirectory = Context.ContentDirectory;
	}
	// 파일 탐색기 등 바깥 변경도 반영되도록 2초마다 다시 읽는다
	if (bNeedsRefresh || std::chrono::steady_clock::now() - LastRefresh > std::chrono::seconds(2))
	{
		Refresh(Context.ContentDirectory);
	}

	if (ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_FOLDER_OPEN, "콘텐츠", "ContentBrowser").c_str(), &bOpen))
	{
		bFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		DrawToolbar(Context);
		ImGui::Separator();

		if (ImGui::BeginTable("##ContentLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImGui::GetContentRegionAvail()))
		{
			ImGui::TableSetupColumn("폴더", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 12.0f);
			ImGui::TableSetupColumn("항목", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow();

			ImGui::TableNextColumn();
			if (ImGui::BeginChild("##FolderTree", ImVec2(0.0f, 0.0f)))
			{
				DrawFolderNode(Context, FolderTree, true);
			}
			ImGui::EndChild();

			ImGui::TableNextColumn();
			DrawItems(Context);
			ImGui::EndTable();
		}
		if (bFocused)
		{
			HandleShortcuts(Context);
		}
		DrawRenamePopup(Context);
		DrawDeletePopup(Context);
	}
	ImGui::End();
}

void FContentBrowserPanel::DrawToolbar(FEditorContext& Context)
{
	const bool bAtRoot = FAssetFileOps::IsSameOrUnder(Context.ContentDirectory, CurrentDirectory);
	ImGui::BeginDisabled(bAtRoot);
	if (ImGui::Button(ICON_FA_ARROW_UP))
	{
		Navigate(CurrentDirectory.parent_path());
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("상위 폴더 (Backspace)");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_PLUS " 추가"))
	{
		ImGui::OpenPopup("##CreateAsset");
	}
	if (ImGui::BeginPopup("##CreateAsset"))
	{
		DrawBackgroundContextMenu(Context);
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	DrawBreadcrumb(Context);

	// 오른쪽: 검색 / 종류 / 보기 방식 / 타일 크기 (경로가 길어 겹치면 다음 줄)
	const float RightWidth = ImGui::GetFontSize() * 25.0f;
	const float RightStart = ImGui::GetContentRegionMax().x - RightWidth;
	if (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x < RightStart)
	{
		ImGui::SameLine(RightStart);
	}
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
	ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS " 검색", SearchText, sizeof(SearchText));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
	ImGui::Combo("##TypeFilter", &TypeFilter, GTypeFilters, IM_ARRAYSIZE(GTypeFilters));
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_TABLE_CELLS, "타일 보기", !bListView))
	{
		bListView = false;
	}
	ImGui::SameLine();
	if (FEditorTheme::ToolButton(ICON_FA_LIST, "목록 보기", bListView))
	{
		bListView = true;
	}
	if (!bListView)
	{
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
		ImGui::SliderFloat("##TileSize", &TileSize, 64.0f, 200.0f, "");
		ImGui::SetItemTooltip("타일 크기");
	}
}

void FContentBrowserPanel::DrawBreadcrumb(FEditorContext& Context)
{
	// Content > Materials > ... (누르면 이동, 에셋을 끌어 놓으면 그 폴더로 이동)
	std::vector<std::filesystem::path> Segments;
	for (std::filesystem::path Directory = CurrentDirectory; FAssetFileOps::IsSameOrUnder(Directory, Context.ContentDirectory); Directory = Directory.parent_path())
	{
		Segments.push_back(Directory);
		if (FAssetFileOps::IsSameOrUnder(Context.ContentDirectory, Directory))
		{
			break;
		}
	}
	std::reverse(Segments.begin(), Segments.end());

	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
	for (size_t Index = 0; Index < Segments.size(); ++Index)
	{
		ImGui::PushID(static_cast<int>(Index));
		if (Index > 0)
		{
			ImGui::SameLine(0.0f, 2.0f);
			ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
			ImGui::SameLine(0.0f, 2.0f);
		}
		const std::string Label = Index == 0 ? std::string(ICON_FA_FOLDER " Content") : FStringConv::ToUtf8(Segments[Index].filename().wstring());
		if (ImGui::Button(Label.c_str()))
		{
			Navigate(Segments[Index]);
		}
		AcceptMoveDrop(Context, Segments[Index]);
		ImGui::PopID();
	}
	ImGui::PopStyleColor();
}

void FContentBrowserPanel::DrawFolderNode(FEditorContext& Context, const FFolderNode& Node, bool bRoot)
{
	ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (Node.Children.empty())
	{
		Flags |= ImGuiTreeNodeFlags_Leaf;
	}
	if (bRoot)
	{
		Flags |= ImGuiTreeNodeFlags_DefaultOpen;
	}
	const bool bCurrent = FAssetFileOps::IsSameOrUnder(Node.Path, CurrentDirectory) && FAssetFileOps::IsSameOrUnder(CurrentDirectory, Node.Path);
	if (bCurrent)
	{
		Flags |= ImGuiTreeNodeFlags_Selected;
	}
	// 현재 폴더의 조상은 펼쳐 둔다
	if (!bCurrent && FAssetFileOps::IsSameOrUnder(CurrentDirectory, Node.Path))
	{
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
	}
	ImGui::PushID(static_cast<int>(MakeEntryId(Node.Path)));
	if (bCurrent)
	{
		ImGui::PushStyleColor(ImGuiCol_Header, FEditorTheme::Accent);
	}
	const bool bOpened = ImGui::TreeNodeEx("##Folder", Flags, "%s %s", bCurrent ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER, Node.Name.c_str());
	if (bCurrent)
	{
		ImGui::PopStyleColor();
	}
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
	{
		Navigate(Node.Path);
	}
	AcceptMoveDrop(Context, Node.Path);
	if (bOpened)
	{
		for (const FFolderNode& Child : Node.Children)
		{
			DrawFolderNode(Context, Child, false);
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void FContentBrowserPanel::DrawItems(FEditorContext& Context)
{
	Visible.clear();
	for (size_t Index = 0; Index < Entries.size(); ++Index)
	{
		if (PassesFilter(Entries[Index]))
		{
			Visible.push_back(Index);
		}
	}

	if (!ImGui::BeginChild("##Items", ImVec2(0.0f, 0.0f)))
	{
		ImGui::EndChild();
		return;
	}
	// 빈 곳 우클릭 메뉴
	if (ImGui::BeginPopupContextWindow("##BackgroundMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		DrawBackgroundContextMenu(Context);
		ImGui::EndPopup();
	}

	const ImGuiMultiSelectFlags MultiFlags = ImGuiMultiSelectFlags_ClearOnEscape | ImGuiMultiSelectFlags_ClearOnClickVoid |
	                                         (bListView ? ImGuiMultiSelectFlags_BoxSelect1d : ImGuiMultiSelectFlags_BoxSelect2d);
	ImGuiMultiSelectIO* MultiIO = ImGui::BeginMultiSelect(MultiFlags, Selection->Size, static_cast<int>(Visible.size()));
	Selection->ApplyRequests(MultiIO);

	if (Visible.empty())
	{
		ImGui::TextDisabled("%s", SearchText[0] != '\0' || TypeFilter != 0 ? "조건에 맞는 항목이 없습니다"
		                                                                    : "빈 폴더입니다. 우클릭하거나 윈도우 탐색기에서 파일을 끌어 놓아 추가하세요");
	}

	if (bListView)
	{
		if (!Visible.empty() && ImGui::BeginTable("##List", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch, 3.0f);
			ImGui::TableSetupColumn("종류", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("크기", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableHeadersRow();
			for (size_t Row = 0; Row < Visible.size(); ++Row)
			{
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::SetNextItemSelectionUserData(static_cast<ImGuiSelectionUserData>(Row));
				DrawListRow(Context, Entries[Visible[Row]]);
			}
			ImGui::EndTable();
		}
	}
	else
	{
		const float Spacing   = ImGui::GetStyle().ItemSpacing.x;
		const float TileWidth = TileSize * ImGui::GetFontSize() / 15.0f;
		const int32 Columns   = FMath::Max(1, static_cast<int32>((ImGui::GetContentRegionAvail().x + Spacing) / (TileWidth + Spacing)));
		for (size_t Index = 0; Index < Visible.size(); ++Index)
		{
			if (Index % static_cast<size_t>(Columns) != 0)
			{
				ImGui::SameLine();
			}
			ImGui::SetNextItemSelectionUserData(static_cast<ImGuiSelectionUserData>(Index));
			DrawTile(Context, Entries[Visible[Index]], TileWidth);
		}
	}

	MultiIO = ImGui::EndMultiSelect();
	Selection->ApplyRequests(MultiIO);

	// 남은 빈 공간: 다른 폴더(트리/경로)에서 끌어온 항목을 현재 폴더로
	const ImVec2 Remaining = ImGui::GetContentRegionAvail();
	if (Remaining.x > 1.0f && Remaining.y > 1.0f)
	{
		ImGui::Dummy(Remaining);
		AcceptMoveDrop(Context, CurrentDirectory);
	}
	ImGui::EndChild();
}

void FContentBrowserPanel::DrawTile(FEditorContext& Context, const FEntry& Entry, float TileWidth)
{
	ImGui::PushID(static_cast<int>(Entry.Id));
	const bool                      bSelected  = Selection->Contains(Entry.Id);
	const FEditorTheme::FAssetStyle Style      = FEditorTheme::GetAssetStyle(Entry.Extension, Entry.bDirectory);
	const float                     LineHeight = ImGui::GetTextLineHeight();
	const float                     Padding    = 4.0f;
	const ImVec2                    Min        = ImGui::GetCursorScreenPos();
	const ImVec2                    Size(TileWidth, TileWidth + LineHeight * 2.0f + Padding * 2.0f);

	// 선택 배경은 강조색, 마우스 올림은 밝은 회색 (Selectable이 그린다)
	ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(FEditorTheme::Accent.x, FEditorTheme::Accent.y, FEditorTheme::Accent.z, 0.55f));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(FEditorTheme::Accent.x, FEditorTheme::Accent.y, FEditorTheme::Accent.z, 0.70f));
	ImGui::Selectable("##Tile", bSelected, ImGuiSelectableFlags_AllowDoubleClick, Size);
	ImGui::PopStyleColor(3);
	HandleItemInteraction(Context, Entry);

	ImDrawList*  DrawList = ImGui::GetWindowDrawList();
	const ImVec2 ThumbMin(Min.x + Padding, Min.y + Padding);
	const ImVec2 ThumbMax(Min.x + TileWidth - Padding, Min.y + TileWidth - Padding);
	DrawList->AddRectFilled(ThumbMin, ThumbMax, IM_COL32(20, 20, 22, 255), 2.0f);

	const uint64 Thumbnail = Entry.bDirectory ? 0 : Thumbnails->Request(Entry.Path);
	if (Thumbnail != 0)
	{
		DrawList->AddImage(static_cast<ImTextureID>(Thumbnail), ThumbMin, ThumbMax);
	}
	else
	{
		// 썸네일이 없는 종류(폴더/스크립트/오디오)나 그리기 전에는 큰 아이콘
		const float  IconSize   = (ThumbMax.y - ThumbMin.y) * 0.45f;
		const ImVec2 IconExtent = ImGui::GetFont()->CalcTextSizeA(IconSize, FLT_MAX, 0.0f, Style.Icon);
		DrawList->AddText(ImGui::GetFont(), IconSize,
		                  ImVec2((ThumbMin.x + ThumbMax.x - IconExtent.x) * 0.5f, (ThumbMin.y + ThumbMax.y - IconExtent.y) * 0.5f), Style.Color, Style.Icon);
	}
	// 종류 색 띠 (언리얼 콘텐츠 브라우저처럼)
	DrawList->AddRectFilled(ImVec2(ThumbMin.x, ThumbMax.y - 3.0f), ThumbMax, Style.Color);

	const float       TextWidth = TileWidth - Padding * 2.0f;
	const ImVec2      NamePos(Min.x + Padding, ThumbMax.y + Padding);
	const std::string Name = Ellipsize(Entry.bDirectory ? Entry.DisplayName : FStringConv::ToUtf8(Entry.Path.stem().wstring()), TextWidth);
	DrawList->AddText(NamePos, ImGui::GetColorU32(ImGuiCol_Text), Name.c_str());
	DrawList->AddText(ImVec2(NamePos.x, NamePos.y + LineHeight), ImGui::GetColorU32(ImGuiCol_TextDisabled), Style.Label);

	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
	{
		const std::string SizeText = Entry.bDirectory ? std::string() : std::format(" · {:.1f} KB", static_cast<double>(Entry.SizeInBytes) / 1024.0);
		ImGui::SetTooltip("%s\n%s%s", ToContentRelative(Entry.Path, Context.ContentDirectory).c_str(), Style.Label, SizeText.c_str());
	}
	ImGui::PopID();
}

void FContentBrowserPanel::DrawListRow(FEditorContext& Context, const FEntry& Entry)
{
	ImGui::PushID(static_cast<int>(Entry.Id));
	const FEditorTheme::FAssetStyle Style = FEditorTheme::GetAssetStyle(Entry.Extension, Entry.bDirectory);
	ImGui::Selectable(("      " + Entry.DisplayName).c_str(), Selection->Contains(Entry.Id), ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns);
	ImGui::GetWindowDrawList()->AddText(ImGui::GetItemRectMin(), Style.Color, Style.Icon);
	HandleItemInteraction(Context, Entry);
	ImGui::TableNextColumn();
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Style.Color), "%s", Style.Label);
	ImGui::TableNextColumn();
	if (!Entry.bDirectory)
	{
		ImGui::TextDisabled("%.1f KB", static_cast<double>(Entry.SizeInBytes) / 1024.0);
	}
	ImGui::PopID();
}

void FContentBrowserPanel::HandleItemInteraction(FEditorContext& Context, const FEntry& Entry)
{
	if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
	{
		OpenEntry(Context, Entry);
	}

	// 끌기: 선택된 항목을 끌면 선택 전체, 아니면 이 항목만
	if (ImGui::BeginDragDropSource())
	{
		std::vector<std::filesystem::path> Paths;
		if (Selection->Contains(Entry.Id))
		{
			Paths = GetSelectedPaths();
		}
		if (Paths.empty())
		{
			Paths.push_back(Entry.Path);
		}
		FContentDragDrop::GetPaths() = Paths;
		ImGui::SetDragDropPayload(FContentDragDrop::PayloadType, nullptr, 0);
		const FEditorTheme::FAssetStyle Style = FEditorTheme::GetAssetStyle(Entry.Extension, Entry.bDirectory);
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Style.Color), "%s", Style.Icon);
		ImGui::SameLine();
		if (Paths.size() > 1)
		{
			ImGui::Text("%s 외 %zu개", Entry.DisplayName.c_str(), Paths.size() - 1);
		}
		else
		{
			ImGui::TextUnformatted(Entry.DisplayName.c_str());
		}
		ImGui::EndDragDropSource();
	}
	if (Entry.bDirectory)
	{
		AcceptMoveDrop(Context, Entry.Path);
	}

	// 우클릭: 선택되지 않은 항목이면 그것만 선택
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !Selection->Contains(Entry.Id))
	{
		Selection->Clear();
		Selection->SetItemSelected(Entry.Id, true);
	}
	if (ImGui::BeginPopupContextItem("##ItemMenu"))
	{
		DrawItemContextMenu(Context, Entry);
		ImGui::EndPopup();
	}
}

void FContentBrowserPanel::AcceptMoveDrop(FEditorContext& Context, const std::filesystem::path& DestinationDirectory)
{
	if (!ImGui::BeginDragDropTarget())
	{
		return;
	}
	if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload())
	{
		const std::vector<std::filesystem::path> Sources = *Paths; // 이동 중 목록이 바뀌지 않도록 복사
		MoveAssets(Context, Sources, DestinationDirectory);
	}
	// 계층에서 끌어 온 엔티티 → 이 폴더에 프리팹 만들기
	if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(FContentDragDrop::EntityPayloadType))
	{
		const FEntity Entity = *static_cast<const FEntity*>(Payload->Data);
		if (!FEditorActions::CreatePrefabs(Context, Entity, DestinationDirectory).empty())
		{
			bNeedsRefresh = true;
		}
	}
	ImGui::EndDragDropTarget();
}

// ---------------------------------------------------------------- 메뉴

void FContentBrowserPanel::DrawItemContextMenu(FEditorContext& Context, const FEntry& Entry)
{
	std::vector<std::filesystem::path> Selected = GetSelectedPaths();
	if (Selected.empty())
	{
		Selected.push_back(Entry.Path);
	}
	const bool bSingle = Selected.size() == 1;

	if (Entry.bDirectory)
	{
		if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " 열기"))
		{
			Navigate(Entry.Path);
		}
	}
	else if (FAssetEditorManager::CanOpen(Entry.Path))
	{
		if (ImGui::MenuItem(ICON_FA_PEN_TO_SQUARE " 편집", nullptr, false, bSingle))
		{
			OpenEntry(Context, Entry);
		}
		if (Entry.Extension == ".estruct")
		{
			const std::string StructPath = FModelLoader::MakeAssetPath(Entry.Path);
			if (ImGui::MenuItem(ICON_FA_TABLE " 이 구조체로 데이터 테이블 만들기"))
			{
				CreateDataFile(Context, FDataTable::Extension, StructPath);
			}
			if (ImGui::MenuItem(ICON_FA_DATABASE " 이 구조체로 데이터 에셋 만들기"))
			{
				CreateDataFile(Context, FDataAsset::Extension, StructPath);
			}
		}
	}
	else if (IsSceneExtension(Entry.Extension))
	{
		if (ImGui::MenuItem(ICON_FA_MOUNTAIN_SUN " 씬 열기", nullptr, false, bSingle))
		{
			OpenEntry(Context, Entry);
		}
	}
	if (IsModelExtension(Entry.Extension) || IsParticleExtension(Entry.Extension) || IsPrefabExtension(Entry.Extension) || IsUIExtension(Entry.Extension) ||
	    IsBuildingExtension(Entry.Extension))
	{
		if (ImGui::MenuItem(ICON_FA_PLUS " 씬에 추가", nullptr, false, !Context.bPlaying))
		{
			AddToScene(Context, Entry);
		}
	}
	ImGui::Separator();
	if (ImGui::MenuItem(ICON_FA_I_CURSOR " 이름 바꾸기", "F2", false, bSingle))
	{
		BeginRename(Entry.Path);
	}
	if (ImGui::MenuItem(ICON_FA_CLONE " 복제"))
	{
		Duplicate(Context, Selected);
	}
	if (ImGui::MenuItem(ICON_FA_TRASH_CAN " 삭제", "Delete"))
	{
		RequestDelete(Context, Selected);
	}
	ImGui::Separator();
	if (ImGui::MenuItem(ICON_FA_COPY " 경로 복사"))
	{
		ImGui::SetClipboardText(ToContentRelative(Entry.Path, Context.ContentDirectory).c_str());
	}
	if (ImGui::MenuItem(ICON_FA_UP_RIGHT_FROM_SQUARE " 탐색기에서 보기"))
	{
		ShowInExplorer(Entry.Path);
	}
}

void FContentBrowserPanel::DrawBackgroundContextMenu(FEditorContext& Context)
{
	if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS " 새 폴더"))
	{
		CreateFolder(Context);
	}
	if (ImGui::MenuItem(ICON_FA_PALETTE " 새 머티리얼"))
	{
		CreateAsset(Context, "NewMaterial", FMaterialAsset::Extension);
	}
	if (ImGui::MenuItem(ICON_FA_FIRE " 새 파티클"))
	{
		CreateAsset(Context, "NewParticle", FParticleSystemAsset::Extension);
	}
	if (ImGui::MenuItem(ICON_FA_DIAGRAM_PROJECT " 새 비헤이비어 트리"))
	{
		CreateAsset(Context, "NewBehaviorTree", L".ebt");
	}
	if (ImGui::MenuItem(ICON_FA_DISPLAY " 새 UI"))
	{
		CreateAsset(Context, "NewUI", FUIAsset::Extension);
	}
	if (ImGui::MenuItem(ICON_FA_LANGUAGE " 새 문자열 표"))
	{
		CreateAsset(Context, "NewStrings", FStringTable::Extension);
	}
	if (ImGui::MenuItem(ICON_FA_PERSON_RUNNING " 새 애니메이션 그래프"))
	{
		CreateAsset(Context, "NewAnimGraph", FAnimGraphAsset::Extension);
	}
	if (ImGui::MenuItem(ICON_FA_CLAPPERBOARD " 새 시퀀스 (컷신)"))
	{
		CreateAsset(Context, "NewSequence", FSequenceAsset::Extension);
	}
	// 데이터: 구조체 → 그 구조체를 고르는 테이블/데이터 에셋
	if (ImGui::MenuItem(ICON_FA_TABLE_LIST " 새 데이터 구조체"))
	{
		CreateAsset(Context, "NewStruct", FDataStruct::Extension);
	}
	for (const bool bTable : { true, false })
	{
		if (ImGui::BeginMenu(bTable ? ICON_FA_TABLE " 새 데이터 테이블" : ICON_FA_DATABASE " 새 데이터 에셋"))
		{
			const std::vector<std::string>& Structs = DataValueWidgets::ScanContentFiles(Context.ContentDirectory, FDataStruct::Extension);
			if (Structs.empty())
			{
				ImGui::TextDisabled("구조체(.estruct)가 없습니다 — 먼저 데이터 구조체를 만드세요");
			}
			ImGui::TextDisabled("구조체 고르기");
			for (const std::string& StructPath : Structs)
			{
				if (ImGui::MenuItem(StructPath.c_str()))
				{
					CreateDataFile(Context, bTable ? FDataTable::Extension : FDataAsset::Extension, StructPath);
				}
			}
			ImGui::EndMenu();
		}
	}
	ImGui::Separator();
	if (ImGui::MenuItem(ICON_FA_ARROWS_ROTATE " 새로 고침"))
	{
		bNeedsRefresh = true;
	}
	if (ImGui::MenuItem(ICON_FA_UP_RIGHT_FROM_SQUARE " 탐색기에서 열기"))
	{
		OpenFolderInExplorer(CurrentDirectory);
	}
}

void FContentBrowserPanel::HandleShortcuts(FEditorContext& Context)
{
	if (ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
	{
		return;
	}
	const std::vector<std::filesystem::path> Selected = GetSelectedPaths();
	if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !Selected.empty())
	{
		RequestDelete(Context, Selected);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_F2) && Selected.size() == 1)
	{
		BeginRename(Selected.front());
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !FAssetFileOps::IsSameOrUnder(Context.ContentDirectory, CurrentDirectory))
	{
		Navigate(CurrentDirectory.parent_path());
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Enter) && Selected.size() == 1)
	{
		for (const FEntry& Entry : Entries)
		{
			if (Entry.Path == Selected.front())
			{
				OpenEntry(Context, Entry);
				break;
			}
		}
	}
}

// ---------------------------------------------------------------- 동작

void FContentBrowserPanel::OpenEntry(FEditorContext& Context, const FEntry& Entry)
{
	if (Entry.bDirectory)
	{
		Navigate(Entry.Path);
	}
	else if (FAssetEditorManager::CanOpen(Entry.Path) && Context.OpenAssetEditorRequest)
	{
		Context.OpenAssetEditorRequest(Entry.Path);
	}
	else if (IsSceneExtension(Entry.Extension) && Context.OpenSceneRequest)
	{
		Context.OpenSceneRequest(Entry.Path);
	}
	else if (Entry.Extension == ".lua" && Context.OpenScriptRequest)
	{
		Context.OpenScriptRequest(Entry.Path); // 스크립트 디버거 소스 보기 (중단점)
	}
	else if (IsBuildingExtension(Entry.Extension))
	{
		// 건물 설정(JSON)은 전용 편집 창이 없어 메모장으로 연다 (씬에 놓을 때는 "씬에 추가")
		ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + std::filesystem::absolute(Entry.Path).wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
	}
}

void FContentBrowserPanel::AddToScene(FEditorContext& Context, const FEntry& Entry)
{
	FScene& Scene = *Context.Scene;
	FEntity Added;
	if (IsModelExtension(Entry.Extension))
	{
		Added = FModelLoader::LoadIntoScene(Entry.Path, Scene, *Context.Resources);
	}
	else if (IsParticleExtension(Entry.Extension))
	{
		Added = Scene.CreateEntity(FStringConv::ToUtf8(Entry.Path.stem().wstring()));
		Scene.GetRegistry().Emplace<FParticleSystemComponent>(Added).Asset = FModelLoader::MakeAssetPath(Entry.Path);
	}
	else if (IsPrefabExtension(Entry.Extension))
	{
		Added = FEditorActions::InstantiatePrefab(Context, Entry.Path, NullEntity);
	}
	else if (IsUIExtension(Entry.Extension))
	{
		// 화면 UI (플레이 중 화면 전체 위)
		Added = Scene.CreateEntity(FStringConv::ToUtf8(Entry.Path.stem().wstring()));
		Scene.GetRegistry().Emplace<FUIComponent>(Added).Asset = FModelLoader::MakeAssetPath(Entry.Path);
	}
	else if (IsBuildingExtension(Entry.Extension))
	{
		// 절차적 건물: 설정만 지정 (인스펙터의 "생성"으로 하위에 층/호실을 만든다)
		Added = Scene.CreateEntity(FStringConv::ToUtf8(Entry.Path.stem().wstring()));
		Scene.GetRegistry().Emplace<FProceduralBuildingComponent>(Added).Config = FModelLoader::MakeAssetPath(Entry.Path);
	}
	if (!Scene.GetRegistry().IsValid(Added))
	{
		Notify(Context, "씬에 추가하지 못했습니다: " + Entry.DisplayName, true);
		return;
	}
	Scene.UpdateTransforms();
	Context.Select(Added);
	Context.MarkEdited("에셋 추가");
}

bool FContentBrowserPanel::MoveAssets(FEditorContext& Context, const std::vector<std::filesystem::path>& Sources,
                                      const std::filesystem::path& DestinationDirectory)
{
	if (Context.bPlaying)
	{
		Notify(Context, "플레이 중에는 파일을 옮길 수 없습니다", true);
		return false;
	}
	if (Context.PrepareAssetChange && !Context.PrepareAssetChange(Sources))
	{
		Notify(Context, "저장하지 않은 편집 창이 있어 옮기지 않았습니다 (먼저 저장하거나 닫으세요)", true);
		return false;
	}

	std::vector<FAssetMove> Moves;
	std::string             Errors;
	for (const std::filesystem::path& Source : Sources)
	{
		// 폴더를 자기 자신 위에 놓은 경우는 조용히 무시
		if (FAssetFileOps::IsSameOrUnder(Source, DestinationDirectory) && FAssetFileOps::IsSameOrUnder(DestinationDirectory, Source))
		{
			continue;
		}
		std::filesystem::path        NewPath;
		const FAssetFileOps::EResult Result = FAssetFileOps::Move(Source, DestinationDirectory, NewPath);
		if (Result == FAssetFileOps::EResult::Ok)
		{
			Moves.push_back({ Source, NewPath });
			MoveSidecar(Source, NewPath, Moves);
		}
		else if (Result != FAssetFileOps::EResult::SameLocation)
		{
			Errors += (Errors.empty() ? "" : ", ") + FStringConv::ToUtf8(Source.filename().wstring()) + ": " + FAssetFileOps::Describe(Result);
		}
	}
	if (!Moves.empty())
	{
		const FAssetReferenceUpdater::FResult Updated = FAssetReferenceUpdater::UpdateAfterMove(Context.ContentDirectory, GetProjectFileOrEmpty(), Moves);
		if (Context.AssetsMoved)
		{
			Context.AssetsMoved(Moves);
		}
		Notify(Context,
		       std::format("{}개 이동 → {} (다른 파일의 참조 {}곳 갱신)", Moves.size(), ToContentRelative(DestinationDirectory, Context.ContentDirectory),
		                   Updated.ReplacedCount),
		       false);
		Selection->Clear();
	}
	if (!Errors.empty())
	{
		Notify(Context, "옮기지 못함 — " + Errors, true);
	}
	bNeedsRefresh = true;
	return !Moves.empty();
}

bool FContentBrowserPanel::RenameAsset(FEditorContext& Context, const std::filesystem::path& Source, const std::wstring& NewName)
{
	if (Context.bPlaying)
	{
		Notify(Context, "플레이 중에는 이름을 바꿀 수 없습니다", true);
		return false;
	}
	if (Context.PrepareAssetChange && !Context.PrepareAssetChange({ Source }))
	{
		Notify(Context, "저장하지 않은 편집 창이 있어 이름을 바꾸지 않았습니다", true);
		return false;
	}
	std::filesystem::path        NewPath;
	const FAssetFileOps::EResult Result = FAssetFileOps::Rename(Source, NewName, NewPath);
	if (Result == FAssetFileOps::EResult::SameLocation)
	{
		return false;
	}
	if (Result != FAssetFileOps::EResult::Ok)
	{
		Notify(Context, std::string("이름을 바꾸지 못했습니다: ") + FAssetFileOps::Describe(Result), true);
		return false;
	}
	std::vector<FAssetMove> Moves = { { Source, NewPath } };
	MoveSidecar(Source, NewPath, Moves);
	const FAssetReferenceUpdater::FResult Updated = FAssetReferenceUpdater::UpdateAfterMove(Context.ContentDirectory, GetProjectFileOrEmpty(), Moves);
	if (Context.AssetsMoved)
	{
		Context.AssetsMoved(Moves);
	}
	Notify(Context, std::format("이름 변경: {} (다른 파일의 참조 {}곳 갱신)", FStringConv::ToUtf8(NewPath.filename().wstring()), Updated.ReplacedCount), false);
	bNeedsRefresh = true;
	Selection->Clear();
	Selection->SetItemSelected(MakeEntryId(NewPath), true);
	return true;
}

void FContentBrowserPanel::Duplicate(FEditorContext& Context, const std::vector<std::filesystem::path>& Sources)
{
	uint32 Count = 0;
	for (const std::filesystem::path& Source : Sources)
	{
		std::filesystem::path NewPath;
		if (FAssetFileOps::Duplicate(Source, NewPath) == FAssetFileOps::EResult::Ok)
		{
			std::error_code ErrorCode;
			if (FModelLoader::IsModelFile(Source))
			{
				const std::vector<std::filesystem::path> From = GetModelSidecars(Source);
				const std::vector<std::filesystem::path> To   = GetModelSidecars(NewPath);
				for (size_t Index = 0; Index < From.size(); ++Index)
				{
					std::filesystem::copy_file(From[Index], To[Index], ErrorCode); // 없으면 실패해도 무시
				}
			}
			++Count;
		}
	}
	Notify(Context, std::format("{}개 복제", Count), Count == 0);
	bNeedsRefresh = true;
}

void FContentBrowserPanel::RequestDelete(FEditorContext& Context, const std::vector<std::filesystem::path>& Targets)
{
	DeleteTargets = Targets;
	for (const std::filesystem::path& Target : Targets)
	{
		if (!FModelLoader::IsModelFile(Target))
		{
			continue;
		}
		for (const std::filesystem::path& Sidecar : GetModelSidecars(Target))
		{
			std::error_code ErrorCode;
			if (std::filesystem::exists(Sidecar, ErrorCode))
			{
				DeleteTargets.push_back(Sidecar);
			}
		}
	}
	DeleteReferenceCount = FAssetReferenceUpdater::FindReferencingFiles(Context.ContentDirectory, GetProjectFileOrEmpty(), Targets).size();
	bOpenDeletePopup     = true;
}

void FContentBrowserPanel::DrawDeletePopup(FEditorContext& Context)
{
	if (bOpenDeletePopup)
	{
		ImGui::OpenPopup("삭제##ContentDelete");
		bOpenDeletePopup = false;
	}
	if (!ImGui::BeginPopupModal("삭제##ContentDelete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}
	if (DeleteTargets.size() == 1)
	{
		ImGui::Text("'%s'을(를) 삭제할까요?", FStringConv::ToUtf8(DeleteTargets.front().filename().wstring()).c_str());
	}
	else
	{
		ImGui::Text("%zu개 항목을 삭제할까요?", DeleteTargets.size());
	}
	ImGui::TextDisabled("휴지통으로 옮기므로 윈도우 휴지통에서 되살릴 수 있습니다.");
	if (DeleteReferenceCount > 0)
	{
		ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 이 에셋을 참조하는 파일이 %zu개 있습니다 (연결이 끊깁니다)", DeleteReferenceCount);
	}
	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Button, FEditorTheme::Danger);
	const bool bConfirm = ImGui::Button(ICON_FA_TRASH_CAN " 휴지통으로");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	const bool bCancel = ImGui::Button("취소") || ImGui::IsKeyPressed(ImGuiKey_Escape);
	if (bConfirm)
	{
		if (Context.bPlaying)
		{
			Notify(Context, "플레이 중에는 삭제할 수 없습니다", true);
		}
		else if (Context.PrepareAssetChange && !Context.PrepareAssetChange(DeleteTargets))
		{
			Notify(Context, "저장하지 않은 편집 창이 있어 삭제하지 않았습니다", true);
		}
		else if (FAssetFileOps::MoveToRecycleBin(DeleteTargets))
		{
			Notify(Context, std::format("{}개 삭제 (휴지통)", DeleteTargets.size()), false);
		}
		else
		{
			Notify(Context, "삭제하지 못했습니다", true);
		}
		Selection->Clear();
		bNeedsRefresh = true;
	}
	if (bConfirm || bCancel)
	{
		DeleteTargets.clear();
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void FContentBrowserPanel::BeginRename(const std::filesystem::path& Path)
{
	RenameTarget = Path;
	strncpy_s(RenameBuffer, sizeof(RenameBuffer), FStringConv::ToUtf8(Path.filename().wstring()).c_str(), _TRUNCATE);
	bOpenRenamePopup = true;
}

void FContentBrowserPanel::DrawRenamePopup(FEditorContext& Context)
{
	if (bOpenRenamePopup)
	{
		ImGui::OpenPopup("이름 바꾸기##ContentRename");
		bOpenRenamePopup = false;
	}
	if (!ImGui::BeginPopupModal("이름 바꾸기##ContentRename", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return;
	}
	if (ImGui::IsWindowAppearing())
	{
		ImGui::SetKeyboardFocusHere();
	}
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
	const bool bEnter = ImGui::InputText("##Name", RenameBuffer, sizeof(RenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	ImGui::TextDisabled("다른 에셋과 열린 씬의 참조도 함께 바뀝니다.");
	const bool bConfirm = ImGui::Button("확인") || bEnter;
	ImGui::SameLine();
	const bool bCancel = ImGui::Button("취소") || ImGui::IsKeyPressed(ImGuiKey_Escape);
	if (bConfirm)
	{
		RenameAsset(Context, RenameTarget, FStringConv::ToWide(RenameBuffer));
	}
	if (bConfirm || bCancel)
	{
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void FContentBrowserPanel::CreateAsset(FEditorContext& Context, const std::string& BaseName, const std::wstring& Extension)
{
	// 현재 폴더에 겹치지 않는 이름으로 기본 에셋 파일을 만들고 편집 창을 연다
	const std::filesystem::path Path = FAssetFileOps::MakeUniquePath(CurrentDirectory, FStringConv::ToWide(BaseName), Extension);
	const std::string           Name = FStringConv::ToUtf8(Path.stem().wstring());
	bool                        bOk  = false;
	if (Extension == FMaterialAsset::Extension)
	{
		FMaterialAsset Asset;
		Asset.Name = Name;
		bOk        = Asset.SaveToFile(Path);
	}
	else if (Extension == FParticleSystemAsset::Extension)
	{
		bOk = FParticleSystemAsset::MakeDefault(Name).SaveToFile(Path);
	}
	else if (Extension == L".ebt")
	{
		bOk = FBehaviorTreeEditor::MakeDefaultAsset().SaveToFile(Path);
	}
	else if (Extension == FUIAsset::Extension)
	{
		bOk = FUIAsset::MakeDefault().SaveToFile(Path);
	}
	else if (Extension == FStringTable::Extension)
	{
		FStringTable Table;
		Table.AddLanguage(FProjectSettings::Get().Localization.DefaultLanguage);
		Table.AddLanguage("en");
		bOk = Table.SaveToFile(Path);
	}
	else if (Extension == FAnimGraphAsset::Extension)
	{
		bOk = FAnimGraphAsset::MakeDefault().SaveToFile(Path);
	}
	else if (Extension == FSequenceAsset::Extension)
	{
		bOk = FSequenceAsset::MakeDefault().SaveToFile(Path);
	}
	else if (Extension == FDataStruct::Extension)
	{
		FDataStruct Struct;
		Struct.Name = Name;
		FDataField Field;
		Field.Name    = "Value";
		Field.Type    = EDataFieldType::Float;
		Field.Default = FDataValue::MakeFloat(0.0f);
		Struct.AddField(std::move(Field));
		bOk = FDataLibrary::Get().SaveStruct(FStringConv::ToUtf8(Path.wstring()), Struct);
	}
	if (!bOk)
	{
		Notify(Context, "에셋을 만들지 못했습니다: " + FStringConv::ToUtf8(Path.wstring()), true);
		return;
	}
	Notify(Context, "새 에셋: " + FStringConv::ToUtf8(Path.filename().wstring()), false);
	bNeedsRefresh = true;
	if (Context.OpenAssetEditorRequest)
	{
		Context.OpenAssetEditorRequest(Path);
	}
}

void FContentBrowserPanel::CreateDataFile(FEditorContext& Context, const std::wstring& Extension, const std::string& StructPath)
{
	// 구조체 기본값으로 채운 테이블(행 없음)/데이터 에셋을 현재 폴더에 만들고 편집 창을 연다
	const std::shared_ptr<const FDataStruct> Struct = FDataLibrary::Get().LoadStruct(StructPath);
	if (Struct == nullptr)
	{
		Notify(Context, "구조체를 읽을 수 없습니다: " + StructPath, true);
		return;
	}
	const std::string BaseName = (Struct->Name.empty() ? std::string("New") : Struct->Name) + (Extension == FDataTable::Extension ? "Table" : "Data");
	const std::filesystem::path Path = FAssetFileOps::MakeUniquePath(CurrentDirectory, FStringConv::ToWide(BaseName), Extension);
	const std::string           File = FStringConv::ToUtf8(Path.wstring());
	std::string                 Error;
	bool                        bOk = false;
	if (Extension == FDataTable::Extension)
	{
		FDataTable Table;
		Table.StructPath = StructPath;
		Table.Rebind(Struct);
		bOk = FDataLibrary::Get().SaveTable(File, Table, &Error);
	}
	else
	{
		FDataAsset Asset;
		Asset.StructPath = StructPath;
		Asset.Rebind(Struct);
		Asset.ResetToDefaults();
		bOk = FDataLibrary::Get().SaveDataAsset(File, Asset, &Error);
	}
	if (!bOk)
	{
		Notify(Context, "데이터 파일을 만들지 못했습니다: " + Error, true);
		return;
	}
	Notify(Context, "새 에셋: " + FStringConv::ToUtf8(Path.filename().wstring()), false);
	bNeedsRefresh = true;
	if (Context.OpenAssetEditorRequest)
	{
		Context.OpenAssetEditorRequest(Path);
	}
}

void FContentBrowserPanel::CreateFolder(FEditorContext& Context)
{
	std::filesystem::path        NewPath;
	const FAssetFileOps::EResult Result = FAssetFileOps::CreateFolder(CurrentDirectory, L"새 폴더", NewPath);
	if (Result != FAssetFileOps::EResult::Ok)
	{
		Notify(Context, std::string("폴더를 만들지 못했습니다: ") + FAssetFileOps::Describe(Result), true);
		return;
	}
	bNeedsRefresh = true;
	BeginRename(NewPath); // 바로 이름 입력
}

void FContentBrowserPanel::ImportExternalFiles(FEditorContext& Context, const std::vector<std::filesystem::path>& Files)
{
	if (CurrentDirectory.empty())
	{
		CurrentDirectory = Context.ContentDirectory;
	}
	uint32      Imported = 0;
	std::string Errors;
	std::vector<std::filesystem::path> ImportedModels;
	for (const std::filesystem::path& File : Files)
	{
		std::filesystem::path        NewPath;
		const FAssetFileOps::EResult Result = FAssetFileOps::Import(File, CurrentDirectory, NewPath);
		if (Result == FAssetFileOps::EResult::Ok)
		{
			++Imported;
			if (FModelLoader::IsModelFile(NewPath))
			{
				ImportedModels.push_back(NewPath);
			}
		}
		else
		{
			Errors += (Errors.empty() ? "" : ", ") + FStringConv::ToUtf8(File.filename().wstring()) + ": " + FAssetFileOps::Describe(Result);
		}
	}
	if (Imported > 0)
	{
		Notify(Context, std::format("{}개 가져옴 → {}", Imported, ToContentRelative(CurrentDirectory, Context.ContentDirectory)), false);
	}
	if (!Errors.empty())
	{
		Notify(Context, "가져오지 못함 — " + Errors, true);
	}
	// 모델을 가져오면 편집 창을 열어 임포트 설정(크기/방향/포함 항목)을 바로 확인할 수 있게 한다
	for (size_t Index = 0; Index < ImportedModels.size() && Index < 3 && Context.OpenAssetEditorRequest; ++Index)
	{
		Context.OpenAssetEditorRequest(ImportedModels[Index]);
	}
	bNeedsRefresh = true;
}

void FContentBrowserPanel::InvalidateThumbnail(const std::filesystem::path& Path)
{
	Thumbnails->Invalidate(Path);
}

void FContentBrowserPanel::MoveSidecar(const std::filesystem::path& Source, const std::filesystem::path& NewPath, std::vector<FAssetMove>& Moves)
{
	// 모델을 옮기거나 이름을 바꾸면 사이드카(.eimport/.emeta)도 같은 이름으로 따라간다
	if (!FModelLoader::IsModelFile(Source))
	{
		return;
	}
	const std::vector<std::filesystem::path> From = GetModelSidecars(Source);
	const std::vector<std::filesystem::path> To   = GetModelSidecars(NewPath);
	for (size_t Index = 0; Index < From.size(); ++Index)
	{
		std::error_code ErrorCode;
		if (!std::filesystem::exists(From[Index], ErrorCode))
		{
			continue;
		}
		std::filesystem::rename(From[Index], To[Index], ErrorCode);
		if (!ErrorCode)
		{
			Moves.push_back({ From[Index], To[Index] });
		}
	}
}
