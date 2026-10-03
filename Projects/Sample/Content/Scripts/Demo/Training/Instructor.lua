-- 교관 (Training 데모, ServerOnly): 주기적으로 훈련병에게 훅(Melee_Hook 몽타주 → Melee_Hook_Rec)을 날린다.
--   PunchHit 노티파이(UAL2_Standard.glb.emeta) 순간 훈련병에게 데미지 100 → 훈련병 사망 → RagdollComponent(EnableOnDeath)가 래그돌을 켠다.
--   훈련병은 HealthComponent DeathAction = RespawnInPlace라 3초 뒤 제자리에서 회복되고(래그돌 꺼짐) Recruit.lua가 일어나기 몽타주를 튼다.
local Instructor = {
	Properties = {
		Recruit  = "Recruit",
		Interval = 4.0, -- 훈련병이 일어난 뒤 다음 훅까지 (초)
	},
}

function Instructor:OnStart()
	self.Recruit = Scene.Find(self.Properties.Recruit)
	Coroutine.Start(function() self:Run() end)
end

function Instructor:Run()
	Wait(2.0)
	while true do
		local Recruit = self.Recruit
		if Recruit and Recruit:IsValid() and not Recruit:IsDead() then
			self.entity:PlayMontage("Melee_Hook", { BlendIn = 0.15, BlendOut = 0.05 })
			Wait(0.45)
			self.entity:PlayMontage("Melee_Hook_Rec", { BlendIn = 0.05, BlendOut = 0.35 })
			Wait(1.4)
			self.entity:PlayMontage("Yes", { BlendIn = 0.3, BlendOut = 0.4, EndTime = 1.6 }) -- 고개를 끄덕인다
			Wait(1.6)
		end
		-- 훈련병이 다시 일어날 때까지
		WaitUntil(function() return Recruit == nil or not Recruit:IsValid() or not Recruit:IsDead() end)
		Wait(self.Properties.Interval)
	end
end

function Instructor:OnAnimNotify_PunchHit()
	local Recruit = self.Recruit
	if Recruit and Recruit:IsValid() and not Recruit:IsDead() then
		Recruit:ApplyDamage(100.0, self.entity)
	end
end

return Instructor
