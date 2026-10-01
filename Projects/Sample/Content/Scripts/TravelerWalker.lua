-- 맵 전환 데모의 자동 "여행자" (ServerOnly, 복제 엔티티): 잠시 기다렸다가 포털 쪽으로 걸어간다 (입력 없이도 맵이 오가도록)
local TravelerWalker = {
	Properties = {
		Goal  = Vector3(500, 0, 50), -- 포털 위치 (cm)
		Delay = 2.5,                 -- 출발 전 대기 (초)
		Speed = 220.0,               -- cm/s
	},
}

function TravelerWalker:OnStart()
	self.Waited = 0.0
end

function TravelerWalker:OnUpdate(dt)
	self.Waited = self.Waited + dt
	if self.Waited < self.Properties.Delay then return end
	local Position = self.entity:GetPosition()
	local ToGoal   = self.Properties.Goal - Position
	local Step     = self.Properties.Speed * dt
	if ToGoal:Length() <= Step then
		self.entity:SetPosition(self.Properties.Goal)
		return
	end
	self.entity:SetPosition(Position + ToGoal:Normalized() * Step)
end

return TravelerWalker
