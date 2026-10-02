-- 불빛 깜빡임: 같은 엔티티의 PointLightComponent.Intensity를 겹친 사인파 + 잡음으로 흔든다 (모닥불)
local ShowcaseFlicker = {
	Properties = {
		BaseIntensity = 30.0, -- 평균 밝기
		Amount        = 0.35, -- 흔들림 비율 (0~1)
		Speed         = 1.0,  -- 배속
	},
}

function ShowcaseFlicker:OnStart()
	self.Time  = math.random() * 10.0
	self.Light = self.entity:GetComponent("PointLightComponent")
	self.Noise = 0.0
end

function ShowcaseFlicker:OnUpdate(dt)
	if self.Light == nil then return end
	local P = self.Properties
	self.Time  = self.Time + dt * P.Speed
	self.Noise = self.Noise + (math.random() * 2.0 - 1.0 - self.Noise) * math.min(1.0, dt * 12.0) -- 부드러운 잡음
	local T    = self.Time
	local Wave = 0.5 * math.sin(T * 7.3) + 0.3 * math.sin(T * 13.1 + 1.7) + 0.2 * math.sin(T * 23.9 + 0.4)
	self.Light.Intensity = P.BaseIntensity * (1.0 + P.Amount * (0.6 * Wave + 0.4 * self.Noise))
end

return ShowcaseFlicker
