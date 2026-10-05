-- HD-2D 데모 관리자 확장 ② 메뉴 (HD2DGame.lua가 Script.Require로 받아 메서드로 붙인다 — 상태는 관리자 self에).
--   Menu: nil | "Title" | "Dialog" | "Inventory" | "Shop" | "Travel" | "Ending" (+ 메타: "Pause" | "Slots" | "Confirm" | "Forge" | "Leaving" — HD2DPause/HD2DCrafting).
--         열려 있는 동안 Game.SetTimeScale(0) — 글자·커서·페이드는 실제 시간.
--   입력은 플레이어 스크립트가 넘긴다 (MenuInput — In: MenuUp/MenuDown/MenuLeft/MenuRight/Confirm/Cancel/Inventory).
--   대화 줄: "대사" 또는 "이름|초상화 id|대사" (이름이 비면 해설 — 이름표 숨김, 초상화 id = UI/Demo/HD2D/Portraits/<id>.png).
--   인벤토리 탭: 1 도구(소모품·재료) / 2 장비(무기·방어구·장신구 — 바꿨을 때 능력치 비교 ↑초록 ↓빨강) / 3 퀘스트(메인 + 받은 서브 퀘스트).
--   상점: Balance.ShopStock, 주인 말 Balance.ShopLines(들어옴/구입/부족/이미 가짐/나감).
--   엔딩·크레딧: 메인 퀘스트 마지막 단계(Ending)에서 Balance.EndingPages 쪽을 차례로 (쪽마다 페이드, E·J로 넘김) → 끝나면 다시 플레이.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local Menu = {}
local PortraitDir = "UI/Demo/HD2D/Portraits/"

function Menu:IsMenuOpen() return self.Menu ~= nil end

function Menu:SetPaused(bPaused)
	Game.SetTimeScale(bPaused and 0.0 or 1.0)
end

-- ================================================================ 대화
local function ParseLine(Line, Name, Portrait)
	local N, P, T = string.match(Line, "^([^|]*)|([^|]*)|(.*)$")
	if T then
		return N, P ~= "" and (PortraitDir .. P .. ".png") or nil, T
	end
	return Name, Portrait, Line
end

function Menu:StartDialog(Name, Lines, OnDone, Portrait)
	if Lines == nil or #Lines == 0 then
		if OnDone then OnDone() end
		return
	end
	self.Menu = "Dialog"
	self.Dialog = { Name = Name, Portrait = Portrait, Lines = Lines, Index = 1, Chars = 0.0, OnDone = OnDone }
	self:SetPaused(true)
	self:Hud():ShowPrompt(nil)
	self:ShowDialogLine()
	self.Report.Dialogs = self.Report.Dialogs + 1
	Audio.PlayOneShot(self.Sounds.Open)
end

