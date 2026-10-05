-- FarmBie 보스 (FarmGame에 섞이는 메서드 모음). 보스 정의 = Data/FarmBie/Bosses.etable, 규칙 = BossRules.edata.
--   보스 밤: 계절 일차가 BossEveryDays 배수(10·20·30) → 중간 보스 1 (Mid 목록에서 계절·일차로 돌아가며), 마지막 날(30)은 계절 보스도 함께.
--     보스 밤은 BossNightMul배 길다(FarmTime:HoursPerSecond). 보스는 밤 시작 8초 뒤 첫 진입로에서 나온다
--   보스 = 좀비 프리팹 Zombie_<보스 이름> + FarmZombieComponent(Boss·처음부터 Alerted = 크리스탈 사냥꾼, AggroTime 동안만 맞으면 플레이어를 쫓음),
--     고유 특성은 C++(소환 요청 SummonRequests → 여기서 좀비 생성, 불길·치유·둔화 오라)
--   처치: 보상 돈·물건 + 계절 보스는 다음 계절 전용 희귀종 씨앗(레어). 밤이 끝날 때 살아 있으면 패배:
--     중간 = 쓰러짐(쓰러짐 벌칙 = 소지금 CollapseGoldLoss + 늦게 기상), 계절 = 소지금 SeasonGoldLoss + 정신력 SeasonSanityLoss
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Boss = {}

local Prefabs = "Prefabs/FarmBie/"
local SeasonIds = { "Spring", "Summer", "Autumn", "Winter" }

function Boss:InitBoss()
	self.BossRules = D.Values("BossRules.edata")
	self.Bosses = {}
end

