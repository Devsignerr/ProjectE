-- FarmBie 체력·정신력·음식·버프 (FarmGame에 섞이는 메서드 모음). 수치는 Data/FarmBie/Vitals.edata, 작물 버프는 Crops.etable Buff + Rarities.etable BuffSize.
--   먹기(Eat 입력 — 고른 칸의 작물): 체력 +(FoodHealthBase + 값×FoodHealthPerPrice)×FoodMul, 정신력 음식(Sanity > 0)은 정신력 +Sanity×FoodMul,
--     레어 이상 + Buff가 있으면 다음 날 아침까지 버프 (같은 종류는 큰 값만). Speed 이동 속도, Power 공격력, Guard 받는 피해 감소, Regen 체력 회복
--   정신력: 낮으면(LowSanity / CriticalSanity) 이동 속도·공격력 배율(StatMul) + 화면이 어둡고 바랜다(FarmTime:ApplyDayNight가 FearLevel을 씀).
--     잃는 곳은 LoseSanity(양, 이유) 하나 — 0이 되면 Collapse("Sanity").
--   쓰러짐(Collapse): 잠 전환 → 다음 날 CollapseWakeHour에 깸, 소지금 CollapseGoldLoss 잃음, 정신력/체력 CollapseSanity/CollapseHealth. 보스 패배(F8)도 같은 길
--   잠 회복: 침대 BedSanity / 새벽 DawnSanity
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Vit = {}

function Vit:InitVitals()
	local V = D.Values("Vitals.edata")
	self.Vitals = V
	self.Health = V.MaxHealth
	self.Sanity = V.StartSanity
	self.Buffs = {}
end

function Vit:StatMul()
	local V = self.Vitals
	if self.Sanity < V.CriticalSanity then return V.CriticalStatMul end
	if self.Sanity < V.LowSanity then return V.LowStatMul end
	return 1.0
end

-- 0(멀쩡) ~ 1(정신력 0) — 화면 효과
function Vit:FearLevel()
	local V = self.Vitals
	if not V or self.Sanity >= V.LowSanity then return 0 end
	return math.min(1, (V.LowSanity - self.Sanity) / V.LowSanity)
end

function Vit:BuffAmount(Kind)
	return self.Buffs[Kind] or 0
end

function Vit:SetSanity(Value)
	local Old = self.Sanity
	self.Sanity = math.max(0, math.min(self.Vitals.MaxSanity, Value))
	-- 문턱을 넘으면 화면 다시 (공포 정도가 바뀜)
	if math.floor(Old / 5) ~= math.floor(self.Sanity / 5) and self.ApplyDayNight then self:ApplyDayNight(true) end
end

function Vit:LoseSanity(Amount, Reason)
	if self.Phase == "Sleep" then return end
	self:SetSanity(self.Sanity - Amount)
	self.Report.SanityLost = (self.Report.SanityLost or 0) + Amount
	Log.Info(string.format("[FarmBie] 정신력 -%d (%s) → %d", Amount, Reason or "", self.Sanity))
	if self.Sanity <= 0 then self:Collapse("Sanity") end
end

function Vit:Damage(Amount, Reason)
	if self.Phase == "Sleep" then return 0 end
	local Taken = Amount * (1 - self:BuffAmount("Guard"))
	self.Health = math.max(0, self.Health - Taken)
	self.Report.DamageTaken = (self.Report.DamageTaken or 0) + Taken
	if self.Health <= 0 and self.OnPlayerDown then self:OnPlayerDown(Reason) end
	return Taken
end

function Vit:Heal(Amount)
	self.Health = math.min(self.Vitals.MaxHealth, self.Health + Amount)
end

-- 쓰러짐 → 잠 전환 (벌칙은 다음 날 아침 ApplySleepRecovery에서)
function Vit:Collapse(Reason)
	if self.Phase == "Sleep" then return end
	self.CollapseReason = Reason
	self.Report.Collapses = (self.Report.Collapses or 0) + 1
	self:Hud():Announce("정신을 잃었다…", Reason == "Sanity" and "정신력이 바닥났다" or "", 2.5)
	self:BeginSleep("Collapse")
end

