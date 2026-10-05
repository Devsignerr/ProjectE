-- FarmBie 탑 (FarmGame에 섞이는 메서드 모음). 수치 = Data/FarmBie/Tower.edata, 층 배치 = TowerPresets.etable.
--   농장: 탑 문 앞(FarmDoor)에서 E → 체크포인트 층부터 탑 씬(Scenes/Tower.escene)으로 (세션에 TowerRun = {Floor, Loot})
--   탑 씬(Map = Tower): 방 하나(24×14칸)를 층마다 다시 꾸민다 — 프리셋('#' 기둥 'B' 상자 'e' 적 자리)을 층·날짜로 정해지는 난수로 고르고,
--     적은 C++ 디펜스 시스템(흐름장 목표 = 플레이어 칸, AttractTiles를 0.25초마다 갱신)이 움직인다. 층이 오를수록 수·체력·공격·종류(KindFloors)가 는다.
--     GuardianEvery(5)층마다 수호자(보스 표 — 일반 좀비처럼 플레이어를 노림) + 그 층이 체크포인트(다음 입장 시작 층).
--     적을 다 잡으면 보물상자(돈·이번 계절 희귀 씨앗(층이 높을수록 높은 희귀도)·철·수호자 층은 크리스탈 조각·처음 도달 무기)와 위층 계단이 열린다
--   쓰러지면 이번 탑에서 얻은 전리품의 LootLossOnDown을 잃고 집 침대로 돌아간다. 출구(남쪽 아치)에서 E = 농장 탑 문 앞으로. 탑 안에서도 시간이 흐른다
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Tower = {}

local Prefabs = "Prefabs/FarmBie/"
local SeasonIds = { "Spring", "Summer", "Autumn", "Winter" }

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function Tower:InitTower()
	local T = D.Values("Tower.edata")
	self.TowerData = T
	self.TowerCheckpoint = self.TowerCheckpoint or 1
	self.TowerWeapons = self.TowerWeapons or {}
	if self.MapId == "Farm" then
		local Door = Vector3(T.FarmDoor[1], T.FarmDoor[2], 0)
		self:AddInteractable({ Pos = Door, Radius = 200, Prompt = function()
			if self.BuildMode or self.CarryingCrystal or self.Phase == "Sleep" then return nil end
			return string.format("E  탑에 들어가기 (%d층부터)", self.TowerCheckpoint)
		end, Act = function() self:EnterTower() end })
		return
	end
	if self.MapId ~= "Tower" then return end
	-- 탑 방 격자를 디펜스 컴포넌트에
	local Dc = self.Defense
	Dc.OriginX, Dc.OriginY, Dc.Tile, Dc.Width, Dc.Height = T.Origin[1], T.Origin[2], T.Tile, T.Width, T.Height
	Dc.AttractTiles = ""
	Dc.CropTiles = ""
	Dc.Active = true
	self.TowerPieces = {}
	local X1 = T.Origin[1] + T.Width * T.Tile
	local Y1 = T.Origin[2] + T.Height * T.Tile
	local CX = T.Origin[1] + T.Width * T.Tile * 0.5
	self:AddInteractable({ Pos = Vector3(CX, Y1 - 60, 0), Radius = 180, Prompt = function() return "E  탑에서 나가기" end,
		Act = function() self:LeaveTower() end })
	self:AddInteractable({ Pos = Vector3(CX, T.Origin[2] + 70, 0), Radius = 190, Prompt = function()
		if not self.bFloorCleared then return nil end
		if self.TowerRun.Floor >= T.Floors then return "탑 꼭대기 — 더 오를 곳이 없다" end
		return string.format("E  %d층으로 올라가기", self.TowerRun.Floor + 1)
	end, Act = function()
		if self.bFloorCleared and self.TowerRun.Floor < T.Floors then self:StartFloor(self.TowerRun.Floor + 1) end
	end })
	self.ChestSpot = Vector3(CX - 250, T.Origin[2] + 160, 0)
	self:AddInteractable({ Pos = self.ChestSpot, Radius = 170, Prompt = function()
		if self.bChestReady and not self.bChestOpened then return "E  보물상자 열기" end
		return nil
	end, Act = function() self:OpenTowerChest() end })
	self.TowerRun = self.TowerRun or { Floor = 1, Loot = {} }
	self:StartFloor(self.TowerRun.Floor)
