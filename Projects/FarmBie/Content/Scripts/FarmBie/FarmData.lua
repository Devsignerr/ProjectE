-- FarmBie 데이터 읽기 (Script.Require("Scripts/FarmBie/FarmData.lua")). 수치는 Data/FarmBie/*.etable·*.edata에 있다
-- (Projects/FarmBie/Tools/FarmBieData.py가 쓴다 — 표를 고치려면 그 스크립트를 고치고 BuildFarmBie.py를 다시 실행).
-- 세대(Data.GetGeneration — 에디터 핫 리로드)마다 한 번 읽어 공유한다. 돌려준 테이블은 읽기 전용으로 쓴다.
local FarmData = {}

local Root = "Data/FarmBie/"
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

local function Rows(File)
	return Memo("Rows:" .. File, function()
		local List = Data.GetRows(Root .. File)
		if not List or #List == 0 then
			Log.Error("[FarmBie] 데이터 표가 비었거나 읽지 못함: " .. Root .. File)
		end
		return List or {}
	end)
end

local function ByName(File)
	return Memo("Map:" .. File, function()
		local Map = {}
		for _, Row in ipairs(Rows(File)) do Map[Row.Name] = Row end
		return Map
	end)
end

local function Values(File)
	return Memo("Values:" .. File, function()
		local V = Data.Load(Root .. File)
		if not V then Log.Error("[FarmBie] 데이터를 읽지 못함: " .. Root .. File) end
		return V
	end)
end

function FarmData.Calendar() return Values("Calendar.edata") end
function FarmData.DayNightKeys() return Rows("DayNightKeys.etable") end

FarmData.Rows = Rows
FarmData.ByName = ByName
FarmData.Values = Values

return FarmData
