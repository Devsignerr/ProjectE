-- Lua 비헤이비어 트리 서비스 예제: "Player"가 Radius 안에 들어오면 블랙보드 Target에 넣고, LoseRadius 밖으로 나가면 비운다.
-- Target이 바뀌면 트리의 Blackboard 데코레이터(중단 모드 Both)가 순찰 ↔ 추적을 바로 바꾼다.
local SeePlayer = {
	Properties = {
		TargetName = "Player",
		Radius     = 450.0, -- 발견 거리 (cm)
		LoseRadius = 700.0, -- 놓치는 거리 (cm)
	},
}

function SeePlayer:OnBecomeRelevant()
	self.Target = Scene.Find(self.Properties.TargetName) -- 선형 탐색이라 한 번만 찾아 둔다
end

function SeePlayer:OnTick(dt)
	local Blackboard = self.entity:GetBlackboard()
	if self.Target == nil or not self.Target:IsValid() then
		Blackboard:Clear("Target")
		return
	end
	local Distance = (self.Target:GetPosition() - self.entity:GetPosition()):Length()
	if not Blackboard:IsSet("Target") then
		if Distance <= self.Properties.Radius then
			Blackboard:Set("Target", self.Target)
			Log.Info("경비: 플레이어 발견 (" .. math.floor(Distance) .. "cm)")
		end
	elseif Distance > self.Properties.LoseRadius then
		Blackboard:Clear("Target")
		Log.Info("경비: 플레이어를 놓쳤다 — 순찰로 돌아간다")
	end
end

return SeePlayer
