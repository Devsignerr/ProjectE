-- FarmBie 창 (FarmGame에 섞이는 메서드 모음): 상점(보부상)·소지품. 창이 열리면 게임 시간을 멈추고(Game.SetTimeScale 0) 입력을 여기서 받는다.
--   입력 표(플레이어가 만든다): MenuUp/MenuDown/MenuLeft/MenuRight(누른 순간 + 누르고 있으면 반복), Confirm(E/도구), Cancel(Esc/구르기), Inventory(I)
--   위젯 이름은 Tools/FarmBieUI.py(ShopWindow/BagWindow)와 약속이다.
local O = Script.Require("Scripts/FarmBie/FarmOptions.lua")

local Menu = {}

local ShopRows = 9

function Menu:IsMenuOpen()
	return self.Menu ~= nil
end

function Menu:OpenMenu(Name)
	self.Menu = Name
	self.MenuIndex = 1
	self.MenuOffset = 0
	self.HeldSlot = nil
	Game.SetTimeScale(0.0)
	if self.CursorSprite then self.CursorSprite.Visible = false end
	local Hud = self:Hud()
	Hud:Show("ShopWindow", Name == "Shop" or Name == "Craft")
	Hud:Show("BagWindow", Name == "Bag")
	Hud:ShowPrompt(nil)
	self:Sfx("Open", 0.7)
	self:RefreshMenu()
end

function Menu:CloseMenu()
	local Hud = self:Hud()
	Hud:Show("ShopWindow", false)
	Hud:Show("BagWindow", false)
	O.Close(self, Hud)
	self.Menu = nil
	self.HeldSlot = nil
	self:Sfx("Close", 0.7)
	Game.SetTimeScale(1.0)
end

-- ---- 일시정지 (Esc): 계속하기 · 설정 · 타이틀로 · 게임 끝내기 — 저장은 잠잘 때만이므로 나갈 때 경고
function Menu:OpenPause()
	self:OpenMenu("Pause")
	O.Open(self, self:Hud(), self:PausePage())
	self.Report.Paused = (self.Report.Paused or 0) + 1
end

function Menu:PausePage()
	local Hud = self:Hud()
	local function Back(Index) return function() O.Open(self, Hud, self:PausePage(), Index) end end
	local Lost = "오늘 아침 이후의 진행은 저장되지 않는다"
	return { Title = "일시정지", Sub = string.format("%d년차 %s %s · 저장은 잠잘 때", self.Year, self:DateText(), self:ClockText()),
		Back = function() self:CloseMenu() end, Items = {
		{ Label = "계속하기", Act = function() self:CloseMenu() end },
		{ Label = "설정", Act = function()
			O.Open(self, Hud, O.SettingsPage(self.Settings, function(S) self:ApplySettings(S) end, Back(2)))
		end },
		{ Label = "타이틀로", Act = function() O.Open(self, Hud, O.ConfirmPage("타이틀로 갈까?", Lost, function() self:GoToTitle() end, Back(3))) end },
		{ Label = "게임 끝내기", Act = function() O.Open(self, Hud, O.ConfirmPage("게임을 끝낼까?", Lost, function() Game.Quit() end, Back(4))) end },
	} }
end

function Menu:GoToTitle()
	if self.bLeaving then return end
	self.bLeaving = true
	self:CloseMenu()
	Game.SetTimeScale(1.0)
	Game.SetPersistent("FarmBie_Session", nil)
	Log.Info("[FarmBie] 타이틀로")
	Game.OpenScene("Scenes/Title.escene")
end

function Menu:OpenShop()
	self:OpenMenu("Shop")
	self:Hud():Set("ShopTitle", "Text", "떠돌이 보부상")
	self:Hud():Set("ShopHint", "Text", "W/S 고르기   E 사기   Esc 닫기")
	self:Hud():Set("ShopPortrait", "Texture", "UI/FarmBie/Peddler.png")
	local Lines = self.Economy.MerchantLines
	local Say = Lines and #Lines > 0 and Lines[(self:TotalDays() % #Lines) + 1] or ""
	self:Hud():Set("ShopSay", "Text", Say)
	self.Report.ShopOpened = (self.Report.ShopOpened or 0) + 1
end

function Menu:OpenBag()
	self:OpenMenu("Bag")
	self.Report.BagOpened = (self.Report.BagOpened or 0) + 1
