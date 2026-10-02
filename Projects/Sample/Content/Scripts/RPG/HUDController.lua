-- RPG HUD (UI/RPG/HUD.eui): 체력/마나/기력 막대, 골드, 스킬(Q/R/Space) 재사용 대기, 물약 퀵 슬롯(1/2), 알림, 상호작용 안내, 사망 화면.
--   값은 플레이어 스크립트(GetStats/GetSkillCooldowns/IsDead/Respawn)와 GameManager에서 읽는다. 둘 다 없으면 해당 부분만 비운다.
--   물약 단축키(UsePotion1/UsePotion2)도 여기서 처리한다.
local HUDController = {
	Properties = {
		PlayerName = "Player",
	},
}

local SkillKeys = { "Skill1", "Skill2", "Dodge" }

-- 액션이 없는 프로젝트 설정(통합 전)에서도 오류 없이: 없으면 대체 키
local function ActionPressed(Action, FallbackKey)
	local bOk, bPressed = pcall(Input.WasActionPressed, Action)
	if bOk then
		return bPressed
	end
	return FallbackKey ~= nil and Input.IsKeyPressed(FallbackKey)
end

local function CallOptional(Target, Method, ...)
	if Target == nil or type(Target[Method]) ~= "function" then
		return nil
	end
	local bOk, Result = pcall(Target[Method], Target, ...)
	return bOk and Result or nil
end

function HUDController:OnStart()
	local Manager = Scene.Find("GameManager")
	self.GM = Manager and Manager:GetScript() or nil
	self.Player = Scene.Find(self.Properties.PlayerName)

	local W = function(Name) return self.entity:GetWidget(Name) end
	self.Bars = {
		{ Bar = W("HealthBar"), Text = W("HealthText"), Value = "Health", Max = "MaxHealth", Label = "HP" },
		{ Bar = W("ManaBar"), Text = W("ManaText"), Value = "Mana", Max = "MaxMana", Label = "MP" },
		{ Bar = W("StaminaBar"), Text = W("StaminaText"), Value = "Stamina", Max = "MaxStamina", Label = "SP" },
	}
	self.GoldText    = W("GoldText")
	self.Toasts      = { W("Toast1"), W("Toast2"), W("Toast3"), W("Toast4") }
	self.PromptPanel = W("PromptPanel")
	self.PromptText  = W("PromptText")
	self.PromptKey   = W("PromptKey")
	self.DeathScreen = W("DeathScreen")
	self.Skills = {}
	for _, Key in ipairs(SkillKeys) do
		self.Skills[Key] = { Cooldown = W(Key .. "Cooldown"), Text = W(Key .. "CooldownText"), Icon = W(Key .. "Icon") }
	end
	self.Potions = {
		{ Count = W("Potion1Count"), Icon = W("Potion1Icon"), Cooldown = W("Potion1Cooldown") },
		{ Count = W("Potion2Count"), Icon = W("Potion2Icon"), Cooldown = W("Potion2Cooldown") },
	}
	self.ShownRevision = -1
	self.ShownGold     = nil
	self.LastPrompt    = nil
	self.bDeadShown    = nil
end

function HUDController:OnUpdate(dt)
	if self.Player == nil or not self.Player:IsValid() then
		self.Player = Scene.Find(self.Properties.PlayerName) -- 리스폰 등으로 바뀌었을 수 있다 (실패 시 다음 프레임에 다시)
	end
	local PlayerScript = self.Player and self.Player:GetScript() or nil

	self:UpdateStats(PlayerScript)
	self:UpdateSkills(PlayerScript)
	self:UpdateInventory()
	self:UpdateToasts()
	self:UpdatePrompt()
	self:UpdateDeath(PlayerScript)

	-- 물약 단축키 (죽었거나 창에 텍스트 입력 중이면 무시)
	if self.GM ~= nil and not self.bDeadShown then
		if ActionPressed("UsePotion1", "1") then
			self.GM:UseQuickPotion(1)
		elseif ActionPressed("UsePotion2", "2") then
			self.GM:UseQuickPotion(2)
		end
	end
end

function HUDController:UpdateStats(PlayerScript)
	local Stats = CallOptional(PlayerScript, "GetStats")
	for _, Entry in ipairs(self.Bars) do
		local Value, Max = Stats and Stats[Entry.Value], Stats and Stats[Entry.Max]
		if Value ~= nil and Max ~= nil and Max > 0 then
			Entry.Bar.Percent = Value / Max
			Entry.Text.Text   = string.format("%s %d / %d", Entry.Label, math.ceil(Value), math.floor(Max))
		else
			Entry.Bar.Percent = 0
			Entry.Text.Text   = Entry.Label .. " -"
		end
	end
