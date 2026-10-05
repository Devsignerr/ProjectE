-- FarmBie 밤 디펜스 흐름 (FarmGame에 섞이는 메서드 모음). 좀비 이동·공격·덫·투사체는 C++(FarmDefenseSystem)가, 여기서는 밤 계획·생성·결과를 맡는다.
--   계획(아침 OnDayStart → PlanNight): 그 밤 진입로(EntrancesPerNight, 날짜로 정해지는 난수 — 낮에 Warn_<이름> 표지로 예고)·좀비 수와 종류(Zombies.etable 가중치·
--     처음 나오는 날·계절 변종)·체력 배율(Night.edata). 해마다 같은 구성 반복
--   밤(OnNightStart → UpdateDefense): 밤 앞 SpawnWindow 동안 일정대로 Scene.SpawnPrefab(Zombie_<종류>) + 표 값 채움. C++ 출력(FarmDefenseComponent)을 읽어
--     플레이어 피해·둔화, 먹힌 작물(CropEvents), 크리스탈 파괴(→ 게임 오버)를 반영. 다 나왔고 다 죽으면 "밤을 버텼다" → 침대에서 잘 수 있다
--   밤 끝(OnNightEnd — 새벽 시간 초과 / 침대): 다 못 잡았으면 라운드 패배 = 남은 좀비가 아침까지 피해(작물·설치물·알아챘으면 크리스탈) + 정신력 감소
--   부재: 밤에 농장 밖이면 덫·포탑 전력으로 계산(ResolveAbsentNight). 밤 중에 돌아오면 그 시각까지 나왔어야 할 좀비를 (이미 잡은 수만큼 빼고) 다시 내보낸다
--   쓰러짐: 체력 0 → 정신력 DeathSanityLoss 잃고 RespawnTime 뒤 집 앞에서 부활(체력 절반)
--   게임 오버: 크리스탈 파괴 → 기록 표시 + 저장 슬롯 삭제 → E로 처음부터
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Def = {}