end

function Menu:MenuInput(In)
	if self.Menu == "Pause" then
		O.Input(self, self:Hud(), In, function(Name) self:Sfx(Name, 0.7) end)
		return
	end
	if In.MenuUp or In.MenuDown or In.MenuLeft or In.MenuRight then self:Sfx("Click", 0.4) end
	if self.Menu == "Shop" then
		local Count = #self.Stock
		if In.MenuUp then self.MenuIndex = math.max(1, self.MenuIndex - 1) end
		if In.MenuDown then self.MenuIndex = math.min(math.max(1, Count), self.MenuIndex + 1) end
		if In.Confirm then
			local bOk, Text = self:Buy(self.MenuIndex)
			self:Sfx(bOk and "Buy" or "Error", 0.7)
			if Text ~= "" then self:Hud():Toast(bOk and self:ItemInfo(self.Stock[self.MenuIndex].Key).Icon or "", Text, bOk and nil or { 1.0, 0.55, 0.45, 1.0 }) end
		end
		if In.Cancel or In.Inventory then self:CloseMenu() return end
	elseif self.Menu == "Craft" then
		local Count = #self.CraftList
		if In.MenuUp then self.MenuIndex = math.max(1, self.MenuIndex - 1) end
		if In.MenuDown then self.MenuIndex = math.min(math.max(1, Count), self.MenuIndex + 1) end
		if In.Confirm then
			local R = self.CraftList[self.MenuIndex]
			local bOk, Text = self:DoCraft(R)
			self:Sfx(bOk and "Confirm" or "Error", 0.7)
			self:Hud():Toast(bOk and self:ItemInfo(R.Output).Icon or "", Text, bOk and nil or { 1.0, 0.55, 0.45, 1.0 })
		end
		if In.Cancel or In.Inventory then self:CloseMenu() return end
	elseif self.Menu == "Bag" then
		local I = self.MenuIndex
		if In.MenuLeft then I = I - 1 end
		if In.MenuRight then I = I + 1 end
		if In.MenuUp then I = I - 9 end
		if In.MenuDown then I = I + 9 end
		self.MenuIndex = math.max(1, math.min(self.BagSize, I))
		if In.Confirm then
			if self.HeldSlot then
				self.Bag[self.HeldSlot], self.Bag[self.MenuIndex] = self.Bag[self.MenuIndex], self.Bag[self.HeldSlot]
				self.HeldSlot = nil
				self.Report.BagMoves = (self.Report.BagMoves or 0) + 1
			elseif self.Bag[self.MenuIndex] then
				self.HeldSlot = self.MenuIndex
			end
		end
		if In.Cancel or In.Inventory then self:CloseMenu() return end
	end
	self:RefreshMenu()
end

