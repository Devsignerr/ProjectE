#include "Editor/Panels/CollisionSettingsEditor.h"

#include "Core/Settings/CollisionSettings.h"
#include "Editor/EditorTheme.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

namespace
{
	bool DrawLayerNames(FCollisionLayerSettings& Settings)
	{
		bool bChanged = false;
		if (!ImGui::BeginTable("Layers", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
		{
			return false;
		}
		ImGui::TableSetupColumn("칸", ImGuiTableColumnFlags_WidthFixed, 60.0f);
		ImGui::TableSetupColumn("이름", ImGuiTableColumnFlags_WidthStretch);
		// 마지막으로 쓰는 칸 + 빈 칸 하나(새 레이어 입력)까지만 보인다
		uint32 LastUsed = 0;
		for (uint32 Index = 0; Index < FCollisionLayerSettings::MaxLayers; ++Index)
		{
			LastUsed = Settings.GetLayerName(Index).empty() ? LastUsed : Index;
		}
		const uint32 VisibleCount = std::min(LastUsed + 2, FCollisionLayerSettings::MaxLayers);
		for (uint32 Index = 0; Index < VisibleCount; ++Index)
		{
			ImGui::PushID(static_cast<int>(Index));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%u", Index);
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-FLT_MIN);
			const std::string& Name = Settings.GetLayerName(Index);
			if (Index == 0)
			{
				ImGui::BeginDisabled();
				ImGui::TextUnformatted(Name.c_str());
				ImGui::EndDisabled();
				ImGui::SetItemTooltip("기본 레이어 (고정). 레이어를 정하지 않은 콜라이더/캐릭터와 지형·폴리지");
			}
			else
			{
				char Buffer[64] = {};
				std::memcpy(Buffer, Name.data(), std::min(Name.size(), sizeof(Buffer) - 1));
				// 입력이 끝났을 때만 반영 (글자마다 바꾸면 행렬 줄이 계속 초기화된다)
				ImGui::InputTextWithHint("##Name", "(사용 안 함)", Buffer, sizeof(Buffer));
				if (ImGui::IsItemDeactivatedAfterEdit())
				{
					bChanged |= Settings.SetLayerName(Index, Buffer);
				}
				const int32 First = Settings.FindLayer(Name);
				if (!Name.empty() && First >= 0 && static_cast<uint32>(First) != Index)
				{
					ImGui::SameLine();
					ImGui::TextColored(FEditorTheme::Warning, ICON_FA_TRIANGLE_EXCLAMATION " 이름 중복 — 뒤 칸은 무시됨");
				}
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
		return bChanged;
	}

	// 유니티식 삼각 행렬: 줄 = 레이어(칸 순서), 열 = 레이어(역순). 줄 칸 ≤ 열 칸인 곳만 체크 상자 (대칭이라 반만)
	bool DrawMatrix(FCollisionLayerSettings& Settings)
	{
		std::vector<uint32> Used;
		for (uint32 Index = 0; Index < FCollisionLayerSettings::MaxLayers; ++Index)
		{
			if (!Settings.GetLayerName(Index).empty())
			{
				Used.push_back(Index);
			}
		}
		const int  Count    = static_cast<int>(Used.size());
		bool       bChanged = false;
		const auto Flags    = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInner | ImGuiTableFlags_HighlightHoveredColumn;
		if (!ImGui::BeginTable("Matrix", Count + 1, Flags))
		{
			return false;
		}
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_NoReorder);
		for (int Column = Count - 1; Column >= 0; --Column)
		{
			ImGui::TableSetupColumn(Settings.GetLayerName(Used[static_cast<size_t>(Column)]).c_str(), ImGuiTableColumnFlags_AngledHeader | ImGuiTableColumnFlags_WidthFixed);
		}
		ImGui::TableAngledHeadersRow();
		for (int Row = 0; Row < Count; ++Row)
		{
			const uint32 RowLayer = Used[static_cast<size_t>(Row)];
			ImGui::PushID(static_cast<int>(RowLayer));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(Settings.GetLayerName(RowLayer).c_str());
			for (int Column = Count - 1, TableColumn = 1; Column >= 0; --Column, ++TableColumn)
			{
				const uint32 ColumnLayer = Used[static_cast<size_t>(Column)];
				if (ColumnLayer < RowLayer)
				{
					continue;
				}
				ImGui::TableSetColumnIndex(TableColumn);
				ImGui::PushID(static_cast<int>(ColumnLayer));
				bool bCollide = Settings.ShouldCollide(RowLayer, ColumnLayer);
				if (ImGui::Checkbox("##Pair", &bCollide))
				{
					Settings.SetCollision(RowLayer, ColumnLayer, bCollide);
					bChanged = true;
				}
				ImGui::SetItemTooltip("%s × %s: %s", Settings.GetLayerName(RowLayer).c_str(), Settings.GetLayerName(ColumnLayer).c_str(),
				                      bCollide ? "부딪힘" : "통과 (트리거 알림도 없음)");
				ImGui::PopID();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
		return bChanged;
	}
} // namespace

bool FCollisionSettingsEditor::Draw(FCollisionLayerSettings& Settings)
{
	bool bChanged = false;
	ImGui::TextDisabled("콜라이더(박스/구/캡슐)와 캐릭터 이동 컴포넌트는 레이어 하나를 이름으로 가진다 (비면 Default)");
	ImGui::TextDisabled("Lua: Physics.Raycast(origin, dir, dist, {\"Ground\", \"Prop\"}) — 생략하면 모든 레이어 (트리거는 항상 제외)");

	ImGui::SeparatorText("레이어");
	bChanged |= DrawLayerNames(Settings);

	ImGui::SeparatorText("충돌 행렬");
	ImGui::TextDisabled("체크 = 두 레이어가 부딪힌다. 끄면 서로 통과하고 트리거 알림·캐릭터 막힘도 없다");
	bChanged |= DrawMatrix(Settings);
	if (ImGui::Button(ICON_FA_CHECK_DOUBLE " 모두 켜기"))
	{
		for (uint32 A = 0; A < FCollisionLayerSettings::MaxLayers; ++A)
		{
			for (uint32 B = A; B < FCollisionLayerSettings::MaxLayers; ++B)
			{
				Settings.SetCollision(A, B, true);
			}
		}
		bChanged = true;
	}
	return bChanged;
}
