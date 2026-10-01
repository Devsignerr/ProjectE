-- 게임플레이 예제 HUD (클라이언트, GameplayHUD.eui): 복제된 게임 상태(GameMode.*)를 보여 주고 최고 기록을 세이브 게임에 남긴다.
--   SaveGame.Save(슬롯, 테이블) / SaveGame.Load(슬롯) → <Saved>/SaveGames/<슬롯>.json (자동 검증 실행은 임시 폴더)
local GameplayHud = {
	Properties = {
		SaveSlot = "GameplayDemo",
	},
}

local StateNames = { None = "게임 모드 없음", WaitingToStart = "곧 시작합니다", InProgress = "진행 중", Ended = "매치 종료" }

function GameplayHud:OnStart()
	self.StateText  = self.entity:GetWidget("StateText")
	self.TimeText   = self.entity:GetWidget("TimeText")
	self.ScoreText  = self.entity:GetWidget("ScoreText")
	self.BestText   = self.entity:GetWidget("BestText")
	self.BannerText = self.entity:GetWidget("BannerText")
	self.LogText    = self.entity:GetWidget("LogText")

	local Saved = SaveGame.Load(self.Properties.SaveSlot)
	self.Best    = Saved and Saved.BestScore or 0
	self.Matches = Saved and Saved.Matches or 0
	self:ShowBest()
end

function GameplayHud:ShowBest()
	self.BestText.Text = string.format("최고 기록 %d (매치 %d회)", self.Best, self.Matches)
end

function GameplayHud:OnUpdate(dt)
	local State  = GameMode.GetState()
	local Player = math.max(Net.GetLocalPlayerId(), 0)
	self.StateText.Text = StateNames[State] or State
	if State == "InProgress" and GameMode.GetTimeRemaining() > 0 then
		self.TimeText.Text = string.format("남은 시간 %d초", GameMode.GetTimeRemaining())
	else
		self.TimeText.Text = "남은 시간 --"
	end
	self.ScoreText.Text = string.format("점수 %d", GameMode.GetScore(Player))
end

function GameplayHud:OnMatchStateChanged(state)
	if state == "InProgress" then
		self.BannerText.Text = ""
		self.LogText.Text    = "포탑이 표적을 부수면 점수를 얻습니다"
	elseif state == "Ended" then
		local Player = math.max(Net.GetLocalPlayerId(), 0)
		local Score  = GameMode.GetScore(Player)
		local Winner = GameMode.GetWinner()
		self.BannerText.Text = Winner == Player and "승리!" or (Winner < 0 and "무승부" or string.format("플레이어 %d 승리", Winner))
		self.Matches = self.Matches + 1
		if Score > self.Best then
			self.Best = Score
			self.LogText.Text = "새 최고 기록!"
		end
		SaveGame.Save(self.Properties.SaveSlot, { BestScore = self.Best, Matches = self.Matches })
		self:ShowBest()
	end
end

return GameplayHud
