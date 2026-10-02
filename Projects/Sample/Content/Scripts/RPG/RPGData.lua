-- RPG 데이터 읽기 (Phase 46-C): 기획 수치는 Data/RPG/*.etable·*.edata에 있고, 스크립트는 이 모듈로 읽는다.
--   local RPGData = Script.Require("Scripts/RPG/RPGData.lua")
--   RPGData.GetItems()              → { [id] = def } (def = { Id, Name, Type, Icon, Model, ModelScale, Price, Damage, Defense, Heal, Mana,
--                                       MaxStack, bTwoHanded, bCurrency, Description } — Items.etable 행, Name = DisplayName)
--   RPGData.GetItem(id)             → def 또는 nil
--   RPGData.GetLootTable(id)        → { Id, Gold = { 최소, 최대 } 또는 nil, Drops = { { 아이템, 확률, 최소, 최대 }, ... } } 또는 nil
--                                     ("Skeleton_Warrior", "SkeletonWarrior", "skeleton warrior" 모두 같은 표 — 공백/_/- 무시, 대소문자 무시)
--   RPGData.GetEnemyStats(row)      → Enemies.etable 행 (필드 이름 = EnemyController Properties + MaxHealth) 또는 nil
--   RPGData.GetShopStock(row)       → Shops.etable 행의 아이템 id 배열 또는 nil
--   RPGData.LoadAsset(path)         → .edata 값 테이블 또는 nil (PlayerBalance/GameBalance)
--   RPGData.ApplyFields(target, row, skip) → row의 필드 중 target에 이미 있는 이름만 덮어쓴다(skip[이름]은 건너뜀). 덮어쓴 수
-- 캐시: 데이터에서 만든 읽기 전용 테이블을 Data.GetGeneration()마다 한 번 만들고 세대가 바뀌면(에디터 핫 리로드) 다시 만든다.
--   돌려준 테이블은 모든 스크립트가 공유하므로 고치지 않는다(게임 상태는 GameManager/인스턴스에 둔다).
local RPGData = {}

RPGData.ItemsTable       = "Data/RPG/Items.etable"
RPGData.LootTablesTable  = "Data/RPG/LootTables.etable"
RPGData.LootEntriesTable = "Data/RPG/LootEntries.etable"
RPGData.EnemiesTable     = "Data/RPG/Enemies.etable"
RPGData.ShopsTable       = "Data/RPG/Shops.etable"

-- 세대별 캐시 (데이터 사본만 — 게임 상태 아님)
local Cache = { Generation = nil }

local function Memo(Key, Build)
	local Generation = Data.GetGeneration()
	if Cache.Generation ~= Generation then
		Cache = { Generation = Generation }
	end
	local Value = Cache[Key]
	if Value == nil then
		Value      = Build()
		Cache[Key] = Value == nil and false or Value
	end
	return Value or nil
end

function RPGData.NormalizeId(Id)
	return (tostring(Id or ""):lower():gsub("[%s_%-]", ""))
end

function RPGData.GetItems()
	return Memo("Items", function()
		local Items = {}
		for _, Row in ipairs(Data.GetRows(RPGData.ItemsTable) or {}) do
			Row.Id          = Row.Name
			Row.Name        = Row.DisplayName
			Row.DisplayName = nil
			Items[Row.Id]   = Row
		end
		if next(Items) == nil then
			Log.Error("RPGData: 아이템 표가 비었거나 읽을 수 없습니다", RPGData.ItemsTable)
		end
		return Items
	end)
end

function RPGData.GetItem(Id)
	return RPGData.GetItems()[Id]
end

function RPGData.GetLootTable(Id)
	local Tables = Memo("Loot", function()
		local Result = {}
		for _, Row in ipairs(Data.GetRows(RPGData.LootTablesTable) or {}) do
			local Table = { Id = Row.Name, Drops = {} }
			if Row.GoldMax > 0 then
				Table.Gold = { Row.GoldMin, Row.GoldMax }
			end
			Result[RPGData.NormalizeId(Row.Name)] = Table
		end
		-- 항목은 파일 순서대로 각 표에 붙인다 (굴리는 순서 = 파일 순서)
		for _, Entry in ipairs(Data.GetRows(RPGData.LootEntriesTable) or {}) do
			local Table = Result[RPGData.NormalizeId(Entry.LootTable)]
			if Table ~= nil and Entry.Item ~= "" then
				Table.Drops[#Table.Drops + 1] = { Entry.Item, Entry.Chance, Entry.MinCount, Entry.MaxCount }
			end
		end
		return Result
	end)
	return Tables[RPGData.NormalizeId(Id)]
end

function RPGData.GetEnemyStats(RowName)
	if RowName == nil or RowName == "" then
		return nil
	end
	local Rows = Memo("Enemies", function()
		local Result = {}
		for _, Row in ipairs(Data.GetRows(RPGData.EnemiesTable) or {}) do
			Result[Row.Name] = Row
		end
		return Result
	end)
	return Rows[RowName]
end

function RPGData.GetShopStock(RowName)
	if RowName == nil or RowName == "" then
		return nil
	end
	local Shops = Memo("Shops", function()
		local Result = {}
		for _, Row in ipairs(Data.GetRows(RPGData.ShopsTable) or {}) do
			Result[Row.Name] = Row.Stock
		end
		return Result
	end)
	return Shops[RowName]
end

function RPGData.LoadAsset(Path)
	if Path == nil or Path == "" then
		return nil
	end
	return Memo("Asset:" .. Path, function()
		return Data.Load(Path)
	end)
end

function RPGData.ApplyFields(Target, Row, Skip)
	local Count = 0
	for Key, Value in pairs(Row) do
		if Key ~= "Name" and Target[Key] ~= nil and not (Skip and Skip[Key]) then
			Target[Key] = Value
			Count = Count + 1
		end
	end
	return Count
end

return RPGData
