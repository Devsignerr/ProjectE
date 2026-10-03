-- 훈련병 (Training 데모, ServerOnly): 교관의 훅에 쓰러지면 래그돌(RagdollComponent EnableOnDeath), 제자리 리스폰 뒤 일어나기 몽타주.
local Recruit = {
	Properties = {},
}

function Recruit:OnDeath(Instigator)
	Log.Info("[Training] 훈련병이 쓰러졌습니다 (래그돌)")
end

function Recruit:OnRespawned()
	-- 리스폰 = 체력 회복 + 래그돌 꺼짐 → 누운 자세에서 일어나는 클립
	self.entity:PlayMontage("LayToIdle", { BlendIn = 0.0, BlendOut = 0.35 })
end

return Recruit
