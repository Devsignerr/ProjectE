#include "Editor/Panels/SortingLayerSettingsEditor.h"

#include "Core/Settings/SortingLayerSettings.h"
#include "Editor/EditorTheme.h" // ICON_FA_*

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace
{
	// 버퍼 → 입력이 끝났을 때만 반영 (글자마다 바꾸면 중복 검사가 입력 도중에 걸린다)
	bool InputName(const char* Id, const char* Hint, const std::string& Current, std::string& OutCommitted)
	{
		char Buffer[64] = {};
		std::memcpy(Buffer, Current.data(), std::min(Current.size(), sizeof(Buffer) - 1));
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint(Id, Hint, Buffer, sizeof(Buffer));
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			OutCommitted = Buffer;
			return true;
		}
		return false;
	}
} // namespace

bool FSortingLayerSettingsEditor::Draw(FSortingLayerSettings& Settings)
{
	bool bChanged = false;
	ImGui::TextDisabled("스프라이트/타일맵 컴포넌트는 정렬 레이어 하나를 이름으로 가진다 (비면 Default)");
	ImGui::TextDisabled("목록 순서 = 그리기 순서: 위 칸이 먼저(뒤), 아래 칸이 나중(앞). 같은 레이어 안은 OrderInLayer가 큰 것이 앞");

	ImGui::SeparatorText("레이어");
	if (!ImGui::BeginTable("SortingLayers", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
	{
		return false;
	}
	ImGui::TableSetupColumn("칸", ImGuiTableColumnFlags_WidthFixed, 48.0f);
	ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("편집", ImGuiTableColumnFlags_WidthFixed, 96.0f);

	// 목록을 바꾸는 버튼은 순회 뒤에 적용한다 (그리는 도중 칸 번호가 바뀌지 않게)
	int32       MoveFrom = -1;
	int32       MoveTo   = -1;
	int32       Remove   = -1;
	const uint32 Count   = Settings.GetLayerCount();
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		ImGui::PushID(static_cast<int>(Index));
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%u", Index);
		ImGui::TableSetColumnIndex(1);
		const std::string& Name = Settings.GetLayerName(Index);
		if (Index == 0)
		{
			ImGui::BeginDisabled();
			ImGui::TextUnformatted(Name.c_str());
			ImGui::EndDisabled();
			ImGui::SetItemTooltip("기본 정렬 레이어 (고정, 항상 맨 뒤). 레이어를 정하지 않았거나 없는 이름의 스프라이트/타일맵");
		}
		else
		{
			std::string Committed;
			// 빈 이름·중복 이름은 받지 않는다 (입력이 원래 이름으로 돌아감)
			if (InputName("##Name", "(이름)", Name, Committed) && Committed != Name)
			{
				bChanged |= Settings.RenameLayer(Index, Committed);
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::BeginDisabled(Index <= 1);
			if (ImGui::SmallButton(ICON_FA_ARROW_UP))
			{
				MoveFrom = static_cast<int32>(Index);
				MoveTo   = static_cast<int32>(Index) - 1;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(Index + 1 >= Count);
			if (ImGui::SmallButton(ICON_FA_ARROW_DOWN))
			{
				MoveFrom = static_cast<int32>(Index);
				MoveTo   = static_cast<int32>(Index) + 1;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_FA_TRASH))
			{
				Remove = static_cast<int32>(Index);
			}
			ImGui::SetItemTooltip("삭제 (이 이름을 쓰던 스프라이트는 Default로 그려진다)");
		}
		ImGui::PopID();
	}

	// 새 레이어 줄
	if (Count < FSortingLayerSettings::MaxLayers)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%u", Count);
		ImGui::TableSetColumnIndex(1);
		std::string Committed;
		if (InputName("##NewLayer", "(새 레이어 이름 입력)", std::string(), Committed) && !Committed.empty())
		{
			bChanged |= Settings.AddLayer(Committed);
		}
	}
	ImGui::EndTable();

	if (MoveFrom >= 0)
	{
		bChanged |= Settings.MoveLayer(static_cast<uint32>(MoveFrom), static_cast<uint32>(MoveTo));
	}
	if (Remove >= 0)
	{
		bChanged |= Settings.RemoveLayer(static_cast<uint32>(Remove));
	}

	return bChanged;
}
