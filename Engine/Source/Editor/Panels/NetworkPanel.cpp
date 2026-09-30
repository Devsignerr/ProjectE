#include "Editor/Panels/NetworkPanel.h"

#include "Core/Math/Math.h"
#include "Core/Paths.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Editor/PlayInEditorNet.h"

#include <imgui.h>

#include <format>

void FNetworkPanel::Draw(FEditorContext& Context)
{
	if (!bOpen || Context.NetPlay == nullptr)
	{
		return;
	}
	FPlayInEditorNet& NetPlay = *Context.NetPlay;
	if (ImGui::Begin(FEditorTheme::PanelTitle(ICON_FA_NETWORK_WIRED, "네트워크", "Network").c_str(), &bOpen))
	{
		// ---- 플레이 설정 (다음 플레이부터)
		ImGui::SeparatorText("플레이 설정");
		ImGui::BeginDisabled(Context.bPlaying);
		FPlayNetSettings& Settings = NetPlay.PendingSettings;
		const char*       Modes[]  = { "1인용", "리슨 서버 (에디터가 호스트)", "전용 서버 (에디터도 클라이언트)" };
		int32             Mode     = static_cast<int32>(Settings.Mode);
		if (ImGui::Combo("모드", &Mode, Modes, IM_ARRAYSIZE(Modes)))
		{
			Settings.Mode = static_cast<FPlayNetSettings::EMode>(Mode);
		}
		ImGui::BeginDisabled(Settings.Mode == FPlayNetSettings::EMode::Standalone);
		ImGui::SliderInt("클라이언트 창", &Settings.ClientCount, 0, 4);
		ImGui::SetItemTooltip("플레이할 때 함께 띄울 런타임 클라이언트 창 수 (정지하면 닫힌다)");
		int32 Port = Settings.Port;
		if (ImGui::InputInt("포트", &Port))
		{
			Settings.Port = static_cast<uint16>(FMath::Clamp(Port, 1, 65535));
		}
		ImGui::EndDisabled();
		ImGui::EndDisabled();

		// ---- 진행 중인 세션
		ImGui::SeparatorText("세션");
		if (!NetPlay.IsActive())
		{
			ImGui::TextDisabled(Context.bPlaying ? "1인용 플레이 중" : "플레이 중이 아님");
		}
		else
		{
			const FNetDriver& Net = NetPlay.GetDriver();
			ImGui::Text("모드: %s   자식 프로세스: %d개", ToString(NetPlay.GetMode()), NetPlay.GetChildProcessCount());
			if (Net.IsServer())
			{
				ImGui::Text("원격 플레이어 %d명", static_cast<int32>(Net.GetPlayers().size()));
				for (const FNetDriver::FRemotePlayer& Player : Net.GetPlayers())
				{
					ImGui::BulletText("%u  %s", Player.PlayerId, Player.Name.c_str());
				}
			}
			else
			{
				const FNetDriver::EClientState State = Net.GetClientState();
				const char* StateText = State == FNetDriver::EClientState::Joined ? "입장" : State == FNetDriver::EClientState::Failed ? "실패" : "접속 중";
				ImGui::Text("클라이언트: %s (플레이어 %u)", StateText, Net.GetLocalPlayerId());
				if (State == FNetDriver::EClientState::Failed)
				{
					ImGui::TextColored(FEditorTheme::Danger, "%s", Net.GetFailureReason().c_str());
				}
			}
			if (ImGui::Button(ICON_FA_PLUS " 클라이언트 창 추가"))
			{
				NetPlay.LaunchRuntimeClient(std::format("127.0.0.1:{}", NetPlay.PendingSettings.Port), NetPlay.GetSceneAsset());
			}
		}

		// ---- LAN 세션
		ImGui::SeparatorText("LAN 세션");
		FLanDiscovery& Lan = NetPlay.GetLanSearch();
		if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS " 찾기"))
		{
			Lan.StartSearch(FPaths::HasProject() ? FPaths::GetProjectName() : std::string());
		}
		Lan.Update();
		if (Lan.GetSessions().empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled(Lan.IsSearching() ? "응답 없음" : "");
		}
		for (const FLanSession& Session : Lan.GetSessions())
		{
			ImGui::PushID(Session.Address.c_str());
			ImGui::Text("%s  (%u/%u)  %s", Session.Name.c_str(), Session.Players, Session.MaxPlayers, Session.Address.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton("런타임으로 접속"))
			{
				NetPlay.LaunchRuntimeClient(Session.Address, Session.SceneAsset);
			}
			ImGui::SetItemTooltip("씬: %s", Session.SceneAsset.c_str());
			ImGui::PopID();
		}
	}
	ImGui::End();
}
