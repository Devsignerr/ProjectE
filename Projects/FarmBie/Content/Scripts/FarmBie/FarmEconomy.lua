-- FarmBie 경제 (FarmGame에 섞이는 메서드 모음): 돈·출하 상자·보부상.
--   출하: 상자에 상호작용 → 고른 칸이 작물이면 그 묶음, 아니면 소지품의 작물 전부를 넣는다. 다음 날 아침 값(작물 값 × 희귀도 배율)을 받는다
--   보부상: 1명, 정해진 요일(Calendar.MerchantDays)에만 MerchantOpenHour~MerchantCloseHour 동안 남쪽 천막 앞. 방문마다 재고를 새로 꾸린다
--           (MerchantStock.etable: 이번 계절 Always 물건 전부 + 나머지에서 가중치로 RandomStockPicks개, 물건마다 한정 수량). 돈으로만 산다
--   재고·밀린 출하는 저장된다 (방문 날 저장/불러오기로 재고를 되살리지 못하게)
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Eco = {}

local SeasonIds = { "Spring", "Summer", "Autumn", "Winter" }
local function Parse2(Text)
	local X, Y = string.match(Text, "([-%d%.]+),([-%d%.]+)")
	return Vector3(tonumber(X), tonumber(Y), 0)
end

function Eco:InitEconomy()
	local E = D.Values("Economy.edata")
	self.Economy = E
	self.Gold = 0
	self.Shipped = {}      -- 오늘 상자에 넣은 { Key, Count }
	self.Stock = {}        -- 이번 방문 재고 { Key, Price, Stock }
	self.StockDay = -1     -- 재고를 꾸린 날 (누적 일수)
	self.ShipSpot = Parse2(self.Properties.ShipSpot)
	self.MerchantSpot = Parse2(self.Properties.MerchantSpot)
	local M = Scene.Find("Merchant")
	self.MerchantSprites = {}
	if M then
		for _, Name in ipairs({ "MerchantBody", "MerchantShadow" }) do
			local Child = M:FindChild(Name)
			if Child then self.MerchantSprites[#self.MerchantSprites + 1] = Child:GetComponent("SpriteComponent") end
		end
	end
	self:AddInteractable({ Pos = self.ShipSpot, Radius = 230, Prompt = function()
		local Keys, Value = self:ShipCandidates()
		if #Keys == 0 then return nil end
		local Info = self:ItemInfo(Keys[1].Key)
		if #Keys == 1 then return string.format("E  출하: %s ×%d  (+%d)", Info.Name, Keys[1].Count, Value) end
		return string.format("E  출하: 작물 전부 %d종  (+%d)", #Keys, Value)
	end, Act = function() self:ShipItems() end })
	self:AddInteractable({ Pos = self.MerchantSpot, Radius = 230, Prompt = function()
		if self:IsMerchantHere() then return "E  보부상과 거래" end
		return nil
	end, Act = function() self:OpenShop() end })
	self.bMerchantShown = nil
end

function Eco:GiveStartGold()
	self.Gold = self.Economy.StartGold
end

function Eco:AddGold(Amount)
	self.Gold = math.max(0, self.Gold + Amount)
end

-- ---- 출하
function Eco:ShipCandidates()
	local List, Value = {}, 0
	local Sel = self:SelectedItem()
	if Sel and self:ItemInfo(Sel.Key).Kind == "Crop" then
		List[1] = { Key = Sel.Key, Count = Sel.Count }
	else
		for I = 1, self.BagSize do
			local S = self.Bag[I]
			if S and self:ItemInfo(S.Key).Kind == "Crop" then List[#List + 1] = { Key = S.Key, Count = S.Count } end
		end
	end
	for _, E in ipairs(List) do Value = Value + self:ItemInfo(E.Key).Price * E.Count end
	return List, Value
end

function Eco:ShipItems()
	local List, Value = self:ShipCandidates()
	for _, E in ipairs(List) do
		self:Take(E.Key, E.Count)
		self.Shipped[#self.Shipped + 1] = { Key = E.Key, Count = E.Count }
	end
	self.Report.Shipped = (self.Report.Shipped or 0) + Value
	self:Hud():Toast("UI/FarmBie/Coin.png", string.format("출하 상자에 넣었다 (내일 아침 +%d)", Value), { 1.0, 0.86, 0.45, 1.0 })
end

function Eco:PendingShipValue()
	local Value = 0
	for _, E in ipairs(self.Shipped) do Value = Value + self:ItemInfo(E.Key).Price * E.Count end
	return Value
end

-- 아침 정산 (FarmGame:OnDayStart) → 받은 돈
function Eco:SettleShipping()
	local Value = self:PendingShipValue()
	self.Shipped = {}
	if Value > 0 then
		self:AddGold(Value)
		self.Report.Income = (self.Report.Income or 0) + Value
	end
	return Value
end

-- ---- 보부상
function Eco:IsMerchantDay()
	local W = self:Weekday()
	for _, Day in ipairs(self.Calendar.MerchantDays) do
		if Day == W then return true end
	end
	return false
end

function Eco:IsMerchantHere()
	local E = self.Economy
	return self.Phase == "Day" and self:IsMerchantDay() and self.Hour >= E.MerchantOpenHour and self.Hour < E.MerchantCloseHour
end

-- 재고 꾸리기 (방문 날마다 한 번, 날짜로 정해지는 난수 — 같은 날은 같은 재고)
function Eco:Restock()
	local Season = SeasonIds[self.Season + 1]
	local Always, Pool, Total = {}, {}, 0
	for _, Row in ipairs(D.Rows("MerchantStock.etable")) do
		if Row.Season == "Any" or Row.Season == Season then
			if Row.Always then
				Always[#Always + 1] = Row
			elseif Row.Weight > 0 then
				Pool[#Pool + 1] = Row
				Total = Total + Row.Weight
			end
		end
	end
	local State = (self:TotalDays() * 7919 + 17) % 2147483648
	local function Rand()
		State = (State * 1103515245 + 12345) % 2147483648
		return State / 2147483648
	end
	local Picked = {}
	for _ = 1, math.min(self.Economy.RandomStockPicks, #Pool) do
		local Roll, Acc = Rand() * Total, 0
		for I, Row in ipairs(Pool) do
			Acc = Acc + Row.Weight
			if Roll < Acc then
				Picked[#Picked + 1] = Row
				Total = Total - Row.Weight
				table.remove(Pool, I)
				break
			end
		end
	end
	self.Stock = {}
	for _, List in ipairs({ Always, Picked }) do
		for _, Row in ipairs(List) do
			local Price = Row.Price > 0 and Row.Price or self:ItemInfo(Row.Key).Price
			self.Stock[#self.Stock + 1] = { Key = Row.Key, Price = Price, Stock = Row.Stock }
		end
	end
	self.StockDay = self:TotalDays()
	self.Report.Restocks = (self.Report.Restocks or 0) + 1
end

function Eco:UpdateMerchant()
	local bHere = self:IsMerchantHere()
	if bHere and self.StockDay ~= self:TotalDays() then self:Restock() end
	if bHere ~= self.bMerchantShown then
		self.bMerchantShown = bHere
		for _, S in ipairs(self.MerchantSprites) do S.Visible = bHere end
		if not bHere and self.Menu == "Shop" then self:CloseMenu() end
	end
end

-- 사기 (재고 번호) → 성공 여부, 글
function Eco:Buy(Index)
	local Entry = self.Stock[Index]
	if not Entry then return false, "" end
	if Entry.Stock <= 0 then return false, "다 팔렸다" end
	if self.Gold < Entry.Price then return false, "돈이 모자라다" end
	if self:Give(Entry.Key, 1, true) < 1 then return false, "소지품이 가득 찼다" end
	self.Gold = self.Gold - Entry.Price
	Entry.Stock = Entry.Stock - 1
	self.Report.Bought = (self.Report.Bought or 0) + 1
	self.Report.Spent = (self.Report.Spent or 0) + Entry.Price
	return true, string.format("%s을(를) 샀다", self:ItemInfo(Entry.Key).Name)
end

-- ---- 저장 조각
function Eco:SaveEconomy(T)
	T.Gold = self.Gold
	T.Shipped = self.Shipped
	T.Stock = self.Stock
	T.StockDay = self.StockDay
end

function Eco:LoadEconomy(T)
	self.Gold = math.floor(T.Gold or 0)
	self.Shipped = {}
	for _, E in ipairs(T.Shipped or {}) do self.Shipped[#self.Shipped + 1] = { Key = E.Key, Count = math.floor(E.Count) } end
	self.Stock = {}
	for _, E in ipairs(T.Stock or {}) do
		self.Stock[#self.Stock + 1] = { Key = E.Key, Price = math.floor(E.Price), Stock = math.floor(E.Stock) }
	end
	self.StockDay = math.floor(T.StockDay or -1)
	self.bMerchantShown = nil
end

return Eco
