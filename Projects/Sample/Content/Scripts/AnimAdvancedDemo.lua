-- 애니메이션 고급 데모 (Demo_AnimAdvanced의 Fox_Walker, Phase 42): 그래프 Animations/FoxAdvanced.eanimgraph
--   2D 블렌드: Speed(0~450) × Direction(-90~90)을 천천히 돌려 샘플 사이를 지나간다
--   Head 레이어: HeadLayer 파라미터를 몇 초마다 켜고 끈다 (목/머리만 Survey)
--   몽타주: E 키(또는 AutoMontagePeriod초마다)로 Survey를 "Upper" 슬롯(상체 마스크)에 한 번 재생 — 끝나면 OnMontageEnded
--   시선: LookTarget 엔티티를 SetLookAtTarget으로 본다 (모델 루트의 LookAtComponent)
local AnimAdvancedDemo = {
	Properties = {
		MaxSpeed          = 450.0, -- cm/s
		Period            = 10.0,  -- 초 (2D 파라미터 한 바퀴)
		LayerPeriod       = 6.0,   -- 초 (Head 레이어 켜고 끄기 한 번)
		AutoMontagePeriod = 5.0,   -- 초 (0이면 E 키로만)
		LookTarget        = "LookTarget",
		Montages          = 0,     -- 끝난 몽타주 수 (확인용)
	},
}

function AnimAdvancedDemo:OnStart()
	self.Time        = 0.0
	self.MontageTime = 0.0
	local Target     = Scene.Find(self.Properties.LookTarget)
	if Target ~= nil then
		self.entity:SetLookAtTarget(Target)
	end
end

function AnimAdvancedDemo:PlayUpperMontage()
	if not self.entity:IsMontagePlaying("Upper") then
		self.entity:PlayMontage("Survey", { Slot = "Upper", BlendIn = 0.25, BlendOut = 0.3, Speed = 1.2 })
	end
end

function AnimAdvancedDemo:OnUpdate(dt)
	local P = self.Properties
	self.Time = self.Time + dt
	local Angle = self.Time * 2.0 * math.pi / P.Period
	self.entity:SetAnimParam("Speed", (0.5 - 0.5 * math.cos(Angle)) * P.MaxSpeed)
	self.entity:SetAnimParam("Direction", math.sin(Angle * 2.0) * 90.0)
	local Layer = 0.5 + 0.5 * math.sin(self.Time * 2.0 * math.pi / P.LayerPeriod)
	self.entity:SetAnimParam("HeadLayer", Layer > 0.5 and 1.0 or 0.0)

	if Input.IsKeyPressed("E") then
		self:PlayUpperMontage()
	end
	if P.AutoMontagePeriod > 0 then
		self.MontageTime = self.MontageTime + dt
		if self.MontageTime >= P.AutoMontagePeriod then
			self.MontageTime = 0.0
			self:PlayUpperMontage()
		end
	end
end

function AnimAdvancedDemo:OnMontageEnded(clip, interrupted, slot)
	self.Properties.Montages = self.Properties.Montages + 1
	Log.Info("몽타주 끝:", clip, slot, interrupted and "(중단)" or "(완료)")
end

return AnimAdvancedDemo
