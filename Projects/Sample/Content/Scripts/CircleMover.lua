-- 시작 위치를 중심으로 수평 원을 그리며 위아래로도 흔든다 (시선 IK 목표 등)
local CircleMover = {
	Properties = {
		Radius    = 150.0, -- cm
		Speed     = 40.0,  -- 도/초
		Bob       = 40.0,  -- cm (위아래)
		BobPeriod = 3.0,   -- 초
	},
}

function CircleMover:OnStart()
	self.Center = self.entity:GetPosition()
	self.Time   = 0.0
end

function CircleMover:OnUpdate(dt)
	self.Time = self.Time + dt
	local P   = self.Properties
	local A   = math.rad(self.Time * P.Speed)
	local Z   = math.sin(self.Time * 2.0 * math.pi / P.BobPeriod) * P.Bob
	self.entity:SetPosition(self.Center + Vector3(math.cos(A) * P.Radius, math.sin(A) * P.Radius, Z))
end

return CircleMover
