#include "Core/Testing/TestFramework.h"
#include "Network/LanDiscovery.h"

#include <chrono>
#include <thread>

// LAN 세션 찾기: 호스트가 같은 PC(루프백 질의)에서 응답하고, 다른 프로젝트 검색에는 보이지 않는다
E_TEST(LanDiscovery_FindsHostOnSameMachine)
{
	constexpr uint16 DiscoveryPort = 27790; // 실제 기본 포트(7778)와 겹치지 않게
	FLanDiscovery    Host;
	FLanHostInfo     Info;
	Info.Name                  = "테스트 방";
	Info.Session.ProjectName   = "LanTest";
	Info.Session.EngineVersion = "0.1.0";
	Info.Session.SceneAsset    = "Scenes/A.escene";
	Info.GamePort              = 27791;
	Info.MaxPlayers            = 8;
	E_EXPECT_TRUE(Host.StartHost(Info, DiscoveryPort));
	Host.SetPlayerCount(3);

	const auto Search = [&](const std::string& Project) {
		FLanDiscovery Client;
		Client.StartSearch(Project, DiscoveryPort);
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
		while (std::chrono::steady_clock::now() < Deadline && Client.GetSessions().empty())
		{
			Host.Update();
			Client.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return Client.GetSessions();
	};

	const std::vector<FLanSession> Found = Search("LanTest");
	E_EXPECT_EQ(Found.size(), 1u);
	if (!Found.empty())
	{
		E_EXPECT_TRUE(Found[0].Name == "테스트 방" && Found[0].SceneAsset == "Scenes/A.escene");
		E_EXPECT_TRUE(Found[0].Players == 3 && Found[0].MaxPlayers == 8);
		E_EXPECT_TRUE(Found[0].Address.ends_with(":27791"));
	}
	E_EXPECT_TRUE(Search("OtherProject").empty());
}
