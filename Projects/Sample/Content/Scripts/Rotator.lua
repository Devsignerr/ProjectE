-- 엔티티를 축 기준으로 일정 속도로 회전시킨다
local Rotator = {
	Properties = {
		Speed = 90.0,            -- 도/초
		Axis  = Vector3(0, 0, 1), -- 회전 축 (기본: 월드 Z = Yaw)
	},
}

function Rotator:OnStart()
	self.Angle        = 0.0
	self.BaseRotation = self.entity:GetRotation()
end

function Rotator:OnUpdate(dt)
	self.Angle = (self.Angle + self.Properties.Speed * dt) % 360
	self.entity:SetRotation(Quat.FromAxisAngle(self.Properties.Axis, self.Angle) * self.BaseRotation)
end

return Rotator
