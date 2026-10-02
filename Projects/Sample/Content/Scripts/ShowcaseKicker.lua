-- 관절 데모가 멈춰 서지 않게: Interval초마다 Targets(쉼표 이름)의 강체에 수평 충격량을 준다 (질량 비례 → 같은 속도 변화)
local ShowcaseKicker = {
	Properties = {
		Targets  = "",
		Interval = 4.0,   -- 초
		Speed    = 250.0, -- cm/s (충격량 = 질량 × 이 값)
	},
}

function ShowcaseKicker:OnStart()
	self.Time  = self.Properties.Interval * 0.5
	self.Index = 0
end

function ShowcaseKicker:OnUpdate(dt)
	self.Time = self.Time + dt
	if self.Time < self.Properties.Interval then return end
	self.Time = 0.0
	for Name in string.gmatch(self.Properties.Targets, "[^,]+") do
		local Target = Scene.Find(Name)
		if Target then
			self.Index = self.Index + 1
			local Angle = self.Index * 2.39996 -- 황금각: 매번 다른 방향
			local Direction = Vector3(math.cos(Angle), math.sin(Angle), 0.15)
			Target:AddImpulse(Direction * (Target:GetMass() * self.Properties.Speed))
		end
	end
end

return ShowcaseKicker
