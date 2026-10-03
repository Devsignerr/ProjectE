-- 검술 대련 (Training 데모, ServerOnly): 두 검사(UAL2 마네킹 + 소켓에 붙은 검·방패)가 번갈아 공격한다.
--   리더(Leader = true) 하나가 코루틴으로 순서를 정한다: 공격 몽타주(Sword_Regular_*) → 상대는 Sword_Block 몽타주.
--   공격 클립의 노티파이(UAL2_Standard.glb.emeta): SwordHit = 칼이 부딪힌 순간(불꽃 빛), SwordFinisher = 마무리 일격 → 상대 Hit_Knockback.
--   짧은 공격(A/B)은 끝나면 회복 클립(*_Rec)을 이어 재생한다 (OnMontageEnded).
local SparringFighter = {
	Properties = {
		Partner = "",      -- 상대 엔티티 이름
		Leader  = false,   -- 순서를 정하는 쪽 (둘 중 하나만)
	},
}

local Attacks = {
	{ Clip = "Sword_Regular_A", Rec = "Sword_Regular_A_Rec", Length = 0.43, RecLength = 0.97, BlockDelay = 0.0 },
	{ Clip = "Sword_Regular_B", Rec = "Sword_Regular_B_Rec", Length = 0.53, RecLength = 1.03, BlockDelay = 0.05 },
	{ Clip = "Sword_Regular_C", Length = 2.0, BlockDelay = 0.35 },
	{ Clip = "Sword_Regular_Combo", Length = 3.0, BlockDelay = 0.2 },
}
local Order = { 1, 2, 4, 1, 3, 2, 1, 4, 2, 3 }

function SparringFighter:OnStart()
	self.Partner = Scene.Find(self.Properties.Partner)
	self.Flash = 0.0
	if self.Properties.Leader then
		-- 부딪힘 불꽃 빛: 두 검사 사이 (리더만 하나 만든다)
		self.Spark = Scene.Create("Sparring_Spark")
		local Light = self.Spark:AddComponent("PointLightComponent")
		Light.Color = Vector3(1.0, 0.75, 0.4)
		Light.Intensity = 0.0
		Light.Radius = 260.0
		self.SparkLight = Light
		Coroutine.Start(function() self:Run() end)
	end
end

function SparringFighter:PartnerScript()
	if self.Partner and self.Partner:IsValid() then
		return self.Partner:GetScript()
	end
	return nil
end

function SparringFighter:Run()
	Wait(1.2)
	local Turn = 0
	while true do
		for _, Index in ipairs(Order) do
			Turn = Turn + 1
			local Other = self:PartnerScript()
			local Attacker, Defender = self, Other
			if Turn % 2 == 0 then Attacker, Defender = Other, self end
			if Attacker and Defender then
				local A = Attacks[Index]
				Attacker:Attack(A)
				Wait(A.BlockDelay)
				Defender:Block()
				Wait(A.Length + (A.RecLength or 0.0) + 0.5)
			else
				Wait(1.0)
			end
		end
		Wait(1.5)
	end
end

function SparringFighter:Attack(A)
	self.Current = A
	self.entity:PlayMontage(A.Clip, { BlendIn = 0.12, BlendOut = A.Rec and 0.05 or 0.25 })
end

function SparringFighter:Block()
	self.entity:PlayMontage("Sword_Block", { BlendIn = 0.1, BlendOut = 0.3 })
end

function SparringFighter:TakeHit()
	self.entity:PlayMontage("Hit_Knockback", { BlendIn = 0.05, BlendOut = 0.3 })
end

function SparringFighter:OnMontageEnded(Clip, Interrupted)
	local A = self.Current
	if A and A.Rec and Clip == A.Clip and not Interrupted then
		self.entity:PlayMontage(A.Rec, { BlendIn = 0.05, BlendOut = 0.3 })
	end
end

function SparringFighter:SparkAt()
	local Leader = self.Properties.Leader and self or self:PartnerScript()
	if not Leader or not Leader.Spark or not Leader.Partner then return end
	local Mid = (self.entity:GetWorldPosition() + Leader.Partner:GetWorldPosition()) * 0.5
	Leader.Spark:SetPosition(Mid + Vector3(0, 0, 140))
	Leader.Flash = 1.0
end

function SparringFighter:OnAnimNotify_SwordHit()
	self:SparkAt()
end

function SparringFighter:OnAnimNotify_SwordFinisher()
	self:SparkAt()
	local Other = self:PartnerScript()
	if Other then Other:TakeHit() end
end

function SparringFighter:OnUpdate(dt)
	if self.SparkLight then
		self.Flash = math.max(0.0, self.Flash - dt * 6.0)
		self.SparkLight.Intensity = self.Flash * 6.0
	end
end

return SparringFighter
