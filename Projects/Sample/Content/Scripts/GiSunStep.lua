-- 동적 GI(Phase 51 DDGI) 시간 응답 확인용: 이 엔티티(방향광)의 방향을 StepFrame번째 프레임에 바로 바꾼다.
--   --screenshot-frames N과 함께 돌려 간접광(프로브)이 몇 프레임 만에 따라오는지 본다 (Tests/GI: 바닥 → 초록 벽으로 해가 옮겨 간다)
--   StepFrame = 0이면 아무것도 하지 않는다. G 키로 언제든 두 방향을 바꾼다 (플레이 중 수동 확인)
local GiSunStep = {
	Properties = {
		StepFrame = 0,
		ToX       = -0.55,
		ToY       = 0.6,
		ToZ       = -0.3,
	},
}

function GiSunStep:OnStart()
	self.Frame        = 0
	self.FromRotation = self.entity:GetRotation()
	self.ToRotation   = Quat.LookRotation(Vector3(self.Properties.ToX, self.Properties.ToY, self.Properties.ToZ))
	self.bStepped     = false
end

function GiSunStep:Toggle()
	self.bStepped = not self.bStepped
	self.entity:SetRotation(self.bStepped and self.ToRotation or self.FromRotation)
end

function GiSunStep:OnUpdate(dt)
	self.Frame = self.Frame + 1
	if self.Properties.StepFrame > 0 and self.Frame == self.Properties.StepFrame then
		self:Toggle()
		Log.Info("GiSunStep: 해 방향 바꿈 (프레임 " .. self.Frame .. ")")
	end
	if Input.IsKeyPressed("G") then
		self:Toggle()
	end
end

return GiSunStep
