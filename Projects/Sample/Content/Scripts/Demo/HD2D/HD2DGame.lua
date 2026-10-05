-- HD-2D 데모 게임 관리 (Scenes/Demo/HD2D.escene의 "HD2DGame" 엔티티 — Tools/DemoMap/HD2DGameplay.py가 배치·속성을 쓴다).
--   맡는 것: 적 자리(소환·부활)·보스, 보물상자·마을 사람(프리팹으로 만들고 상호작용), 일행 상태(골드·소지품·장비 무기·퀘스트 단계),
--            메뉴(대화·인벤토리·상점 — 열려 있는 동안 Game.SetTimeScale(0), 메뉴 글자·커서는 실제 시간), 효과 조각·투사체·줍는 것·독 웅덩이
--            (스크립트 없는 FxSprite 프리팹 조각 — 만든 직후 콜백에서 모양을 정하고 여기서 움직이고 지운다), 화면 흔들림, 자동 검증 결과 판정.
--   다른 스크립트는 Scene.Find("HD2DGame"):GetScript()로 쓴다 (플레이어 = HD2DPlayer.lua, 적 = HD2DEnemy.lua, 보스 = HD2DBoss.lua, UI = HD2DHud.lua).
--   좌표: 카메라가 +Y 쪽에서 -Y를 내려다본다 → 화면 오른쪽 = +X, 화면 위(안쪽) = -Y. 스프라이트는 XZ 평면(앞면 +Y)에 서 있다.
--   수치·대사: HD2DData.lua (Data/Demo/HD2D/*). 난수는 결정적(LCG) — 자동 검증 화면이 실행마다 같도록 math.random을 쓰지 않는다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local HD2DGame = {
	Properties = {
		Enemies  = "",  -- "종류,x,y,z;..." (z = 캡슐 중심)
		Npcs     = "",  -- "id,x,y,z;..."
		Chests   = "",  -- "x,y,z,아이템*개수+...;..." (Gold*n = 골드)
		Boss     = "",  -- "x,y,z"
		Path     = "",  -- "x,y;..." 마을 → 들판 길 (자동 조종이 따라 걷는다)
		AutoPlay = "",  -- "" | Full | Inventory | Shop | Dialog | Combat | Boss (HD2DAutoPilot.lua)
		RespawnMinDistance = 900.0,
	},
}

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
	self.Report = { Kills = {}, WeaponHits = {}, Chests = 0, Bought = {}, Used = {}, InventoryOpened = 0, Dialogs = 0, LevelUps = 0,
	                Pickups = 0, Projectiles = 0, PoisonTicks = 0, Equips = 0, BossPatterns = {} }

	-- 일행 상태
	local B = D.Balance()
	self.Gold = B.StartGold
	self.Items = {}        -- 아이템 id → 개수
	self:AddItem(B.StartWeapon, 1, false)
	for _, Id in ipairs(B.StartItems) do self:AddItem(Id, 1, false) end
	self.Equipped = B.StartWeapon
	self.QuestStage, self.QuestKills = 0, 0
	self.Menu = nil        -- nil | "Dialog" | "Inventory" | "Shop"

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
	for Item in string.gmatch(self.Properties.Chests, "[^;]+") do
		local X, Y, Z, Contents = string.match(Item, "([-%d%.]+),([-%d%.]+),([-%d%.]+),(.+)")
		if X then self:SpawnChest(Vector3(tonumber(X), tonumber(Y), tonumber(Z)), Contents) end
	end
	if self.Properties.Boss ~= "" then
		self.BossPos = Parse3(self.Properties.Boss)
		Scene.SpawnPrefab(Prefabs .. "Golem.eprefab", self.BossPos)
	end
	self.Path = {}
	for X, Y in string.gmatch(self.Properties.Path, "([-%d%.]+),([-%d%.]+)") do
		self.Path[#self.Path + 1] = Vector3(tonumber(X), tonumber(Y), 0)
	end
	Log.Info(string.format("[HD2D] 시작: 적 자리 %d곳, 마을 사람 %d명, 보물상자 %d개, 보스 %s, 시나리오 '%s'",
		#self.Slots, #self.Npcs, #self.Chests, self.BossPos and "있음" or "없음", self.Properties.AutoPlay))
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
	-- 전리품: 골드 동전 몇 개 + 확률 아이템
	local Gold = Row.GoldMin + math.floor(self:Random() * (Row.GoldMax - Row.GoldMin + 1))
	local Ground = Vector3(Pos.X, Pos.Y, Pos.Z + S.Foot + 2)
	self:DropLoot(Ground, Gold, (Row.DropItem ~= "" and self:Random() < Row.DropChance) and Row.DropItem or nil, S.bBoss and 8 or 3)
	local Player = self:GetPlayer()
	if Player then Player:AddExp(Row.Exp) end
	local SlotIndex = self.SlotOf[S.entity.Id]
	if SlotIndex and Row.RespawnTime > 0 then
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
	Log.Info(string.format("[HD2D] 처치: %s (골드 %d, 경험치 %d)", Row.DisplayName, Gold, Row.Exp))
end

function HD2DGame:OnBossKilled(S)
	local Pos = S.entity:GetWorldPosition()
	self.bBossDead = true
	self:Hud():ShowBoss(nil)
	self:Hud():Announce("고대의 바위 골렘을 쓰러뜨렸다!", "촌장 바르톨로에게 돌아가 보고하자", 3.5)
	self:SpawnChest(Pos + Vector3(0, 170, S.Foot + 47), "HiPotion*2+Gold*100", true) -- 상자 루트 = 지면 + 45
	if self.QuestStage == 2 then
		self:SetQuestStage(3)
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
	if WeaponId then self.Report.WeaponHits[WeaponId] = (self.Report.WeaponHits[WeaponId] or 0) + 1 end
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

-- ================================================================ 마을 사람 · 보물상자 · 상호작용
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

function HD2DGame:SpawnChest(Pos, Contents, bReward)
	local Chest = { Pos = Pos, Contents = Contents, bOpened = false }
	self.Chests[#self.Chests + 1] = Chest
	Scene.SpawnPrefab(Prefabs .. "Chest.eprefab", Pos, function(E)
		Chest.Entity = E
		Chest.Body = E:FindChild("Body")
		if bReward then
			self:SpawnFx("Poof", Pos + Vector3(0, 20, -20), { Scale = 1.4 })
			self:SpawnFx("Sparkle", Pos + Vector3(0, 30, 30), { Blend = 2, Scale = 1.6 })
		end
	end)
end

function HD2DGame:RefreshMarkers()
	for _, Npc in ipairs(self.Npcs) do
		if Npc.Marker then
			local bShow = false
			if Npc.Row.Role == "Elder" then
				bShow = self.QuestStage == 0 or self.QuestStage == 3 or (self.QuestStage == 1 and self.QuestKills >= D.Quest(1).KillGoal)
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
		if Npc.Entity and L < 175 and L < BestDist then
			Best, BestDist, Text = Npc, L, "E  " .. Npc.Row.DisplayName .. "와(과) 이야기하기"
		end
	end
	for _, Chest in ipairs(self.Chests) do
		local L = Flat(Chest.Pos - Pos):Length()
		if Chest.Entity and not Chest.bOpened and L < 160 and L < BestDist then
			Best, BestDist, Text = Chest, L, "E  보물상자 열기"
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
	else
		self:TalkTo(T)
	end
	return true
end

function HD2DGame:OpenChest(Chest)
	Chest.bOpened = true
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
		local bAdvance = Stage == 0 or Stage == 3 or (Stage == 1 and self.QuestKills >= D.Quest(1).KillGoal)
		if bAdvance then
			local Next = D.Quest(Stage + 1)
			self:StartDialog(Row.DisplayName, Next.Lines, function() self:SetQuestStage(Stage + 1) end)
		else
			self:StartDialog(Row.DisplayName, D.Quest(Stage).WaitLines)
		end
	elseif Row.Role == "Shop" then
		self:StartDialog(Row.DisplayName, Row.Lines, function() self:OpenShop() end)
	else
		self:StartDialog(Row.DisplayName, Row.Lines)
	end
end

-- ================================================================ 퀘스트
function HD2DGame:SetQuestStage(Stage)
	self.QuestStage = Stage
	local Q = D.Quest(Stage)
	if Stage == 4 then
		local B = D.Balance()
		self:AddGold(B.QuestRewardGold, true)
		if B.QuestRewardItem ~= "" then self:AddItem(B.QuestRewardItem, 1, true) end
		self:Hud():Announce("퀘스트 완료!", Q.Title, 3.0)
		self:SpawnLevelFx(self:GetPlayer().entity:GetWorldPosition())
	elseif Stage == 2 then
		self:Hud():Announce("새 목표", Q.Objective, 2.5)
	end
	self:RefreshQuest()
	Log.Info(string.format("[HD2D] 퀘스트 단계 %d: %s", Stage, Q.Objective))
end

function HD2DGame:RefreshQuest()
	local Q = D.Quest(self.QuestStage)
	local Text = Q.Objective
	if Q.KillGoal > 0 then
		if self.QuestKills >= Q.KillGoal then
			Text = "마물 퇴치 완료! 촌장 바르톨로에게 보고하자"
		else
			Text = string.format("%s (%d/%d)", Q.Objective, self.QuestKills, Q.KillGoal)
		end
	end
	local H = self:Hud()
	if H then H:SetQuest(Q.Title, Text) end
	self:RefreshMarkers()
end

-- ================================================================ 일행 (골드·소지품·장비)
function HD2DGame:AddGold(N, bToast)
	self.Gold = self.Gold + N
	if bToast then
		self:Hud():Toast("UI/Demo/HD2D/Icons/Coin.png", string.format("%d 골드", N))
	end
end

function HD2DGame:Count(Id) return self.Items[Id] or 0 end

function HD2DGame:AddItem(Id, N, bToast)
	local Row = D.Item(Id)
	if not Row then
		Log.Error("[HD2D] 아이템 표에 없음:", Id)
		return
	end
	if Row.Kind == "Weapon" then
		self.Items[Id] = 1
	else
		self.Items[Id] = (self.Items[Id] or 0) + N
	end
	if bToast then
		self:Hud():Toast(Row.Icon, N > 1 and string.format("%s ×%d 획득", Row.DisplayName, N) or (Row.DisplayName .. " 획득"))
	end
end

function HD2DGame:GetWeapon()
	return D.Weapon(self.Equipped)
end

function HD2DGame:Equip(Id, bToast)
	if self:Count(Id) <= 0 or self.Equipped == Id then return false end
	self.Equipped = Id
	self.Report.Equips = self.Report.Equips + 1
	Audio.PlayOneShot(Snd.Equip)
	if bToast then self:Hud():Toast(D.Weapon(Id).Icon, D.Weapon(Id).DisplayName .. " 장비") end
	local Player = self:GetPlayer()
	if Player then Player:OnWeaponChanged() end
	return true
end

-- 가진 무기를 순서대로 돌려 낀다 (R)
function HD2DGame:CycleWeapon()
	local Order = D.WeaponOrder
	local Current = 1
	for I, Id in ipairs(Order) do
		if Id == self.Equipped then Current = I end
	end
	for Step = 1, #Order - 1 do
		local Id = Order[(Current - 1 + Step) % #Order + 1]
		if self:Count(Id) > 0 then
			return self:Equip(Id, true)
		end
	end
	return false
end

-- 소모품 쓰기. 썼으면 true
function HD2DGame:UseItem(Id)
	local Row = D.Item(Id)
	local Player = self:GetPlayer()
	if not Row or self:Count(Id) <= 0 or not Player then return false end
	if Row.Kind == "Weapon" then return self:Equip(Id, true) end
	self.Items[Id] = self.Items[Id] - 1
	if Row.Kind == "Heal" then
		Player:Heal(Row.Amount, 0)
	elseif Row.Kind == "Mana" then
		Player:Heal(0, Row.Amount)
	else
		Player:Heal(99999, 99999)
	end
	self.Report.Used[Id] = (self.Report.Used[Id] or 0) + 1
	Audio.PlayOneShot(Snd.Potion)
	Log.Info("[HD2D] 아이템 사용: " .. Row.DisplayName)
	return true
end

-- 가진 것 중 첫 번째 (단축키: 회복약 → 고급 회복약 / 마나 물약)
function HD2DGame:QuickUse(Ids)
	for _, Id in ipairs(Ids) do
		if self:Count(Id) > 0 then return self:UseItem(Id) end
	end
	self:Hud():Toast(D.Item(Ids[1]).Icon, D.Item(Ids[1]).DisplayName .. "이(가) 없다")
	Audio.PlayOneShot(Snd.Error)
	return false
end

-- ================================================================ 메뉴 (대화·인벤토리·상점)
function HD2DGame:IsMenuOpen() return self.Menu ~= nil end

function HD2DGame:SetPaused(bPaused)
	Game.SetTimeScale(bPaused and 0.0 or 1.0)
end

function HD2DGame:StartDialog(Name, Lines, OnDone)
	if Lines == nil or #Lines == 0 then
		if OnDone then OnDone() end
		return
	end
	self.Menu = "Dialog"
	self.Dialog = { Name = Name, Lines = Lines, Index = 1, Chars = 0.0, OnDone = OnDone }
	self:SetPaused(true)
	self:Hud():ShowPrompt(nil)
	self:Hud():ShowDialog(Name, Lines[1], 0)
	self.Report.Dialogs = self.Report.Dialogs + 1
	Audio.PlayOneShot(Snd.Open)
end

function HD2DGame:UpdateDialog(UDt, In)
	local Dlg = self.Dialog
	local Line = Dlg.Lines[Dlg.Index]
	local Total = utf8.len(Line) or #Line
	if Dlg.Chars < Total then
		Dlg.Chars = math.min(Total, Dlg.Chars + UDt * 42.0)
	end
	if In.Confirm then
		if Dlg.Chars < Total then
			Dlg.Chars = Total
		elseif Dlg.Index < #Dlg.Lines then
			Dlg.Index = Dlg.Index + 1
			Dlg.Chars = 0
			Audio.PlayOneShot(Snd.Move)
		else
			self:CloseMenu()
			if Dlg.OnDone then Dlg.OnDone() end
			return
		end
	end
	self:Hud():ShowDialog(Dlg.Name, Dlg.Lines[Dlg.Index], math.floor(Dlg.Chars), Dlg.Chars >= (utf8.len(Dlg.Lines[Dlg.Index]) or 0))
end

function HD2DGame:CloseMenu()
	local Was = self.Menu
	self.Menu = nil
	self:SetPaused(false)
	local H = self:Hud()
	if Was == "Dialog" then H:HideDialog() else H:ShowMenu(nil) end
	Audio.PlayOneShot(Snd.Close)
end

-- 목록 줄 (인벤토리: 무기 → 소모품, 상점: 진열)
function HD2DGame:BuildRows()
	local Rows = {}
	if self.Menu == "Shop" then
		for _, Id in ipairs(D.Balance().ShopStock) do Rows[#Rows + 1] = Id end
	else
		for _, Id in ipairs(D.WeaponOrder) do
			if self:Count(Id) > 0 then Rows[#Rows + 1] = Id end
		end
		for _, Id in ipairs(D.ConsumableOrder) do
			if self:Count(Id) > 0 then Rows[#Rows + 1] = Id end
		end
	end
	return Rows
end

function HD2DGame:OpenInventory()
	self.Menu = "Inventory"
	self.MenuIndex = 1
	self.MenuNote = nil
	self:SetPaused(true)
	self.Report.InventoryOpened = self.Report.InventoryOpened + 1
	self:Hud():ShowPrompt(nil)
	self:Hud():ShowMenu("Inv")
	self:RefreshMenu()
	Audio.PlayOneShot(Snd.Open)
end

function HD2DGame:OpenShop()
	self.Menu = "Shop"
	self.MenuIndex = 1
	self.MenuNote = nil
	self:SetPaused(true)
	self:Hud():ShowMenu("Shop")
	self:RefreshMenu()
	Audio.PlayOneShot(Snd.Open)
end

function HD2DGame:SelectedId()
	local Rows = self:BuildRows()
	return Rows[self.MenuIndex], Rows
end

function HD2DGame:RefreshMenu()
	local Rows = self:BuildRows()
	if #Rows == 0 then self.MenuIndex = 1 else self.MenuIndex = math.max(1, math.min(self.MenuIndex, #Rows)) end
	local Player = self:GetPlayer()
	local Prefix = self.Menu == "Shop" and "Shop" or "Inv"
	local Lines = {}
	for I, Id in ipairs(Rows) do
		local Row = D.Item(Id)
		local Right
		if self.Menu == "Shop" then
			Right = (Row.Kind == "Weapon" and self:Count(Id) > 0) and "보유" or string.format("%d G", Row.Price)
		else
			Right = Row.Kind == "Weapon" and (Id == self.Equipped and "장비 중" or "") or string.format("×%d", self:Count(Id))
		end
		Lines[I] = { Icon = Row.Icon, Name = Row.DisplayName, Right = Right, bDim = self.Menu == "Shop" and Row.Price > self.Gold and Right ~= "보유" }
	end
	local Status = Player and string.format("Lv %d    HP %d/%d    MP %d/%d    공격력 ×%.2f", Player.Level, math.ceil(Player.Health), Player.MaxHealth,
		math.floor(Player.Mana), Player.MaxMana, Player:DamageScale()) or ""
	local H = self:Hud()
	H:SetMenuRows(Prefix, Lines, self.MenuIndex)
	H:SetMenuHeader(Prefix, Status, self.Gold)
	local Id = Rows[self.MenuIndex]
	if Id then
		local Row = D.Item(Id)
		local Type, Desc, Stats = "", Row.Description, ""
		if Row.Kind == "Weapon" then
			local W = D.Weapon(Row.Weapon)
			local KindName = { Slash = "베기", Thrust = "찌르기", Arrow = "활", Bolt = "마법" }
			Type = "무기 · " .. KindName[W.Kind]
			Desc = W.Description
			Stats = string.format("공격력 %d    사거리 %d", W.Damage, math.floor(W.Range / 10 + 0.5) * 10)
			if W.ManaCost > 0 then Stats = Stats .. string.format("    마나 %d", W.ManaCost) end
			if self.Menu ~= "Shop" and Id == self.Equipped then Stats = Stats .. "\n지금 장비하고 있다" end
		else
			Type = ({ Heal = "회복 아이템", Mana = "마나 회복 아이템", Elixir = "귀한 회복 아이템" })[Row.Kind] or ""
			Stats = string.format("소지 %d개", self:Count(Id))
		end
		if self.Menu == "Shop" then Stats = Stats .. string.format("\n가격 %d 골드", Row.Price) end
		if self.MenuNote then Stats = Stats .. "\n" .. self.MenuNote end
		H:SetMenuDetail(Prefix, Row.Icon, Row.DisplayName, Type, Desc, Stats)
	else
		H:SetMenuDetail(Prefix, nil, "", "", "소지품이 없다", "")
	end
end

function HD2DGame:MenuConfirm()
	local Id = self:SelectedId()
	if not Id then return end
	local Row = D.Item(Id)
	self.MenuNote = nil
	if self.Menu == "Shop" then
		if Row.Kind == "Weapon" and self:Count(Id) > 0 then
			self.MenuNote = "이미 가지고 있다"
			Audio.PlayOneShot(Snd.Error)
		elseif self.Gold < Row.Price then
			self.MenuNote = "골드가 부족하다"
			Audio.PlayOneShot(Snd.Error)
		else
			self.Gold = self.Gold - Row.Price
			self:AddItem(Id, 1, true)
			self.Report.Bought[Id] = (self.Report.Bought[Id] or 0) + 1
			self.MenuNote = "구입했다!"
			Audio.PlayOneShot(Snd.Buy)
			Log.Info(string.format("[HD2D] 구입: %s (%d G, 남은 골드 %d)", Row.DisplayName, Row.Price, self.Gold))
		end
	else
		if Row.Kind == "Weapon" then
			if not self:Equip(Id, true) then Audio.PlayOneShot(Snd.Error) end
		else
			if self:UseItem(Id) then self.MenuNote = Row.DisplayName .. "을(를) 사용했다" end
		end
	end
	self:RefreshMenu()
end

-- 메뉴가 열려 있는 동안 플레이어 스크립트가 입력을 넘긴다 (In: MenuUp/MenuDown/Confirm/Cancel/Inventory)
function HD2DGame:MenuInput(In)
	local UDt = Time.GetUnscaledDelta()
	if self.Menu == "Dialog" then
		self:UpdateDialog(UDt, In)
		return
	end
	if In.Cancel or In.Inventory then
		self:CloseMenu()
		return
	end
	local Rows = self:BuildRows()
	if In.MenuUp and #Rows > 0 then
		self.MenuIndex = (self.MenuIndex - 2) % #Rows + 1
		self.MenuNote = nil
		Audio.PlayOneShot(Snd.Move)
		self:RefreshMenu()
	elseif In.MenuDown and #Rows > 0 then
		self.MenuIndex = self.MenuIndex % #Rows + 1
		self.MenuNote = nil
		Audio.PlayOneShot(Snd.Move)
		self:RefreshMenu()
	elseif In.Confirm then
		self:MenuConfirm()
	end
end

-- UI 단추 (마우스): 줄 클릭 = 고르고 확인, 올리기 = 고르기
function HD2DGame:OnMenuRowClicked(Index)
	if self.Menu ~= "Inventory" and self.Menu ~= "Shop" then return end
	self.MenuIndex = Index
	self:MenuConfirm()
end

function HD2DGame:OnMenuRowHovered(Index)
	if (self.Menu == "Inventory" or self.Menu == "Shop") and self.MenuIndex ~= Index and Index <= #self:BuildRows() then
		self.MenuIndex = Index
		self.MenuNote = nil
		self:RefreshMenu()
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
		P.Fx = self:SpawnFx("Bolt", Desc.Pos, { Blend = 2, Scale = 1.4, Life = 3.0 })
	else
		P.Fx = self:SpawnSprite({ Sprite = FxSprite, Slice = "Arrow", Position = Desc.Pos, Rotation = HD2DGame.ScreenAngle(Desc.Dir), Blend = 0,
		                          Life = 3.0, Scale = 1.3, Color = P.Team == "Enemy" and { 1, 0.75, 0.75, 1 } or { 1, 1, 1, 1 } })
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
	self.ShakeTime = math.max(0.0, self.ShakeTime - Dt)
	if not self.bQuestShown then
		self.bQuestShown = true
		self:RefreshQuest()
	end
	self:UpdateFx(Dt)
	self:UpdateProjectiles(Dt)
	self:UpdatePickups(Dt)
	self:UpdateHazards(Dt)
	self:UpdateSlots(Dt)
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
	Log.Info(string.format("[HD2D] 자동 플레이 요약: %s | 처치 {%s} | 무기 명중 {%s} | 상자 %d, 줍기 %d, 대화 %d, 인벤토리 %d, 장비 교체 %d, 투사체 %d, 독 %d, 퀘스트 %d단계, 골드 %d",
		Summary or "", table.concat(Kills, ", "), table.concat(Hits, ", "), R.Chests, R.Pickups, R.Dialogs, R.InventoryOpened, R.Equips,
		R.Projectiles, R.PoisonTicks, self.QuestStage, self.Gold))
	if #Failures == 0 then
		Log.Info("[HD2D] 결과: 실패 0건")
	else
		Log.Error("[HD2D] 결과: 실패 " .. #Failures .. "건 — " .. table.concat(Failures, ", "))
	end
end

return HD2DGame
