-- HD-2D 데모 데이터 읽기 (Script.Require("Scripts/Demo/HD2D/HD2DData.lua")). 수치·대사는 Data/Demo/HD2D/*.etable·*.edata에 있다
-- (Tools/DemoMap/HD2DGameplay.py가 쓴다 — 표를 고치려면 그 스크립트를 고치고 BuildHD2D.py를 다시 실행).
--   Weapon(id) / Item(id) / Enemy(kind) / Npc(id) → 행, Quest(stage) → Quests.etable "Stage<n>" 행, Balance() → Balance.edata 값
-- 세대(Data.GetGeneration — 에디터 핫 리로드)마다 한 번 읽어 공유한다. 돌려준 테이블은 읽기 전용으로 쓴다 (모듈 값은 상태 안에서 공유된다).
local HD2DData = {}

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

local function ByName(File)
	return Memo(File, function()
		local Map = {}
		for _, Row in ipairs(Data.GetRows(Root .. File) or {}) do
			Map[Row.Name] = Row
		end
		if next(Map) == nil then
			Log.Error("[HD2D] 데이터 표가 비었거나 읽지 못함:", Root .. File)
		end
		return Map
	end)
end

function HD2DData.Weapon(Id) return ByName("Weapons.etable")[Id] end
function HD2DData.Item(Id) return ByName("Items.etable")[Id] end
function HD2DData.Enemy(Kind) return ByName("Enemies.etable")[Kind] end
function HD2DData.Npc(Id) return ByName("Npcs.etable")[Id] end
function HD2DData.Quest(Stage) return ByName("Quests.etable")["Stage" .. tostring(Stage)] end

function HD2DData.Balance()
	return Memo("Balance", function()
		local Value = Data.Load(Root .. "Balance.edata")
		if Value == nil then
			Log.Error("[HD2D] Balance.edata를 읽지 못함")
		end
		return Value
	end)
end

-- 무기 아이템 순서 (인벤토리 표시 순서) / 소모품 순서
HD2DData.WeaponOrder = { "Sword", "Spear", "Bow", "Staff" }
HD2DData.ConsumableOrder = { "Potion", "HiPotion", "Ether", "Elixir" }

return HD2DData
