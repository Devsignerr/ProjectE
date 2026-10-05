-- HD-2D 데모 관리자 확장 ⑥ 대장간 (HD2DGame.lua가 Script.Require로 받아 메서드로 붙인다 — 상태는 관리자 self에).
--   대장장이 브론과 이야기를 마치면(서브 퀘스트 진행 중 대사·끝난 뒤 대사 — HD2DParty:TalkSubQuest → OnSubQuestTalkDone) 대장간 창(Menu = "Forge")이 열린다.
--   탭 1 무기 강화: 가진 무기마다 +1~+3 (Upgrades 표 — 재료·골드, 공격력 보너스는 HD2DMeta:UpgradedWeapon), 탭 2 물약 조합 (Recipes 표).
--   창은 Meta.eui의 ForgeWindow(HUD 인벤토리와 같은 MenuWindow 모양): 목록 10줄 + 설명(필요 재료는 비교 줄 4칸 — 충분하면 초록, 모자라면 빨강).
--   대장장이 말은 Meta.edata ForgeLines(들어옴/강화/조합/재료 부족/골드 부족/최대/나감).
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")
local M = Script.Require("Scripts/Demo/HD2D/HD2DMetaData.lua")

local Forge = {}
local Green, Red, White = { 0.55, 1.0, 0.55, 1 }, { 1.0, 0.5, 0.45, 1 }, { 0.9, 0.88, 0.84, 1 }
local Rows = 10

local function Line(I)
	local L = (M.Balance() or {}).ForgeLines or {}
	return L[I] or ""
end

-- 대장장이와 대화가 끝났을 때 (서브 퀘스트 진행 중·완료 뒤 대사)
function Forge:OnSubQuestTalkDone(Npc)
	if Npc.Id == "Smith" then self:OpenForge() end
end

function Forge:OpenForge()
	self.Menu = "Forge"
	self.ForgeTab = self.ForgeTab or 1
	self.ForgeIndex = 1
	self.ForgeSay = Line(1)
	self.ForgeNote = nil
	self:SetPaused(true)
	self:Hud():ShowPrompt(nil)
	self:Hud():SetHudVisible(false)
	local H = self:MetaHud()
	H:Show("ForgeShade", true, "Visible")
	H:Show("ForgeWindow", true, "SelfHitTestInvisible")
	H:SetPos("ForgeStatus", 268, 31)
	self.Report.ForgeOpened = (self.Report.ForgeOpened or 0) + 1
	self:RefreshForge()
	Audio.PlayOneShot(self.Sounds.Open)
	Log.Info("[HD2D] 대장간 열림")
end

function Forge:CloseForge()
	self.Menu = nil
	self:SetPaused(false)
	local H = self:MetaHud()
	H:Show("ForgeShade", false)
	H:Show("ForgeWindow", false)
	self:Hud():SetHudVisible(true)
	Audio.PlayOneShot(self.Sounds.Close)
	self:Hud():Toast(D.Npc("Smith").Portrait, D.Npc("Smith").DisplayName .. ": " .. Line(7))
end

