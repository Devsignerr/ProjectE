-- 게임플레이 예제 규칙 (서버, GameModeComponent 엔티티): 매치가 끝나면 잠시 뒤 다시 시작하고 처치를 로그로 남긴다.
--   규칙 값(시작 대기/제한 시간/목표 점수/리스폰 지연)은 GameModeComponent, 진행은 엔진(FGameWorld)이 한다
local GameRules = {
	Properties = {
		RestartDelay = 5.0, -- 끝난 뒤 다시 시작까지 (초)
	},
}

function GameRules:OnMatchStateChanged(state)
	Log.Info("매치 상태: " .. state)
	if state == "Ended" then
		Log.Info(string.format("매치 종료 — 승자 %d, 플레이어 0 점수 %d", GameMode.GetWinner(), GameMode.GetScore(0)))
		self.RestartTimer = self.Properties.RestartDelay
	end
end

function GameRules:OnEntityDied(victim, instigator)
	Log.Info(string.format("%s 파괴 (가해자 %s, 점수 %d)", victim:GetName(), instigator and instigator:GetName() or "없음", GameMode.GetScore(0)))
end

function GameRules:OnUpdate(dt)
	if self.RestartTimer then
		self.RestartTimer = self.RestartTimer - dt
		if self.RestartTimer <= 0 then
			self.RestartTimer = nil
			GameMode.Restart()
		end
	end
end

return GameRules