local Prefabs = "Prefabs/FarmBie/"
local SeasonIds = { "Spring", "Summer", "Autumn", "Winter" }
local EntranceNames = { North = "북쪽", South = "남쪽", West = "서쪽", East = "동쪽" }

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function Def:InitDefense()
	self.Night = D.Values("Night.edata")
	self.Entrances = {}
	for _, Text in ipairs(self.Map.Entrances) do
		local Name, X, Y = string.match(Text, "^(%a+),([-%d%.]+),([-%d%.]+)$")
		self.Entrances[#self.Entrances + 1] = { Name = Name, Pos = Vector3(tonumber(X), tonumber(Y), 0) }
	end
	self.Zombies = {}
	self.NightKills = self.NightKills or 0
	self.Defense = self.entity:GetComponent("FarmDefenseComponent")
	if not self.Defense then return end
	local Dc, M = self.Defense, self.Map
	Dc.OriginX, Dc.OriginY, Dc.Tile, Dc.Width, Dc.Height = M.OriginX, M.OriginY, M.Tile, M.Width, M.Height
	Dc.StaticBlocked = M.StaticBlocked
	Dc.AttractTiles = M.AttractTiles
	Dc.AlertRadius = self.Night.CrystalAlertRadius
	Dc.BlockCost = self.Night.BlockCost
	Dc.Active = false
	self:SyncCropTiles()
end

-- ---- 계획 (아침·처음 시작·불러오기)
function Def:NightRandom()
	self.NightState = (self.NightState * 1103515245 + 12345) % 2147483648
	return self.NightState / 2147483648
end

function Def:PlanNight()
	local N = self.Night
	self.NightState = (self:TotalDays() * 104729 + 7) % 2147483648
	-- 진입로
	local Pool = {}
	for _, E in ipairs(self.Entrances) do Pool[#Pool + 1] = E end
	local Chosen = {}
	for _ = 1, math.min(N.EntrancesPerNight, #Pool) do
		local I = 1 + math.floor(self:NightRandom() * #Pool)
		Chosen[#Chosen + 1] = table.remove(Pool, I)
	end
	-- 좀비 수·종류
	local Season = SeasonIds[self.Season + 1]
	local Count = math.floor(N.BaseCount + N.PerDay * self.Day + N.PerSeason * self.Season + 0.5)
	local Kinds, Total = {}, 0
	for _, Row in ipairs(D.Rows("Zombies.etable")) do
		if self.Day >= Row.MinDay and (Row.Season == "Any" or Row.Season == Season) then
			Kinds[#Kinds + 1] = Row
			Total = Total + Row.Weight
		end
	end
	local Window = self:NightLength() * N.SpawnWindow
	local Spawns = {}
	if self.bPeacefulNights then Count = 0 end -- 자동 검증: 디펜스가 아닌 시나리오는 좀비 없는 밤
	for I = 1, Count do
		local Roll, Acc, Pick = self:NightRandom() * Total, 0, Kinds[1]
		for _, Row in ipairs(Kinds) do
			Acc = Acc + Row.Weight
			if Roll < Acc then Pick = Row break end
		end
		local T = 2.0 + (I - 1) / math.max(1, Count) * Window
		Spawns[#Spawns + 1] = { T = T, Kind = Pick.Name, Entrance = Chosen[1 + (I - 1) % #Chosen] }
	end
	self.TonightPlan = { Entrances = Chosen, Spawns = Spawns, HpMul = 1 + N.HpPerDay * self.Day + N.HpPerSeason * self.Season }
	self.NightKills = 0
	self:ShowWarnings(true)
	return self.TonightPlan
end

function Def:NightLength()
	local C = self.Calendar
	return C.NightRealMinutes * 60
end

function Def:PlanText()
	local P = self.TonightPlan
	if not P then return "" end
	local Names = {}
	for _, E in ipairs(P.Entrances) do Names[#Names + 1] = EntranceNames[E.Name] or E.Name end
	return string.format("오늘 밤: %s 입구 — 좀비 %d", table.concat(Names, "·"), #P.Spawns)
end

function Def:ShowWarnings(bShow)
	if self.MapId ~= "Farm" then return end
	local Planned = {}
	for _, E in ipairs(self.TonightPlan and self.TonightPlan.Entrances or {}) do Planned[E.Name] = true end
	for _, E in ipairs(self.Entrances) do
		local W = Scene.Find("Warn_" .. E.Name)
		if W then W:GetComponent("SpriteComponent").Visible = bShow and Planned[E.Name] == true end
	end
end

-- ---- 밤
function Def:OnNightStart()
	if not self.TonightPlan then self:PlanNight() end
	self.SpawnIndex = 1
	self.bNightCleared = false
	self.bRoundLost = false
	self:ShowWarnings(false)
	if self.Defense and self.MapId == "Farm" then
		self.Defense.Active = true
		self.Defense.Kills = 0
		self.KillBase = 0
	end
	self.Report.Nights = (self.Report.Nights or 0) + 1
	if self.OnBossNightStart then self:OnBossNightStart() end
end

function Def:NightElapsed()
	if self.Phase ~= "Night" then return 0 end
	return (self.Hour - self.Calendar.NightStartHour) / self:HoursPerSecond()
end

function Def:SpawnZombie(Kind, Entrance, HpMul, DamageMul)
	local Row = D.ByName("Zombies.etable")[Kind]
	local Jitter = Vector3((self:NightRandom() - 0.5) * 220, (self:NightRandom() - 0.5) * 220, 0)
	if Entrance.Name == "West" or Entrance.Name == "East" then Jitter.X = Jitter.X * 0.3 else Jitter.Y = Jitter.Y * 0.3 end
	local Pos = Entrance.Pos + Jitter
	local Z = { Kind = Kind }
	self.Zombies[#self.Zombies + 1] = Z
	Scene.SpawnPrefab(Prefabs .. "Zombie_" .. Kind .. ".eprefab", Vector3(Pos.X, Pos.Y, 0), function(E)
		Z.Entity = E
		local C = E:GetComponent("FarmZombieComponent")
		Z.Comp = C
		C.Hp, C.MaxHp = Row.Hp * HpMul, Row.Hp * HpMul
		C.Speed, C.Damage, C.AttackInterval = Row.Speed, Row.Damage * (DamageMul or 1.0), Row.Interval
		C.StructureDamageMul, C.CropEatTime, C.BodyRadius = Row.StructureMul, Row.CropEat, Row.Radius
		C.Explode, C.ExplodeRadius, C.ExplodeDamage = Row.Explode, Row.ExplodeRadius, Row.ExplodeDamage
		C.RegenPerSec, C.SlowOnHit = Row.Regen, Row.Slow
	end)
	self.Report.Spawned = (self.Report.Spawned or 0) + 1
	return Z
end

-- 아직 생성 콜백 전(엔티티 없음)인 좀비 수
function Def:PendingZombies()
	local N = 0
	for _, Z in ipairs(self.Zombies) do if not Z.Entity then N = N + 1 end end
	return N
end

-- 살아 있는 좀비 (C++가 지운 엔티티·죽은 것 제외, 생성 대기 제외)
function Def:LiveZombies()
	local List = {}
	for _, Z in ipairs(self.Zombies) do
		if Z.Entity and Z.Entity:IsValid() and Z.Comp and not Z.Comp.Dead then List[#List + 1] = Z end
	end
	return List
end

function Def:UpdateDefense(Dt)
	local Dc = self.Defense
	if not Dc or self.MapId ~= "Farm" or self.Phase == "GameOver" then return end
	-- 크리스탈 감지 반경 = 크리스탈 단계
	if self.Crystal then Dc.CrystalDetectRadius = D.Rows("CrystalLevels.etable")[self.Crystal.Level].DetectRadius end
	-- 플레이어 피해·둔화
	if Dc.PlayerDamage > 0 then
		local Amount = Dc.PlayerDamage
		Dc.PlayerDamage = 0
		if not self.PlayerDown then
			self:Damage(Amount, "좀비")
			local P = self:Player()
			if P then P:OnHurt() end
		end
	end
	-- 먹힌 작물
	if Dc.CropEvents ~= "" then
		local Events = Dc.CropEvents
		Dc.CropEvents = ""
		for X, Y in string.gmatch(Events, "(%d+),(%d+);") do
			local T = self:GetTile(tonumber(X), tonumber(Y))
			if T and T.Crop then
				T.Crop = nil
				T.Fert = 0
				self:RefreshTile(T)
				self.Report.CropsEaten = (self.Report.CropsEaten or 0) + 1
			end
		end
		self:SyncCropTiles()
	end
	-- 크리스탈 파괴 → 게임 오버
	if self.Crystal and self.Crystal.Comp and self.Crystal.Comp.Destroyed then
		self:GameOver("크리스탈이 무너졌다")
		return
	end
	-- 쓰러짐 → 부활
	if self.PlayerDown then
		self.PlayerDown = self.PlayerDown - Dt
		self:Hud():Set("RespawnText", "Text", string.format("쓰러졌다… %d초 뒤 일어난다", math.ceil(self.PlayerDown)))
		if self.PlayerDown <= 0 then self:RespawnPlayer() end
	end
	if self.Phase ~= "Night" then
		self.SpawnIndex = nil
		return
	end
	-- 시각을 바로 밤으로 옮긴 경우(SetHour — 밤 시작 훅 없음)도 여기서 밤을 시작한다
	if not self.SpawnIndex then self:OnNightStart() end
	-- 생성 일정
	local Plan = self.TonightPlan
	local Elapsed = self:NightElapsed()
	while Plan and self.SpawnIndex <= #Plan.Spawns and Plan.Spawns[self.SpawnIndex].T <= Elapsed do
		local S = Plan.Spawns[self.SpawnIndex]
		self.SpawnIndex = self.SpawnIndex + 1
		if (self.SkipSpawns or 0) > 0 then
			self.SkipSpawns = self.SkipSpawns - 1 -- 부재 중 이미 잡은 것
		else
			self:SpawnZombie(S.Kind, S.Entrance, Plan.HpMul)
		end
	end
	self.NightKills = (self.KillBaseNight or 0) + Dc.Kills
	if Dc.CrystalFound and not self.bAlertShown then
		self.bAlertShown = true
		self:Hud():Announce("크리스탈이 들켰다!", "좀비들이 크리스탈로 몰려간다 — 막아라", 3.0)
		self.Report.CrystalFound = (self.Report.CrystalFound or 0) + 1
	end
	-- 다 나왔고 다 잡음
	if not self.bNightCleared and Plan and self.SpawnIndex > #Plan.Spawns and self:PendingZombies() == 0 and #self:LiveZombies() == 0 and self:BossCleared() then
		self.bNightCleared = true
		self.Report.NightsCleared = (self.Report.NightsCleared or 0) + 1
		if #Plan.Spawns > 0 then self:Hud():Announce("밤을 버텼다!", "남은 시간은 쉬어도 된다 — 집 앞에서 잠자기", 3.5) end
		Log.Info(string.format("[FarmBie] 밤 정리: %s 처치 %d", self:DateText(), Dc.Kills))
	end
end

function Def:BossCleared()
	if self.IsBossAlive then return not self:IsBossAlive() end
	return true
end

function Def:IsNightCleared()
	return self.bNightCleared == true
end

-- 밤 끝 (새벽 시간 초과 또는 침대). 게임 오버면 true
function Def:OnNightEnd()
	local Dc = self.Defense
	if self.MapId ~= "Farm" then
		return self:ResolveAbsentNight()
	end
	local Live = self:LiveZombies()
	local Notes = {}
	if not self.bNightCleared and (#Live > 0 or self:PendingZombies() > 0 or (self.TonightPlan and (self.SpawnIndex or 1) <= #self.TonightPlan.Spawns)) then
		self.bRoundLost = true
		local N = self.Night
		local Eaten, Broken = 0, 0
		for _, Z in ipairs(Live) do
			local Pos = Z.Entity:GetWorldPosition()
			Eaten = Eaten + self:EatNearestCrops(Pos, N.LossCropsPerZombie)
			Broken = Broken + self:DamageNearestStructure(Pos, N.LossStructureDamage)
			if Z.Comp.Alerted and self.Crystal and self.Crystal.Comp then
				self.Crystal.Comp.Hp = self.Crystal.Comp.Hp - N.LossCrystalDamage
			end
		end
		self.Report.RoundsLost = (self.Report.RoundsLost or 0) + 1
		Notes[#Notes + 1] = string.format("밤을 막지 못했다 — 작물 %d 피해", Eaten)
		self:LoseSanity(self.Vitals.RoundLossSanity, "라운드 패배")
		Log.Info(string.format("[FarmBie] 라운드 패배: 남은 좀비 %d, 작물 %d, 설치물 %d", #Live, Eaten, Broken))
	end
	if self.OnBossNightEnd then self:OnBossNightEnd(Notes) end
	for _, Z in ipairs(self.Zombies) do
		if Z.Entity and Z.Entity:IsValid() then Scene.Destroy(Z.Entity) end
	end
	self.Zombies = {}
	if Dc then Dc.Active = false end
	self.PendingNightNotes = Notes
	self:Hud():Show("RespawnText", false)
	if self.PlayerDown then self:RespawnPlayer() end
	if self.Crystal and self.Crystal.Comp and self.Crystal.Comp.Hp <= 0 then
		self:GameOver("크리스탈이 무너졌다")
		return true
	end
	return false
end

function Def:EatNearestCrops(Pos, Count)
	local Eaten = 0
	for _ = 1, Count do
		local Best, BestD = nil, 500
		for _, T in pairs(self.Tiles) do
			if T.Crop and not T.Crop.Dead then
				local Dd = Flat(self:TileCenter(T.TX, T.TY) - Pos):Length()
				if Dd < BestD then Best, BestD = T, Dd end
			end
		end
		if not Best then break end
		Best.Crop = nil
		self:RefreshTile(Best)
		Eaten = Eaten + 1
	end
	self.Report.CropsEaten = (self.Report.CropsEaten or 0) + Eaten
	self:SyncCropTiles()
	return Eaten
end

function Def:DamageNearestStructure(Pos, Amount)
	local Best, BestD = nil, 400
	for _, S in pairs(self.Structures) do
		if S.Comp then
			local Dd = Flat(self:TileCenter(S.TX, S.TY) - Pos):Length()
			if Dd < BestD then Best, BestD = S, Dd end
		end
	end
	if not Best then return 0 end
	Best.Comp.Hp = Best.Comp.Hp - Amount
	if Best.Comp.Hp <= 0 then
		Best.Comp.Destroyed = true
		return 1
	end
	return 0
end

-- 플레이어가 농장 밖에서 밤을 넘김: 덫·포탑 전력으로 막은 양을 어림하고 나머지는 남은 좀비 피해로 (저장 데이터에 직접 반영)
function Def:ResolveAbsentNight()
	local Plan = self.TonightPlan
	if not Plan then return false end
	local N = self.Night
	local TotalHp, Count = 0, #Plan.Spawns - (self.NightKills or 0)
	if Count <= 0 then return false end
	for I = 1, Count do
		TotalHp = TotalHp + D.ByName("Zombies.etable")[Plan.Spawns[I].Kind].Hp * Plan.HpMul
	end
	local Power = 0
	for _, S in pairs(self.Structures) do
		local Row = D.ByName("Buildables.etable")[S.Id]
		if Row and Row.Damage > 0 then
			if Row.Cooldown > 0 then
				Power = Power + Row.Damage / Row.Cooldown * self:NightLength() * 0.25 * N.AbsentTurretShare
			else
				Power = Power + Row.Damage * 2 -- 지뢰: 몇 마리
			end
		end
	end
	local Left = math.max(0, TotalHp - Power)
	local Remaining = math.ceil(Left / math.max(1, TotalHp / Count) - 1e-6)
	local Notes = {}
	if Remaining > 0 then
		local CropsLost = 0
		for _, T in pairs(self.Tiles) do
			if CropsLost >= Remaining * N.LossCropsPerZombie then break end
			if T.Crop and not T.Crop.Dead and not self:IsGreenhouseTile(T.TX, T.TY) then
				T.Crop = nil
				CropsLost = CropsLost + 1
			end
		end
		local Hit = 0
		for _, S in pairs(self.Structures) do
			if Hit >= Remaining then break end
			S.Hp = (S.Hp or 100) - N.LossStructureDamage
			Hit = Hit + 1
		end
		if self.Crystal then self.Crystal.Hp = (self.Crystal.Hp or 400) - math.ceil(Remaining * 0.5) * N.LossCrystalDamage end
		self.Report.RoundsLost = (self.Report.RoundsLost or 0) + 1
		self:LoseSanity(self.Vitals.RoundLossSanity, "자리를 비운 밤")
		Notes[#Notes + 1] = string.format("농장을 비운 밤 — 좀비 %d가 날뛰었다 (작물 %d 피해)", Remaining, CropsLost)
	else
		Notes[#Notes + 1] = "농장을 비웠지만 덫과 포탑이 밤을 막아냈다"
	end
	self.PendingNightNotes = Notes
	self.Report.AbsentNights = (self.Report.AbsentNights or 0) + 1
	if self.Crystal and (self.Crystal.Hp or 1) <= 0 then
		self:GameOver("자리를 비운 사이 크리스탈이 무너졌다")
		return true
	end
	return false
end

-- 밤 중에 농장으로 돌아옴 (세션 도착): 그 시각까지 나왔어야 할 좀비를 다시 내보낸다 (이미 잡은 수만큼 빼고)
function Def:ResumeNight()
	if self.MapId ~= "Farm" or self.Phase ~= "Night" then return end
	if not self.TonightPlan then self:PlanNight() end
	self.SpawnIndex = 1
	self.SkipSpawns = self.NightKills or 0
	self.KillBaseNight = self.NightKills or 0
	self.Defense.Active = true
	self.Defense.Kills = 0
	self:ShowWarnings(false)
end

-- ---- 작물 칸을 C++에 알림
function Def:SyncCropTiles()
	if not self.Defense then return end
	local Parts = {}
	for _, T in pairs(self.Tiles or {}) do
		if T.Crop and not T.Crop.Dead then Parts[#Parts + 1] = T.TX .. "," .. T.TY end
	end
	table.sort(Parts)
	local Text = table.concat(Parts, ";")
	if Text ~= self.Defense.CropTiles then
		self.Defense.CropTiles = Text
		self.Defense.CropRevision = self.Defense.CropRevision + 1
	end
end

-- ---- 쓰러짐·부활
function Def:OnPlayerDown(Reason)
	if self.MapId == "Tower" then
		self:TowerDefeat()
		return
	end
	if self.PlayerDown or self.Phase == "Sleep" or self.Phase == "GameOver" then return end
	self.PlayerDown = self.Night.RespawnTime
	self.Report.PlayerDowns = (self.Report.PlayerDowns or 0) + 1
	self:Hud():Show("RespawnText", true)
	local P = self:Player()
	if P then P:SetDown(true) end
	self:LoseSanity(self.Vitals.DeathSanityLoss, "쓰러짐")
end

function Def:RespawnPlayer()
	self.PlayerDown = nil
	self.Health = math.max(self.Health, self.Vitals.MaxHealth * 0.5)
	self:Hud():Show("RespawnText", false)
	local P = self:Player()
	if P then
		P:SetDown(false)
		local Pos = P.entity:GetWorldPosition()
		P:Teleport(Vector3(self.SleepSpot.X, self.SleepSpot.Y + 60, Pos.Z))
	end
	self.Report.Respawns = (self.Report.Respawns or 0) + 1
end

-- ---- 게임 오버
function Def:GameOver(Reason)
	if self.Phase == "GameOver" then return end
	self.Phase = "GameOver"
	self.Report.GameOvers = (self.Report.GameOvers or 0) + 1
	if self.Defense then self.Defense.Active = false end
	local Days = self:TotalDays() - 1
	SaveGame.Delete(self:SlotName())
	local Hud = self:Hud()
	Hud:Show("GameOverWindow", true, "Visible")
	Hud:Set("GameOverTitle", "Text", Reason or "크리스탈이 무너졌다")
	Hud:Set("GameOverBody", "Text", string.format("%d년차 %s까지 %d일을 버텼다.\n쓰러뜨린 좀비 %d · 거둔 작물 %d · 번 돈 %d",
		self.Year, self:DateText(), Days, (self.Report.TotalKills or 0) + (self.Defense and self.Defense.Kills or 0), self.Report.Harvested or 0, self.Report.Income or 0))
	Game.SetTimeScale(0.0)
	Log.Info(string.format("[FarmBie] 게임 오버: %s (%d일 생존)", Reason or "", Days))
end

function Def:RestartGame()
	Game.SetTimeScale(1.0)
	Game.SetPersistent("FarmBie_Session", nil)
	Game.OpenScene("Scenes/Farm.escene")
end

-- 저장 조각: 그 밤 처치 수(밤 중 맵 이동 세션용)
function Def:SaveDefense(T)
	T.NightKills = self.NightKills or 0
end

function Def:LoadDefense(T)
	self.NightKills = math.floor(T.NightKills or 0)
end

return Def