end

-- ---- 농장 → 탑 / 탑 → 농장
function Tower:EnterTower()
	self.TowerRun = { Floor = self.TowerCheckpoint, Loot = {} }
	self.Report.TowerEntries = (self.Report.TowerEntries or 0) + 1
	self:TravelTo("Scenes/Tower.escene", "Floor")
end

function Tower:LeaveTower()
	Log.Info(string.format("[FarmBie] 탑에서 나감: %d층", self.TowerRun.Floor))
	self.TowerRun = nil
	self:TravelTo("Scenes/Farm.escene", "FromTower")
end

-- ---- 층
function Tower:TowerRandom()
	self.TowerState = (self.TowerState * 1103515245 + 12345) % 2147483648
	return self.TowerState / 2147483648
end

function Tower:ClearFloor()
	for _, E in ipairs(self.TowerPieces or {}) do
		if E:IsValid() then Scene.Destroy(E) end
	end
	self.TowerPieces = {}
	for _, Z in ipairs(self.Zombies or {}) do
		if Z.Entity and Z.Entity:IsValid() then Scene.Destroy(Z.Entity) end
	end
	self.Zombies = {}
end

function Tower:TowerCell(TX, TY)
	local T = self.TowerData
	return Vector3(T.Origin[1] + (TX + 0.5) * T.Tile, T.Origin[2] + (TY + 0.5) * T.Tile, 0)
end

