#include "Editor/Panels/ContentBrowserPanel.h"

#include "Core/Log.h"
#include "Core/StringConv.h"
#include "Editor/EditorContext.h"
#include "Renderer/ModelLoader.h"
#include "Scene/Scene.h"

#include <imgui.h>

#include <algorithm>

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

	bool IsModelExtension(const std::string& Extension) { return Extension == ".glb" || Extension == ".gltf"; }
	bool IsImageExtension(const std::string& Extension)
	{
		return Extension == ".png" || Extension == ".jpg" || Extension == ".jpeg" || Extension == ".tga" || Extension == ".bmp";
	}
} // namespace

void FContentBrowserPanel::Draw(FEditorContext& Context)
{
	if (!bOpen)
	{
		return;
	}

	const std::filesystem::path& Root = Context.ContentDirectory;
	if (CurrentDirectory.empty())
	{
		CurrentDirectory = Root;
	}
	if (bNeedsRefresh)
	{
		Refresh(Root);
	}

	if (ImGui::Begin("콘텐츠", &bOpen))
	{
		// 상단 바: 상위 폴더 / 새로 고침 / 현재 경로
		std::error_code ErrorCode;
		const bool bAtRoot = std::filesystem::equivalent(CurrentDirectory, Root, ErrorCode);
		ImGui::BeginDisabled(bAtRoot);
		if (ImGui::Button("상위 폴더"))
		{
			CurrentDirectory = CurrentDirectory.parent_path();
			bNeedsRefresh    = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("새로 고침"))
		{
			bNeedsRefresh = true;
		}
		ImGui::SameLine();
		const std::filesystem::path Relative = std::filesystem::relative(CurrentDirectory, Root, ErrorCode);
		ImGui::TextDisabled("%s", ErrorCode || Relative.empty() ? "/" : FStringConv::ToUtf8(Relative.wstring()).c_str());
		ImGui::Separator();

		if (ImGui::BeginTable("##Entries", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch, 3.0f);
			ImGui::TableSetupColumn("크기", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("동작", ImGuiTableColumnFlags_WidthStretch, 1.5f);
			ImGui::TableHeadersRow();

			for (const FEntry& Entry : Entries)
			{
				ImGui::TableNextRow();
				ImGui::PushID(Entry.DisplayName.c_str());

				ImGui::TableNextColumn();
				if (Entry.bDirectory)
				{
					if (ImGui::Selectable(("[폴더] " + Entry.DisplayName).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
					    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						CurrentDirectory = Entry.Path;
						bNeedsRefresh    = true;
					}
				}
				else
				{
					ImGui::TextUnformatted(Entry.DisplayName.c_str());
				}

				ImGui::TableNextColumn();
				if (!Entry.bDirectory)
				{
					ImGui::TextDisabled("%.1f KB", static_cast<double>(Entry.SizeInBytes) / 1024.0);
				}

				ImGui::TableNextColumn();
				if (IsModelExtension(Entry.Extension))
				{
					if (ImGui::SmallButton("씬에 추가"))
					{
						const FEntity LoadedRoot = FModelLoader::LoadIntoScene(Entry.Path, *Context.Scene, *Context.Resources);
						if (Context.Scene->GetRegistry().IsValid(LoadedRoot))
						{
							Context.Scene->UpdateTransforms();
							Context.Select(LoadedRoot);
						}
						else
						{
							E_LOG(LogEditor, Error, "모델 로드 실패: {}", Entry.DisplayName);
						}
					}
				}
				else if (IsImageExtension(Entry.Extension))
				{
					ImGui::TextDisabled("이미지");
				}

				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}
	ImGui::End();
}

void FContentBrowserPanel::Refresh(const std::filesystem::path& Root)
{
	bNeedsRefresh = false;
	Entries.clear();

	std::error_code ErrorCode;
	if (!std::filesystem::exists(CurrentDirectory, ErrorCode))
	{
		CurrentDirectory = Root;
	}
	for (const std::filesystem::directory_entry& DirectoryEntry : std::filesystem::directory_iterator(CurrentDirectory, ErrorCode))
	{
		FEntry Entry;
		Entry.Path        = DirectoryEntry.path();
		Entry.DisplayName = FStringConv::ToUtf8(Entry.Path.filename().wstring());
		Entry.Extension   = ToLower(FStringConv::ToUtf8(Entry.Path.extension().wstring()));
		Entry.bDirectory  = DirectoryEntry.is_directory(ErrorCode);
		Entry.SizeInBytes = Entry.bDirectory ? 0 : static_cast<uint64_t>(DirectoryEntry.file_size(ErrorCode));
		Entries.push_back(std::move(Entry));
	}

	// 폴더 먼저, 이름순
	std::sort(Entries.begin(), Entries.end(), [](const FEntry& A, const FEntry& B) {
		if (A.bDirectory != B.bDirectory)
		{
			return A.bDirectory;
		}
		return A.DisplayName < B.DisplayName;
	});
}