-- 아침 (FarmGame:OnDayStart): 잠 종류에 따른 회복·쓰러짐 벌칙, 버프 끝 → 알림 글
function Vit:ApplySleepRecovery(Notes)
	local V = self.Vitals
	self.Buffs = {}
	if self.SleepReason == "Collapse" then
		local Loss = math.floor(self.Gold * V.CollapseGoldLoss + 0.5)
		self.Gold = self.Gold - Loss
		self.Hour = V.CollapseWakeHour
		self:SetSanity(math.max(self.Sanity, V.CollapseSanity))
		self.Health = math.max(self.Health, V.CollapseHealth)
		Notes[#Notes + 1] = string.format("쓰러졌다 — 소지금 -%d, %d시에 깸", Loss, math.floor(V.CollapseWakeHour))
		self.Report.CollapseGoldLost = (self.Report.CollapseGoldLost or 0) + Loss
		self.CollapseReason = nil
		self:ApplyDayNight(true)
	else
		local Gain = self.SleepReason == "Bed" and V.BedSanity or V.DawnSanity
		self:SetSanity(self.Sanity + Gain)
		self.Health = V.MaxHealth
	end
end

-- ---- 먹기
function Vit:CanEat(Key)
	if Key == nil then return false end
	local Kind = self:ItemInfo(Key).Kind
	return Kind == "Crop" or Kind == "Food"
end

function Vit:Eat(Key)
	if not self:CanEat(Key) then return false end
	local V = self.Vitals
	local Info = self:ItemInfo(Key)
	if Info.Kind == "Food" then
		-- 숲 음식 (Items.etable Health/Sanity)
		self:Take(Key, 1)
		self:Heal(Info.Row.Health)
		local Parts = { string.format("체력 +%d", Info.Row.Health) }
		if Info.Row.Sanity > 0 then
			self:SetSanity(self.Sanity + Info.Row.Sanity)
			Parts[#Parts + 1] = string.format("정신력 +%d", Info.Row.Sanity)
		end
		self.Report.Eaten = (self.Report.Eaten or 0) + 1
		self:Hud():Toast(Info.Icon, Info.Name .. " 먹음: " .. table.concat(Parts, ", "), { 0.75, 1.0, 0.7, 1.0 })
		return true
	end
	local Crop, R = Info.Crop, Info.RarityRow
	self:Take(Key, 1)
	local Hp = math.floor((V.FoodHealthBase + Crop.Price * V.FoodHealthPerPrice) * R.FoodMul + 0.5)
	self:Heal(Hp)
	local Parts = { string.format("체력 +%d", Hp) }
	if Crop.Sanity > 0 then
		local San = math.floor(Crop.Sanity * R.FoodMul + 0.5)
		self:SetSanity(self.Sanity + San)
		Parts[#Parts + 1] = string.format("정신력 +%d", San)
	end
	if Crop.Buff ~= "None" and R.BuffSize > 0 then
		self.Buffs[Crop.Buff] = math.max(self.Buffs[Crop.Buff] or 0, R.BuffSize)
		Parts[#Parts + 1] = string.format("%s +%d%% (아침까지)", self:BuffName(Crop.Buff), math.floor(R.BuffSize * 100 + 0.5))
	end
	self.Report.Eaten = (self.Report.Eaten or 0) + 1
	self:Hud():Toast(Info.Icon, Info.Name .. " 먹음: " .. table.concat(Parts, ", "), { 0.75, 1.0, 0.7, 1.0 })
	return true
end

local BuffNames = { Speed = "이동 속도", Power = "공격력", Guard = "방어", Regen = "회복" }
function Vit:BuffName(Kind)
	return BuffNames[Kind] or Kind
end

function Vit:BuffText()
	local Parts = {}
	for _, Kind in ipairs({ "Speed", "Power", "Guard", "Regen" }) do
		local A = self.Buffs[Kind]
		if A and A > 0 then Parts[#Parts + 1] = string.format("%s +%d%%", BuffNames[Kind], math.floor(A * 100 + 0.5)) end
	end
	local Mul = self:StatMul()
	if Mul < 1 then Parts[#Parts + 1] = string.format("정신 불안 −%d%%", math.floor((1 - Mul) * 100 + 0.5)) end
	return table.concat(Parts, "  ")
end

-- 매 프레임 (게임 Dt)
function Vit:UpdateVitals(Dt)
	local Regen = self:BuffAmount("Regen")
	if Regen > 0 and self.Health < self.Vitals.MaxHealth then
		self:Heal(self.Vitals.MaxHealth * Regen * 0.1 * Dt)
	end
	local P = self:Player()
	if P and P.Mover and P.BaseWalkSpeed then
		P.Mover.MaxWalkSpeed = P.BaseWalkSpeed * self:StatMul() * (1 + self:BuffAmount("Speed"))
	end
end

-- ---- 저장 조각
function Vit:SaveVitals(T)
	T.Health, T.Sanity = self.Health, self.Sanity
	T.Buffs = self.Buffs
end

function Vit:LoadVitals(T)
	self.Health = T.Health or self.Vitals.MaxHealth
	self.Sanity = T.Sanity or self.Vitals.StartSanity
	self.Buffs = {}
	for K, V in pairs(T.Buffs or {}) do self.Buffs[K] = V end
end

return Vit
