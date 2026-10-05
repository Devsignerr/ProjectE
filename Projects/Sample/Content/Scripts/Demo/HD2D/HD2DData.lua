-- HD-2D 데모 데이터 읽기 (Script.Require("Scripts/Demo/HD2D/HD2DData.lua")). 수치·대사는 Data/Demo/HD2D/*.etable·*.edata에 있다
-- (Tools/DemoMap/HD2DGameplay.py가 쓴다 — 표를 고치려면 그 스크립트를 고치고 BuildHD2D.py를 다시 실행).
--   Weapon(id) / Item(id) / Enemy(kind) / Npc(id) / SubQuest(id) → 행, Quest(stage) → Quests.etable "Stage<n>" 행, Balance() → Balance.edata 값
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
function HD2DData.SubQuest(Id) return ByName("SubQuests.etable")[Id] end

function HD2DData.Region(Map) return ByName("Regions.etable")[Map] end

-- 낮밤 설정 (DayNight.edata) / 시각 열쇠 (DayNightKeys.etable — 파일 순서 = 시각 순)
function HD2DData.DayNight()
	return Memo("DayNight", function() return Data.Load(Root .. "DayNight.edata") end)
end

function HD2DData.DayNightKeys()
	return Memo("DayNightKeys", function() return Data.GetRows(Root .. "DayNightKeys.etable") end)
end

-- 메인 퀘스트 마지막 단계 번호 (Quests.etable의 Stage<n> 중 가장 큰 n — 엔딩 단계)
function HD2DData.FinalQuestStage()
	return Memo("FinalStage", function()
		local Last = 0
		while ByName("Quests.etable")["Stage" .. tostring(Last + 1)] do Last = Last + 1 end
		return Last
	end)
end

-- 적 행의 그림·몸 모양 종류 (동굴 변형은 바탕 종류의 그림을 색만 바꿔 쓴다 — 행의 Look, 비면 자기 종류)
function HD2DData.EnemyLook(Kind)
	local Row = HD2DData.Enemy(Kind)
	return (Row and Row.Look ~= nil and Row.Look ~= "") and Row.Look or Kind
end

function HD2DData.Balance()
	return Memo("Balance", function()
		local Value = Data.Load(Root .. "Balance.edata")
		if Value == nil then
			Log.Error("[HD2D] Balance.edata를 읽지 못함")
		end
		return Value
	end)
end

-- 표시 순서: 무기 / 방어구 / 장신구 (장비 탭) · 소모품 / 재료 (도구 탭) · 서브 퀘스트 (퀘스트 탭)
HD2DData.WeaponOrder = { "Sword", "Spear", "Bow", "Staff", "CrystalSword", "Harpoon" }
HD2DData.ArmorOrder = { "LeatherVest", "ChainMail", "KnightPlate", "SailorCoat" }
HD2DData.AccessoryOrder = { "LuckyRing", "SwiftCharm", "LifeAmulet", "CrystalCharm", "PearlRing", "CompassCharm" }
HD2DData.ConsumableOrder = { "Potion", "HiPotion", "Ether", "Elixir", "GrilledFish" }
HD2DData.MaterialOrder = { "Jelly", "BatWing", "GoblinFang", "OldBone", "Spore", "CrystalShard", "GolemCore", "LostCat", "LighthouseLens" } -- 재료는 HD2DMetaGen
HD2DData.SubQuestOrder = { "Cat", "Smith", "Scarecrow", "Lighthouse", "Pirates", "Captain" }

return HD2DData
