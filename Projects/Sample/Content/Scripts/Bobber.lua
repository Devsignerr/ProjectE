-- 시작 위치를 기준으로 위아래로 흔든다 (사인파)
local Bobber = {
	Properties = {
		Amplitude = 50.0, -- cm
		Frequency = 0.5,  -- Hz
		Phase     = 0.0,  -- 0~1 (여러 개를 엇갈리게)
	},
}

function Bobber:OnStart()
	self.Origin = self.entity:GetPosition()
	self.Time   = 0.0
end

function Bobber:OnUpdate(dt)
	self.Time = self.Time + dt
	local P      = self.Properties
	local Offset = math.sin((self.Time * P.Frequency + P.Phase) * 2.0 * math.pi) * P.Amplitude
	self.entity:SetPosition(self.Origin + Vector3(0, 0, Offset))
end

return Bobber
