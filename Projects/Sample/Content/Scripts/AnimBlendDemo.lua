-- 애니메이션 그래프 데모 (Demo_Animation의 Fox_Graph): Speed 파라미터를 0 → MaxSpeed → 0으로 천천히 오가며
-- 블렌드 스페이스(Survey → Walk → Run)가 섞이는 모습을 보여 준다. 그래프는 Animations/FoxCharacter.eanimgraph
local AnimBlendDemo = {
	Properties = {
		MaxSpeed = 450.0, -- cm/s (그래프의 Run 샘플 위치)
		Period   = 8.0,   -- 초 (한 번 오가는 시간)
	},
}

function AnimBlendDemo:OnStart()
	self.Time = 0.0
	self.LastState = nil
end

function AnimBlendDemo:OnUpdate(dt)
	self.Time = self.Time + dt
	local P = self.Properties
	local Speed = (0.5 - 0.5 * math.cos(self.Time * 2.0 * math.pi / P.Period)) * P.MaxSpeed
	self.entity:SetAnimParam("Speed", Speed)
	local State = self.entity:GetAnimState()
	if State ~= self.LastState then
		Log.Info("애니메이션 상태:", State)
		self.LastState = State
	end
end

return AnimBlendDemo
