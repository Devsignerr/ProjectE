-- 능력 데모 HUD (클라이언트, UI/AbilityHUD.eui): 내 폰(Abilities.FindLocal)의 속성 막대, 능력 쿨다운, 활성 효과, 마지막 실패(거절 포함),
-- 훈련용 허수아비 상태. 값은 소유 클라이언트의 보기(서버 복제 + 예측)이므로 발동 즉시 마나·쿨다운이 바뀐다.
-- 검증: 콘솔 변수 ability.AutoCast=1 (--cvar ability.AutoCast=1)이면 능력을 순서대로 자동 발동한다 (쿨다운 중 재발동 실패 포함)
local AbilityHud = { Properties = { AutoCastInterval = 0.6 } }

local Slots = {
	{ Name = "Fireball", Key = "Q", Label = "화염구" },
	{ Name = "Dash",     Key = "R", Label = "대시" },
	{ Name = "Shield",   Key = "K", Label = "방어막" },
	{ Name = "HealZone", Key = "E", Label = "치유 지대" },
}
local AutoOrder = { "Shield", "Fireball", "Fireball", "Dash", "HealZone", "Fireball", "Dash", "Fireball" }
local EffectNames = { Burn = "화상", Shield = "방어막", Dashing = "대시", Regen = "재생" }
local Reasons = { Cooldown = "쿨다운", Cost = "자원 부족", Blocked = "막힘", Active = "이미 발동 중", Dead = "사망" }

function AbilityHud:OnStart()
	self.W = {}
	for _, Name in ipairs({ "HealthText", "HealthBar", "ManaText", "ManaBar", "StaminaText", "StaminaBar", "SlotsText", "EffectsText", "FailText", "DummyText" }) do
		self.W[Name] = self.entity:GetWidget(Name)
	end
	self.AutoIndex = 1
	self.AutoTimer = 1.5
end

local function Bar(Pawn, Text, Widget, Label, Name, MaxName)
	local Value, Max = Pawn:GetAttribute(Name) or 0, Pawn:GetAttribute(MaxName) or 1
	Text.Text      = string.format("%s %d / %d", Label, math.floor(Value + 0.5), math.floor(Max + 0.5))
	Widget.Percent = Max > 0 and Value / Max or 0
end

function AbilityHud:OnUpdate(dt)
	local W    = self.W
	local Pawn = Abilities.FindLocal()
	local Dummy = Scene.Find("TrainingDummy")
	if Dummy and Dummy:HasAbilitySystem() then
		local Health = Dummy:GetAttribute("Health") or 0
		local Burn = ""
		for _, E in ipairs(Dummy:GetActiveEffects()) do
			if E.Name == "Burn" then Burn = string.format("  화상 %d스택 %.1f초", E.Stacks, E.Remaining) end
		end
		W.DummyText.Text = string.format("허수아비 체력 %d%s", math.floor(Health + 0.5), Burn)
	end
	if not Pawn then
		W.SlotsText.Text = "플레이어를 기다리는 중..."
		return
	end
	Bar(Pawn, W.HealthText, W.HealthBar, "체력", "Health", "MaxHealth")
	Bar(Pawn, W.ManaText, W.ManaBar, "마나", "Mana", "MaxMana")
	Bar(Pawn, W.StaminaText, W.StaminaBar, "스태미나", "Stamina", "MaxStamina")

	local Parts = {}
	for _, Slot in ipairs(Slots) do
		local Remaining = Pawn:GetAbilityCooldown(Slot.Name)
		local State = Pawn:IsAbilityActive(Slot.Name) and "발동 중" or (Remaining > 0 and string.format("%.1f초", Remaining) or "준비")
		Parts[#Parts + 1] = string.format("%s %s %s", Slot.Key, Slot.Label, State)
	end
	W.SlotsText.Text = table.concat(Parts, "  |  ")

	local Effects = {}
	for _, E in ipairs(Pawn:GetActiveEffects()) do
		local Label = EffectNames[E.Name]
		if Label then
			Effects[#Effects + 1] = (E.Remaining >= 0 and string.format("%s %.1f초", Label, E.Remaining) or Label) .. (E.Predicted and "(예측)" or "")
		end
	end
	if Pawn:HasTag("State.Shielded") then Effects[#Effects + 1] = "받는 피해 ×" .. string.format("%.1f", Pawn:GetAttribute("DamageTaken") or 1) end
	W.EffectsText.Text = #Effects > 0 and ("효과: " .. table.concat(Effects, ", ")) or "효과: 없음"

	local Failed, Reason, Ago = Pawn:GetLastAbilityFailure()
	if Failed and Ago < 2.0 then
		local Text = Reasons[Reason] or Reason
		if Reason:sub(1, 9) == "Rejected:" then
			Text = "서버 거절(" .. (Reasons[Reason:sub(10)] or Reason:sub(10)) .. ") — 예측 되돌림"
		end
		W.FailText.Text = string.format("%s 실패: %s", Failed, Text)
	else
		W.FailText.Text = ""
	end

	if Abilities.IsAutoCast() then
		self.AutoTimer = self.AutoTimer - dt
		if self.AutoTimer <= 0 then
			self.AutoTimer = self.Properties.AutoCastInterval
			Pawn:TryActivateAbility(AutoOrder[self.AutoIndex])
			self.AutoIndex = self.AutoIndex % #AutoOrder + 1
		end
	end
end

return AbilityHud