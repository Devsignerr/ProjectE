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

-- ---------------------------------------------------------------- 머리 위 체력바 / 데미지 숫자 (Camera.WorldToScreen + 위젯 복제)
-- 적은 GameManager:TrackEnemy → AddEnemyBar, 피해는 GameManager:ShowDamageNumber → AddDamageNumber로 들어온다.
-- 위젯은 OnLateUpdate에서 만든다(UI 인스턴스가 준비되기 전에 불릴 수 있음). 위치 계산도 같은 프레임 위치를 쓰려고 OnLateUpdate에서.
local BarHeight      = 235  -- cm: 적 발 기준 체력바 높이
local BarShowTime    = 4.0  -- 초: 맞은 뒤 전투가 아니어도 보이는 시간
local DamageLife     = 0.9  -- 초
local DamageRise     = 60   -- UI 단위: 떠오르는 높이
local DamageHeight   = 0    -- cm: 호출하는 쪽이 이미 머리 높이를 준다 (적 +170)
local MaxDamageCount = 24
local DamageStyles = {
	Normal = { Color = Vector4(1, 1, 1, 1),        Size = 32 },
	Crit   = { Color = Vector4(1, 0.82, 0.2, 1),   Size = 42 },
	Heal   = { Color = Vector4(0.45, 1, 0.5, 1),   Size = 32 },
	Player = { Color = Vector4(1, 0.35, 0.3, 1),   Size = 34 },
}

function HUDController:EnsureOverlay()
	if self.EnemyBars == nil then
		self.EnemyBars     = {}
		self.PendingBars   = {}
		self.DamageNumbers = {}
		self.OverlaySerial = 0
	end
end

function HUDController:AddEnemyBar(Entity)
	self:EnsureOverlay()
	table.insert(self.PendingBars, Entity)
end

function HUDController:AddDamageNumber(WorldPos, Amount, Kind)
	self:EnsureOverlay()
	if #self.DamageNumbers >= MaxDamageCount then
		self:RemoveDamageNumber(1)
	end
	local Value = math.floor((tonumber(Amount) or 0) + 0.5)
	local Text  = tostring(Value)
	if Kind == "Heal" then
		Text = "+" .. Text
	elseif Kind == "Crit" then
		Text = Text .. "!"
	end
	table.insert(self.DamageNumbers, {
		Pos = WorldPos + Vector3(0, 0, DamageHeight), Text = Text, Style = DamageStyles[Kind] or DamageStyles.Normal,
		Age = 0, Jitter = (math.random() - 0.5) * 36,
	})
end

function HUDController:NextOverlayName(Prefix)
	self.OverlaySerial = self.OverlaySerial + 1
	return Prefix .. self.OverlaySerial
end

function HUDController:OnLateUpdate(dt)
	self:EnsureOverlay()
	self:UpdateEnemyBars(dt)
	self:UpdateDamageNumbers(dt)
end

function HUDController:UpdateEnemyBars(dt)
	-- 새로 등록된 적: 위젯 만들기
	for _, Entity in ipairs(self.PendingBars) do
		local bKnown = false
		for _, Bar in ipairs(self.EnemyBars) do
			bKnown = bKnown or Bar.Entity == Entity
		end
		if not bKnown and Entity:IsValid() then
			local Name   = self:NextOverlayName("EnemyBar")
			local Widget = self.entity:CloneWidget("EnemyBarTemplate", Name)
			local Script = Entity:GetScript()
			local Label  = self.entity:GetWidget(Name .. ".Name")
			if Label ~= nil then
				Label.Text = CallOptional(Script, "GetDisplayName") or Entity:GetName()
			end
			Widget.Visible = false
			table.insert(self.EnemyBars, { Entity = Entity, Name = Name, Widget = Widget, Hp = self.entity:GetWidget(Name .. ".Hp"),
			                               LastFraction = 1, ShowTime = 0 })
		end
	end
	self.PendingBars = {}

	for Index = #self.EnemyBars, 1, -1 do
		local Bar    = self.EnemyBars[Index]
		local Script = Bar.Entity:IsValid() and Bar.Entity:GetScript() or nil
		if Script == nil or CallOptional(Script, "IsDead") then
			self.entity:RemoveWidget(Bar.Name)
			table.remove(self.EnemyBars, Index)
		else
			local Fraction = CallOptional(Script, "GetHealthFraction") or 1
			if Fraction < Bar.LastFraction - 0.0001 then
				Bar.ShowTime = BarShowTime
			end
			Bar.LastFraction = Fraction
			Bar.ShowTime     = math.max(0, Bar.ShowTime - dt)
			local State      = Script.State
			local bCombat    = State == "Chase" or State == "Attack" or State == "Retreat"
			local bVisible   = false
			if bCombat or Bar.ShowTime > 0 then
				local X, Y, bOnScreen = Camera.WorldToScreen(Bar.Entity:GetWorldPosition() + Vector3(0, 0, BarHeight), self.entity)
				if bOnScreen then
					Bar.Widget.Position = Vector2(X, Y)
					if Bar.Hp ~= nil then
						Bar.Hp.Percent = Fraction
					end
					bVisible = true
				end
			end
			if Bar.Widget.Visible ~= bVisible then
				Bar.Widget.Visible = bVisible
			end
		end
	end
end

function HUDController:RemoveDamageNumber(Index)
	local Number = self.DamageNumbers[Index]
	if Number.Name ~= nil then
		self.entity:RemoveWidget(Number.Name)
	end
	table.remove(self.DamageNumbers, Index)
end

function HUDController:UpdateDamageNumbers(dt)
	for Index = #self.DamageNumbers, 1, -1 do
		local Number = self.DamageNumbers[Index]
		Number.Age = Number.Age + dt
		if Number.Age >= DamageLife then
			self:RemoveDamageNumber(Index)
		else
			if Number.Widget == nil then
				Number.Name   = self:NextOverlayName("Dmg")
				Number.Widget = self.entity:CloneWidget("DmgTemplate", Number.Name)
				Number.Widget.Text     = Number.Text
				Number.Widget.Color    = Number.Style.Color
				Number.Widget.FontSize = Number.Style.Size
				Number.Widget.Visible  = true
			end
			local T = Number.Age / DamageLife
			local X, Y, bOnScreen = Camera.WorldToScreen(Number.Pos, self.entity)
			Number.Widget.Visible = bOnScreen
			if bOnScreen then
				local Rise = DamageRise * (1 - (1 - T) * (1 - T)) -- 빠르게 떠올라 천천히 멈춤
				Number.Widget.Position = Vector2(X + Number.Jitter, Y - Rise)
				Number.Widget.Opacity  = T < 0.6 and 1 or (1 - (T - 0.6) / 0.4)
			end
		end
	end
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