function Tower:StartFloor(N)
	local T = self.TowerData
	self:ClearFloor()
	self.TowerRun.Floor = N
	self.bFloorCleared = false
	self.bChestReady, self.bChestOpened = false, false
	self.TowerState = (self:TotalDays() * 7349 + N * 104729 + 3) % 2147483648
	-- 배치 프리셋
	local Presets = D.Rows("TowerPresets.etable")
	local Preset = Presets[1 + math.floor(self:TowerRandom() * #Presets)]
	self.TowerPresetName = Preset.Name
	local Blocked, EnemyCells = {}, {}
	for Y, Row in ipairs(Preset.Rows) do
		for X = 1, #Row do
			local Ch = string.sub(Row, X, X)
			local TX, TY = X - 1, Y - 1
			if Ch == "#" or Ch == "B" then
				Blocked[#Blocked + 1] = TX .. "," .. TY
				local Pos = self:TowerCell(TX, TY)
				Scene.SpawnPrefab(Prefabs .. (Ch == "#" and "TowerColumn" or "TowerCrates") .. ".eprefab", Pos, function(E)
					self.TowerPieces[#self.TowerPieces + 1] = E
				end)
			elseif Ch == "e" and math.abs(TX - T.Width / 2) + math.abs(TY - (T.Height - 1)) >= 7 then
				EnemyCells[#EnemyCells + 1] = { TX, TY } -- 입구(아래 가운데) 가까이는 비움 — 들어서자마자 둘러싸이지 않게
			end
		end
	end
	self.Defense.StaticBlocked = table.concat(Blocked, ";")
	-- 적
	local Kinds = {}
	for _, Text in ipairs(T.KindFloors) do
		local Kind, From = string.match(Text, "^(%a+),(%d+)$")
		if N >= tonumber(From) then Kinds[#Kinds + 1] = Kind end
	end
	local HpMul = 1 + T.HpPerFloor * N
	local DamageMul = 1 + T.DamagePerFloor * N
	local bGuardian = N % T.GuardianEvery == 0
	local Count = math.floor(T.BaseEnemies + T.EnemiesPerFloor * N + 0.5) - (bGuardian and 2 or 0)
	for I = #EnemyCells, 2, -1 do
		local J = 1 + math.floor(self:TowerRandom() * I)
		EnemyCells[I], EnemyCells[J] = EnemyCells[J], EnemyCells[I]
	end
	for I = 1, Count do
		local Cell = EnemyCells[(I - 1) % #EnemyCells + 1]
		local Kind = Kinds[1 + math.floor(self:TowerRandom() * #Kinds)]
		self:SpawnZombie(Kind, { Name = "Tower", Pos = self:TowerCell(Cell[1], Cell[2]) }, HpMul, DamageMul)
	end
	if bGuardian then
		local Id = T.GuardianBosses[math.min(#T.GuardianBosses, N // T.GuardianEvery)]
		self:SpawnGuardian(Id, self:TowerCell(12, 3), HpMul)
		if N > self.TowerCheckpoint then
			self.TowerCheckpoint = N
			self.Report.TowerCheckpoint = N
		end
	end
	self.Report.TowerBest = math.max(self.Report.TowerBest or 0, N)
	self.Report.TowerFloors = (self.Report.TowerFloors or 0) + 1
	self:Hud():Announce(string.format("탑 %d층", N), bGuardian and "수호자가 기다린다 — 체크포인트" or ("배치: " .. self:PresetName(Preset.Name)), 2.5)
	-- 플레이어는 입구로
	local P = self:Player()
	if P and self.bTowerStarted then
		local Pos = P.entity:GetWorldPosition()
		local Start = Scene.Find("Spawn_Floor"):GetWorldPosition()
		P:Teleport(Vector3(Start.X, Start.Y, Pos.Z))
	end
	self.bTowerStarted = true
	Log.Info(string.format("[FarmBie] 탑 %d층 시작: %s, 적 %d%s", N, Preset.Name, Count, bGuardian and " + 수호자" or ""))
end

local PresetNames = { Hall = "넓은 홀", Pillars = "기둥 숲", Crates = "창고", Maze = "미로", Ring = "고리 방", Cross = "십자 회랑" }
function Tower:PresetName(Name)
	return PresetNames[Name] or Name
end

function Tower:SpawnGuardian(Id, Pos, HpMul)
	local Row = D.ByName("Bosses.etable")[Id]
	local T = self.TowerData
	local Z = { Kind = Id, bGuardian = true, Row = Row }
	self.Zombies[#self.Zombies + 1] = Z
	self.Guardian = Z
	Scene.SpawnPrefab(Prefabs .. "Zombie_" .. Id .. ".eprefab", Pos, function(E)
		Z.Entity = E
		local C = E:GetComponent("FarmZombieComponent")
		Z.Comp = C
		C.Hp = Row.Hp * T.GuardianHpMul * HpMul
		C.MaxHp = C.Hp
		C.Speed, C.Damage, C.AttackInterval = Row.Speed, Row.Damage, Row.Interval
		C.StructureDamageMul, C.CropEatTime, C.BodyRadius = Row.StructureMul, Row.CropEat, Row.Radius
		C.SummonKind, C.SummonInterval, C.SummonCount = Row.SummonKind, Row.SummonInterval, Row.SummonCount
		C.AuraRadius, C.AuraStructureDps, C.AuraHeal, C.AuraSlow = Row.AuraRadius, Row.AuraStructureDps, Row.AuraHeal, Row.AuraSlow
	end)
	self:Hud():Announce(Row.DisplayName, "탑의 수호자", 2.5)
end

-- 매 프레임 (탑 씬)
function Tower:UpdateTower(Dt)
	local Dc = self.Defense
	if not Dc or not self.TowerRun then return end
	-- 적이 플레이어를 쫓도록 흐름장 목표 = 플레이어 칸
	self.TowerTargetTimer = (self.TowerTargetTimer or 0) - Dt
	local P = self:Player()
	if P and self.TowerTargetTimer <= 0 then
		self.TowerTargetTimer = 0.25
		local T = self.TowerData
		local Pos = P.entity:GetWorldPosition()
		local TX = math.floor((Pos.X - T.Origin[1]) / T.Tile)
		local TY = math.floor((Pos.Y - T.Origin[2]) / T.Tile)
		Dc.AttractTiles = TX .. "," .. TY
	end
	-- 플레이어 피해
	if Dc.PlayerDamage > 0 then
		local Amount = Dc.PlayerDamage
		Dc.PlayerDamage = 0
		if P and not self.bTowerDefeat then
			self:Damage(Amount, "탑")
			P:OnHurt()
		end
	end
	if not self.TowerRun then return end -- 방금 쓰러져 집으로 가는 중
	-- 수호자 소환
	if Dc.SummonRequests ~= "" then
		local Requests = Dc.SummonRequests
		Dc.SummonRequests = ""
		for Kind, X, Y in string.gmatch(Requests, "(%a+),([-%d]+),([-%d]+);") do
			self:SpawnZombie(Kind, { Name = "Summon", Pos = Vector3(tonumber(X), tonumber(Y), 0) }, 1 + self.TowerData.HpPerFloor * self.TowerRun.Floor)
		end
	end
	-- 층 정리
	if not self.bFloorCleared and self:PendingZombies() == 0 and #self:LiveZombies() == 0 and #self.Zombies > 0 then
		self.bFloorCleared = true
		self.bChestReady = true
		local C = self.ChestSpot
		Scene.SpawnPrefab(Prefabs .. "TowerChest.eprefab", Vector3(C.X, C.Y, 0), function(E) self.TowerPieces[#self.TowerPieces + 1] = E end)
		self.Report.TowerCleared = (self.Report.TowerCleared or 0) + 1
		local Top = self.TowerRun.Floor >= self.TowerData.Floors
		self:Hud():Announce(Top and "탑 정복!" or string.format("%d층 정리!", self.TowerRun.Floor), "보물상자가 나타났다" .. (Top and "" or " · 북쪽 계단이 열렸다"), 3.0)
		if Top then self.Report.TowerConquered = (self.Report.TowerConquered or 0) + 1 end
	end
	-- HUD
	local Hud = self:Hud()
	Hud:Show("DefensePanel", true)
	Hud:Show("CrystalRow", false)
	Hud:Set("DefenseText", "Text", self.bFloorCleared and string.format("탑 %d층 · 정리 완료", self.TowerRun.Floor)
		or string.format("탑 %d층 · 남은 적 %d", self.TowerRun.Floor, #self:LiveZombies() + self:PendingZombies()))
	local G = self.Guardian
	local bShow = G and G.Comp and G.Entity and G.Entity:IsValid() and not G.Comp.Dead
	Hud:Show("BossPanel", bShow == true)
	if bShow then
		Hud:Set("BossName", "Text", G.Row.DisplayName)
		Hud:Set("BossBar", "Percent", math.floor(G.Comp.Hp / math.max(1, G.Comp.MaxHp) * 200 + 0.5) / 200)
	end
end

function Tower:GiveLoot(Key, Count)
	self:Give(Key, Count, true)
	self.TowerRun.Loot[#self.TowerRun.Loot + 1] = { Key = Key, Count = Count }
	return string.format("%s ×%d", self:ItemInfo(Key).Name, Count)
end

function Tower:OpenTowerChest()
	if not self.bChestReady or self.bChestOpened then return end
	self.bChestOpened = true
	local T = self.TowerData
	local N = self.TowerRun.Floor
	local Parts = {}
	local Gold = N * T.GoldPerFloor
	self:AddGold(Gold)
	self.TowerRun.Gold = (self.TowerRun.Gold or 0) + Gold
	Parts[#Parts + 1] = Gold .. "골드"
	-- 이번 계절 희귀 씨앗 (층이 높을수록 높은 희귀도)
	local Season = SeasonIds[self.Season + 1]
	local Crops = {}
	for _, C in ipairs(D.Rows("Crops.etable")) do
		if C.Season == Season and not C.Exclusive then Crops[#Crops + 1] = C end
	end
	local Rarity = N < 8 and 1 or (N < 15 and 2 or (self:TowerRandom() < 0.5 and 3 or 2))
	local Crop = Crops[1 + math.floor(self:TowerRandom() * #Crops)]
	Parts[#Parts + 1] = self:GiveLoot(string.format("Seed:%s:%d", Crop.Name, Rarity), N >= 10 and 2 or 1)
	if N >= 4 then Parts[#Parts + 1] = self:GiveLoot("Iron", 1 + N // 5) end
	if N % T.GuardianEvery == 0 then Parts[#Parts + 1] = self:GiveLoot("CrystalShard", 1 + N // 10) end
	for _, Text in ipairs(T.WeaponFloors) do
		local Weapon, Floor = string.match(Text, "^(%a+),(%d+)$")
		if N >= tonumber(Floor) and not self.TowerWeapons[Weapon] then
			self.TowerWeapons[Weapon] = true
			Parts[#Parts + 1] = self:GiveLoot(Weapon, 1)
		end
	end
	if self:CountItem("Bow") > 0 then Parts[#Parts + 1] = self:GiveLoot("Arrow", 10) end
	self.Report.TowerChests = (self.Report.TowerChests or 0) + 1
	self:Hud():Announce("보물상자", table.concat(Parts, ", "), 3.5)
end

-- 탑에서 쓰러짐: 이번 전리품 일부를 잃고 집으로
function Tower:TowerDefeat()
	if self.bTowerDefeat then return end
	self.bTowerDefeat = true
	local Lost = {}
	for _, L in ipairs(self.TowerRun.Loot) do
		local N = math.floor(L.Count * self.TowerData.LootLossOnDown + 0.5)
		N = math.min(N, self:CountItem(L.Key))
		if N > 0 then
			self:Take(L.Key, N)
			Lost[#Lost + 1] = string.format("%s ×%d", self:ItemInfo(L.Key).Name, N)
		end
	end
	local GoldLost = math.floor((self.TowerRun.Gold or 0) * self.TowerData.LootLossOnDown + 0.5)
	self.Gold = math.max(0, self.Gold - GoldLost)
	self.Health = self.Vitals.MaxHealth * 0.5
	self.Report.TowerDowns = (self.Report.TowerDowns or 0) + 1
	self.TowerLostText = #Lost > 0 and ("잃은 전리품: " .. table.concat(Lost, ", ") .. string.format(", %d골드", GoldLost)) or "잃은 전리품 없음"
	Log.Info("[FarmBie] 탑에서 쓰러짐 — " .. self.TowerLostText)
	self.TravelBanner = { "탑에서 쓰러졌다", self.TowerLostText }
	self.TowerRun = nil
	self:TravelTo("Scenes/Farm.escene", "Bed")
end

-- 저장 조각
function Tower:SaveTower(T)
	T.TowerCheckpoint = self.TowerCheckpoint
	T.TowerWeapons = self.TowerWeapons
	T.TowerRun = self.TowerRun
end

function Tower:LoadTower(T)
	self.TowerCheckpoint = math.floor(T.TowerCheckpoint or 1)
	self.TowerWeapons = {}
	for K, V in pairs(T.TowerWeapons or {}) do self.TowerWeapons[K] = V end
	self.TowerRun = nil
	if T.TowerRun then
		self.TowerRun = { Floor = math.floor(T.TowerRun.Floor or 1), Loot = {}, Gold = T.TowerRun.Gold or 0 }
		for _, L in ipairs(T.TowerRun.Loot or {}) do self.TowerRun.Loot[#self.TowerRun.Loot + 1] = { Key = L.Key, Count = math.floor(L.Count) } end
	end
end

return Tower
