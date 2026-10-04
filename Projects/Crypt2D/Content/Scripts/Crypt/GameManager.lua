-- Crypt2D 게임 관리자 (씬의 GameManager 엔티티). 한 판(런)의 진행을 모두 맡는다:
--   층 조립(Dungeon.lua → 지형/뒷벽 타일맵 SetTiles), 방 입장 → 문 잠금 + 적 소환 → 전멸 시 열림 + 보상, 출구 포털 → 다음 층,
--   마지막(3층)은 보스 방. 투사체·코인·하트·효과·잔상은 스크립트 없는 Sprite2D 프리팹 조각으로 만들어 여기서 움직이고 지운다
--   (적 수십·탄 수백도 스크립트 인스턴스를 늘리지 않게). 카메라(부드러운 추적 + 조준 쪽 기울임 + 방 경계 고정 + 흔들림),
--   히트스톱 = Game.HitStop(실제 시간 동안 게임 시간 배율 0 — 이동기·플립북·스크립트 dt 모두 멈춤), 일시정지 = Game.SetTimeScale(0)
--   (공중에서 멈춰도 떨어지지 않는다), 상점 방(ShopItems.etable — 코인으로 회복·최대 체력·무기), 사망/승리, 최고 기록 저장.
--   조준 = 실제 마우스 커서(Input.GetMouseUIPosition — Player.lua)이므로 커서는 숨기고 잠그지 않는다 (입력 모드 GameAndUI, HUD 조준점이 대신).
-- 속성: Seed (0 = 최고 기록의 판 수로 매번 다르게), AutoPlay ("" / "Test" 검증 코스 / "Boss" 보스전 / "Explore" 이웃 방 탐험 /
--       "Death" 일부러 죽어 사망 화면 / "Climb" 위 문으로 방 세 개 오르기),
--       StartFloor (1~3). 자동 플레이는 AutoPilot.lua가 플레이어 입력을 대신 넣고 "[Crypt2D] … 결과: 실패 N건"을 남긴다.
local U         = Script.Require("Scripts/Crypt/Util.lua")
local CryptData = Script.Require("Scripts/Crypt/CryptData.lua")
local Dungeon   = Script.Require("Scripts/Crypt/Dungeon.lua")
local AutoPilot = Script.Require("Scripts/Crypt/AutoPilot.lua")

local GM = {
	Properties = {
		Seed = 0,
		AutoPlay = "",
		StartFloor = 1,
	},
}

local Cell = U.Cell
local SaveSlot = "Crypt2D"
local FinalFloor = 3
local HalfViewH = 480.0            -- OrthoHeight 960 / 2
local HalfViewW = 480.0 * 16 / 9   -- 1280x720

local Paths = {
	Sprite2D = "Prefabs/Crypt/Sprite2D.eprefab",
	Torch    = "Prefabs/Crypt/Torch.eprefab",
	Player   = "Prefabs/Crypt/Player.eprefab",
	Gen      = "Sprites/Crypt/Generated.esprite",
}

-- 효과 수명 (플립북 프레임 수 / fps)
local FxLife = {
	Fx_Death = 9 / 20, Fx_Flame = 5 / 14, Fx_Slash = 3 / 24, Fx_Spark = 3 / 24, Fx_Dust = 4 / 16,
}

function GM:OnStart()
	self.Data     = CryptData
	self.Balance  = CryptData.Balance()
	self.Terrain  = Scene.Find("Terrain")
	self.BackWall = Scene.Find("BackWall")
	self.BackDecor = Scene.Find("BackDecor")
	self.Camera   = Scene.Find("Camera2D")
	local HudEntity = Scene.Find("HUD")
	self.HudEntity = HudEntity
	self.Hud = nil -- HUD 스크립트는 첫 OnUpdate에서 (OnStart 순서 무관)

	self.Record = SaveGame.Load(SaveSlot) or {}
	self.Record.Runs = (self.Record.Runs or 0) + 1
	local Seed = math.tointeger(self.Properties.Seed) or 0
	if Seed == 0 then
		Seed = self.Record.Runs * 7919 + 1013
	end
	self.Seed = Seed
	self.Rng = U.Rng(Seed)
	Log.Info(string.format("[Crypt2D] 새 판: 시드 %d, 자동 플레이 '%s'", Seed, self.Properties.AutoPlay))

	self.Floor     = U.Clamp(math.tointeger(self.Properties.StartFloor) or 1, 1, FinalFloor)
	self.Gold      = 0
	self.Kills     = 0
	self.RunTime   = 0.0
	self.Weapons   = { self.Balance.StartWeapons[1], self.Balance.StartWeapons[2] }
	self.WeaponSlot = 1

	self.Projectiles = {}
	self.Pickups     = {}
	self.Fx          = {}
	self.Fades       = {}
	self.Interactables = {}
	self.FloorEntities = {}
	self.SpawnInit   = {}
	self.Shake       = 0.0
	self.ShakeTime   = 0.0
	self.bPaused     = false
	self.bGameOver   = false
	self.CamX, self.CamZ = 0.0, 0.0
	self.bCamSnap    = true

	if self.Properties.AutoPlay ~= "" then
		self.Pilot = AutoPilot.New(self.Properties.AutoPlay, self)
	end
	self:BuildFloor(self.Floor)
	self:SetGameplayInput(true)
end

-- 게임 입력 (조준 = 실제 마우스, 커서 숨김 — HUD 조준점) ↔ 메뉴 입력 (보이는 커서). 잠그지 않는다 (잠그면 커서 위치가 고정된다)
function GM:SetGameplayInput(bGameplay)
	Game.SetInputMode(bGameplay and "GameAndUI" or "UIOnly")
	Game.SetMouseLocked(false)
	Game.SetCursorVisible(not bGameplay or self.Pilot ~= nil)
end

-- ================================================================ 층
function GM:ClearFloor()
	for _, E in ipairs(self.FloorEntities) do
		if E:IsValid() then E:Destroy() end
	end
	for _, List in ipairs({ self.Projectiles, self.Pickups, self.Fx, self.Fades }) do
		for _, Item in ipairs(List) do
			if Item.Entity and Item.Entity:IsValid() then Item.Entity:Destroy() end
		end
	end
	if self.Layout then
		for _, Room in ipairs(self.Layout.Rooms) do
			for _, E in ipairs(Room.Enemies) do
				if E:IsValid() then E:Destroy() end
			end
		end
	end
	self.FloorEntities, self.Projectiles, self.Pickups, self.Fx, self.Fades, self.Interactables = {}, {}, {}, {}, {}, {}
	self.Boss = nil