-- 오늘 밤 보스 목록 (행)
function Boss:BossesTonight()
	local List = {}
	if not self:IsBossNight() then return List end
	local Mids = {}
	for _, Row in ipairs(D.Rows("Bosses.etable")) do
		if Row.Kind == "Mid" then Mids[#Mids + 1] = Row end
	end
	local Index = (self.Season + math.floor(self.Day / self.Calendar.BossEveryDays) - 1) % #Mids + 1 -- 계절마다 순서가 한 칸씩 돈다
	List[#List + 1] = Mids[Index]
	if self:IsSeasonBossNight() then
		for _, Row in ipairs(D.Rows("Bosses.etable")) do
			if Row.Kind == "Season" and Row.Season == SeasonIds[self.Season + 1] then List[#List + 1] = Row end
		end
	end
	return List
end

function Boss:OnBossNightStart()
	self.Bosses = {}
	self.BossSpawnAt = nil
	if self.bPeacefulNights or self.MapId ~= "Farm" then return end
	local Rows = self:BossesTonight()
	if #Rows == 0 then return end
	self.BossSpawnAt = 8.0
	for _, Row in ipairs(Rows) do self.Bosses[#self.Bosses + 1] = { Row = Row, bSpawned = false } end
	self.Report.BossNights = (self.Report.BossNights or 0) + 1
end

function Boss:SpawnBoss(B)
	local Row = B.Row
	local Plan = self.TonightPlan
	local Entrance = Plan and Plan.Entrances[1] or self.Entrances[1]
	local HpMul = Plan and Plan.HpMul or 1.0
	B.bSpawned = true
	local Z = { Kind = Row.Name, bBoss = true, Row = Row }
	B.Z = Z
	self.Zombies[#self.Zombies + 1] = Z
	Scene.SpawnPrefab(Prefabs .. "Zombie_" .. Row.Name .. ".eprefab", Vector3(Entrance.Pos.X, Entrance.Pos.Y, 0), function(E)
		Z.Entity = E
		local C = E:GetComponent("FarmZombieComponent")
		Z.Comp = C
		C.Hp, C.MaxHp = Row.Hp * HpMul, Row.Hp * HpMul
		C.Speed, C.Damage, C.AttackInterval = Row.Speed, Row.Damage, Row.Interval
		C.StructureDamageMul, C.CropEatTime, C.BodyRadius = Row.StructureMul, Row.CropEat, Row.Radius
		C.Boss, C.AggroTime, C.Alerted = true, Row.AggroTime, true
		C.SummonKind, C.SummonInterval, C.SummonCount = Row.SummonKind, Row.SummonInterval, Row.SummonCount
		C.AuraRadius, C.AuraStructureDps, C.AuraHeal, C.AuraSlow = Row.AuraRadius, Row.AuraStructureDps, Row.AuraHeal, Row.AuraSlow
	end)
	self:Sfx("Roar", 0.9)
	self:Hud():Announce(Row.DisplayName, Row.Kind == "Season" and "계절의 끝을 알리는 보스가 나타났다!" or "보스가 크리스탈을 노린다!", 3.5)
	Log.Info(string.format("[FarmBie] 보스 등장: %s (%s)", Row.DisplayName, Row.Name))
end

function Boss:IsBossAlive()
	for _, B in ipairs(self.Bosses or {}) do
		if not B.bSpawned or not B.bDead then return true end
	end
	return false
end

function Boss:UpdateBoss(Dt)
	local Dc = self.Defense
	if not Dc or self.MapId ~= "Farm" then return end
	-- 소환 요청 (C++ → 여기서 생성)
	if Dc.SummonRequests ~= "" then
		local Requests = Dc.SummonRequests
		Dc.SummonRequests = ""
		local HpMul = self.TonightPlan and self.TonightPlan.HpMul or 1.0
		for Kind, X, Y in string.gmatch(Requests, "(%a+),([-%d]+),([-%d]+);") do
			self:SpawnZombie(Kind, { Name = "Summon", Pos = Vector3(tonumber(X), tonumber(Y), 0) }, HpMul)
			self.Report.Summoned = (self.Report.Summoned or 0) + 1
		end
	end
	if self.Phase ~= "Night" or #self.Bosses == 0 then
		self:Hud():Show("BossPanel", false)
		return
	end
	if self.BossSpawnAt and self:NightElapsed() >= self.BossSpawnAt then
		self.BossSpawnAt = nil
		for _, B in ipairs(self.Bosses) do self:SpawnBoss(B) end
	end
	-- 처치 확인 + HP 막대 (살아 있는 첫 보스)
	local Shown
	for _, B in ipairs(self.Bosses) do
		local Z = B.Z
		if Z and not B.bDead and Z.Comp then
			local bGone = not (Z.Entity and Z.Entity:IsValid())
			if bGone or Z.Comp.Dead then
				B.bDead = true
				self:OnBossDefeated(B.Row)
			elseif not Shown then
				Shown = B
			end
		end
	end
	local Hud = self:Hud()
	Hud:Show("BossPanel", Shown ~= nil)
	if Shown then
		Hud:Set("BossName", "Text", Shown.Row.DisplayName)
		Hud:Set("BossBar", "Percent", math.floor(Shown.Z.Comp.Hp / math.max(1, Shown.Z.Comp.MaxHp) * 200 + 0.5) / 200)
	end
end

function Boss:OnBossDefeated(Row)
	self:AddGold(Row.RewardGold)
	local Parts = { string.format("%d골드", Row.RewardGold) }
	for Key, Count in string.gmatch(Row.RewardItems, "([%w]+)%*(%d+)") do
		self:Give(Key, tonumber(Count), true)
		Parts[#Parts + 1] = string.format("%s ×%s", self:ItemInfo(Key).Name, Count)
	end
	if Row.Kind == "Season" then
		-- 다음 계절 전용 희귀종 씨앗 (레어)
		local Next = SeasonIds[(self.Season + 1) % 4 + 1]
		for _, C in ipairs(D.Rows("Crops.etable")) do
			if C.Exclusive and C.Season == Next then
				self:Give(string.format("Seed:%s:1", C.Name), 2, true)
				Parts[#Parts + 1] = C.DisplayName .. " 씨앗 (레어) ×2"
			end
		end
	end
	self.Report.BossKills = (self.Report.BossKills or 0) + 1
	self:Sfx("Bell", 0.8)
	self.Report["BossKill_" .. Row.Name] = 1
	self:Hud():Announce(Row.DisplayName .. " 처치!", "보상: " .. table.concat(Parts, ", "), 4.0)
	Log.Info(string.format("[FarmBie] 보스 처치: %s — %s", Row.DisplayName, table.concat(Parts, ", ")))
end

-- 밤 끝 (FarmDefense:OnNightEnd): 살아 있는 보스 → 패배 벌칙
function Boss:OnBossNightEnd(Notes)
	local R = self.BossRules
	for _, B in ipairs(self.Bosses or {}) do
		if not B.bDead then
			if B.Row.Kind == "Mid" then
				self.bForceCollapse = true -- 쓰러짐: 잠 전환이 Collapse가 되어 아침에 소지금·늦은 기상 벌칙
				Notes[#Notes + 1] = B.Row.DisplayName .. "을(를) 막지 못해 쓰러졌다"
			else
				local Loss = math.floor(self.Gold * R.SeasonGoldLoss + 0.5)
				self.Gold = self.Gold - Loss
				self:LoseSanity(R.SeasonSanityLoss, "계절 보스 패배")
				Notes[#Notes + 1] = string.format("%s에게 졌다 — 소지금 -%d, 정신력 -%d", B.Row.DisplayName, Loss, R.SeasonSanityLoss)
			end
			self.Report.BossLosses = (self.Report.BossLosses or 0) + 1
			Log.Info("[FarmBie] 보스 패배: " .. B.Row.DisplayName)
		end
	end
	self.Bosses = {}
	self:Hud():Show("BossPanel", false)
end

return Boss
