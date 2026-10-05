-- FarmBie 소지품·핫바 (FarmGame에 섞이는 메서드 모음).
--   물건 열쇠(문자열): 도구·비료 = Items.etable 행 이름 ("Hoe", "Can", "FertBasic" …), 씨앗 = "Seed:<작물>:<희귀도 0~3>", 작물 = "Crop:<작물>:<희귀도>"
--   칸 self.Bag[1..BagSize] = { Key, Count } | nil. 앞 9칸이 핫바, self.Selected = 고른 칸 번호(1~9)
--   이름·아이콘·종류는 ItemInfo(열쇠)로 (표에서 만들어 캐시). 얻을 때 Give → HUD 알림, 쓰면 Take
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Inv = {}

local BagSize = 36
local IconDir = "UI/FarmBie/Icons/"

function Inv:InitInventory()
	self.Bag = {}
	self.Selected = 1
	self.Water = 0
	self.ItemCache = {}
end

function Inv:GiveStartItems()
	for _, Entry in ipairs(D.Values("Farming.edata").StartItems) do
		local Key, Count = string.match(Entry, "^(.-)%*(%d+)$")
		self:Give(Key or Entry, tonumber(Count) or 1, true)
	end
	self.Water = D.Values("Farming.edata").CanCapacity
end

-- 열쇠 → { Name, Icon, Kind("Tool"|"Fertilizer"|"Seed"|"Crop"…), Crop(행), Rarity, Price }
function Inv:ItemInfo(Key)
	local Info = self.ItemCache[Key]
	if Info then return Info end
	local Kind, CropId, R = string.match(Key, "^(%a+):(%w+):(%d)$")
	if Kind then
		local Crop = D.ByName("Crops.etable")[CropId]
		local Rarity = D.Rows("Rarities.etable")[tonumber(R) + 1]
		if not Crop or not Rarity then
			Log.Error("[FarmBie] 모르는 물건: " .. Key)
			return { Name = Key, Icon = "", Kind = "Unknown", Price = 0 }
		end
		local Suffix = tonumber(R) > 0 and (" (" .. Rarity.DisplayName .. ")") or ""
		Info = { Kind = Kind, Crop = Crop, CropId = CropId, Rarity = tonumber(R), RarityRow = Rarity,
		         Name = Crop.DisplayName .. (Kind == "Seed" and " 씨앗" or "") .. Suffix,
		         Icon = IconDir .. Kind .. "_" .. CropId .. "_" .. R .. ".png",
		         Price = math.floor((Kind == "Seed" and Crop.SeedPrice * 0.5 or Crop.Price) * Rarity.PriceMul + 0.5) }
	else
		local Row = D.ByName("Items.etable")[Key]
		if not Row then
			Log.Error("[FarmBie] 모르는 물건: " .. Key)
			return { Name = Key, Icon = "", Kind = "Unknown", Price = 0 }
		end
		Info = { Kind = Row.Kind, Name = Row.DisplayName, Icon = IconDir .. Key .. ".png", Price = Row.Price, Row = Row }
	end
	Info.Key = Key
	self.ItemCache[Key] = Info
	return Info
end

function Inv:CountItem(Key)
	local N = 0
	for I = 1, BagSize do
		local S = self.Bag[I]
		if S and S.Key == Key then N = N + S.Count end
	end
	return N
end

-- 얻기: 같은 열쇠 칸에 더하고, 없으면 빈 칸(핫바 먼저). 다 못 넣으면 넣은 수를 돌려준다
function Inv:Give(Key, Count, bQuiet)
	Count = Count or 1
	local Left = Count
	for I = 1, BagSize do
		local S = self.Bag[I]
		if S and S.Key == Key then
			S.Count = S.Count + Left
			Left = 0
			break
		end
	end
	if Left > 0 then
		for I = 1, BagSize do
			if not self.Bag[I] then
				self.Bag[I] = { Key = Key, Count = Left }
				Left = 0
				break
			end
		end
	end
	local Given = Count - Left
	if Given > 0 and not bQuiet then
		local Info = self:ItemInfo(Key)
		self:Hud():Toast(Info.Icon, string.format("+%d %s", Given, Info.Name), Info.RarityRow and Info.RarityRow.Color)
	end
	if Left > 0 then self:Hud():Toast("", "소지품이 가득 찼다", { 1.0, 0.5, 0.45, 1.0 }) end
	self.Report.Gained = (self.Report.Gained or 0) + Given
	return Given
end

function Inv:Take(Key, Count)
	Count = Count or 1
	if self:CountItem(Key) < Count then return false end
	for I = BagSize, 1, -1 do
		local S = self.Bag[I]
		if S and S.Key == Key then
			local N = math.min(S.Count, Count)
			S.Count = S.Count - N
			Count = Count - N
			if S.Count <= 0 then self.Bag[I] = nil end
			if Count <= 0 then break end
		end
	end
	return true
end

function Inv:SelectedItem()
	return self.Bag[self.Selected]
end

function Inv:SelectSlot(Index)
	Index = ((Index - 1) % 9) + 1
	if Index ~= self.Selected then
		self.Selected = Index
		local S = self.Bag[Index]
		self:Hud():ShowItemName(S and self:ItemInfo(S.Key).Name or "")
	end
end

-- 저장 조각
function Inv:SaveInventory(T)
	local Bag = {}
	for I = 1, BagSize do
		local S = self.Bag[I]
		if S then Bag[#Bag + 1] = { Slot = I, Key = S.Key, Count = S.Count } end
	end
	T.Bag, T.Selected, T.Water = Bag, self.Selected, self.Water
end

function Inv:LoadInventory(T)
	self.Bag = {}
	for _, S in ipairs(T.Bag or {}) do
		self.Bag[math.floor(S.Slot)] = { Key = S.Key, Count = math.floor(S.Count) }
	end
	self.Selected = math.floor(T.Selected or 1)
	self.Water = math.floor(T.Water or 0)
end

Inv.BagSize = BagSize

return Inv