end

function GM:BuildFloor(Floor)
	self:ClearFloor()
	self.Floor = Floor
	local Mode = self.Properties.AutoPlay
	if Mode == "Test" or Mode == "Death" then
		self.Layout = Dungeon.TestLayout()
	elseif Mode == "Climb" then
		self.Layout = Dungeon.ClimbLayout()
	elseif Floor >= FinalFloor then
		self.Layout = Dungeon.BossLayout()
	else
		local Counts = self.Balance.FloorRooms
		self.Layout = Dungeon.GenerateLayout(self.Rng, Counts[math.min(Floor, #Counts)])
	end
	local Built = Dungeon.BuildTiles(self.Layout, self.Rng, Cell)
	self.Terrain:ClearTiles()
	self.Terrain:SetTiles(Built.Terrain)
	self.BackWall:ClearTiles()
	self.BackWall:SetTiles(Built.Back)
	self.BackDecor:ClearTiles()
	self.BackDecor:SetTiles(Built.Decor)
	Log.Info(string.format("[Crypt2D] %d층 조립: 방 %d개, 지형 타일 %d, 뒷벽 %d, 장식 %d, 횃불 %d", Floor, #self.Layout.Rooms, #Built.Terrain,
		#Built.Back, #Built.Decor, #Built.Torches))

	for _, Torch in ipairs(Built.Torches) do
		self:SpawnTracked(Paths.Torch, U.V(Torch.X, Torch.Z, 0))
	end
	-- 방별 소품: 상자, 패럴랙스 장식 기둥
	for _, Room in ipairs(self.Layout.Rooms) do
		for _, M in ipairs(Room.Markers) do
			if M.Char == "c" then
				self:SpawnSprite({ Sprite = Paths.Gen, Slice = "Chest", Layer = "Props", X = M.X, Z = M.Z, Lit = true }, function(E)
					self.FloorEntities[#self.FloorEntities + 1] = E
					self.Interactables[#self.Interactables + 1] = { Entity = E, X = M.X, Z = M.Z + 30, Kind = "Chest", Text = "F  상자 열기" }
				end)
			end
		end
		self:SpawnParallax(Room)
		if Room.Kind == "Shop" then
			self:SpawnShop(Room)
		end
	end

	-- 플레이어 (첫 층) 또는 순간이동
	local Start = self.Layout.Start
	local P = nil
	for _, M in ipairs(Start.Markers) do
		if M.Char == "p" then P = M end
	end
	local SX, SZ = P and P.X or (Start.Rect[1] + Start.Rect[3]) * 0.5, (P and P.Z or Start.Rect[2] + 2 * Cell) + 84
	if self.Player == nil then
		Scene.SpawnPrefab(Paths.Player, U.V(SX, SZ, 4), function(Root) self.Player = Root end)
	else
		self.Player:SetPosition(U.V(SX, SZ, 4))
	end
	self.CamX, self.CamZ, self.bCamSnap = SX, SZ, true
	self.CurrentRoom = nil
	self:EnterRoom(Start)
	if self.Hud then self:RefreshHud() end
	self.PendingToast = (Floor >= FinalFloor) and "최하층 — 제단" or string.format("지하 %d층", Floor)
end

function GM:SpawnTracked(Prefab, Position, OnSpawned)
	Scene.SpawnPrefab(Prefab, Position, function(Root)
		self.FloorEntities[#self.FloorEntities + 1] = Root
		if OnSpawned then OnSpawned(Root) end
	end)
end

-- 배경 장식 판(교회 팩 backgrounds.png)을 방 안쪽에 두고 카메라 이동에 따라 늦게 따라오게 (OnLateUpdate)
function GM:SpawnParallax(Room)
	local Panels = { "Window", "Pillar", "Altar", "Gargoyle", "Lantern" }
	local X0, Z0, X1, Z1 = table.unpack(Room.Rect)
	local Count = 3
	for I = 1, Count do
		local Slice = Panels[self.Rng:Int(1, #Panels)]
		local AX = U.Lerp(X0 + 300, X1 - 300, (I - 0.5) / Count)
		local AZ = Z0 + 2 * Cell + self.Rng:Range(0, 2 * Cell)
		-- 판(바탕을 투명으로 키 처리한 그림)은 뒷벽보다 어둡게 — 멀리 있는 느낌
		self:SpawnSprite({ Sprite = "Sprites/Crypt/Backdrop.esprite", Slice = Slice, Layer = "Background", Order = 1, X = AX, Z = AZ, Y = -50,
		                   Color = { 0.6, 0.58, 0.78, 1 }, Lit = false }, function(E)
			self.FloorEntities[#self.FloorEntities + 1] = E
			Room.Parallax = Room.Parallax or {}
			Room.Parallax[#Room.Parallax + 1] = { Entity = E, X = AX, Z = AZ }
		end)
	end
end

-- ================================================================ 상점 (Shop 방 — 상인 + 진열대 3자리, 물건은 ShopItems.etable에서 층 조건·가중치로 겹치지 않게)
function GM:PickShopOffers(Count)
	local Pool = {}
	for _, Id in ipairs(CryptData.ShopItemIds()) do
		local Item = CryptData.ShopItem(Id)
		local bHeld = Item.Kind == "Weapon" and (Item.Weapon == self.Weapons[1] or Item.Weapon == self.Weapons[2])
		if Item.MinFloor <= self.Floor and not bHeld then Pool[#Pool + 1] = { Id, Item.Weight } end
	end
	local Offers = {}
	for _ = 1, Count do
		local Id = self.Rng:Weighted(Pool)
		if Id == nil then break end
		Offers[#Offers + 1] = Id
		for I, Entry in ipairs(Pool) do
			if Entry[1] == Id then table.remove(Pool, I) break end
		end
	end
	return Offers
end

function GM:SpawnShop(Room)
	local Stands = {}
	for _, M in ipairs(Room.Markers) do
		if M.Char == "m" then
			-- 상인: 사제 시트를 금빛으로 (적이 아님 — 스크립트·바디 없음)
			self:SpawnSprite({ Sprite = "Sprites/Crypt/Wizard.esprite", Slice = "Wizard0", Flipbook = "Sprites/Crypt/Wizard_Idle.eflipbook",
			                   Layer = "Props", X = M.X, Z = M.Z + 80, Color = { 1.0, 0.85, 0.55, 1 }, Lit = false }, function(E)
				self.FloorEntities[#self.FloorEntities + 1] = E
			end)
		elseif M.Char == "s" then
			Stands[#Stands + 1] = M
		end
	end
	local Offers = self:PickShopOffers(#Stands)
	for I, M in ipairs(Stands) do
		self:SpawnSprite({ Sprite = Paths.Gen, Slice = "Pedestal", Layer = "Props", X = M.X, Z = M.Z, Lit = true }, function(E)
			self.FloorEntities[#self.FloorEntities + 1] = E
		end)
		local Id = Offers[I]
		if Id then
			local Item = CryptData.ShopItem(Id)
			local Offer = { Kind = "Shop", ItemId = Id, X = M.X, Z = M.Z + 60, Price = Item.Price,
			                Text = string.format("F  구매: %s (%d 코인)", Item.DisplayName, Item.Price) }
			self:SpawnSprite({ Sprite = Paths.Gen, Slice = Item.Slice, Layer = "Pickups", X = M.X, Z = M.Z + 70,
			                   Rotation = Item.Kind == "Weapon" and 45 or nil }, function(E)
				self.FloorEntities[#self.FloorEntities + 1] = E
				Offer.Entity = E
			end)
			self.Interactables[#self.Interactables + 1] = Offer
		end
	end
	Room.ShopOffers = Offers
	Log.Info(string.format("[Crypt2D] 상점: %s", table.concat(Offers, ", ")))
end

-- 구매: 코인이 모자라면 알림만. 반환 = 샀는가
function GM:BuyOffer(Offer)
	local Item = CryptData.ShopItem(Offer.ItemId)
	if self.Gold < Item.Price then
		self:Toast(string.format("코인이 모자랍니다 (%d / %d)", self.Gold, Item.Price), 1.2)
		self:Sound("Hurt")
		return false
	end
	local Player = self:PlayerScript()
	self.Gold = self.Gold - Item.Price
	Offer.bUsed = true
	if Offer.Entity and Offer.Entity:IsValid() then Offer.Entity:Destroy() end
	local PX, PZ = self:PlayerPos()
	if Item.Kind == "Heal" then
		if Player then Player:Heal(Item.Amount) end
		self:Sound("Heal")
		self:ShowNumber(PX, PZ + 100, "+" .. math.floor(Item.Amount), { 0.4, 1, 0.5, 1 })
	elseif Item.Kind == "MaxHealth" then
		if Player then
			Player.MaxHealth = Player.MaxHealth + Item.Amount
			Player:Heal(Item.Amount)
		end
		self:Sound("Heal")
		self:ShowNumber(PX, PZ + 100, "최대 +" .. math.floor(Item.Amount), { 1, 0.85, 0.4, 1 })
	else
		self.Weapons[self.WeaponSlot] = Item.Weapon
		if Player then Player:OnWeaponsChanged() end
		self:Sound("Equip")
	end
	self:SpawnFx("Fx_Spark", Offer.X, Offer.Z, { Color = { 1, 0.9, 0.5, 1 } })
	self:Toast(Item.DisplayName, 1.2)
	self:RefreshHud()
	Log.Info(string.format("[Crypt2D] 구매: %s (%d 코인, 남은 코인 %d)", Offer.ItemId, Item.Price, self.Gold))
	return true
end

-- ================================================================ 방
function GM:RoomAtPosition(X, Z)
	if not self.Layout then return nil end
	return Dungeon.RoomAt(self.Layout, math.floor(X / (Dungeon.W * Cell)), math.floor(Z / (Dungeon.H * Cell)))
end

function GM:EnterRoom(Room)
	if self.CurrentRoom == Room then return end
	self.CurrentRoom = Room
	Room.Visited = true
	Room.Known = true
	for Side in pairs(Room.Doors) do
		local DX = (Side == "R" and 1) or (Side == "L" and -1) or 0
		local DY = (Side == "U" and 1) or (Side == "D" and -1) or 0
		local N = Dungeon.RoomAt(self.Layout, Room.SX + DX, Room.SY + DY)
		if N then N.Known = true end
	end
	if Room.State == "Idle" and (Room.Kind == "Start" or Room.Kind == "Treasure" or Room.Kind == "Shop" or Room.Kind == "Climb") then
		Room.State = "Cleared"
	end
	if self.Hud then self.Hud:UpdateMinimap(self.Layout, Room) end
	Log.Info(string.format("[Crypt2D] 방 입장: (%d, %d) %s/%s 상태 %s", Room.SX, Room.SY, Room.Kind, Room.Template.Name, Room.State))
end

-- 문에서 충분히 들어온 뒤에 잠근다 (문 칸 안에서 잠기면 끼인다)
function GM:IsDeepInside(Room, X, Z)
	local X0, Z0, X1, Z1 = table.unpack(Room.Rect)
	local M = 3.5 * Cell
	return X > X0 + M and X < X1 - M and Z > Z0 + 2.2 * Cell and Z < Z1 - 3.5 * Cell
end

function GM:LockRoom(Room)
	Room.State = "Active"
	self.Terrain:SetTiles(Dungeon.GateTiles(Room))
	self:Sound("Door")
	self:AddShake(6, 0.25)
	-- 적 소환 (표식 g/w/f/e/b)
	local Ratio = self.Balance.SpawnRatio[math.min(self.Floor, #self.Balance.SpawnRatio)] or 1
	if Room.Kind == "Boss" or self.Properties.AutoPlay == "Test" then Ratio = 1 end
	local Spawned = 0
	for _, M in ipairs(Room.Markers) do
		local Kind = self:PickEnemyKind(M.Char)
		if Kind and (Room.Kind == "Boss" or self.Rng:Chance(Ratio)) then
			local Z = M.Z
			local Def = CryptData.Enemy(Kind)
			if Def.Behavior ~= "Flyer" and Def.Behavior ~= "Boss" then Z = Z + 90 else Z = Z + Cell * 0.5 end
			self:SpawnEnemy(Kind, M.X, Z, Room)
			Spawned = Spawned + 1
		end
	end
	if Spawned == 0 then
		self:SpawnEnemy("Skeleton", (Room.Rect[1] + Room.Rect[3]) * 0.5, Room.Rect[2] + 2 * Cell + 90, Room)
	end
	Room.PendingSpawns = Spawned
	Log.Info(string.format("[Crypt2D] 문 잠김 — 적 %d 소환 (%s)", math.max(Spawned, 1), Room.Template.Name))
end

function GM:PickEnemyKind(Char)
	local Floor = math.min(self.Floor, FinalFloor)
	local function Pool(Behaviors)
		local Items = {}
		for Kind, Def in pairs(CryptData.Enemies()) do
			for _, B in ipairs(Behaviors) do
				if Def.Behavior == B and Def.MinFloor <= Floor then Items[#Items + 1] = { Kind, Def.Weight } end
			end
		end
		table.sort(Items, function(A, B) return A[1] < B[1] end) -- pairs 순서에 기대지 않게 (결정적)
		return self.Rng:Weighted(Items)
	end
	if Char == "g" then return Pool({ "Melee", "Charger", "Leaper" })
	elseif Char == "e" then return Pool({ "Melee", "Charger", "Leaper", "Caster" })
	elseif Char == "w" then return Pool({ "Caster" })
	elseif Char == "f" then return Pool({ "Flyer" })
	elseif Char == "b" then return "Angel" end
	return nil
end

function GM:SpawnEnemy(Kind, X, Z, Room)
	Room = Room or self.CurrentRoom
	Scene.SpawnPrefab("Prefabs/Crypt/" .. Kind .. ".eprefab", U.V(X, Z, 0), function(Root)
		Room.Enemies[#Room.Enemies + 1] = Root
		self.SpawnInit[Root.Id] = { Room = Room, Kind = Kind }
	end)
end

function GM:TakeSpawnInit(Entity)
	local Init = self.SpawnInit[Entity.Id]
	self.SpawnInit[Entity.Id] = nil
	return Init
end

function GM:CountLiveEnemies(Room)
	local Count = 0
	local Alive = {}
	for _, E in ipairs(Room.Enemies) do
		if E:IsValid() then
			local S = E:GetScript()
			if S == nil or not S.bDead then
				Count = Count + 1
				Alive[#Alive + 1] = E
			end
		end
	end
	Room.Enemies = Alive
	return Count
end

function GM:ClearRoom(Room)
	Room.State = "Cleared"
	local Set, Erase = Dungeon.OpenTiles(Room)
	self.Terrain:BeginTileEdit()
	for _, C in ipairs(Erase) do self.Terrain:EraseTile(C[1], C[2]) end
	if #Set > 0 then self.Terrain:SetTiles(Set) end
	self.Terrain:EndTileEdit()
	self:Sound("DoorOpen")
	self:Toast("방 정리!", 1.4)
	-- 보상: 코인 + 하트(확률)
	local PX, PZ = self:PlayerPos()
	local CX = U.Clamp(PX, Room.Rect[1] + 3 * Cell, Room.Rect[3] - 3 * Cell)
	local CZ = PZ + 140
	for I = 1, self.Balance.ClearGold do
		self:SpawnPickup("Coin", CX, CZ, self.Rng:Range(-260, 260), self.Rng:Range(300, 650))
	end
	if self.Rng:Chance(self.Balance.HeartChance) then
		self:SpawnPickup("Heart", CX, CZ, 0, 500)
	end
	if Room.Kind == "Exit" then
		self:SpawnPortal(Room)
	end
	if self.Hud then self.Hud:UpdateMinimap(self.Layout, Room) end
	Log.Info(string.format("[Crypt2D] 방 정리: (%d, %d) 문 열림", Room.SX, Room.SY))
end

function GM:SpawnPortal(Room)
	for _, M in ipairs(Room.Markers) do
		if M.Char == "x" then
			self:SpawnSprite({ Sprite = Paths.Gen, Slice = "Portal0", Flipbook = "Sprites/Crypt/Fx_Portal.eflipbook", Layer = "Props", X = M.X, Z = M.Z }, function(E)
				self.FloorEntities[#self.FloorEntities + 1] = E
				self.Interactables[#self.Interactables + 1] = { Entity = E, X = M.X, Z = M.Z + 70, Kind = "Portal", Text = "F  다음 층으로" }
			end)
			self:SpawnFx("Fx_Death", M.X, M.Z + 60, { Color = { 0.7, 0.5, 1, 1 } })
			self:Sound("Portal")
		end
	end
end

-- ================================================================ 갱신
-- AI를 멈출 때: 일시정지·히트스톱(게임 시간 배율 0 — dt 0으로 불린다)·판이 끝나고 잠시 뒤
function GM:IsFrozen()
	return self.bPaused or Time.GetTimeScale() == 0 or self.bGameOver and self.GameOverTimer ~= nil and self.GameOverTimer > 1.2
end

-- 플레이어 스크립트 (OnStart가 끝난 뒤부터 — 생성 다음 프레임)
function GM:PlayerScript()
	local S = self.Player and self.Player:IsValid() and self.Player:GetScript() or nil
	if S and S.Health then return S end
	return nil
end

function GM:PlayerPos()
	if self.Player and self.Player:IsValid() then
		local P = self.Player:GetWorldPosition()
		return P.X, P.Z
	end
	return self.CamX, self.CamZ
end

function GM:OnUpdate(Dt)
	if self.Hud == nil and self.HudEntity then
		local Hud = self.HudEntity:GetScript()
		if Hud and Hud.GM then -- HUD OnStart가 끝난 뒤부터
			self.Hud = Hud
			self:RefreshHud()
			self.Hud:UpdateMinimap(self.Layout, self.CurrentRoom)
		end
	end
	if self.PendingToast and self.Hud then
		self:Toast(self.PendingToast, 2.0)
		self.PendingToast = nil
	end

	-- 일시정지 (게임 오버 중에는 사망/승리 화면이 대신)
	if not self.bGameOver and self.Hud and self:PlayerScript() and Input.WasActionPressed("Pause") then
		self:SetPaused(not self.bPaused)
	end
	if self.bPaused then return end

	if self.bGameOver then
		self.GameOverTimer = self.GameOverTimer + Dt
		if self.GameOverTimer > 1.4 and not self.bResultShown then
			self.bResultShown = true
			self:ShowResult()
		end
		if self.bResultShown and Input.WasActionPressed("Confirm") then
			self:Restart()
		end
	end

	if not self.bGameOver then
		self.RunTime = self.RunTime + Dt
	end
	if self.Pilot then self.Pilot:Update(Dt) end

	-- 방 판정
	if self.Player and self.Player:IsValid() then
		local PX, PZ = self:PlayerPos()
		local Room = self:RoomAtPosition(PX, PZ)
		if Room then
			self:EnterRoom(Room)
			if Room.State == "Idle" and (Room.Kind == "Combat" or Room.Kind == "Exit" or Room.Kind == "Boss") and self:IsDeepInside(Room, PX, PZ) then
				self:LockRoom(Room)
			elseif Room.State == "Active" then
				if Room.PendingSpawns then
					-- 소환은 지연 생성 — 등록이 끝난 다음 프레임부터 센다
					if #Room.Enemies >= math.max(Room.PendingSpawns, 1) then Room.PendingSpawns = nil end
				elseif self:CountLiveEnemies(Room) == 0 and not self.bGameOver then
					self:ClearRoom(Room)
				end
			end
		end
	end

	self:UpdateProjectiles(Dt)
	self:UpdatePickups(Dt)
	self:UpdateFx(Dt)
	self:UpdateInteraction()
end

function GM:OnLateUpdate(Dt)
	if self.bPaused then
		if self.Hud then self.Hud:LateUpdate(0) end
		return
	end
	-- 카메라: 플레이어 + 조준 쪽 기울임 → 방 경계 고정 → 흔들림
	local PX, PZ = self:PlayerPos()
	local Player = self:PlayerScript()
	local TX, TZ = PX, PZ + 60
	if Player and Player.AimX then
		TX = TX + U.Clamp((Player.AimX - PX) * 0.18, -160, 160)
		TZ = TZ + U.Clamp((Player.AimZ - PZ) * 0.12, -100, 100)
	end
	local Room = self.CurrentRoom
	-- 보스전: 플레이어와 보스를 함께 담도록 가운데로
	if Room and Room.Kind == "Boss" and self.Boss and self.Boss.entity:IsValid() then
		local B = self.Boss.entity:GetWorldPosition()
		TX, TZ = U.Lerp(TX, B.X, 0.3), U.Lerp(TZ, B.Z, 0.5)
	end
	if Room then
		local X0, Z0, X1, Z1 = table.unpack(Room.Rect)
		local InX0, InX1 = X0 + Cell + HalfViewW, X1 - Cell - HalfViewW
		local InZ0, InZ1 = Z0 + Cell * 0.5 + HalfViewH, Z1 - Cell * 0.5 - HalfViewH
		TX = (InX0 < InX1) and U.Clamp(TX, InX0, InX1) or (X0 + X1) * 0.5
		TZ = (InZ0 < InZ1) and U.Clamp(TZ, InZ0, InZ1) or (Z0 + Z1) * 0.5
	end
	if self.bCamSnap then
		self.CamX, self.CamZ, self.bCamSnap = TX, TZ, false
	else
		local K = U.Smooth(7.0, Dt)
		self.CamX = U.Lerp(self.CamX, TX, K)
		self.CamZ = U.Lerp(self.CamZ, TZ, K)
	end
	local SX, SZ = 0.0, 0.0
	if self.ShakeTime > 0 then
		self.ShakeTime = self.ShakeTime - Dt
		local A = self.Shake * U.Clamp(self.ShakeTime / 0.25, 0, 1)
		SX, SZ = self.Rng:Range(-A, A), self.Rng:Range(-A, A)
	end
	if self.Camera then
		self.Camera:SetPosition(U.V(self.CamX + SX, self.CamZ + SZ, 2000))
	end
	-- 패럴랙스: 장식 판은 방 가운데 기준 카메라 이동의 35%만큼 늦게
	if Room and Room.Parallax then
		local MX, MZ = (Room.Rect[1] + Room.Rect[3]) * 0.5, (Room.Rect[2] + Room.Rect[4]) * 0.5
		for _, P in ipairs(Room.Parallax) do
			if P.Entity:IsValid() then
				P.Entity:SetPosition(U.V(P.X + (self.CamX - MX) * 0.35, P.Z + (self.CamZ - MZ) * 0.25, -50))
			end
		end
	end
	if self.Hud then self.Hud:LateUpdate(Dt) end
end

-- ================================================================ 조각 (Sprite2D 프리팹) — 효과·투사체·코인
-- Desc: { Sprite, Slice, Flipbook, Layer, Order, X, Z, Y, Rotation, FlipX, FlipY, Color = {r,g,b,a}, Blend, Lit }
function GM:SpawnSprite(Desc, OnSpawned)
	Scene.SpawnPrefab(Paths.Sprite2D, U.V(Desc.X, Desc.Z, Desc.Y or 8), function(E)
		local S = E:GetComponent("SpriteComponent")
		S.Sprite = Desc.Sprite or Paths.Gen
		S.Slice = Desc.Slice or ""
		S.SortingLayer = Desc.Layer or "FX"
		S.OrderInLayer = Desc.Order or 0
		S.Lit = Desc.Lit == true
		if Desc.Color then S.Color = Vector4(Desc.Color[1], Desc.Color[2], Desc.Color[3], Desc.Color[4]) end
		if Desc.Blend then S.Blend = Desc.Blend end
		S.Visible = true
		if Desc.FlipX or Desc.FlipY then E:SetSpriteFlip(Desc.FlipX == true, Desc.FlipY == true) end
		if Desc.Rotation then E:SetRotation(U.Rot2D(Desc.Rotation)) end
		if Desc.Scale then E:SetScale(Vector3(Desc.Scale, 1, Desc.Scale)) end
		if Desc.Flipbook then E:PlayFlipbook(Desc.Flipbook) end
		if OnSpawned then OnSpawned(E) end
	end)
end

function GM:SpawnFx(Flipbook, X, Z, Opt)
	Opt = Opt or {}
	local Sprites = { Fx_Death = "Sprites/Crypt/DeathFx.esprite", Fx_Flame = "Sprites/Crypt/Cemetery.esprite" }
	self:SpawnSprite({ Sprite = Sprites[Flipbook] or Paths.Gen, Flipbook = "Sprites/Crypt/" .. Flipbook .. ".eflipbook", Layer = Opt.Layer or "FX",
	                   X = X, Z = Z, Y = 12, Rotation = Opt.Rotation, FlipX = Opt.FlipX, FlipY = Opt.FlipY, Color = Opt.Color,
	                   Blend = Opt.Blend, Scale = Opt.Scale }, function(E)
		self.Fx[#self.Fx + 1] = { Entity = E, Life = Opt.Life or FxLife[Flipbook] or 0.5, Follow = Opt.Follow }
	end)
end

-- 잔상: 지금 슬라이스 사본이 알파를 잃으며 사라짐
function GM:SpawnAfterimage(Sprite, Slice, X, Z, FlipX, Color, Life)
	self:SpawnSprite({ Sprite = Sprite, Slice = Slice, Layer = "Characters", Order = -1, X = X, Z = Z, Y = 2, FlipX = FlipX,
	                   Color = Color or { 0.6, 0.85, 1.0, 0.6 } }, function(E)
		self.Fades[#self.Fades + 1] = { Entity = E, Life = Life or 0.22, Max = Life or 0.22, Color = Color or { 0.6, 0.85, 1.0, 0.6 } }
	end)
end

function GM:UpdateFx(Dt)
	local Keep = {}
	for _, F in ipairs(self.Fx) do
		F.Life = F.Life - Dt
		if F.Life <= 0 or not F.Entity:IsValid() then
			if F.Entity:IsValid() then F.Entity:Destroy() end
		else
			if F.Follow and F.Follow:IsValid() then
				local P = F.Follow:GetWorldPosition()
				F.Entity:SetPosition(U.V(P.X + (F.OffX or 0), P.Z + (F.OffZ or 0), 12))
			end
			Keep[#Keep + 1] = F
		end
	end
	self.Fx = Keep
	Keep = {}
	for _, F in ipairs(self.Fades) do
		F.Life = F.Life - Dt
		if F.Life <= 0 or not F.Entity:IsValid() then
			if F.Entity:IsValid() then F.Entity:Destroy() end
		else
			local C = F.Color
			F.Entity:GetComponent("SpriteComponent").Color = Vector4(C[1], C[2], C[3], C[4] * (F.Life / F.Max))
			Keep[#Keep + 1] = F
		end
	end
	self.Fades = Keep
end

-- ================================================================ 투사체
-- P: { Team = "Player"|"Enemy", X, Z, VX, VZ, Damage, Crit, Kind = "Bolt"|"Fireball"|"Orb", Radius, Range, Pierce, Explosion, Knockback }
function GM:SpawnProjectile(P)
	local Looks = {
		Bolt = { Slice = "Bolt" }, Fireball = { Sprite = "Sprites/Crypt/Fireball.esprite", Slice = "Fireball0", Flipbook = "Sprites/Crypt/Fx_Fireball.eflipbook" },
		Orb = { Slice = "Orb0", Flipbook = "Sprites/Crypt/Fx_Orb.eflipbook" },
	}
	local Look = Looks[P.Kind] or Looks.Bolt
	P.Radius = P.Radius or 20
	P.Travel = 0
	P.Range = P.Range or 1600
	P.Hit = {}
	P.Pierce = P.Pierce or 0
	P.Life = P.Life or 6
	self.Projectiles[#self.Projectiles + 1] = P
	self:SpawnSprite({ Sprite = Look.Sprite or Paths.Gen, Slice = Look.Slice, Flipbook = Look.Flipbook, Layer = "Projectiles", X = P.X, Z = P.Z, Y = 10,
	                   Rotation = U.AngleOf(P.VX, P.VZ), FlipX = (P.Kind == "Fireball" and P.VX < 0) or nil }, function(E)
		if P.bDone then E:Destroy() else P.Entity = E end
	end)
end

function GM:FinishProjectile(P, X, Z)
	P.bDone = true
	if P.Entity and P.Entity:IsValid() then P.Entity:Destroy() end
	if P.Kind == "Fireball" then
		self:SpawnFx("Fx_Flame", X, Z - 20, { Scale = P.Explosion and P.Explosion > 0 and 1.5 or 1.0 })
		if P.Explosion and P.Explosion > 0 then
			self:Sound("Explode")
			self:AddShake(5, 0.2)
			if P.Team == "Player" then
				for _, E in ipairs(Physics2D.OverlapCircle(U.V(X, Z), P.Explosion, "Enemy")) do
					local S = E:GetScript()
					if S and S.TakeDamage and not P.Hit[E.Id] then
						local DX, DZ = U.Normalize(E:GetWorldPosition().X - X, E:GetWorldPosition().Z - Z)
						S:TakeDamage(P.Damage * 0.7, P.Crit, DX, DZ, P.Knockback or 300)
					end
				end
			end
		end
	elseif P.Kind == "Orb" then
		self:SpawnFx("Fx_Spark", X, Z, { Color = { 0.9, 0.5, 1, 1 } })
	else
		self:SpawnFx("Fx_Spark", X, Z)
	end
end

function GM:UpdateProjectiles(Dt)
	local Keep = {}
	local Player = self:PlayerScript()
	for _, P in ipairs(self.Projectiles) do
		if not P.bDone then
			local Step = U.Length(P.VX, P.VZ) * Dt
			local DX, DZ = U.Normalize(P.VX, P.VZ)
			local Wall = Physics2D.Raycast(U.V(P.X, P.Z), U.V(DX, DZ), Step + P.Radius * 0.5, "Terrain")
			P.X, P.Z = P.X + P.VX * Dt, P.Z + P.VZ * Dt
			P.Travel = P.Travel + Step
			P.Life = P.Life - Dt
			if P.Entity and P.Entity:IsValid() then
				P.Entity:SetPosition(U.V(P.X, P.Z, 10))
			end
			if P.Team == "Player" then
				for _, E in ipairs(Physics2D.OverlapCircle(U.V(P.X, P.Z), P.Radius, "Enemy")) do
					if not P.bDone and not P.Hit[E.Id] then
						local S = E:GetScript()
						if S and S.TakeDamage and not S.bDead then
							P.Hit[E.Id] = true
							S:TakeDamage(P.Damage, P.Crit, DX, DZ, P.Knockback or 250)
							if P.Pierce > 0 then
								P.Pierce = P.Pierce - 1
							else
								self:FinishProjectile(P, P.X, P.Z)
							end
						end
					end
				end
			elseif Player and not Player.bDead then
				local PX, PZ = self:PlayerPos()
				-- 캡슐(반지름 32, 높이 160) 근사: 세로 선분까지 거리
				local CZ = U.Clamp(P.Z, PZ - 48, PZ + 48)
				if U.Length(P.X - PX, P.Z - CZ) < P.Radius + 30 then
					if Player:TakeDamage(P.Damage, P.X, P.Z) or Player:IsInvulnerable() then
						self:FinishProjectile(P, P.X, P.Z)
					end
				end
			end
			if not P.bDone and (Wall ~= nil or P.Travel > P.Range or P.Life <= 0) then
				local HX, HZ = P.X, P.Z
				if Wall then HX, HZ = Wall.Point.X, Wall.Point.Z end
				self:FinishProjectile(P, HX, HZ)
			end
			if not P.bDone then Keep[#Keep + 1] = P end
		end
	end
	self.Projectiles = Keep
end

-- ================================================================ 줍기 (코인·하트·무기)
function GM:SpawnPickup(Kind, X, Z, VX, VZ, WeaponId)
	local Item = { Kind = Kind, X = X, Z = Z, VX = VX or 0, VZ = VZ or 0, Age = 0, WeaponId = WeaponId }
	self.Pickups[#self.Pickups + 1] = Item
	local Desc = { Layer = "Pickups", X = X, Z = Z }
	if Kind == "Coin" then
		Desc.Slice, Desc.Flipbook = "Coin0", "Sprites/Crypt/Fx_Coin.eflipbook"
	elseif Kind == "Heart" then
		Desc.Slice = "Heart"
	else
		Desc.Slice = CryptData.Weapon(WeaponId).Slice
		Desc.Rotation = 45
	end
	self:SpawnSprite(Desc, function(E)
		if Item.bDone then E:Destroy() else Item.Entity = E end
	end)
	return Item
end

function GM:UpdatePickups(Dt)
	local PX, PZ = self:PlayerPos()
	local Player = self:PlayerScript()
	local Keep = {}
	for _, Item in ipairs(self.Pickups) do
		Item.Age = Item.Age + Dt
		if Item.Kind == "Weapon" then
			-- 무기: 떠서 흔들림, F로 바꿔 든다 (상호작용)
			Item.Bob = (Item.Bob or 0) + Dt
			if Item.Entity and Item.Entity:IsValid() then
				Item.Entity:SetPosition(U.V(Item.X, Item.Z + 14 + math.sin(Item.Bob * 3) * 6, 8))
			end
			Keep[#Keep + 1] = Item
		else
			local DX, DZ = PX - Item.X, PZ - Item.Z
			local Dist = U.Length(DX, DZ)
			if Item.Age > 0.5 and Dist < 260 and Player and not Player.bDead then
				-- 자석
				local NX, NZ = U.Normalize(DX, DZ)
				local Pull = 1400 * (1 - Dist / 300)
				Item.VX = U.Lerp(Item.VX, NX * Pull, U.Smooth(10, Dt))
				Item.VZ = U.Lerp(Item.VZ, NZ * Pull, U.Smooth(10, Dt))
			else
				Item.VZ = Item.VZ - 2400 * Dt
			end
			local NX, NZ = Item.X + Item.VX * Dt, Item.Z + Item.VZ * Dt
			if Item.VZ < 0 then
				local Hit = Physics2D.Raycast(U.V(Item.X, Item.Z + 4), U.V(0, -1), -Item.VZ * Dt + 6, "Terrain")
				if Hit then
					NZ = Hit.Point.Z + 2
					Item.VZ = (Item.VZ < -300) and -Item.VZ * 0.35 or 0
					Item.VX = Item.VX * 0.6
				end
			end
			if Item.VX ~= 0 then
				local DirX = U.Sign(Item.VX)
				if Physics2D.Raycast(U.V(Item.X, Item.Z + 10), U.V(DirX, 0), math.abs(Item.VX * Dt) + 8, "Terrain") then
					Item.VX = -Item.VX * 0.4
					NX = Item.X
				end
			end
			Item.X, Item.Z = NX, NZ
			if Item.Entity and Item.Entity:IsValid() then Item.Entity:SetPosition(U.V(Item.X, Item.Z, 8)) end
			if Item.Age > 0.3 and Dist < 60 and Player and not Player.bDead then
				Item.bDone = true
				if Item.Entity and Item.Entity:IsValid() then Item.Entity:Destroy() end
				if Item.Kind == "Coin" then
					self.Gold = self.Gold + 1
					self:Sound("Coin")
				else
					Player:Heal(self.Balance.HeartHeal)
					self:Sound("Heal")
					self:ShowNumber(PX, PZ + 100, "+" .. math.floor(self.Balance.HeartHeal), { 0.4, 1, 0.5, 1 })
				end
				self:RefreshHud()
			else
				Keep[#Keep + 1] = Item
			end
		end
	end
	self.Pickups = Keep
end

-- ================================================================ 상호작용 (상자·떨어진 무기·포털)
function GM:UpdateInteraction()
	local PX, PZ = self:PlayerPos()
	local Best, BestDist = nil, 130
	for _, I in ipairs(self.Interactables) do
		if not I.bUsed then
			local D = U.Length(I.X - PX, I.Z - PZ)
			if D < BestDist then Best, BestDist = I, D end
		end
	end
	for _, Item in ipairs(self.Pickups) do
		if Item.Kind == "Weapon" and not Item.bDone then
			local D = U.Length(Item.X - PX, Item.Z - PZ)
			if D < BestDist then
				Best, BestDist = { Kind = "Weapon", Item = Item, Text = "F  " .. CryptData.Weapon(Item.WeaponId).DisplayName .. " 들기" }, D
			end
		end
	end
	self.Focus = Best
	if self.Hud then self.Hud:ShowPrompt(Best and Best.Text or nil) end
end

function GM:Interact()
	local I = self.Focus
	if I == nil or self.bGameOver then return end
	if I.Kind == "Chest" then
		I.bUsed = true
		I.Entity:GetComponent("SpriteComponent").Slice = "ChestOpen"
		self:Sound("ChestOpen")
		self:SpawnFx("Fx_Spark", I.X, I.Z + 20)
		-- 들고 있지 않은 무기 하나 (층 조건) + 코인
		local Choices = {}
		for _, Id in ipairs(CryptData.WeaponIds()) do
			if Id ~= self.Weapons[1] and Id ~= self.Weapons[2] and CryptData.Weapon(Id).MinFloor <= self.Floor then Choices[#Choices + 1] = Id end
		end
		if #Choices > 0 then
			self:SpawnPickup("Weapon", I.X, I.Z + 40, 0, 0, self.Rng:Pick(Choices))
		end
		for _ = 1, 6 do self:SpawnPickup("Coin", I.X, I.Z + 30, self.Rng:Range(-300, 300), self.Rng:Range(400, 700)) end
		Log.Info("[Crypt2D] 상자 열림")
	elseif I.Kind == "Weapon" then
		local Item = I.Item
		local Old = self.Weapons[self.WeaponSlot]
		self.Weapons[self.WeaponSlot] = Item.WeaponId
		Item.WeaponId = Old
		if Item.Entity and Item.Entity:IsValid() then
			Item.Entity:GetComponent("SpriteComponent").Slice = CryptData.Weapon(Old).Slice
		end
		self:Sound("Equip")
		local Player = self:PlayerScript()
		if Player then Player:OnWeaponsChanged() end
		self:RefreshHud()
		self:Toast(CryptData.Weapon(self.Weapons[self.WeaponSlot]).DisplayName, 1.2)
	elseif I.Kind == "Shop" then
		self:BuyOffer(I)
	elseif I.Kind == "Portal" then
		I.bUsed = true
		self:Sound("Portal")
		Log.Info(string.format("[Crypt2D] 포털 → %d층", self.Floor + 1))
		self:BuildFloor(self.Floor + 1)
	end
end

-- ================================================================ 전투 공용
function GM:DamagePlayer(Amount, SX, SZ)
	local Player = self:PlayerScript()
	if Player then return Player:TakeDamage(Amount, SX, SZ) end
	return false
end

-- 히트스톱: 실제 시간 Seconds 동안 게임 전체 정지 (겹치면 긴 쪽 — 엔진 Game.HitStop)
function GM:SetHitStop(Seconds)
	Game.HitStop(Seconds)
end

function GM:AddShake(Amount, Time)
	self.Shake = math.max(self.Shake * U.Clamp(self.ShakeTime / 0.25, 0, 1), Amount)
	self.ShakeTime = math.max(self.ShakeTime, Time)
end

function GM:Sound(Name)
	Audio.PlayOneShot("Audio/Crypt/" .. Name .. ".wav")
end

function GM:ShowNumber(X, Z, Text, Color, Scale)
	if self.Hud then self.Hud:ShowDamage(X, Z, Text, Color, Scale) end
end

function GM:Toast(Text, Time)
	if self.Hud then self.Hud:ShowToast(Text, Time) end
end

function GM:OnEnemyDied(Enemy, Def, X, Z)
	self.Kills = self.Kills + 1
	local Count = self.Rng:Int(Def.GoldMin, Def.GoldMax)
	for _ = 1, Count do
		self:SpawnPickup("Coin", X, Z + 20, self.Rng:Range(-250, 250), self.Rng:Range(350, 700))
	end
	self:SpawnFx(Def.Behavior == "Boss" and "Fx_Death" or "Fx_Death", X, Z, { Scale = Def.Behavior == "Boss" and 2.5 or 1.0 })
	self:Sound("Explode")
	self:RefreshHud()
end

function GM:OnBossDefeated()
	Log.Info("[Crypt2D] 보스 처치")
	self.bVictory = true
	self:Toast("세라핌 처치!", 3.0)
	if self.Hud then self.Hud:ShowBoss(nil) end
	self:EndRun(true)
end

function GM:OnPlayerDied()
	Log.Info("[Crypt2D] 플레이어 사망")
	self:EndRun(false)
end

function GM:EndRun(bWin)
	if self.bGameOver then return end
	self.bGameOver = true
	self.bWin = bWin
	self.GameOverTimer = 0.0
	local R = self.Record
	R.BestFloor = math.max(R.BestFloor or 0, self.Floor)
	R.BestKills = math.max(R.BestKills or 0, self.Kills)
	R.MostGold  = math.max(R.MostGold or 0, self.Gold)
	if bWin then
		R.Wins = (R.Wins or 0) + 1
		if R.BestTime == nil or self.RunTime < R.BestTime then R.BestTime = math.floor(self.RunTime) end
	end
	SaveGame.Save(SaveSlot, R)
end

function GM:ShowResult()
	local Text = string.format("도달 층  %d\n처치  %d\n코인  %d\n시간  %d:%02d\n\n최고 기록 — %d층 · %d처치 · 승리 %d회",
		self.Floor, self.Kills, self.Gold, math.floor(self.RunTime / 60), math.floor(self.RunTime % 60),
		self.Record.BestFloor or 0, self.Record.BestKills or 0, self.Record.Wins or 0)
	if self.Hud then
		if self.bWin then self.Hud:ShowVictory(Text) else self.Hud:ShowDeath(Text) end
	end
	-- 결과 화면: 뒤 세계는 멈추고(시간 배율 0) 커서를 보인다. 게임 입력도 받아 Confirm(Enter/R)으로 다시 시작 (단추는 UI가 먼저)
	Game.SetTimeScale(0)
	Game.SetInputMode("GameAndUI")
	Game.SetMouseLocked(false)
	Game.SetCursorVisible(true)
end

function GM:SetPaused(bPaused)
	self.bPaused = bPaused
	Game.SetTimeScale(bPaused and 0 or 1) -- 이동기·물리·플립북까지 멈춘다 (공중에서 멈춰도 떨어지지 않는다)
	if self.Hud then self.Hud:ShowPause(bPaused) end
	self:SetGameplayInput(not bPaused)
	Log.Info(bPaused and "[Crypt2D] 일시정지" or "[Crypt2D] 계속")
end

function GM:Restart()
	Game.OpenScene(Game.GetCurrentScene() ~= "" and Game.GetCurrentScene() or "Scenes/Crypt.escene")
end

function GM:GoTitle()
	Game.OpenScene("Scenes/Title.escene")
end

-- HUD 메뉴 단추 (Hud.lua가 넘긴다)
function GM:OnMenu(Name)
	self:Sound("UIConfirm")
	if Name == "Resume" then self:SetPaused(false)
	elseif Name == "Restart" then self:Restart()
	elseif Name == "Title" then self:GoTitle()
	elseif Name == "Quit" then Game.Quit() end
end

function GM:RefreshHud()
	local Hud = self.Hud
	if Hud == nil then return end
	local Player = self:PlayerScript()
	if Player then
		Hud:SetHealth(Player.Health, Player.MaxHealth)
		Hud:SetDash(Player.DashCharges, Player.MaxDashCharges, Player.DashRechargeFraction or 0)
	end
	Hud:SetGold(self.Gold, self.Kills)
	Hud:SetFloor(self.Floor >= FinalFloor and "최하층" or string.format("지하 %d층", self.Floor))
	Hud:SetWeapons(self.Weapons, self.WeaponSlot)
end

function GM:CurrentWeapon()
	return CryptData.Weapon(self.Weapons[self.WeaponSlot])
end

function GM:SwapWeapon()
	self.WeaponSlot = 3 - self.WeaponSlot
	self:Sound("Equip")
	self:RefreshHud()
end

return GM
