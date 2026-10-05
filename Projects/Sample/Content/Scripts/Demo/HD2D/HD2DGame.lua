-- HD-2D 데모 게임 관리 (Scenes/Demo/HD2D.escene의 "HD2DGame" 엔티티 — Tools/DemoMap/HD2DGameplay.py가 배치·속성을 쓴다. 다른 맵도 같은 스크립트).
--   맡는 것: 적 자리(소환·부활)·보스, 보물상자·마을 사람·소품(프리팹으로 만들고 상호작용), 효과 조각·투사체·줍는 것·독 웅덩이
--            (스크립트 없는 FxSprite 프리팹 조각 — 만든 직후 콜백에서 모양을 정하고 여기서 움직이고 지운다), 화면 흔들림, 자동 검증 결과 판정.
--   확장 모듈(메서드를 이 클래스에 붙인다): HD2DParty.lua = 일행·장비·퀘스트·서브 퀘스트·저장/불러오기·맵 이동 세션,
--            HD2DMenu.lua = 타이틀·대화·인벤토리(탭)·상점 메뉴·엔딩, HD2DDungeon.lua = 던전 장치(가시 함정·보스 방 문·수정 가시 분출).
--   보스: 맵마다 BossKind(골렘 = HD2DBoss.lua, 수정 거미 여왕 = HD2DSpiderQueen.lua). 처치 여부는 맵별(IsBossDefeated), 퀘스트 단계의 BossGoal이
--            이 맵이면 다음 단계로. 자동 검증이 맵을 건너가면(동굴 → 마을) 시나리오를 Persistent "HD2D_AutoScenario"로 넘긴다.
--   시작 순서: 맵 이동으로 왔으면 세션을 불러와 Spawn_<이름>에 플레이어를 둔다 → 아니면 Title 속성이 켜진 맵은 타이틀 화면 → 처음부터(시작 연출)/이어하기.
--   다른 스크립트는 Scene.Find("HD2DGame"):GetScript()로 쓴다 (플레이어 = HD2DPlayer.lua, 적 = HD2DEnemy.lua, 보스 = HD2DBoss.lua, UI = HD2DHud.lua).
--   좌표: 카메라가 +Y 쪽에서 -Y를 내려다본다 → 화면 오른쪽 = +X, 화면 위(안쪽) = -Y. 스프라이트는 XZ 평면(앞면 +Y)에 서 있다.
--   수치·대사: HD2DData.lua (Data/Demo/HD2D/*). 난수는 결정적(LCG) — 자동 검증 화면이 실행마다 같도록 math.random을 쓰지 않는다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local HD2DGame = {
	Properties = {
		Map      = "Village", -- 맵 id (저장·연 상자 키)
		Enemies  = "",  -- "종류,x,y,z;..." (z = 캡슐 중심)
		Npcs     = "",  -- "id,x,y,z;..."
		Chests   = "",  -- "x,y,z,아이템*개수+...;..." (Gold*n = 골드)
		Props    = "",  -- "id,x,y,z;..." (SavePoint / Cat)
		Boss     = "",  -- "x,y,z" (z = 캡슐 중심)
		BossKind = "Golem", -- 보스 프리팹 / Enemies.etable 행 (동굴 = SpiderQueen)
		BossLift = 179.0,   -- 보스 캡슐 중심 - 발 (보상 상자 높이)
		BossReward = "HiPotion*2+Gold*100", -- 보스 보상 상자 내용
		Respawn  = true,    -- 쓰러진 적이 다시 나타나는가 (던전은 끔)
		Traps    = "",  -- 가시 함정판 "x,y,z,반폭X,반폭Y;..." (HD2DDungeon.lua — 씬의 Trap_<번호> 가시 묶음)
		Gate     = "",  -- 보스 방 문 "x,y,z,창살 이동" (CaveGate 프리팹)
		Arena    = "",  -- 보스 방 "x,y,반지름" (들어서면 문이 닫힌다)
		Path     = "",  -- "x,y;..." 마을 → 들판 길 (내비메시가 없을 때 자동 조종이 따라 걷는다)
		AutoPlay = "",  -- "" | Full | TitleShot | Inventory | Equip | Shop | Dialog | Combat | Boost | Boss (HD2DAutoPilot.lua)
		Title    = false, -- 시작할 때 타이틀 화면 (맵 이동으로 온 경우는 건너뜀)
		RespawnMinDistance = 900.0,
	},
}
for _, Module in ipairs({ "Scripts/Demo/HD2D/HD2DParty.lua", "Scripts/Demo/HD2D/HD2DMenu.lua", "Scripts/Demo/HD2D/HD2DDungeon.lua" }) do
	for Name, Fn in pairs(Script.Require(Module)) do HD2DGame[Name] = Fn end
end

local Prefabs = "Prefabs/Demo/HD2D/"
local FxSprite = "Sprites/HD2D/Fx.esprite"
local FxLife = { Slash = 0.18, Spark = 0.17, Dust = 0.29, Poof = 0.36, Thrust = 0.21, Burst = 0.31, Sparkle = 0.33, Alert = 0.21 }
local Snd = {
	Open = "Asset/Kenney_InterfaceSounds/open.wav", Close = "Asset/Kenney_InterfaceSounds/close.wav", Move = "Asset/Kenney_InterfaceSounds/click.wav",
	Confirm = "Asset/Kenney_InterfaceSounds/confirm.wav", Error = "Asset/Kenney_InterfaceSounds/error.wav", Pickup = "Asset/Kenney_InterfaceSounds/pickup_item.wav",
	Potion = "Asset/Kenney_InterfaceSounds/potion.wav", Coin = "Asset/Kenney_RPGAudio/coins_pickup.wav", Buy = "Asset/Kenney_RPGAudio/coins_buy.wav",
	Equip = "Asset/Kenney_RPGAudio/equip.wav", Drop = "Asset/Kenney_RPGAudio/item_drop.wav",
}
HD2DGame.Sounds = Snd

local function Flat(V) return Vector3(V.X, V.Y, 0) end
local function Parse3(Text)
	local X, Y, Z = string.match(Text, "([-%d%.]+),([-%d%.]+),([-%d%.]+)")
	return Vector3(tonumber(X), tonumber(Y), tonumber(Z))
end

-- 화면 각 (반시계 +, 도) — 바닥 방향 (X, Y)가 화면에 보이는 방향. 카메라 피치 28도라 안쪽(-Y)은 sin(28) ≈ 0.47배로 줄어 보인다
function HD2DGame.ScreenAngle(V)
	return math.deg(math.atan(-V.Y * 0.47, V.X))
end

function HD2DGame:OnStart()
	self.Time = 0.0
	self.Seed = 12345
	self.Shake, self.ShakeTime = 0.0, 0.0
	self.Enemies = {}      -- 엔티티 Id → 적 스크립트 (보스 포함)
	self.Fx = {}           -- { Entity, Life, Max, Fade, Color, Vel, Grow, Scale }
	self.Projectiles = {}
	self.Pickups = {}
	self.Hazards = {}
	self.Slots = {}        -- 적 자리 { Kind, Pos, Respawn(초, nil = 살아 있음/대기 없음) }
	self.SlotOf = {}       -- 적 엔티티 Id → 자리 번호
	self.Chests = {}
	self.Npcs = {}
	self.Props_ = {}
	self.Report = { Kills = {}, WeaponHits = {}, Chests = 0, Bought = {}, Used = {}, InventoryOpened = 0, Dialogs = 0, LevelUps = 0,
	                Pickups = 0, Projectiles = 0, PoisonTicks = 0, Equips = 0, GearEquips = 0, BossPatterns = {}, Boosts = 0, BoostMax = 0,
	                SubDone = 0, Saves = 0, TrapRises = 0, TrapHits = 0, Eruptions = 0, GateCloses = 0, GateOpens = 0, Afterimages = 0, Endings = 0 }
	-- 맵 이동으로 이어지는 자동 검증 (동굴 → 마을): 씬 속성이 비었으면 앞 씬이 넘긴 시나리오
	if self.Properties.AutoPlay == "" then
		self.Properties.AutoPlay = Game.GetPersistent("HD2D_AutoScenario", "")
	end
	self.Menu = nil        -- nil | "Title" | "Dialog" | "Inventory" | "Shop" | "Travel"
	self.Mode = "Play"     -- Title | Intro | Play | Travel
	self:InitParty()
	local bSession = self:TryResumeSession()

	for Item in string.gmatch(self.Properties.Enemies, "[^;]+") do
		local Kind, Rest = string.match(Item, "^(%a+),(.+)$")
		if Kind then
			self.Slots[#self.Slots + 1] = { Kind = Kind, Pos = Parse3(Rest) }
			self:SpawnSlot(#self.Slots)
		end
	end
	for Item in string.gmatch(self.Properties.Npcs, "[^;]+") do
		local Id, Rest = string.match(Item, "^(%a+),(.+)$")
		if Id then self:SpawnNpc(Id, Parse3(Rest)) end
	end
	local Index = 0
	for Item in string.gmatch(self.Properties.Chests, "[^;]+") do
		local X, Y, Z, Contents = string.match(Item, "([-%d%.]+),([-%d%.]+),([-%d%.]+),(.+)")
		if X then
			Index = Index + 1
			self:SpawnChest(Vector3(tonumber(X), tonumber(Y), tonumber(Z)), Contents, false, self.Properties.Map .. ":" .. Index)
		end
	end
	for Item in string.gmatch(self.Properties.Props, "[^;]+") do
		local Id, Rest = string.match(Item, "^(%a+),(.+)$")
		if Id then self:SpawnProp(Id, Parse3(Rest)) end
	end
	if self.Properties.Boss ~= "" then
		self.BossPos = Parse3(self.Properties.Boss)
		if not self:IsBossDefeated() then
			Scene.SpawnPrefab(Prefabs .. self.Properties.BossKind .. ".eprefab", self.BossPos)
		elseif not self.Opened[self.Properties.Map .. ":Boss"] then
			self:SpawnChest(self.BossPos + Vector3(0, 170, -self.Properties.BossLift + 47), self.Properties.BossReward, false, self.Properties.Map .. ":Boss")
		end
	end
	self:InitDungeon()
	self.Path = {}
	for X, Y in string.gmatch(self.Properties.Path, "([-%d%.]+),([-%d%.]+)") do
		self.Path[#self.Path + 1] = Vector3(tonumber(X), tonumber(Y), 0)
	end
	if bSession then
		self:Hud():FadeFrom(1.0, 0.6)
	elseif self.Properties.Title then
		self.bOpenTitle = true -- HUD·플레이어가 준비된 첫 갱신에서
	end
	Log.Info(string.format("[HD2D] 시작: 맵 %s, 적 자리 %d곳, 마을 사람 %d명, 보물상자 %d개, 소품 %d개, 보스 %s(%s), 함정 %d, 시나리오 '%s'%s",
		self.Properties.Map, #self.Slots, #self.Npcs, #self.Chests, #self.Props_, self.Properties.BossKind,
		self.BossPos and (self:IsBossDefeated() and "처치함" or "있음") or "없음", #self.Traps, self.Properties.AutoPlay, bSession and ", 맵 이동 세션" or ""))
end

-- 결정적 난수 (0~1)
function HD2DGame:Random()
	self.Seed = (self.Seed * 1103515245 + 12345) % 2147483648
	return (self.Seed % 100000) / 100000.0
end

function HD2DGame:GetPlayer()
	if not self.PlayerScript then
		local P = Scene.Find("Player")
		self.PlayerScript = P and P:GetScript() or nil
	end
	return self.PlayerScript
end

function HD2DGame:Hud()
	if not self.HudScript then
		local H = Scene.Find("HUD")
		self.HudScript = H and H:GetScript() or nil
	end
	return self.HudScript
end

-- ================================================================ 적
function HD2DGame:SpawnSlot(Index)
	local Slot = self.Slots[Index]
	Slot.Respawn = nil
	Scene.SpawnPrefab(Prefabs .. Slot.Kind .. ".eprefab", Slot.Pos, function(E) self.SlotOf[E.Id] = Index end)
end

function HD2DGame:SpawnEnemy(Kind, Pos)
	Scene.SpawnPrefab(Prefabs .. Kind .. ".eprefab", Pos)
end

function HD2DGame:RegisterEnemy(S) self.Enemies[S.entity.Id] = S end
function HD2DGame:UnregisterEnemy(S) self.Enemies[S.entity.Id] = nil end

-- 적이 죽을 때 부른다 (스크립트가 스스로 엔티티를 지우기 전)
function HD2DGame:OnEnemyKilled(S)
	local Row = S.Row
	local Pos = S.entity:GetWorldPosition()
	self.Report.Kills[S.Kind] = (self.Report.Kills[S.Kind] or 0) + 1
	-- 전리품: 골드 동전 몇 개 + 확률 아이템 (+ 대장간 의뢰 중이면 슬라임 젤리)
	local Gold = Row.GoldMin + math.floor(self:Random() * (Row.GoldMax - Row.GoldMin + 1))
	local Ground = Vector3(Pos.X, Pos.Y, Pos.Z + S.Foot + 2)
	self:DropLoot(Ground, Gold, (Row.DropItem ~= "" and self:Random() < Row.DropChance) and Row.DropItem or nil, S.bBoss and 8 or 3)
	if S.Kind == "Slime" and self:SubState("Smith") == "Active" and self:Count("Jelly") + self:PendingPickups("Jelly") < D.SubQuest("Smith").Count
		and self:Random() < 0.85 then
		self:SpawnPickup(Ground, "Jelly", 1)
	end
	local Player = self:GetPlayer()
	if Player then Player:AddExp(Row.Exp) end
	local SlotIndex = self.SlotOf[S.entity.Id]
	if SlotIndex and Row.RespawnTime > 0 and self.Properties.Respawn then
		self.Slots[SlotIndex].Respawn = Row.RespawnTime
	end
	if not S.bBoss and self.QuestStage == 1 then
		self.QuestKills = self.QuestKills + 1
		self:RefreshQuest()
		local Goal = D.Quest(1).KillGoal
		if self.QuestKills == Goal then
			self:Hud():Announce("마물 퇴치 완료!", "촌장 바르톨로에게 보고하자", 2.5)
			Audio.PlayOneShot(Snd.Confirm)
		end
	end
	if not S.bBoss then self:CountSubQuestKill(Pos) end
	Log.Info(string.format("[HD2D] 처치: %s (골드 %d, 경험치 %d)", Row.DisplayName, Gold, Row.Exp))
end

function HD2DGame:PendingPickups(Id)
	local N = 0
	for _, P in ipairs(self.Pickups) do
		if P.Id == Id then N = N + P.Value end
	end
	return N
end

function HD2DGame:OnBossKilled(S)
	local Pos = S.entity:GetWorldPosition()
	self.bBossDead = true
	self:SetBossDefeated()
	self:Hud():ShowBoss(nil)
	-- 이 맵 보스가 지금 퀘스트 목표면 다음 단계 (마을 골렘 = 2 → 3, 동굴 여왕 = 4 → 5)
	local Q = D.Quest(self.QuestStage)
	local bGoal = Q and Q.BossGoal == self.Properties.Map
	self:Hud():Announce(S.Row.DisplayName .. "을(를) 쓰러뜨렸다!", bGoal and D.Quest(self.QuestStage + 1).Objective or nil, 3.5)
	self:Fanfare()
	self:SpawnChest(Pos + Vector3(0, 170, S.Foot + 47), self.Properties.BossReward, true, self.Properties.Map .. ":Boss") -- 상자 루트 = 지면 + 45
	self:OnDungeonBossKilled()
	if bGoal then
		self:SetQuestStage(self.QuestStage + 1, true)
	end
end

-- 살아 있는 적 스크립트 목록 (보스 포함)
function HD2DGame:AliveEnemies()
	local List = {}
	for _, S in pairs(self.Enemies) do
		if S.entity:IsValid() and not S.bDead and not S.bDormant then
			List[#List + 1] = S
		end
	end
	table.sort(List, function(A, B) return A.entity.Id < B.entity.Id end) -- 결정적 순서
	return List
end

-- 중심에서 수평 반경 안 (적 판정 반지름 포함)
function HD2DGame:FindEnemies(Center, Radius)
	local Found = {}
	for _, S in ipairs(self:AliveEnemies()) do
		local D_ = Flat(S.entity:GetWorldPosition() - Center):Length()
		if D_ <= Radius + S.Radius then Found[#Found + 1] = S end
	end
	return Found
end

-- 원점에서 방향 Dir로 Range 안, 부채꼴 Arc(도) 안 (적 판정 반지름만큼 너그럽게)
function HD2DGame:FindEnemiesInCone(Origin, Dir, Range, Arc)
	local Found = {}
	local CosHalf = math.cos(math.rad(Arc * 0.5))
	for _, S in ipairs(self:AliveEnemies()) do
		local To = Flat(S.entity:GetWorldPosition() - Origin)
		local L = To:Length()
		if L <= Range + S.Radius then
			if L < S.Radius + 20 or (To * (1.0 / L)):Dot(Dir) >= CosHalf - S.Radius / math.max(L, 1) then
				Found[#Found + 1] = S
			end
		end
	end
	return Found
end

-- 방향 Dir 직선(폭 Width) 위 Range 안 (찌르기)
function HD2DGame:FindEnemiesInLine(Origin, Dir, Range, Width)
	local Found = {}
	for _, S in ipairs(self:AliveEnemies()) do
		local To = Flat(S.entity:GetWorldPosition() - Origin)
		local Along = To:Dot(Dir)
		local Side = (To - Dir * Along):Length()
		if Along > -S.Radius and Along <= Range + S.Radius and Side <= Width + S.Radius then
			Found[#Found + 1] = S
		end
	end
	return Found
end

function HD2DGame:NearestEnemy(Pos, MaxDist, Filter)
	local Best, BestDist = nil, MaxDist or 1.0e9
	for _, S in ipairs(self:AliveEnemies()) do
		if Filter == nil or Filter(S) then
			local L = Flat(S.entity:GetWorldPosition() - Pos):Length()
			if L < BestDist then Best, BestDist = S, L end
		end
	end
	return Best, BestDist
end

-- 플레이어 공격 한 대 (근접·투사체 공용). 맞혔으면 true
function HD2DGame:HitEnemy(S, Damage, Dir, Knockback, bCrit, WeaponId)
	if not S:TakeHit(Damage, Dir, Knockback) then return false end
	local P = S.entity:GetWorldPosition()
	self:DamageNumber(P + Vector3(0, 0, S.HitHeight or 60), tostring(Damage), bCrit and { 1, 0.86, 0.3, 1 } or { 1, 1, 1, 1 }, bCrit and 1.35 or 1.0)
	self:SpawnFx("Spark", P + Vector3(0, 40, (S.HitHeight or 60) - 30), { Blend = 2, Scale = bCrit and 2.0 or 1.4 })
	if WeaponId then
		self.Report.WeaponHits[WeaponId] = (self.Report.WeaponHits[WeaponId] or 0) + 1
		local Player = self:GetPlayer()
		if Player then Player:OnHitLanded() end
	end
	return true
end

function HD2DGame:UpdateSlots(Dt)
	local Player = self:GetPlayer()
	local PP = Player and Player.entity:GetWorldPosition() or Vector3(0, 0, 0)
	for Index, Slot in ipairs(self.Slots) do
		if Slot.Respawn then
			Slot.Respawn = Slot.Respawn - Dt
			if Slot.Respawn <= 0 then
				if Flat(Slot.Pos - PP):Length() >= self.Properties.RespawnMinDistance then
					self:SpawnSlot(Index)
				else
					Slot.Respawn = 2.0 -- 플레이어가 가까우면 조금 뒤에
				end
			end
		end
	end
end

-- ================================================================ 마을 사람 · 보물상자 · 소품 · 상호작용
function HD2DGame:SpawnNpc(Id, Pos)
	local Row = D.Npc(Id)
	if not Row then
		Log.Error("[HD2D] 마을 사람 표에 없음:", Id)
		return
	end
	local Npc = { Id = Id, Row = Row, Pos = Pos }
	self.Npcs[#self.Npcs + 1] = Npc
	Scene.SpawnPrefab(Prefabs .. "Npc.eprefab", Pos, function(E)
		Npc.Entity = E
		Npc.Body = E:FindChild("Body")
		Npc.Marker = E:FindChild("Marker")
		Npc.Body:PlayFlipbook(Row.Flipbook)
		self:RefreshMarkers()
	end)
end

-- 보물상자: Key = "맵:번호" — 이미 연 상자(저장)는 열린 모양으로 만든다
function HD2DGame:SpawnChest(Pos, Contents, bReward, Key)
	local Chest = { Pos = Pos, Contents = Contents, bOpened = Key ~= nil and self.Opened[Key] == true, Key = Key }
	self.Chests[#self.Chests + 1] = Chest
	Scene.SpawnPrefab(Prefabs .. "Chest.eprefab", Pos, function(E)
		Chest.Entity = E
		Chest.Body = E:FindChild("Body")
		if Chest.bOpened then
			Chest.Body:GetComponent("SpriteComponent").Slice = "Chest2"
		end
		if bReward then
			self:SpawnFx("Poof", Pos + Vector3(0, 20, -20), { Scale = 1.4 })
			self:SpawnFx("Sparkle", Pos + Vector3(0, 30, 30), { Blend = 2, Scale = 1.6 })
		end
	end)
	return Chest
end

-- 소품: SavePoint = 게시판(저장), Cat = 서브 퀘스트 고양이 (받은 뒤 아직 못 찾았을 때만 보임)
function HD2DGame:SpawnProp(Id, Pos)
	local Prop = { Id = Id, Pos = Pos }
	self.Props_[#self.Props_ + 1] = Prop
	Scene.SpawnPrefab(Prefabs .. (Id == "Cat" and "PropSmall.eprefab" or "Prop.eprefab"), Pos, function(E)
		Prop.Entity = E
		Prop.Body = E:FindChild("Body")
		Prop.Marker = E:FindChild("Marker")
		local S = Prop.Body:GetComponent("SpriteComponent")
		if Id == "Cat" then
			Prop.Body:PlayFlipbook("Sprites/HD2D/Cat_Idle.eflipbook")
			Prop.Body:SetPosition(Vector3(0, 0, -45))
		else
			S.Slice = "Board"
		end
		self:RefreshProps()
	end)
end

function HD2DGame:IsPropActive(Prop)
	if Prop.Id == "Cat" then
		return self:SubState("Cat") == "Active" and self:Count("LostCat") == 0
	end
	return true
end

function HD2DGame:RefreshProps()
	for _, Prop in ipairs(self.Props_) do
		if Prop.Body then
			local bShow = self:IsPropActive(Prop)
			local S = Prop.Body:GetComponent("SpriteComponent")
			if S.Visible ~= bShow then S.Visible = bShow end
			local Shadow = Prop.Entity:FindChild("Shadow")
			if Shadow then Shadow:GetComponent("SpriteComponent").Visible = bShow end
			local M = Prop.Marker and Prop.Marker:GetComponent("SpriteComponent")
			if M then M.Visible = bShow and Prop.Id == "Cat" end
		end
	end
end

function HD2DGame:RefreshMarkers()
	for _, Npc in ipairs(self.Npcs) do
		if Npc.Marker then
			local bShow = false
			local Role = Npc.Row.Role
			if Role == "Elder" then
				bShow = self:CanAdvanceByTalk()
			elseif Role == "Quest" then
				local Id = Npc.Row.SubQuest
				bShow = self:SubState(Id) == nil or self:SubReady(Id)
			end
			local S = Npc.Marker:GetComponent("SpriteComponent")
			if S.Visible ~= bShow then S.Visible = bShow end
		end
	end
end

-- 플레이어가 매 프레임 부른다: 가장 가까운 상호작용 대상 → 안내 표시
function HD2DGame:UpdateInteract(Pos)
	local Best, BestDist, Text = nil, 1.0e9, nil
	for _, Npc in ipairs(self.Npcs) do
		local L = Flat(Npc.Pos - Pos):Length()
		if Npc.Entity and L < 210 and L < BestDist then
			Best, BestDist, Text = Npc, L, "E  " .. Npc.Row.DisplayName .. "와(과) 이야기하기"
		end
	end
	for _, Chest in ipairs(self.Chests) do
		local L = Flat(Chest.Pos - Pos):Length()
		if Chest.Entity and not Chest.bOpened and L < 160 and L < BestDist then
			Best, BestDist, Text = Chest, L, "E  보물상자 열기"
		end
	end
	for _, Prop in ipairs(self.Props_) do
		local L = Flat(Prop.Pos - Pos):Length()
		if Prop.Entity and self:IsPropActive(Prop) and L < 170 and L < BestDist then
			Best, BestDist, Text = Prop, L, Prop.Id == "Cat" and "E  고양이를 안아 올리기" or "E  게시판에 모험을 기록하기 (저장)"
		end
	end
	self.Target = Best
	local H = self:Hud()
	if H then H:ShowPrompt(self.Menu == nil and Text or nil) end
	return Best
end

function HD2DGame:Interact()
	local T = self.Target
	if not T then return false end
	if T.Contents then
		self:OpenChest(T)
	elseif T.Row then
		self:TalkTo(T)
	elseif T.Id == "Cat" then
		self:AddItem("LostCat", 1, true)
		self:SpawnFx("Sparkle", T.Pos + Vector3(0, 30, 10), { Blend = 2, Scale = 1.2 })
		Audio.PlayOneShot(self.Sounds.Pickup)
		self:StartDialog("", { "아르펜|Hero|찾았다, 미미! 리나가 걱정하고 있어. 같이 돌아가자.", "|Cat|고양이는 \"냐아\" 하고 품에 얌전히 안겼다." },
			function() self:OnSubQuestChanged() end)
		self:OnSubQuestChanged()
	elseif T.Id == "SavePoint" then
		self:StartDialog("", { "||낡은 게시판에 지금까지의 모험을 적어 두었다." }, function()
			if self:SaveGame() then
				self:Hud():Toast("UI/Demo/HD2D/Icons/Coin.png", "기록했다")
				self:SpawnHealFx(self:GetPlayer().entity:GetWorldPosition(), { 1, 0.9, 0.6, 1 })
			end
		end)
	end
	return true
end

function HD2DGame:OpenChest(Chest)
	Chest.bOpened = true
	if Chest.Key then self.Opened[Chest.Key] = true end
	Chest.Body:PlayFlipbook("Sprites/HD2D/Chest_Open.eflipbook")
	self:SpawnFx("Sparkle", Chest.Pos + Vector3(0, 30, 40), { Blend = 2, Scale = 1.6 })
	self:SpawnSprite({ Sprite = FxSprite, Slice = "Pillar", Position = Chest.Pos + Vector3(0, 20, -45), Blend = 2, Life = 0.7, Fade = true,
	                   Color = { 1, 0.85, 0.5, 0.9 }, Scale = 1.2 })
	Audio.PlayOneShot(Snd.Open)
	Audio.PlayOneShot(Snd.Pickup)
	for Part in string.gmatch(Chest.Contents, "[^+]+") do
		local Id, N = string.match(Part, "(%a+)%*(%d+)")
		if Id == "Gold" then
			self:AddGold(tonumber(N), true)
		elseif Id then
			self:AddItem(Id, tonumber(N), true)
		end
	end
	self.Report.Chests = self.Report.Chests + 1
	Log.Info("[HD2D] 보물상자 열림: " .. Chest.Contents)
end

function HD2DGame:TalkTo(Npc)
	local Row = Npc.Row
	if Row.Role == "Elder" then
		local Stage = self.QuestStage
		if self:CanAdvanceByTalk() then
			local Next = D.Quest(Stage + 1)
			self:StartDialog(Row.DisplayName, Next.Lines, function() self:SetQuestStage(Stage + 1) end, Row.Portrait)
		else
			self:StartDialog(Row.DisplayName, D.Quest(Stage).WaitLines, nil, Row.Portrait)
		end
	elseif Row.Role == "Shop" then
		self:StartDialog(Row.DisplayName, Row.Lines, function() self:OpenShop() end, Row.Portrait)
	elseif Row.Role == "Quest" then
		self:TalkSubQuest(Npc)
	else
		self:StartDialog(Row.DisplayName, Row.Lines, nil, Row.Portrait)
	end
end

-- ================================================================ 효과 조각
-- Opt: Rotation(화면 반시계 도), FlipX, FlipY, Scale(수 또는 {x, z}), Color {r,g,b,a}, Blend(0 알파/2 가산/3 마스크), Life, Lit, Flat(바닥에 눕힘),
--      Vel(cm/s), Grow(배율/초), Fade
function HD2DGame:SpawnFx(Name, Position, Opt)
	Opt = Opt or {}
	return self:SpawnSprite({ Sprite = FxSprite, Flipbook = "Sprites/HD2D/Fx_" .. Name .. ".eflipbook", Position = Position, Rotation = Opt.Rotation,
	                          FlipX = Opt.FlipX, FlipY = Opt.FlipY, Scale = Opt.Scale, Color = Opt.Color, Blend = Opt.Blend or 0, Lit = Opt.Lit,
	                          Life = Opt.Life or FxLife[Name] or 0.3, Flat = Opt.Flat, Vel = Opt.Vel, Grow = Opt.Grow, Fade = Opt.Fade })
end

-- 잔상: 지금 슬라이스의 사본이 색 알파를 잃으며 사라진다
function HD2DGame:SpawnAfterimage(Sprite, Slice, Position, FlipX, Color, Life)
	self:SpawnSprite({ Sprite = Sprite, Slice = Slice, Position = Position, FlipX = FlipX, Blend = 0, Life = Life or 0.22, Fade = true,
	                   Color = Color or { 0.55, 0.8, 1.0, 0.55 } })
end

local FlatRotation = nil

-- 효과 조각 하나. 돌려준 표의 Entity는 다음 프레임(생성 콜백)에 채워진다
function HD2DGame:SpawnSprite(Desc)
	local F = { Life = Desc.Life, Max = Desc.Life, Fade = Desc.Fade, Color = Desc.Color or { 1, 1, 1, 1 }, Vel = Desc.Vel, Grow = Desc.Grow,
	            Scale = Desc.Scale or 1, Pos = Desc.Position }
	self.Fx[#self.Fx + 1] = F
	Scene.SpawnPrefab("Prefabs/Demo/HD2D/FxSprite.eprefab", Desc.Position, function(E)
		F.Entity = E
		local S = E:GetComponent("SpriteComponent")
		S.Sprite = Desc.Sprite
		S.Slice = Desc.Slice or ""
		S.Blend = Desc.Blend or 0
		S.Lit = Desc.Lit == true
		local C = F.Color
		S.Color = Vector4(C[1], C[2], C[3], C[4])
		S.Visible = true
		if Desc.FlipX or Desc.FlipY then E:SetSpriteFlip(Desc.FlipX == true, Desc.FlipY == true) end
		if Desc.Flat then
			FlatRotation = FlatRotation or Quat.FromEuler(0, 0, -90)
			E:SetRotation(FlatRotation)
		elseif Desc.Rotation then
			E:SetRotation(Quat.FromAxisAngle(Vector3(0, 1, 0), -Desc.Rotation))
		end
		self:ApplyScale(F)
		if Desc.Flipbook then E:PlayFlipbook(Desc.Flipbook) end
	end)
	return F
end

function HD2DGame:ApplyScale(F)
	local Sc = F.Scale
	if type(Sc) == "table" then
		F.Entity:SetScale(Vector3(Sc[1], 1, Sc[2]))
	elseif Sc ~= 1 then
		F.Entity:SetScale(Vector3(Sc, Sc, Sc))
	end
end

function HD2DGame:UpdateFx(Dt)
	local Keep = {}
	for _, F in ipairs(self.Fx) do
		F.Life = F.Life - Dt
		local E = F.Entity
		if F.Life <= 0 or (E and not E:IsValid()) then
			if E and E:IsValid() then E:Destroy() end
		else
			if E then
				if F.Vel then
					F.Pos = F.Pos + F.Vel * Dt
					E:SetPosition(F.Pos)
				end
				if F.Grow then
					F.Scale = (type(F.Scale) == "table" and F.Scale[1] or F.Scale) + F.Grow * Dt
					self:ApplyScale(F)
				end
				if F.Fade then
					local C = F.Color
					E:GetComponent("SpriteComponent").Color = Vector4(C[1], C[2], C[3], C[4] * (F.Life / F.Max))
				end
			end
			Keep[#Keep + 1] = F
		end
	end
	self.Fx = Keep
end

function HD2DGame:KillFx(F)
	if F then F.Life = 0 end
end

-- 레벨 업·회복 연출: 빛기둥 + 반짝임
function HD2DGame:SpawnLevelFx(Pos)
	self:SpawnSprite({ Sprite = FxSprite, Slice = "Pillar", Position = Pos + Vector3(0, 20, -85), Blend = 2, Life = 0.9, Fade = true,
	                   Color = { 1, 0.9, 0.55, 0.95 }, Scale = { 1.3, 1.6 } })
	for I = 0, 2 do
		self:SpawnFx("Sparkle", Pos + Vector3((I - 1) * 50, 30, 20 + I * 35), { Blend = 2, Scale = 1.3 + I * 0.2, Life = 0.33 + I * 0.05 })
	end
end

function HD2DGame:SpawnHealFx(Pos, Color)
	self:SpawnFx("Sparkle", Pos + Vector3(0, 30, 30), { Blend = 2, Scale = 1.5, Color = Color })
	self:SpawnFx("Sparkle", Pos + Vector3(30, 30, 70), { Blend = 2, Scale = 1.0, Color = Color, Life = 0.4 })
end

function HD2DGame:DamageNumber(Pos, Text, Color, Scale)
	local H = self:Hud()
	if H then H:ShowDamage(Pos, Text, Color, Scale) end
end

-- ================================================================ 투사체
-- Desc: Kind(Arrow/Bolt/EnemyArrow/Rock), Pos, Dir(수평 단위), Speed, Range, Damage, Knockback, Splash, Team("Player"/"Enemy"), Crit, Weapon,
--       Rock은 Target(착지점)·Duration·Height (포물선)
function HD2DGame:SpawnProjectile(Desc)
	local P = { Kind = Desc.Kind, Pos = Desc.Pos, Dir = Desc.Dir, Speed = Desc.Speed or 1000, Travel = 0, Range = Desc.Range or 1200, Damage = Desc.Damage,
	            Knockback = Desc.Knockback or 300, Splash = Desc.Splash or 0, Team = Desc.Team or "Player", Crit = Desc.Crit, Weapon = Desc.Weapon,
	            Trail = 0, Time = 0 }
	if P.Kind == "Rock" then
		P.From, P.Target, P.Duration, P.Height = Desc.Pos, Desc.Target, Desc.Duration or 0.9, Desc.Height or 320
		P.Warn = self:SpawnSprite({ Sprite = FxSprite, Slice = "Warn", Position = Desc.Target + Vector3(0, 0, 3), Flat = true, Blend = 0, Life = P.Duration + 0.1,
		                            Scale = 1.2, Color = { 1, 1, 1, 0.9 } })
		P.Fx = self:SpawnFx("Rock", Desc.Pos, { Lit = true, Blend = 3, Scale = 1.6, Life = P.Duration + 0.2 })
	elseif P.Kind == "Bolt" then
		P.Fx = self:SpawnFx("Bolt", Desc.Pos, { Blend = 2, Scale = 1.4 * (Desc.Scale or 1), Life = 3.0 })
	elseif P.Kind == "Web" then
		-- 거미줄 탄 (수정 거미 여왕): 도는 실 뭉치
		P.Fx = self:SpawnSprite({ Sprite = "Sprites/HD2D/CaveFx.esprite", Flipbook = "Sprites/HD2D/CaveFx_Web.eflipbook", Position = Desc.Pos, Blend = 0,
		                          Life = 3.0, Scale = 1.5 * (Desc.Scale or 1) })
	else
		P.Fx = self:SpawnSprite({ Sprite = FxSprite, Slice = "Arrow", Position = Desc.Pos, Rotation = HD2DGame.ScreenAngle(Desc.Dir), Blend = 0,
		                          Life = 3.0, Scale = 1.3 * (Desc.Scale or 1), Color = P.Team == "Enemy" and { 1, 0.75, 0.75, 1 } or { 1, 1, 1, 1 } })
	end
	self.Projectiles[#self.Projectiles + 1] = P
	self.Report.Projectiles = self.Report.Projectiles + 1
	return P
end

function HD2DGame:ExplodeProjectile(P, Pos)
	if P.Kind == "Bolt" then
		self:SpawnFx("Burst", Pos + Vector3(0, 20, 0), { Blend = 2, Scale = 1.0 + P.Splash / 120.0 })
		self:AddShake(5, 0.12)
		Audio.PlayOneShot("Audio/RPG/HitHeavy.wav")
		for _, S in ipairs(self:FindEnemies(Pos, P.Splash)) do
			if not P.Hit[S] then
				local Away = Flat(S.entity:GetWorldPosition() - Pos)
				Away = Away:Length() > 1 and Away:Normalized() or P.Dir
				self:HitEnemy(S, math.floor(P.Damage * 0.6), Away, P.Knockback, false, P.Weapon)
			end
		end
	elseif P.Kind == "Rock" then
		self:SpawnFx("Poof", Pos + Vector3(0, 10, -20), { Scale = 1.3 })
		self:SpawnFx("Dust", Pos + Vector3(-40, 5, -10), { Scale = 1.4 })
		self:SpawnFx("Dust", Pos + Vector3(40, 5, -10), { Scale = 1.4, FlipX = true })
		self:AddShake(7, 0.15)
		Audio.PlayOneShot("Audio/RPG/HitHeavy.wav")
		local Player = self:GetPlayer()
		if Player and Flat(Player.entity:GetWorldPosition() - Pos):Length() < 140 then
			Player:TakeDamage(P.Damage, Pos)
		end
	end
end

function HD2DGame:UpdateProjectiles(Dt)
	local Keep = {}
	local Player = self:GetPlayer()
	for _, P in ipairs(self.Projectiles) do
		P.Time = P.Time + Dt
		local bDone = false
		if P.Kind == "Rock" then
			local T = math.min(P.Time / P.Duration, 1.0)
			local Pos = P.From + (P.Target - P.From) * T + Vector3(0, 0, math.sin(T * math.pi) * P.Height)
			if P.Fx.Entity then P.Fx.Entity:SetPosition(Pos) end
			if T >= 1.0 then
				self:ExplodeProjectile(P, P.Target)
				bDone = true
			end
		else
			local Step = P.Speed * Dt
			P.Pos = P.Pos + P.Dir * Step
			P.Travel = P.Travel + Step
			if P.Fx.Entity then P.Fx.Entity:SetPosition(P.Pos) end
			if P.Kind == "Bolt" then
				P.Trail = P.Trail - Dt
				if P.Trail <= 0 then
					P.Trail = 0.03
					self:SpawnSprite({ Sprite = FxSprite, Slice = "Bolt1", Position = P.Pos + Vector3(0, -2, 0), Blend = 2, Life = 0.22, Fade = true,
					                   Color = { 0.55, 0.7, 1.0, 0.8 }, Scale = 1.0, Grow = -3.0 })
				end
			end
			if P.Team == "Player" then
				P.Hit = P.Hit or {}
				for _, S in ipairs(self:FindEnemies(P.Pos, 30)) do
					if not P.Hit[S] then
						P.Hit[S] = true
						self:HitEnemy(S, P.Damage, P.Dir, P.Knockback, P.Crit, P.Weapon)
						if P.Kind == "Bolt" then
							self:ExplodeProjectile(P, P.Pos)
						end
						Game.HitStop(0.03)
						bDone = true
						break
					end
				end
			elseif Player and not Player.bDead then
				if Flat(Player.entity:GetWorldPosition() - P.Pos):Length() < 50 then
					Player:TakeDamage(P.Damage, P.Pos - P.Dir * 100)
					self:SpawnFx("Spark", P.Pos, { Blend = 2, Color = { 1, 0.5, 0.4, 1 } })
					bDone = true
				end
			end
			if P.Travel >= P.Range then
				if P.Kind == "Bolt" then self:ExplodeProjectile(P, P.Pos) end
				bDone = true
			end
		end
		if bDone then
			self:KillFx(P.Fx)
			self:KillFx(P.Warn)
		else
			Keep[#Keep + 1] = P
		end
	end
	self.Projectiles = Keep
end

-- ================================================================ 독 웅덩이 (독버섯)
function HD2DGame:SpawnPoison(Pos, Radius, Life, Damage)
	local Fx = self:SpawnFx("Poison", Pos + Vector3(0, 0, 3), { Flat = true, Blend = 0, Scale = Radius / 120.0, Life = Life, Color = { 1, 1, 1, 0.85 } })
	self.Hazards[#self.Hazards + 1] = { Pos = Pos, Radius = Radius, Life = Life, Damage = Damage, Tick = 0.0, Fx = Fx }
	self:SpawnFx("Poof", Pos + Vector3(0, 10, 10), { Scale = 1.4, Color = { 0.8, 0.55, 1.0, 1 } })
end

function HD2DGame:UpdateHazards(Dt)
	local Keep = {}
	local Player = self:GetPlayer()
	for _, H in ipairs(self.Hazards) do
		H.Life = H.Life - Dt
		H.Tick = H.Tick - Dt
		if H.Life > 0 then
			if Player and H.Tick <= 0 and Flat(Player.entity:GetWorldPosition() - H.Pos):Length() < H.Radius then
				H.Tick = 0.5
				if Player:TakeDamage(H.Damage, H.Pos, { bNoKnockback = true, bNoInvuln = true, Color = { 0.75, 0.45, 1.0, 1 } }) then
					self.Report.PoisonTicks = self.Report.PoisonTicks + 1
				end
			end
			Keep[#Keep + 1] = H
		end
	end
	self.Hazards = Keep
end

-- ================================================================ 줍는 것 (골드·아이템)
-- Pos = 지면 위 자리
function HD2DGame:DropLoot(Pos, Gold, ItemId, Pieces)
	Pieces = math.max(1, math.min(Pieces or 3, Gold))
	local Base = Vector3(Pos.X, Pos.Y, Pos.Z)
	for I = 1, Pieces do
		local Value = math.floor(Gold / Pieces) + ((I <= Gold % Pieces) and 1 or 0)
		self:SpawnPickup(Base, "Gold", Value)
	end
	if ItemId then self:SpawnPickup(Base, ItemId, 1) end
end

function HD2DGame:SpawnPickup(Pos, Id, Value)
	local A = self:Random() * math.pi * 2
	local Speed = 120 + self:Random() * 180
	-- Pos = 떨어진 자리의 지면 높이 (동전 피벗 = 아래)
	local P = { Id = Id, Value = Value, Pos = Pos + Vector3(0, 0, 40), Ground = Pos.Z, Vel = Vector3(math.cos(A) * Speed, math.sin(A) * Speed * 0.6, 520), Age = 0 }
	if Id == "Gold" then
		P.Fx = self:SpawnSprite({ Sprite = "Sprites/HD2D/Props.esprite", Flipbook = "Sprites/HD2D/Coin_Spin.eflipbook", Position = Pos, Blend = 3, Lit = true,
		                          Life = 30, Scale = 1.4 })
	else
		P.Fx = self:SpawnSprite({ Sprite = "Sprites/HD2D/Props.esprite", Slice = "Bag", Position = Pos, Blend = 3, Lit = true, Life = 30, Scale = 1.5 })
	end
	self.Pickups[#self.Pickups + 1] = P
end

function HD2DGame:UpdatePickups(Dt)
	local Keep = {}
	local Player = self:GetPlayer()
	local PP = Player and Player.entity:GetWorldPosition() or nil
	for _, P in ipairs(self.Pickups) do
		P.Age = P.Age + Dt
		local bTaken = false
		local Foot = P.Ground
		if P.Age < 0.6 then
			-- 튀어 오른 뒤 떨어짐
			P.Vel = P.Vel + Vector3(0, 0, -1800 * Dt)
			P.Pos = P.Pos + P.Vel * Dt
			if P.Pos.Z < Foot then
				P.Pos = Vector3(P.Pos.X, P.Pos.Y, Foot)
				P.Vel = Vector3(P.Vel.X * 0.4, P.Vel.Y * 0.4, math.abs(P.Vel.Z) * 0.35)
			end
		elseif PP then
			local To = Flat(PP - P.Pos)
			local L = To:Length()
			if L < 280 then
				-- 끌려옴
				local Step = math.min(L, (500 + (280 - L) * 4) * Dt)
				P.Pos = P.Pos + To:Normalized() * Step
				P.Pos = Vector3(P.Pos.X, P.Pos.Y, Foot)
			end
			if L < 55 then bTaken = true end
		end
		if P.Fx.Entity then P.Fx.Entity:SetPosition(P.Pos + Vector3(0, 0, 0)) end
		if bTaken then
			self:KillFx(P.Fx)
			self.Report.Pickups = self.Report.Pickups + 1
			if P.Id == "Gold" then
				self.Gold = self.Gold + P.Value
				Audio.PlayOneShot(Snd.Coin)
				self:DamageNumber(PP + Vector3(0, 0, 40), "+" .. P.Value .. "G", { 1, 0.85, 0.35, 1 }, 0.8)
			else
				self:AddItem(P.Id, P.Value, true)
				Audio.PlayOneShot(Snd.Pickup)
			end
		elseif P.Age > 29 then
			self:KillFx(P.Fx)
		else
			Keep[#Keep + 1] = P
		end
	end
	self.Pickups = Keep
end

-- ================================================================ 화면 흔들림 (카메라는 플레이어 스크립트가 놓으며 이 오프셋을 더한다)
function HD2DGame:AddShake(Amount, Time)
	self.Shake = math.max(self.Shake, Amount)
	self.ShakeTime = math.max(self.ShakeTime, Time)
end

function HD2DGame:GetShakeOffset()
	if self.ShakeTime <= 0 then return Vector3(0, 0, 0) end
	local A = self.Shake * math.min(1.0, self.ShakeTime / 0.15)
	return Vector3(math.sin(self.Time * 91.0) * A, 0, math.cos(self.Time * 73.0) * A)
end

function HD2DGame:OnUpdate(Dt)
	self.Time = self.Time + Dt
	self.PlayTime = (self.PlayTime or 0) + Dt
	self.ShakeTime = math.max(0.0, self.ShakeTime - Dt)
	if not self.bQuestShown then
		self.bQuestShown = true
		self:RefreshQuest()
		self:RefreshProps()
	end
	if self.bOpenTitle then
		self.bOpenTitle = false
		self:OpenTitle()
	end
	if self.ArrivePos then
		local Player = self:GetPlayer()
		if Player and Player.bStarted then
			Player:Teleport(self.ArrivePos)
			Player:SnapCamera()
			self.ArrivePos = nil
		end
	end
	self:UpdateFx(Dt)
	self:UpdateProjectiles(Dt)
	self:UpdatePickups(Dt)
	self:UpdateHazards(Dt)
	self:UpdateSlots(Dt)
	self:UpdateDungeon(Dt)
end

-- ================================================================ 자동 검증 결과 (HD2DAutoPilot.lua가 끝에 부른다)
function HD2DGame:ReportAutoPlay(Failures, Summary)
	local R = self.Report
	local Kills = {}
	for Kind, N in pairs(R.Kills) do Kills[#Kills + 1] = Kind .. " " .. N end
	table.sort(Kills)
	local Hits = {}
	for Id, N in pairs(R.WeaponHits) do Hits[#Hits + 1] = Id .. " " .. N end
	table.sort(Hits)
	local Patterns = {}
	for Name, N in pairs(R.BossPatterns) do Patterns[#Patterns + 1] = Name .. " " .. N end
	table.sort(Patterns)
	Log.Info(string.format("[HD2D] 자동 플레이 요약: %s | 처치 {%s} | 무기 명중 {%s} | 보스 패턴 {%s} | 상자 %d, 줍기 %d, 대화 %d, 인벤토리 %d, 장비 교체 %d(방어구·장신구 %d), 부스트 %d(최대 %d단계), 서브 퀘스트 %d, 저장 %d, 투사체 %d, 독 %d, 함정 솟음 %d(맞음 %d), 수정 가시 %d, 문 닫힘 %d/열림 %d, 잔상 %d, 엔딩 %d, 퀘스트 %d단계, 골드 %d",
		Summary or "", table.concat(Kills, ", "), table.concat(Hits, ", "), table.concat(Patterns, ", "), R.Chests, R.Pickups, R.Dialogs, R.InventoryOpened, R.Equips,
		R.GearEquips, R.Boosts, R.BoostMax, R.SubDone, R.Saves, R.Projectiles, R.PoisonTicks, R.TrapRises, R.TrapHits, R.Eruptions, R.GateCloses, R.GateOpens,
		R.Afterimages, R.Endings, self.QuestStage, self.Gold))
	if Failures == nil then
		return -- 단계 요약만 (자동 검증이 씬을 다시 여는 중간)
	elseif #Failures == 0 then
		Log.Info("[HD2D] 결과: 실패 0건")
	else
		Log.Error("[HD2D] 결과: 실패 " .. #Failures .. "건 — " .. table.concat(Failures, ", "))
	end
end

return HD2DGame