function Menu:ShowDialogLine()
	local Dlg = self.Dialog
	local Name, Portrait, Text = ParseLine(Dlg.Lines[Dlg.Index], Dlg.Name, Dlg.Portrait)
	Dlg.Text = Text
	self:Hud():ShowDialog(Name, Portrait, Text, math.floor(Dlg.Chars), Dlg.Chars >= (utf8.len(Text) or #Text))
end

function Menu:UpdateDialog(UDt, In)
	local Dlg = self.Dialog
	local Total = utf8.len(Dlg.Text) or #Dlg.Text
	if Dlg.Chars < Total then
		Dlg.Chars = math.min(Total, Dlg.Chars + UDt * 42.0 * self:TextSpeed()) -- 설정: 글자 속도 (HD2DMeta)
	end
	if In.Confirm then
		if Dlg.Chars < Total then
			Dlg.Chars = Total
		elseif Dlg.Index < #Dlg.Lines then
			Dlg.Index = Dlg.Index + 1
			Dlg.Chars = 0
			Audio.PlayOneShot(self.Sounds.Move)
		else
			self:CloseMenu()
			if Dlg.OnDone then Dlg.OnDone() end
			return
		end
	end
	self:ShowDialogLine()
end

function Menu:CloseMenu()
	local Was = self.Menu
	self.Menu = nil
	self:SetPaused(false)
	local H = self:Hud()
	if Was == "Dialog" then H:HideDialog() else H:ShowMenu(nil) end
	Audio.PlayOneShot(self.Sounds.Close)
	self:AfterMenuClosed(Was) -- 일시정지 메뉴에서 연 인벤토리면 일시정지로 (HD2DPause)
end

-- ================================================================ 타이틀
function Menu:OpenTitle()
	self.Mode = "Title"
	self.Menu = "Title"
	self.MenuIndex = 1
	self.bCanContinue = self:AnySaveExists() -- 슬롯 1~3 중 하나라도 (HD2DMeta)
	self:SetPaused(true)
	local H = self:Hud()
	H:SetHudVisible(false)
	H:ShowTitle(true, D.Balance().TitleName, D.Balance().TitleSub, self.bCanContinue)
	H:SetTitleSelection(self.MenuIndex, self.bCanContinue, self.bCanContinue and "" or "저장된 기록이 없습니다")
	H:FadeFrom(1.0, 1.2)
	Log.Info("[HD2D] 타이틀 화면 (이어하기 " .. (self.bCanContinue and "가능" or "없음") .. ")")
end

function Menu:TitleConfirm()
	local H = self:Hud()
	if self.MenuIndex == 2 then
		if not self.bCanContinue then
			Audio.PlayOneShot(self.Sounds.Error)
			return
		end
		Audio.PlayOneShot(self.Sounds.Confirm)
		self:OpenSlots("Load", "Title") -- 슬롯 고르기 → ContinueFromSlot (HD2DPause)
		return
	end
	-- 처음부터: 시작 연출 (해설 + 주인공 대사, 카메라는 제자리)
	Audio.PlayOneShot(self.Sounds.Confirm)
	self.Menu = nil
	H:ShowTitle(false)
	H:SetHudVisible(true)
	H:FadeFrom(1.0, 1.0)
	self.Mode = "Intro"
	self.Report.NewGames = (self.Report.NewGames or 0) + 1
	self:StartDialog("", D.Balance().IntroLines, function()
		self.Mode = "Play"
		self:Hud():Announce(D.Balance().TitleName, "광장의 촌장에게 말을 걸어 보자", 3.0)
		Log.Info("[HD2D] 새 게임 시작")
	end)
end

-- ================================================================ 인벤토리 · 상점
-- 목록 줄 (아이템/서브 퀘스트 id)
function Menu:BuildRows()
	local Rows = {}
	local function AddOwned(Order)
		for _, Id in ipairs(Order) do
			if self:Count(Id) > 0 then Rows[#Rows + 1] = Id end
		end
	end
	if self.Menu == "Shop" then
		for _, Id in ipairs(self:ShopStockList()) do Rows[#Rows + 1] = Id end
	elseif self.MenuTab == 2 then
		AddOwned(D.WeaponOrder)
		AddOwned(D.ArmorOrder)
		AddOwned(D.AccessoryOrder)
	elseif self.MenuTab == 3 then
		Rows[1] = "#Main"
		for _, Id in ipairs(D.SubQuestOrder) do
			if self.Sub[Id] then Rows[#Rows + 1] = "#" .. Id end
		end
	else
		AddOwned(D.ConsumableOrder)
		AddOwned(D.MaterialOrder)
	end
	return Rows
end

function Menu:OpenInventory()
	self.Menu = "Inventory"
	self.MenuIndex = 1
	self.MenuTab = self.MenuTab or 1
	self.MenuNote = nil
	self:SetPaused(true)
	self.Report.InventoryOpened = self.Report.InventoryOpened + 1
	self:Hud():ShowPrompt(nil)
	self:Hud():ShowMenu("Inv")
	self:RefreshMenu()
	Audio.PlayOneShot(self.Sounds.Open)
end

function Menu:OpenShop()
	self.Menu = "Shop"
	self.MenuIndex = 1
	self.MenuNote = nil
	self.ShopSay = self:ShopLine(1)
	self:SetPaused(true)
	self:Hud():ShowMenu("Shop")
	self:ApplyShopLook()
	self:RefreshMenu()
	Audio.PlayOneShot(self.Sounds.Open)
end

function Menu:SelectedId()
	local Rows = self:BuildRows()
	return Rows[self.MenuIndex], Rows
end

local KindNames = { Slash = "베기", Thrust = "찌르기", Arrow = "활", Bolt = "마법" }
local Green, Red, White = { 0.55, 1.0, 0.55, 1 }, { 1.0, 0.5, 0.45, 1 }, { 0.9, 0.88, 0.84, 1 }

-- 장비 비교 줄: 지금 낀 것 → 고른 것 (↑ 초록 / ↓ 빨강)
function Menu:CompareLines(Row, Id)
	local Lines = {}
	local Player = self:GetPlayer()
	local function Add(Label, Old, New, Fmt)
		if math.abs(New - Old) < 1e-4 then
			Lines[#Lines + 1] = { Text = string.format("%s  " .. Fmt, Label, New), Color = White }
		else
			local Up = New > Old
			Lines[#Lines + 1] = { Text = string.format("%s  " .. Fmt .. "  →  " .. Fmt .. "  %s", Label, Old, New, Up and "▲" or "▼"), Color = Up and Green or Red }
		end
	end
	if Row.Kind == "Weapon" then
		local Cur, New = self:GetWeaponById(self.Equipped), self:GetWeaponById(Row.Weapon) -- 강화 포함 (HD2DMeta)
		Add("공격력", Cur.Damage, New.Damage, "%.0f")
		Add("사거리", Cur.Range, New.Range, "%.0f")
		Add("공격 속도", 1.0 / (Cur.AttackTime + Cur.Cooldown * 0.25), 1.0 / (New.AttackTime + New.Cooldown * 0.25), "%.1f")
	else
		local Slot = self:GearSlot(Row)
		local Cur = self:GearStats()
		local Swap = self:GearStats({ Slot = Slot, Id = (self[Slot] == Id and Slot == "Accessory") and nil or Id })
		local Base = Player and Player:BaseMaxHealth() or 0
		Add("방어력", Cur.Defense, Swap.Defense, "%.0f")
		Add("최대 HP", Base + Cur.HealthBonus, Base + Swap.HealthBonus, "%.0f")
		Add("이동 속도", 100 + Cur.SpeedBonus * 100, 100 + Swap.SpeedBonus * 100, "%.0f%%")
		Add("치명타", (D.Balance().CritChance + Cur.CritBonus) * 100, (D.Balance().CritChance + Swap.CritBonus) * 100, "%.0f%%")
	end
	return Lines
end

function Menu:RefreshMenu()
	local Rows = self:BuildRows()
	if #Rows == 0 then self.MenuIndex = 1 else self.MenuIndex = math.max(1, math.min(self.MenuIndex, #Rows)) end
	local Player = self:GetPlayer()
	local Prefix = self.Menu == "Shop" and "Shop" or "Inv"
	local H = self:Hud()
	local Lines = {}
	for I, Id in ipairs(Rows) do
		if string.sub(Id, 1, 1) == "#" then
			local QId = string.sub(Id, 2)
			if QId == "Main" then
				Lines[I] = { Icon = "UI/Demo/HD2D/Icons/Sword.png", Name = "★ " .. D.Quest(self.QuestStage).Title,
				             Right = self.QuestStage >= D.FinalQuestStage() and "완료" or "진행 중" }
			else
				local Q = D.SubQuest(QId)
				local Giver = D.Npc(Q.Giver)
				Lines[I] = { Icon = Giver.Portrait, Name = Q.Title, Right = self.Sub[QId].State == "Done" and "완료" or (self:SubReady(QId) and "보고 가능" or "진행 중"),
				             bDim = self.Sub[QId].State == "Done" }
			end
		else
			local Row = D.Item(Id)
			local Right
			if self.Menu == "Shop" then
				Right = (self:IsGear(Row) and self:Count(Id) > 0) and "보유" or string.format("%d G", Row.Price)
			elseif self:IsGear(Row) then
				Right = self:IsEquipped(Id) and "E" or ""
			else
				Right = string.format("×%d", self:Count(Id))
			end
			Lines[I] = { Icon = Row.Icon, Name = self:ItemDisplayName(Id), Right = Right, bDim = self.Menu == "Shop" and Row.Price > self.Gold and Right ~= "보유" }
		end
	end
	local Status = Player and string.format("Lv %d   HP %d/%d   MP %d/%d   방어 %.0f   BP %d", Player.Level, math.ceil(Player.Health), Player.MaxHealth,
		math.floor(Player.Mana), Player.MaxMana, Player.Defense or 0, Player.BP or 0) or ""
	H:SetMenuRows(Prefix, Lines, self.MenuIndex)
	H:SetMenuHeader(Prefix, Status, self.Gold)
	if Prefix == "Inv" then H:SetMenuTabs(Prefix, self.MenuTab) else H:SetShopSay(self.ShopSay or "") end
	local Id = Rows[self.MenuIndex]
	local Compare = {}
	if Id and string.sub(Id, 1, 1) == "#" then
		local QId = string.sub(Id, 2)
		if QId == "Main" then
			local Q = D.Quest(self.QuestStage)
			H:SetMenuDetail(Prefix, "UI/Demo/HD2D/Icons/Sword.png", Q.Title, "메인 퀘스트 · 촌장 바르톨로", "목표: " .. self:MainObjective(),
				self.QuestStage >= D.FinalQuestStage() and "완료했다" or string.format("단계 %d / %d", self.QuestStage, D.FinalQuestStage()))
		else
			local Q = D.SubQuest(QId)
			local Giver = D.Npc(Q.Giver)
			local Reward = {}
			if Q.RewardItem ~= "" then Reward[#Reward + 1] = D.Item(Q.RewardItem).DisplayName end
			if Q.RewardGold > 0 then Reward[#Reward + 1] = Q.RewardGold .. " 골드" end
			H:SetMenuDetail(Prefix, Giver.Portrait, Q.Title, "서브 퀘스트 · " .. Giver.DisplayName, Q.Summary,
				self:SubProgressText(QId) .. "\n보상: " .. table.concat(Reward, ", "))
		end
	elseif Id then
		local Row = D.Item(Id)
		local Type, Desc, Stats = "", Row.Description, ""
		if Row.Kind == "Weapon" then
			local W = D.Weapon(Row.Weapon)
			Type = "무기 · " .. KindNames[W.Kind]
			Desc = W.Description
			if W.ManaCost > 0 then Stats = string.format("한 번에 마나 %d", W.ManaCost) end
		elseif Row.Kind == "Armor" or Row.Kind == "Accessory" then
			Type = Row.Kind == "Armor" and "방어구" or "장신구"
		elseif Row.Kind == "Material" then
			Type = self:MaterialKindName(Id) -- 대장간 재료 / 귀중품 (HD2DMeta)
			Stats = string.format("소지 %d개", self:Count(Id))
		else
			Type = ({ Heal = "회복 아이템", Mana = "마나 회복 아이템", Elixir = "귀한 회복 아이템" })[Row.Kind] or ""
			Stats = string.format("소지 %d개", self:Count(Id))
		end
		if self:IsGear(Row) then
			Stats = self:IsEquipped(Id) and "지금 장비하고 있다" or (Stats ~= "" and Stats or "장비하면:")
			Compare = self:CompareLines(Row, Id)
		end
		if self.Menu == "Shop" then Stats = Stats .. (Stats ~= "" and "\n" or "") .. string.format("가격 %d 골드 (소지 %d)", Row.Price, self:Count(Id)) end
		if self.MenuNote then Stats = Stats .. "\n" .. self.MenuNote end
		H:SetMenuDetail(Prefix, Row.Icon, Row.DisplayName, Type, Desc, Stats)
	else
		H:SetMenuDetail(Prefix, nil, "", "", self.MenuTab == 2 and "장비가 없다" or "소지품이 없다", "")
	end
	H:SetMenuCompare(Prefix, Compare)
end

function Menu:MenuConfirm()
	local Id = self:SelectedId()
	if not Id or string.sub(Id, 1, 1) == "#" then return end
	local Row = D.Item(Id)
	self.MenuNote = nil
	if self.Menu == "Shop" then
		if self:IsGear(Row) and self:Count(Id) > 0 then
			self.ShopSay = self:ShopLine(4)
			Audio.PlayOneShot(self.Sounds.Error)
		elseif self.Gold < Row.Price then
			self.ShopSay = self:ShopLine(3)
			Audio.PlayOneShot(self.Sounds.Error)
		else
			self.Gold = self.Gold - Row.Price
			self:AddItem(Id, 1, true)
			self.Report.Bought[Id] = (self.Report.Bought[Id] or 0) + 1
			self.ShopSay = self:ShopLine(2)
			Audio.PlayOneShot(self.Sounds.Buy)
			Log.Info(string.format("[HD2D] 구입: %s (%d G, 남은 골드 %d)", Row.DisplayName, Row.Price, self.Gold))
		end
	else
		if self:IsGear(Row) then
			if not self:Equip(Id, true) then Audio.PlayOneShot(self.Sounds.Error) end
		elseif Row.Kind == "Material" then
			Audio.PlayOneShot(self.Sounds.Error)
			self.MenuNote = "지금은 쓸 수 없다"
		else
			if self:UseItem(Id) then self.MenuNote = Row.DisplayName .. "을(를) 사용했다" end
		end
	end
	self:RefreshMenu()
end

-- ================================================================ 엔딩 · 크레딧
local EndingPageTime = 4.5 -- 쪽마다 (실제 시간 — 확인 버튼으로 넘김)

function Menu:OpenEnding()
	if self.Menu ~= nil then
		-- 다른 창(대화 등)이 열려 있으면 닫힐 때까지 미룬다
		Timer.After(0.5, function() self:OpenEnding() end, { Unscaled = true })
		return
	end
	self.Menu = "Ending"
	self.Mode = "Ending"
	self.Ending = { Page = 1, Time = 0.0 }
	self:SetPaused(true)
	local H = self:Hud()
	H:ShowPrompt(nil)
	H:SetHudVisible(false)
	H:ShowEnding(true)
	H:FadeFrom(1.0, 1.0)
	self:ShowEndingPage()
	self.Report.Endings = self.Report.Endings + 1
	Log.Info("[HD2D] 엔딩·크레딧 시작")
end

function Menu:ShowEndingPage()
	local Pages = D.Balance().EndingPages or {}
	local Text = Pages[self.Ending.Page] or ""
	local Title, Body = string.match(Text, "^([^|]*)|(.*)$")
	self:Hud():SetEndingPage(Title or "", Body or Text, 0.0)
end

function Menu:UpdateEnding(UDt, In)
	local E = self.Ending
	local Pages = D.Balance().EndingPages or {}
	E.Time = E.Time + UDt
	-- 쪽 페이드: 0.6초 들어오고 마지막 0.6초 나간다
	local Alpha = math.max(0.0, math.min(1.0, E.Time / 0.6, (EndingPageTime - E.Time) / 0.6))
	self:Hud():SetEndingAlpha(Alpha)
	if (In.Confirm and E.Time > 0.4) or E.Time >= EndingPageTime then
		if E.Page >= #Pages then
			self:CloseEnding()
			return
		end
		E.Page, E.Time = E.Page + 1, 0.0
		Audio.PlayOneShot(self.Sounds.Move)
		self:ShowEndingPage()
	end
end

function Menu:CloseEnding()
	self.Menu = nil
	self.Mode = "Play"
	self.Ending = nil
	self:SetPaused(false)
	local H = self:Hud()
	H:ShowEnding(false)
	H:SetHudVisible(true)
	H:FadeFrom(1.0, 1.0)
	H:Announce("THE END", "고맙습니다! 들판과 유적을 자유롭게 돌아다닐 수 있다", 3.5)
	Log.Info("[HD2D] 엔딩·크레딧 끝 → 자유 탐험")
end

function Menu:MenuInput(In)
	local UDt = Time.GetUnscaledDelta()
	if self:MetaMenuInput(UDt, In) then return end -- 일시정지·슬롯·확인·대장간 (HD2DPause/HD2DCrafting)
	if self.Menu == "Travel" then
		self:UpdateTravel(UDt)
		return
	end
	if self.Menu == "Ending" then
		self:UpdateEnding(UDt, In)
		return
	end
	if self.Menu == "Dialog" then
		self:UpdateDialog(UDt, In)
		return
	end
	if self.Menu == "Title" then
		if In.MenuUp or In.MenuDown then
			self.MenuIndex = self.MenuIndex == 1 and 2 or 1
			Audio.PlayOneShot(self.Sounds.Move)
			self:Hud():SetTitleSelection(self.MenuIndex, self.bCanContinue, self.bCanContinue and "" or "저장된 기록이 없습니다")
		elseif In.Confirm then
			self:TitleConfirm()
		end
		return
	end
	if In.Cancel or In.Inventory then
		local bShop = self.Menu == "Shop"
		self:CloseMenu()
		if bShop then
			local Keeper, Portrait = self:ShopKeeperName()
			self:Hud():Toast(Portrait, Keeper .. ": " .. self:ShopLine(5))
		end
		return
	end
	if self.Menu == "Inventory" and (In.MenuLeft or In.MenuRight) then
		self.MenuTab = (self.MenuTab - 1 + (In.MenuRight and 1 or -1)) % 3 + 1
		self.MenuIndex = 1
		self.MenuNote = nil
		self.Report.TabSwitches = (self.Report.TabSwitches or 0) + 1
		Audio.PlayOneShot(self.Sounds.Move)
		self:RefreshMenu()
		return
	end
	local Rows = self:BuildRows()
	if In.MenuUp and #Rows > 0 then
		self.MenuIndex = (self.MenuIndex - 2) % #Rows + 1
		self.MenuNote = nil
		Audio.PlayOneShot(self.Sounds.Move)
		self:RefreshMenu()
	elseif In.MenuDown and #Rows > 0 then
		self.MenuIndex = self.MenuIndex % #Rows + 1
		self.MenuNote = nil
		Audio.PlayOneShot(self.Sounds.Move)
		self:RefreshMenu()
	elseif In.Confirm then
		self:MenuConfirm()
	end
end

-- UI 단추 (마우스)
function Menu:OnMenuRowClicked(Index)
	if self.Menu == "Title" then
		self.MenuIndex = Index
		self:TitleConfirm()
		return
	end
	if self.Menu ~= "Inventory" and self.Menu ~= "Shop" then return end
	self.MenuIndex = Index
	self:MenuConfirm()
end

function Menu:OnMenuRowHovered(Index)
	if self.Menu == "Title" and self.MenuIndex ~= Index then
		self.MenuIndex = Index
		self:Hud():SetTitleSelection(self.MenuIndex, self.bCanContinue, self.bCanContinue and "" or "저장된 기록이 없습니다")
	elseif (self.Menu == "Inventory" or self.Menu == "Shop") and self.MenuIndex ~= Index and Index <= #self:BuildRows() then
		self.MenuIndex = Index
		self.MenuNote = nil
		self:RefreshMenu()
	end
end

return Menu
