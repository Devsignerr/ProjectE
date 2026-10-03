-- 돌진 레인 (Training 데모, ServerOnly): 루트 모션 몽타주(Sword_Dash_RM / Shield_Dash_RM)로 앞으로 돌진하고, 레인 끝에 가까우면 돌아선다.
--   이 엔티티 = 모델 루트 (AnimationComponent RootMotionMode = MontagesOnly) — 캐릭터 이동 컴포넌트가 없으므로 루트 모션이 트랜스폼을 직접 옮긴다.
--   모델은 모델 공간 -X를 본다: 엔티티 Yaw 180 = 월드 +X를 본다
local Dasher = {
	Properties = {
		MinX = -800.0,
		MaxX = 300.0,
		TurnMargin = 260.0, -- 레인 끝에서 이 안이면 돌아선다 (cm)
	},
}

function Dasher:OnStart()
	self.Dir = 1.0 -- +X로 달린다
	Coroutine.Start(function() self:Run() end)
end

function Dasher:SetFacing(Yaw)
	self.entity:SetRotation(Quat.FromEuler(0.0, Yaw, 0.0))
end

function Dasher:TurnAround()
	local From = self.Dir > 0 and 180.0 or 0.0
	local To = From + 180.0
	local Steps = 24
	for I = 1, Steps do
		local T = I / Steps
		T = T * T * (3.0 - 2.0 * T)
		self:SetFacing(From + (To - From) * T)
		Wait()
	end
	self.Dir = -self.Dir
	self:SetFacing(self.Dir > 0 and 180.0 or 0.0)
end

function Dasher:Run()
	local Clips = { "Sword_Dash_RM", "Shield_Dash_RM" }
	local Count = 0
	Wait(1.0)
	while true do
		Count = Count + 1
		self.entity:PlayMontage(Clips[(Count - 1) % 2 + 1], { BlendIn = 0.1, BlendOut = 0.25 })
		Wait(0.1)
		WaitUntil(function() return not self.entity:IsMontagePlaying() end)
		Wait(0.7)
		local X = self.entity:GetWorldPosition().X
		if (self.Dir > 0 and X > self.Properties.MaxX - self.Properties.TurnMargin)
			or (self.Dir < 0 and X < self.Properties.MinX + self.Properties.TurnMargin) then
			self:TurnAround()
			Wait(0.6)
		end
	end
end

return Dasher
