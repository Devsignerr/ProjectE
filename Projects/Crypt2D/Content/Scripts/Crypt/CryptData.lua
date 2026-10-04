-- Crypt2D 데이터 읽기 (Script.Require("Scripts/Crypt/CryptData.lua")). 수치는 Data/Crypt/*.etable·*.edata에 있다 (BuildCrypt2D.py가 쓴다).
--   CryptData.Weapons() → { [id] = 행 }, CryptData.WeaponIds() → 파일 순서 id 배열, CryptData.Weapon(id)
--   CryptData.Enemies() → { [Kind] = 행 }, CryptData.Enemy(kind), CryptData.Balance() → Balance.edata 값
--   CryptData.ShopItems() → { [id] = 행 }, CryptData.ShopItemIds() → 파일 순서 id 배열, CryptData.ShopItem(id) (상점 방 물건)
-- 세대(Data.GetGeneration — 에디터 핫 리로드)마다 한 번 읽어 공유한다. 돌려준 테이블은 읽기 전용으로 쓴다.
local CryptData = {}

local Cache = { Generation = nil }

local function Memo(Key, Build)
	local Generation = Data.GetGeneration()
	if Cache.Generation ~= Generation then
		Cache = { Generation = Generation }
	end
	if Cache[Key] == nil then
		Cache[Key] = Build() or false
	end
	return Cache[Key] or nil
end

local function ByName(Path)
	local Map = {}
	local Order = {}
	for _, Row in ipairs(Data.GetRows(Path) or {}) do
		Map[Row.Name] = Row
		Order[#Order + 1] = Row.Name
	end
	if #Order == 0 then
		Log.Error("[Crypt2D] 데이터 표가 비었거나 읽지 못함:", Path)
	end
	return { Map = Map, Order = Order }
end

function CryptData.Weapons()
	return Memo("Weapons", function() return ByName("Data/Crypt/Weapons.etable") end).Map
end

function CryptData.WeaponIds()
	return Memo("Weapons", function() return ByName("Data/Crypt/Weapons.etable") end).Order
end

function CryptData.Weapon(Id)
	return CryptData.Weapons()[Id]
end

function CryptData.Enemies()
	return Memo("Enemies", function() return ByName("Data/Crypt/Enemies.etable") end).Map
end

function CryptData.Enemy(Kind)
	return CryptData.Enemies()[Kind]
end

function CryptData.ShopItems()
	return Memo("ShopItems", function() return ByName("Data/Crypt/ShopItems.etable") end).Map
end

function CryptData.ShopItemIds()
	return Memo("ShopItems", function() return ByName("Data/Crypt/ShopItems.etable") end).Order
end

function CryptData.ShopItem(Id)
	return CryptData.ShopItems()[Id]
end

function CryptData.Balance()
	return Memo("Balance", function()
		local Value = Data.Load("Data/Crypt/Balance.edata")
		if Value == nil then
			Log.Error("[Crypt2D] Balance.edata를 읽지 못함")
		end
		return Value
	end)
end

return CryptData