-- 줄: 탭 1 = 가진 무기 id, 탭 2 = 조합 행
function Forge:ForgeRows()
	local Out = {}
	if self.ForgeTab == 1 then
		for _, Id in ipairs(D.WeaponOrder) do
			if self:Count(Id) > 0 then Out[#Out + 1] = { Weapon = Id } end
		end
	else
		for _, Row in ipairs(M.Recipes()) do Out[#Out + 1] = { Recipe = Row } end
	end
	return Out
end

-- 이 줄의 비용 (재료 목록, 골드, 다음 단계 행 또는 조합 행 — 최대 단계면 nil)
function Forge:ForgeCost(R)
	if R.Weapon then
		local Next = M.Upgrade(R.Weapon, self:UpgradeLevel(R.Weapon) + 1)
		if not Next then return nil end
		return M.ParseCost(Next.Materials), Next.Gold, Next
	end
	return M.ParseCost(R.Recipe.Materials), R.Recipe.Gold, R.Recipe
end

function Forge:RefreshForge()
	local List = self:ForgeRows()
	self.ForgeIndex = math.max(1, math.min(self.ForgeIndex, math.max(1, #List)))
	local H = self:MetaHud()
	H:SetTabs("ForgeTab", 2, self.ForgeTab)
	H:Set("ForgeStatus", "Text", "「" .. (self.ForgeSay or "") .. "」")
	H:Set("ForgeGold", "Text", string.format("%d G", self.Gold))
	for I = 1, Rows do
		local R = List[I]
		local Name = "Forge"
		local K = I - 1
		H:Set(Name .. "Row" .. K, "Visible", R ~= nil)
		if R then
			local bSel = I == self.ForgeIndex
			H:Show(Name .. "Sel" .. K, bSel)
			H:Set(Name .. "Cursor" .. K, "Visibility", bSel and "HitTestInvisible" or "Hidden")
			local Cost, Gold = self:ForgeCost(R)
			local bCan = Cost ~= nil and self:CanPay(Cost, Gold)
			if R.Weapon then
				local Row = D.Item(R.Weapon)
				local Level = self:UpgradeLevel(R.Weapon)
				H:Set(Name .. "Icon" .. K, "Texture", Row.Icon)
				H:Set(Name .. "Name" .. K, "Text", self:ItemDisplayName(R.Weapon))
				H:Set(Name .. "Count" .. K, "Text", Level >= M.MaxUpgrade and "최대" or string.format("+%d → +%d", Level, Level + 1))
			else
				local Row = D.Item(R.Recipe.Result)
				H:Set(Name .. "Icon" .. K, "Texture", Row.Icon)
				H:Set(Name .. "Name" .. K, "Text", Row.DisplayName .. (R.Recipe.Count > 1 and (" ×" .. R.Recipe.Count) or ""))
				H:Set(Name .. "Count" .. K, "Text", string.format("보유 %d", self:Count(R.Recipe.Result)))
			end
			H:TextTone(Name .. "Name" .. K, bSel, not bCan)
		end
	end
	local R = List[self.ForgeIndex]
	local Cmp = {}
	if not R then
		self:ForgeDetail(nil, "", "", self.ForgeTab == 1 and "강화할 무기가 없다" or "조합법이 없다", "")
	else
		local Cost, Gold, Info = self:ForgeCost(R)
		if R.Weapon then
			local Row, W = D.Item(R.Weapon), D.Weapon(R.Weapon)
			local Level = self:UpgradeLevel(R.Weapon)
			local Now = self:GetWeaponById(R.Weapon).Damage
			local Stats
			if not Info then
				Stats = string.format("공격력 %d\n더 이상 강화할 수 없다", Now)
			else
				local After = W.Damage + Info.Damage
				Stats = string.format("공격력 %d → %d  (+%d)\n비용 %d G", Now, After, After - Now, Gold)
			end
			if self.ForgeNote then Stats = Stats .. "\n" .. self.ForgeNote end
			if Info then Stats = Stats .. "\n\n필요한 재료" end
			self:ForgeDetail(Row.Icon, self:ItemDisplayName(R.Weapon), string.format("무기 강화 · 지금 +%d / 최대 +%d", Level, M.MaxUpgrade), W.Description, Stats)
		else
			local Row = D.Item(R.Recipe.Result)
			local Stats = string.format("만들면 %s ×%d (지금 %d개)\n비용 %d G", Row.DisplayName, R.Recipe.Count, self:Count(R.Recipe.Result), Gold)
			if self.ForgeNote then Stats = Stats .. "\n" .. self.ForgeNote end
			Stats = Stats .. "\n\n필요한 재료"
			self:ForgeDetail(Row.Icon, Row.DisplayName, "물약 조합", Row.Description, Stats)
		end
		for _, C in ipairs(Cost or {}) do
			local Item = D.Item(C.Id)
			local Have = self:Count(C.Id)
			Cmp[#Cmp + 1] = { Text = string.format("%s ×%d     (가진 것 %d)", Item and Item.DisplayName or C.Id, C.N, Have), Color = Have >= C.N and Green or Red }
		end
		if Cost and #Cmp == 0 then Cmp[1] = { Text = "재료 없이 만들 수 있다", Color = White } end
	end
	for K = 0, 3 do
		local L = Cmp[K + 1]
		H:Show("ForgeCmp" .. K, L ~= nil)
		if L then
			H:Set("ForgeCmp" .. K, "Text", "·  " .. L.Text)
			H:SetColor("ForgeCmp" .. K, L.Color[1], L.Color[2], L.Color[3], L.Color[4])
		end
	end
end

function Forge:ForgeDetail(Icon, Name, Type, Desc, Stats)
	local H = self:MetaHud()
	H:Show("ForgeDetailIcon", Icon ~= nil)
	if Icon then H:Set("ForgeDetailIcon", "Texture", Icon) end
	H:Set("ForgeDetailName", "Text", Name)
	H:Set("ForgeDetailType", "Text", Type)
	H:Set("ForgeDetailDesc", "Text", Desc)
	H:Set("ForgeDetailStats", "Text", Stats)
end

function Forge:ForgeConfirm()
	local R = self:ForgeRows()[self.ForgeIndex]
	if not R then return end
	local Cost, Gold, Info = self:ForgeCost(R)
	self.ForgeNote = nil
	if not Info then
		self.ForgeSay = Line(6)
		Audio.PlayOneShot(self.Sounds.Error)
	elseif not self:CanPay(Cost, 0) then
		self.ForgeSay = Line(4)
		Audio.PlayOneShot(self.Sounds.Error)
	elseif self.Gold < Gold then
		self.ForgeSay = Line(5)
		Audio.PlayOneShot(self.Sounds.Error)
	else
		self:Pay(Cost, Gold)
		if R.Weapon then
			self.Upgrades[R.Weapon] = self:UpgradeLevel(R.Weapon) + 1
			self.Stats.Upgrades = self.Stats.Upgrades + 1
			self.Report.Upgrades = (self.Report.Upgrades or 0) + 1
			self.ForgeSay = Line(2)
			self.ForgeNote = string.format("+%d 로 강화했다!", self.Upgrades[R.Weapon])
			local Player = self:GetPlayer()
			if Player and self.Equipped == R.Weapon then Player:OnWeaponChanged() end
			Audio.PlayOneShot("Audio/RPG/HitHeavy.wav", 0.8, 1.3)
			Audio.PlayOneShot(self.Sounds.Equip)
			Log.Info(string.format("[HD2D] 무기 강화: %s +%d (공격력 %d)", R.Weapon, self.Upgrades[R.Weapon], self:GetWeaponById(R.Weapon).Damage))
		else
			self:AddItem(R.Recipe.Result, R.Recipe.Count, false)
			self.Stats.Crafts = self.Stats.Crafts + 1
			self.Report.Crafts = (self.Report.Crafts or 0) + 1
			self.ForgeSay = Line(3)
			self.ForgeNote = string.format("%s ×%d 을(를) 만들었다!", D.Item(R.Recipe.Result).DisplayName, R.Recipe.Count)
			Audio.PlayOneShot(self.Sounds.Potion)
			Audio.PlayOneShot(self.Sounds.Buy, 0.8, 1.2)
			Log.Info(string.format("[HD2D] 조합: %s ×%d", R.Recipe.Result, R.Recipe.Count))
		end
	end
	self:RefreshForge()
end

function Forge:ForgeInput(In)
	if In.Cancel or In.Inventory then
		self:CloseForge()
		return
	end
	local List = self:ForgeRows()
	if In.MenuLeft or In.MenuRight then
		self.ForgeTab = self.ForgeTab == 1 and 2 or 1
		self.ForgeIndex, self.ForgeNote = 1, nil
		Audio.PlayOneShot(self.Sounds.Move)
		self:RefreshForge()
	elseif (In.MenuUp or In.MenuDown) and #List > 0 then
		self.ForgeIndex = (self.ForgeIndex - 1 + (In.MenuDown and 1 or -1)) % #List + 1
		self.ForgeNote = nil
		Audio.PlayOneShot(self.Sounds.Move)
		self:RefreshForge()
	elseif In.Confirm then
		self:ForgeConfirm()
	end
end

return Forge
