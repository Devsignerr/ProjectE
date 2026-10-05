-- HD-2D 메타 시스템 데이터 읽기 (Script.Require — 상태 없음, 세대별 캐시만). 표는 Tools/DemoMap/HD2DMetaGen.py가 쓴다:
--   MaterialDrops(적 종류 → 재료·확률·개수), Upgrades(<무기>_<단계> → 공격력 보너스·골드·재료), Recipes(조합 — 표 순서 = 화면 순서),
--   Bestiary(적 종류 → 서식지·설명·그림 — 표 순서 = 도감 순서), QuestJournal(Main_<장 번호>/Sub_<id> → 일지 요약·한 줄 목표·보고 안내). 재료 아이템 자체는 Items.etable (HD2DData.Item).
--   "아이템*개수" 문자열은 ParseCost로 { { Id, N } ... }.
local MetaData = {}

local Root = "Data/Demo/HD2D/"
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

-- 표 → { Rows = 순서 목록, ByName = 이름 → 행 }
local function Table(File)
	return Memo(File, function()
		local Rows = Data.GetRows(Root .. File) or {}
		if #Rows == 0 then Log.Error("[HD2D] 메타 데이터 표가 비었거나 읽지 못함:", Root .. File) end
		local ByName = {}
		for _, Row in ipairs(Rows) do ByName[Row.Name] = Row end
		return { Rows = Rows, ByName = ByName }
	end)
end

function MetaData.Drop(Kind) return Table("MaterialDrops.etable").ByName[Kind] end
function MetaData.Upgrade(Weapon, Level) return Table("Upgrades.etable").ByName[Weapon .. "_" .. tostring(Level)] end
function MetaData.Recipes() return Table("Recipes.etable").Rows end
function MetaData.Bestiary() return Table("Bestiary.etable").Rows end
function MetaData.BestiaryEntry(Kind) return Table("Bestiary.etable").ByName[Kind] end
function MetaData.Journal(Key) return Table("QuestJournal.etable").ByName[Key] end

function MetaData.ItemRows() return Table("Items.etable").Rows end -- 아이템 표 순서 (도감 "물건" 탭)

function MetaData.Balance()
	return Memo("Meta.edata", function()
		local Value = Data.Load(Root .. "Meta.edata")
		if Value == nil then Log.Error("[HD2D] Meta.edata를 읽지 못함") end
		return Value
	end)
end

MetaData.MaxUpgrade = 3

-- "Jelly*2" 목록 → { { Id = "Jelly", N = 2 } ... }
function MetaData.ParseCost(List)
	local Out = {}
	for _, Text in ipairs(List or {}) do
		local Id, N = string.match(Text, "^(%a+)%*(%d+)$")
		if Id then Out[#Out + 1] = { Id = Id, N = tonumber(N) } end
	end
	return Out
end

return MetaData
