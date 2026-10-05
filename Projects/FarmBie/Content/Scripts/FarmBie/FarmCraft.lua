-- FarmBie 작업대 제작·전투 (FarmGame에 섞이는 메서드 모음).
--   제작: 작업대(FarmMap.Workbench)에서 E → 제작 창(상점 창 위젯을 같이 씀). 제작법 = Data/FarmBie/Recipes.etable — 재료 "물건*수",
--     작물은 "Crop:<작물>:<최소 희귀도>+" (조건을 만족하는 가장 낮은 희귀도부터 쓴다 — 레어 작물로 작물별 고유 탄약·덫)
--   전투: 고른 칸의 무기(Weapons.etable — 도구 괭이·도끼·곡괭이도 근접 무기) → PlayerAttack이 C++ 디펜스에 공격 명령(AttackSeq 증가).
--     투사체 무기는 탄약을 쓴다 (특수 탄약 SpecialAmmo.etable이 있으면 먼저 — 피해 배율). 피해 = 무기 × 정신력 배율 × (1 + 공격력 버프)
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Craft = {}

function Craft:InitCraft()
	if self.MapId ~= "Farm" then return end
	local W = self.Map.Workbench
	self:AddInteractable({ Pos = Vector3(W[1], W[2] + 90, 0), Radius = 190, Prompt = function()
		if self.BuildMode or self.CarryingCrystal then return nil end
		return "E  작업대 (제작)"
	end, Act = function() self:OpenCraft() end })
end

-- "Crop:VeinPepper:1+" 같은 재료 열쇠 → 가진 물건 중 조건에 맞는 열쇠 목록(낮은 희귀도부터)
function Craft:MatchKeys(Key)
	local Kind, Id, R, Plus = string.match(Key, "^(%a+):(%w+):(%d)(%+?)$")
	if not Kind then return { Key } end
	if Plus == "" then return { Key } end
	local List = {}
	for Rarity = tonumber(R), 3 do List[#List + 1] = string.format("%s:%s:%d", Kind, Id, Rarity) end
	return List
end

function Craft:ParseInputs(Text)
	local List = {}
	for Key, Count in string.gmatch(Text or "", "([^,%*]+)%*(%d+)") do List[#List + 1] = { Key = Key, Count = tonumber(Count) } end
	return List
end

function Craft:InputCount(Key)
	local N = 0
	for _, K in ipairs(self:MatchKeys(Key)) do N = N + self:CountItem(K) end
	return N
end

function Craft:CanCraft(Recipe)
	for _, In in ipairs(self:ParseInputs(Recipe.Inputs)) do
		if self:InputCount(In.Key) < In.Count then return false end
	end
	return true
end

function Craft:InputName(Key)
	local Kind, Id, R, Plus = string.match(Key, "^(%a+):(%w+):(%d)(%+?)$")
	if Kind then
		local Crop = D.ByName("Crops.etable")[Id]
		local Rarity = D.Rows("Rarities.etable")[tonumber(R) + 1]
		return string.format("%s (%s%s)", Crop.DisplayName, Rarity.DisplayName, Plus == "+" and " 이상" or "")
	end
	return self:ItemInfo(Key).Name
end

function Craft:InputsText(Recipe)
	local Parts = {}
	for _, In in ipairs(self:ParseInputs(Recipe.Inputs)) do
		Parts[#Parts + 1] = string.format("%s ×%d (%d)", self:InputName(In.Key), In.Count, self:InputCount(In.Key))
	end
	return table.concat(Parts, "\n")
end

function Craft:DoCraft(Recipe)
	if not self:CanCraft(Recipe) then return false, "재료가 모자라다" end
	for _, In in ipairs(self:ParseInputs(Recipe.Inputs)) do
		local Left = In.Count
		for _, K in ipairs(self:MatchKeys(In.Key)) do
			local N = math.min(Left, self:CountItem(K))
			if N > 0 then
				self:Take(K, N)
				Left = Left - N
			end
			if Left <= 0 then break end
		end
	end
	self:Give(Recipe.Output, Recipe.Count, true)
	self.Report.Crafted = (self.Report.Crafted or 0) + 1
	return true, string.format("%s ×%d 제작", self:ItemInfo(Recipe.Output).Name, Recipe.Count)
end

function Craft:OpenCraft()
	self.CraftList = D.Rows("Recipes.etable")
	self:OpenMenu("Craft")
	local Hud = self:Hud()
	Hud:Set("ShopTitle", "Text", "작업대")
	Hud:Set("ShopSay", "Text", "재료를 모아 탄약과 덫을 만든다. 레어 작물로는 특별한 것을.")
	Hud:Set("ShopHint", "Text", "W/S 고르기   E 만들기   Esc 닫기")
	Hud:Set("ShopPortrait", "Texture", "UI/FarmBie/Icons/Workbench.png")
	self.Report.CraftOpened = (self.Report.CraftOpened or 0) + 1
end

-- ---- 전투
function Craft:WeaponRow(Key)
	return Key and D.ByName("Weapons.etable")[Key] or nil
end

-- 쏠 탄약 (특수 탄약 먼저) → 열쇠, 피해 배율, 그림 | nil
function Craft:PickAmmo(WeaponKey, Row)
	for _, A in ipairs(D.Rows("SpecialAmmo.etable")) do
		if A.For == WeaponKey and self:CountItem(A.Name) > 0 then return A.Name, A.DamageMul, A.Slice end
	end
	if Row.Ammo ~= "" and self:CountItem(Row.Ammo) > 0 then return Row.Ammo, 1.0, Row.Slice end
	return nil
end

-- 공격 명령 (플레이어 도구·무기 동작의 효과 시점) → 실제로 공격했는지
function Craft:PlayerAttack(WeaponKey, Pos, Facing)
	local Row = self:WeaponRow(WeaponKey)
	local Dc = self.Defense
	if not Row or not Dc or self.MapId ~= "Farm" then return false end
	local Mul = self:StatMul() * (1 + self:BuffAmount("Power"))
	if Row.Kind == "Shot" then
		local Ammo, AmmoMul, Slice = self:PickAmmo(WeaponKey, Row)
		if not Ammo then
			self:Hud():Toast("", self:ItemInfo(Row.Ammo).Name .. "이(가) 없다", { 1.0, 0.6, 0.5, 1.0 })
			return false
		end
		self:Take(Ammo, 1)
		Mul = Mul * AmmoMul
		Dc.AttackKind = "Shot"
		Dc.ShotCount, Dc.ShotSpread, Dc.ShotSpeed, Dc.ShotSlice = Row.Shots, Row.Spread, Row.ShotSpeed, Slice
		self.Report.Shots = (self.Report.Shots or 0) + 1
	else
		Dc.AttackKind = "Melee"
		self.Report.Swings = (self.Report.Swings or 0) + 1
	end
	Dc.AttackPos = Pos
	Dc.AttackDir = Facing
	Dc.AttackRange, Dc.AttackArc, Dc.AttackDamage, Dc.AttackKnockback = Row.Range, Row.Arc, Row.Damage * Mul, Row.Knockback
	Dc.AttackSeq = Dc.AttackSeq + 1
	return true
end

return Craft