-- ---- 그리기
function Menu:ShowDetail(Prefix, Key, Extra)
	local Hud = self:Hud()
	if not Key then
		Hud:Show(Prefix .. "DetailIcon", false)
		Hud:Set(Prefix .. "DetailName", "Text", "")
		Hud:Set(Prefix .. "DetailDesc", "Text", "")
		Hud:Set(Prefix .. "DetailInfo", "Text", "")
		return
	end
	local Info = self:ItemInfo(Key)
	Hud:Show(Prefix .. "DetailIcon", true)
	Hud:Set(Prefix .. "DetailIcon", "Texture", Info.Icon)
	Hud:Set(Prefix .. "DetailName", "Text", Info.Name)
	local Desc = Info.Crop and Info.Crop.Description or (Info.Row and Info.Row.Description or "")
	Hud:Set(Prefix .. "DetailDesc", "Text", Desc)
	local Lines = {}
	if Info.Kind == "Seed" then
		local C = Info.Crop
		Lines[#Lines + 1] = string.format("%s 작물 · %d일%s", self.Calendar.SeasonNames[({ Spring = 1, Summer = 2, Autumn = 3, Winter = 4 })[C.Season]],
			C.Days, C.Regrow > 0 and string.format(" · %d일마다 다시 열림", C.Regrow) or "")
		Lines[#Lines + 1] = string.format("수확물 값 %d", math.floor(C.Price * Info.RarityRow.PriceMul + 0.5))
	elseif Info.Kind == "Crop" then
		Lines[#Lines + 1] = string.format("출하 값 %d", Info.Price)
		if Info.Crop.Sanity > 0 then Lines[#Lines + 1] = string.format("먹으면 정신력 +%d", Info.Crop.Sanity) end
	end
	Lines[#Lines + 1] = string.format("가진 수 %d", self:CountItem(Key))
	if Extra then Lines[#Lines + 1] = Extra end
	Hud:Set(Prefix .. "DetailInfo", "Text", table.concat(Lines, "\n"))
end

function Menu:RefreshMenu()
	local Hud = self:Hud()
	if self.Menu == "Shop" then
		Hud:Set("ShopGold", "Text", tostring(self.Gold))
		local Count = #self.Stock
		if self.MenuIndex > self.MenuOffset + ShopRows then self.MenuOffset = self.MenuIndex - ShopRows end
		if self.MenuIndex <= self.MenuOffset then self.MenuOffset = self.MenuIndex - 1 end
		for Row = 0, ShopRows - 1 do
			local I = self.MenuOffset + Row + 1
			local E = self.Stock[I]
			Hud:Show("ShopRow" .. Row, E ~= nil)
			Hud:Show("ShopSel" .. Row, I == self.MenuIndex)
			if E then
				local Info = self:ItemInfo(E.Key)
				Hud:Set("ShopIcon" .. Row, "Texture", Info.Icon)
				Hud:Set("ShopName" .. Row, "Text", Info.Name)
				Hud:Set("ShopPrice" .. Row, "Text", tostring(E.Price))
				Hud:Set("ShopStock" .. Row, "Text", E.Stock > 0 and ("×" .. E.Stock) or "품절")
			end
		end
		local Sel = self.Stock[self.MenuIndex]
		self:ShowDetail("Shop", Sel and Sel.Key, Sel and (self.Gold < Sel.Price and "돈이 모자라다" or nil))
		if Count == 0 then Hud:Set("ShopSay", "Text", "오늘은 팔 물건이 없네…") end
	elseif self.Menu == "Craft" then
		Hud:Set("ShopGold", "Text", tostring(self.Gold))
		local List = self.CraftList
		if self.MenuIndex > self.MenuOffset + ShopRows then self.MenuOffset = self.MenuIndex - ShopRows end
		if self.MenuIndex <= self.MenuOffset then self.MenuOffset = self.MenuIndex - 1 end
		for Row = 0, ShopRows - 1 do
			local I = self.MenuOffset + Row + 1
			local R = List[I]
			Hud:Show("ShopRow" .. Row, R ~= nil)
			Hud:Show("ShopSel" .. Row, I == self.MenuIndex)
			if R then
				local Info = self:ItemInfo(R.Output)
				Hud:Set("ShopIcon" .. Row, "Texture", Info.Icon)
				Hud:Set("ShopName" .. Row, "Text", string.format("%s ×%d", Info.Name, R.Count))
				Hud:Set("ShopPrice" .. Row, "Text", self:CanCraft(R) and "제작" or "부족")
				Hud:Set("ShopStock" .. Row, "Text", "가짐 " .. self:CountItem(R.Output))
			end
		end
		local Sel = List[self.MenuIndex]
		self:ShowDetail("Shop", Sel and Sel.Output, Sel and ("재료\n" .. self:InputsText(Sel)) or nil)
	elseif self.Menu == "Bag" then
		for I = 1, self.BagSize do
			local W = I - 1
			local S = self.Bag[I]
			local Bg = (I == self.MenuIndex or I == self.HeldSlot) and "UI/FarmBie/SlotSel.png" or "UI/FarmBie/Slot.png"
			Hud:Set("BagBg" .. W, "Texture", Bg)
			Hud:Show("BagIcon" .. W, S ~= nil)
			if S then
				local Info = self:ItemInfo(S.Key)
				Hud:Set("BagIcon" .. W, "Texture", Info.Icon)
				Hud:Set("BagCount" .. W, "Text", Info.Kind == "Tool" and "" or tostring(S.Count))
				Hud:Set("BagIcon" .. W, "Opacity", I == self.HeldSlot and 0.5 or 1.0)
			else
				Hud:Set("BagCount" .. W, "Text", "")
			end
		end
		local S = self.Bag[self.MenuIndex]
		self:ShowDetail("Bag", S and S.Key, self.HeldSlot and "옮길 자리에서 E" or nil)
	end
end

return Menu