end

function HUDController:UpdateSkills(PlayerScript)
	local Cooldowns = CallOptional(PlayerScript, "GetSkillCooldowns") or {}
	for _, Key in ipairs(SkillKeys) do
		local Widgets = self.Skills[Key]
		local Entry   = Cooldowns[Key]
		local Remaining, Duration = 0, 0
		if Entry ~= nil then
			Remaining, Duration = Entry.Remaining or 0, Entry.Duration or 0
		end
		if Remaining > 0 and Duration > 0 then
			Widgets.Cooldown.Percent = math.min(Remaining / Duration, 1)
			Widgets.Text.Text        = Remaining >= 1 and string.format("%d", math.ceil(Remaining)) or string.format("%.1f", Remaining)
			Widgets.Icon.Opacity     = 0.55
		else
			Widgets.Cooldown.Percent = 0
			Widgets.Text.Text        = ""
			Widgets.Icon.Opacity     = Entry ~= nil and 1.0 or 0.35 -- 플레이어가 이 스킬을 알려 주지 않으면 흐리게
		end
	end
end

function HUDController:UpdateInventory()
	if self.GM == nil then
		self.GoldText.Text = "-"
		return
	end
	if self.GM.Revision == self.ShownRevision then
		return
	end
	self.ShownRevision = self.GM.Revision
	local Gold = self.GM:GetGold()
	if self.ShownGold ~= nil and Gold ~= self.ShownGold then
		self.entity:PlayUIAnimation("Pulse")
	end
	self.ShownGold     = Gold
	self.GoldText.Text = tostring(Gold)
	for Index, Widgets in ipairs(self.Potions) do
		local Count = self.GM:CountQuickPotion(Index)
		local Def   = self.GM:GetItemDef(self.GM:GetQuickPotion(Index))
		Widgets.Count.Text   = tostring(Count)
		Widgets.Icon.Opacity = Count > 0 and 1.0 or 0.35
		if Def ~= nil then
			Widgets.Icon.Texture = Def.Icon
		end
	end
end

function HUDController:UpdateToasts()
	local List = self.GM and self.GM:GetToasts() or {}
	-- 아래 줄이 가장 최근
	local First = #self.Toasts - #List
	for Index, Widget in ipairs(self.Toasts) do
		local Toast = List[Index - First]
		if Toast ~= nil then
			Widget.Text    = Toast.Text
			Widget.Opacity = Toast.Alpha
		else
			Widget.Text = ""
		end
	end
end

function HUDController:UpdatePrompt()
	local Prompt = self.GM and self.GM:GetPrompt() or nil
	if self.GM ~= nil and self.GM:IsAnyWindowOpen() then
		Prompt = nil -- 창이 열려 있으면 안내를 숨긴다
	end
	local Text = Prompt and Prompt.Text or nil
	if Text == self.LastPrompt then
		return
	end
	self.LastPrompt = Text
	if Text ~= nil then
		self.PromptText.Text = Text
		self.PromptKey.Text  = Prompt.Key or "E"
		self.PromptPanel.Visibility = "HitTestInvisible"
	else
		self.PromptPanel.Visibility = "Collapsed"
	end
end

function HUDController:UpdateDeath(PlayerScript)
	local bDead = CallOptional(PlayerScript, "IsDead") == true
	if bDead ~= self.bDeadShown then
		self.bDeadShown = bDead
		self.DeathScreen.Visibility = bDead and "SelfHitTestInvisible" or "Collapsed"
		if bDead then
			-- 사망 화면이 떠 있는 동안 다른 창은 닫는다
			for _, Name in ipairs({ "InventoryUI", "ShopUI" }) do
				local Window = Scene.Find(Name)
				CallOptional(Window and Window:GetScript() or nil, "Close")
			end
		end
	end
	if bDead and Input.IsKeyPressed("R") then
		self:Respawn()
	end
end

function HUDController:OnUIClicked_RespawnButton()
	self:Respawn()
end

function HUDController:Respawn()
	local PlayerScript = self.Player and self.Player:GetScript() or nil
	if PlayerScript == nil or type(PlayerScript.Respawn) ~= "function" then
		Log.Warn("HUD: 플레이어에 Respawn()이 없습니다")
		return
	end
	CallOptional(PlayerScript, "Respawn")
	if self.GM ~= nil then
		self.GM:PlaySound("Confirm")
	end
end

return HUDController
