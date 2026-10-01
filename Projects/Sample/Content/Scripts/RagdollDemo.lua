-- 사망 래그돌 예제 (Phase 30-3): Delay초 뒤 Victims(쉼표 이름)에게 큰 데미지 → 체력 0이면 RagdollComponent가 래그돌을 켠다
-- (리스폰하면 원래대로). LuaTarget은 데미지 없이 entity:EnableRagdoll()로 켰다가 HoldSeconds 뒤 DisableRagdoll()로 끈다
local RagdollDemo = {
	Properties = {
		Victims     = "",
		LuaTarget   = "",
		Delay       = 1.0,  -- 초
		HoldSeconds = 4.0,  -- Lua 래그돌 유지 시간
		Repeat      = 8.0,  -- 이 주기로 반복 (리스폰 3초 뒤 다시)
	},
}

function RagdollDemo:OnStart()
	self.Timer = 0.0
	self.Stage = 0
end

function RagdollDemo:OnUpdate(dt)
	self.Timer = self.Timer + dt
	local P = self.Properties
	if self.Stage == 0 and self.Timer >= P.Delay then
		self.Stage = 1
		for Name in string.gmatch(P.Victims, "[^,]+") do
			local Victim = Scene.Find(Name)
			if Victim then
				Victim:ApplyDamage(10000, self.entity)
			end
		end
		local Target = Scene.Find(P.LuaTarget)
		if Target then
			Log.Info("Lua 래그돌 켜기:", tostring(Target:EnableRagdoll()))
		end
	elseif self.Stage == 1 and self.Timer >= P.Delay + P.HoldSeconds then
		self.Stage = 2
		local Target = Scene.Find(P.LuaTarget)
		if Target and Target:IsRagdollActive() then
			Target:DisableRagdoll()
		end
	elseif self.Stage == 2 and self.Timer >= P.Repeat then
		self.Stage = 0
		self.Timer = 0.0
	end
end

return RagdollDemo
