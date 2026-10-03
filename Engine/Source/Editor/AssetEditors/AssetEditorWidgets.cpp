#include "Editor/AssetEditors/AssetEditorWidgets.h"

#include "Core/StringConv.h"
#include "Editor/ContentBrowser/ContentDragDrop.h"

#include <imgui.h>

#include <algorithm>
#include <cwctype>

namespace
{
	bool IsImageFile(const std::filesystem::path& Path)
	{
		std::wstring Extension = Path.extension().wstring();
		std::transform(Extension.begin(), Extension.end(), Extension.begin(), [](wchar_t Char) { return static_cast<wchar_t>(std::towlower(Char)); });
		return Extension == L".png" || Extension == L".jpg" || Extension == L".jpeg" || Extension == L".tga" || Extension == L".bmp";
	}
} // namespace

std::vector<std::string> FAssetEditorWidgets::ScanImageFiles(const std::filesystem::path& ContentDirectory, const std::filesystem::path& RelativeTo)
{
	std::vector<std::string> Files;
	std::error_code          ErrorCode;
	for (auto It = std::filesystem::recursive_directory_iterator(ContentDirectory, ErrorCode); !ErrorCode && It != std::filesystem::recursive_directory_iterator();
	     It.increment(ErrorCode))
	{
		if (It->is_regular_file(ErrorCode) && IsImageFile(It->path()))
		{
			const std::filesystem::path Relative = std::filesystem::relative(It->path(), RelativeTo, ErrorCode);
			if (!ErrorCode)
			{
				Files.push_back(FStringConv::ToUtf8(Relative.generic_wstring()));
			}
		}
	}
	std::sort(Files.begin(), Files.end());
	return Files;
}

bool FAssetEditorWidgets::TextureCombo(const char* Id, std::string& InOutPath, const std::vector<std::string>& Files, const char* EmptyLabel)
{
	bool bChanged = false;
	if (ImGui::BeginCombo(Id, InOutPath.empty() ? EmptyLabel : InOutPath.c_str()))
	{
		if (ImGui::Selectable(EmptyLabel, InOutPath.empty()))
		{
			InOutPath.clear();
			bChanged = true;
		}
		for (const std::string& File : Files)
		{
			if (ImGui::Selectable(File.c_str(), File == InOutPath))
			{
				InOutPath = File;
				bChanged  = true;
			}
		}
		ImGui::EndCombo();
	}
	return bChanged;
}

bool FAssetEditorWidgets::AcceptTextureDrop(std::string& InOutPath, const std::filesystem::path& RelativeTo)
{
	bool bChanged = false;
	if (ImGui::BeginDragDropTarget())
	{
		if (const std::vector<std::filesystem::path>* Paths = FContentDragDrop::AcceptPayload(); Paths != nullptr && !Paths->empty() && IsImageFile(Paths->front()))
		{
			std::error_code             ErrorCode;
			const std::filesystem::path Relative = std::filesystem::relative(Paths->front(), RelativeTo, ErrorCode);
			if (!ErrorCode && !Relative.empty())
			{
				InOutPath = FStringConv::ToUtf8(Relative.generic_wstring());
				bChanged  = true;
			}
		}
		ImGui::EndDragDropTarget();
	}
	return bChanged;
}

void FAssetEditorWidgets::Hint(const char* Text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", Text);
	ImGui::PopStyleColor();
}
