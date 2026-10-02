-- RPG 게임 매니저 (Phase 45 트랙 D): 아이템 정의, 가방(20칸, 겹치기), 장비, 골드, 상점 거래, 전리품, 알림, 저장.
--   씬 엔티티 이름 "GameManager". 다른 스크립트는 다음처럼 얻고 nil이면 조용히 건너뛴다:
--     local gm = Scene.Find("GameManager"); gm = gm and gm:GetScript()
--   공개 API (공통 계약):
--     gm:GetGold() / gm:AddGold(n) / gm:SpendGold(n) → bool
--     gm:GiveItem(id, count) → bool (전부 들어갈 때만 넣는다, "gold"는 골드) / gm:RemoveItem(id, count) → bool / gm:CountItem(id)
--     gm:GetItemDef(id) → { Id, Name, Type("Weapon"|"Shield"|"Consumable"|"Loot"), Icon, Model, Price, Damage, Defense, Heal, Mana, Description, ... }
--     gm:SpawnLoot(pos, lootTableId) — 전리품 표(Skeleton_Warrior/Minion/Rogue/Mage, Chest)를 굴려 줍는 아이템(Prefabs/RPG/Pickup.eprefab)을 흩뿌린다
--     gm:Notify(text) — 화면 알림(HUD) / gm:ShowDamageNumber(worldPos, amount, kind), gm:TrackEnemy(entity) — 화면 표시는 HUD(머리 위 체력바·데미지 숫자)
--   UI/상호작용용 추가 API: GetSlot/UseSlot/UseItem/UseQuickPotion/EquipSlot/Unequip/GetEquipped/GetEquipmentStats,
--     BuyItem/SellSlot/GetSellPrice/ParseStock, SetPrompt/ClearPrompt/GetPrompt, GetToasts, OpenWindow/CloseWindow/IsAnyWindowOpen,
--     PlaySound, GetPlayer/GetPlayerScript, Save/Load. 화면은 Revision이 바뀔 때만 다시 그리면 된다.
-- 다른 스크립트보다 OnStart가 늦게 돌 수 있으므로 모든 공개 함수는 EnsureInit()으로 상태를 먼저 만든다.
local GameManager = {
	Properties = {
		StartGold     = 30,       -- 저장이 없을 때 시작 골드
		SaveSlot      = "RPGDemo",
		LoadOnStart   = true,     -- 시작 시 저장 불러오기
		AutoSave      = true,     -- 바뀌면 잠시 뒤 저장
		PlayerName    = "Player", -- 플레이어 엔티티 이름
		PickupPrefab  = "Prefabs/RPG/Pickup.eprefab",
		DebugFillBag  = false,    -- 시험용: 시작할 때 아이템을 여러 개 넣는다
	},
}

local SlotCount     = 20
local ToastDuration = 3.5
local MaxToasts     = 4
local IconDir       = "Asset/Icons/7Soul_RPG_Icons/"

-- ---------------------------------------------------------------- 아이템 정의
-- Price = 상점 구매가 (판매가는 절반), MaxStack = 한 칸에 겹치는 수, Model = 줍는 아이템/장착 모델 (Content 기준)
local Items = {
	gold = { Name = "골드", Type = "Loot", bCurrency = true, Icon = IconDir .. "I_GoldCoin.png",
		Model = "Asset/KayKit/Dungeon/coin.glb", ModelScale = 1.6, Price = 1, Description = "어디서나 쓰이는 금화." },

	potion_hp_small = { Name = "체력 물약(소)", Type = "Consumable", Icon = IconDir .. "P_Red02.png",
		Model = "Asset/KayKit/Dungeon/bottle_A_labeled_brown.glb", ModelScale = 1.4, Price = 15, Heal = 30, MaxStack = 20,
		Description = "붉은 약초를 달인 물약. 체력을 조금 회복한다." },
	potion_hp_large = { Name = "체력 물약(대)", Type = "Consumable", Icon = IconDir .. "P_Red01.png",
		Model = "Asset/KayKit/Dungeon/bottle_A_labeled_brown.glb", ModelScale = 2.0, Price = 40, Heal = 80, MaxStack = 20,
		Description = "진하게 졸인 물약. 체력을 크게 회복한다." },
	potion_mp = { Name = "마나 물약", Type = "Consumable", Icon = IconDir .. "P_Blue01.png",
		Model = "Asset/KayKit/Dungeon/bottle_A_labeled_green.glb", ModelScale = 1.4, Price = 20, Mana = 40, MaxStack = 20,
		Description = "푸른 빛이 도는 물약. 마나를 회복한다." },

	dagger = { Name = "단검", Type = "Weapon", Icon = IconDir .. "W_Dagger002.png",
		Model = "Asset/KayKit/Weapons/dagger.gltf", ModelScale = 1.0, Price = 30, Damage = 8,
		Description = "가볍고 빠른 단검." },
	sword_1handed = { Name = "기사의 검", Type = "Weapon", Icon = IconDir .. "W_Sword001.png",
		Model = "Asset/KayKit/Weapons/sword_1handed.gltf", ModelScale = 0.8, Price = 60, Damage = 12,
		Description = "균형 잡힌 한손검. 기사단의 표준 장비." },
	axe_1handed = { Name = "손도끼", Type = "Weapon", Icon = IconDir .. "W_Axe001.png",
		Model = "Asset/KayKit/Weapons/axe_1handed.gltf", ModelScale = 0.8, Price = 80, Damage = 15,
		Description = "묵직한 한손 도끼. 뼈도 쪼갠다." },
	sword_2handed = { Name = "대검", Type = "Weapon", Icon = IconDir .. "W_Sword007.png",
		Model = "Asset/KayKit/Weapons/sword_2handed.gltf", ModelScale = 0.7, Price = 150, Damage = 22, bTwoHanded = true,
		Description = "두 손으로 휘두르는 거대한 검. 방패와 함께 쓸 수 없다." },

	shield_round = { Name = "원형 방패", Type = "Shield", Icon = IconDir .. "E_Wood02.png",
		Model = "Asset/KayKit/Weapons/shield_round.gltf", ModelScale = 0.8, Price = 40, Defense = 1,
		Description = "나무로 만든 둥근 방패." },
	shield_square = { Name = "사각 방패", Type = "Shield", Icon = IconDir .. "E_Wood03.png",
		Model = "Asset/KayKit/Weapons/shield_square.gltf", ModelScale = 0.8, Price = 70, Defense = 2,
		Description = "몸을 넓게 가려 주는 사각 방패." },
	shield_badge = { Name = "문장 방패", Type = "Shield", Icon = IconDir .. "E_Metal08.png",
		Model = "Asset/KayKit/Weapons/shield_badge.gltf", ModelScale = 0.8, Price = 110, Defense = 3,
		Description = "왕국의 문장이 새겨진 강철 방패." },
	shield_spikes = { Name = "가시 방패", Type = "Shield", Icon = IconDir .. "E_Metal02.png",
		Model = "Asset/KayKit/Weapons/shield_spikes.gltf", ModelScale = 0.8, Price = 160, Defense = 4,
		Description = "가시가 박힌 방패. 막는 것만으로도 위협적이다." },

	bone = { Name = "뼈 조각", Type = "Loot", Icon = IconDir .. "I_Bone.png",
		Model = "Asset/KayKit/Village/sack.gltf", ModelScale = 0.35, Price = 6, MaxStack = 99,
		Description = "해골 병사에게서 떨어진 뼈. 상인이 사 간다." },
	fang = { Name = "날카로운 송곳니", Type = "Loot", Icon = IconDir .. "I_Fang.png",
		Model = "Asset/KayKit/Village/sack.gltf", ModelScale = 0.35, Price = 12, MaxStack = 99,
		Description = "해골 도적의 송곳니. 장신구 재료로 쓰인다." },
	magic_shard = { Name = "마력 결정", Type = "Loot", Icon = IconDir .. "I_Crystal01.png",
		Model = "Asset/KayKit/Village/sack.gltf", ModelScale = 0.35, Price = 24, MaxStack = 99,
		Description = "해골 마법사의 마력이 굳은 결정." },
	ruby = { Name = "루비", Type = "Loot", Icon = IconDir .. "I_Ruby.png",
		Model = "Asset/KayKit/Dungeon/coin_stack_small.glb", ModelScale = 1.2, Price = 80, MaxStack = 99,
		Description = "붉게 빛나는 보석. 비싸게 팔린다." },
}
for Id, Def in pairs(Items) do
	Def.Id          = Id
	Def.Damage      = Def.Damage or 0
	Def.Defense     = Def.Defense or 0
	Def.Heal        = Def.Heal or 0
	Def.Mana        = Def.Mana or 0
	Def.MaxStack    = Def.MaxStack or 1
	Def.ModelScale  = Def.ModelScale or 1.0
	Def.Description = Def.Description or ""
end

local TypeNames = { Weapon = "무기", Shield = "방패", Consumable = "소모품", Loot = "전리품" }

-- ---------------------------------------------------------------- 전리품 표
-- Gold = { 최소, 최대 } (항상), Drops = { 아이템, 확률(0~1), 최소, 최대 }
local LootTables = {
	skeletonwarrior = { Gold = { 5, 12 }, Drops = {
		{ "bone", 0.6, 1, 2 }, { "potion_hp_small", 0.25, 1, 1 }, { "sword_1handed", 0.06, 1, 1 }, { "shield_round", 0.06, 1, 1 } } },
	skeletonminion = { Gold = { 2, 6 }, Drops = {
		{ "bone", 0.7, 1, 2 }, { "potion_hp_small", 0.15, 1, 1 }, { "axe_1handed", 0.04, 1, 1 } } },
	skeletonrogue = { Gold = { 4, 10 }, Drops = {
		{ "fang", 0.5, 1, 1 }, { "potion_mp", 0.2, 1, 1 }, { "dagger", 0.08, 1, 1 } } },
	skeletonmage = { Gold = { 6, 14 }, Drops = {
		{ "magic_shard", 0.5, 1, 1 }, { "potion_mp", 0.35, 1, 1 }, { "ruby", 0.05, 1, 1 }, { "shield_badge", 0.03, 1, 1 } } },
	chest = { Gold = { 20, 40 }, Drops = {
		{ "potion_hp_large", 0.5, 1, 1 }, { "ruby", 0.2, 1, 1 }, { "shield_square", 0.15, 1, 1 } } },
	-- 시험용: 모든 종류가 확실히 나온다
	test = { Gold = { 5, 5 }, Drops = { { "potion_hp_small", 1.0, 2, 2 }, { "bone", 1.0, 1, 1 }, { "sword_2handed", 1.0, 1, 1 } } },
}

-- "Skeleton_Warrior", "SkeletonWarrior", "skeleton warrior" 모두 같은 표
local function NormalizeTableId(Id)
	return (tostring(Id or ""):lower():gsub("[%s_%-]", ""))
end

-- 다른 스크립트(플레이어 등)의 메서드를 안전하게 부른다: 없거나 오류면 nil (오류는 경고 로그 한 번)
local function SafeCall(Target, Method, ...)
	if Target == nil then
		return nil
	end
	local Function = Target[Method]
	if type(Function) ~= "function" then
		return nil
	end
	local bOk, Result = pcall(Function, Target, ...)
	if not bOk then
		Log.Warn("GameManager: " .. Method .. " 호출 실패:", Result)
		return nil
	end
	return Result
end

-- ---------------------------------------------------------------- 초기화
function GameManager:EnsureInit()
	if self.bInit then
		return
	end
	self.bInit        = true
	self.Gold         = 0
	self.Slots        = {}         -- [1..20] = { Id, Count } 또는 nil
	self.Equipped     = { Weapon = nil, Shield = nil } -- 아이템 id
	self.Revision     = 1          -- 가방/골드/장비가 바뀔 때마다 증가 (UI는 이 값이 바뀔 때만 다시 그린다)
	self.Toasts       = {}
	self.Prompt       = nil        -- { Owner, Text, Key }
	self.OpenWindows  = {}
	self.SaveTimer    = -1
	self.Clock        = 0
	self.EquipApplied = false
	self.PlayerFoundFrames = 0

	local bLoaded = false
	if self.Properties.LoadOnStart then
		bLoaded = self:Load()
	end
	if not bLoaded then
		self.Gold = self.Properties.StartGold
		self:GiveItem("potion_hp_small", 3)
		self:GiveItem("potion_mp", 1)
		-- 새 게임 장비 = 플레이어 프리팹 기본 장비 (비워 두면 ApplyEquipment가 맨손으로 만든다)
		self.Equipped.Weapon = "sword_1handed"
		self.Equipped.Shield = "shield_badge"
	end
	if self.Properties.DebugFillBag then
		self:GiveItem("potion_hp_large", 2)
		self:GiveItem("bone", 7)
		self:GiveItem("magic_shard", 2)
		self:GiveItem("ruby", 1)
		self:GiveItem("axe_1handed", 1)
		self:GiveItem("shield_badge", 1)
		self:GiveItem("dagger", 1)
		self.Equipped.Weapon = self.Equipped.Weapon or "sword_1handed"
		self.Equipped.Shield = self.Equipped.Shield or "shield_round"
		self.Gold = self.Gold + 250
	end
	self.SaveTimer = -1 -- 시작 상태는 저장하지 않는다
end

function GameManager:OnStart()
	self:EnsureInit()
	Log.Info(string.format("[RPG] 게임 매니저 시작: 골드 %d, 가방 %d칸 사용", self.Gold, self:CountUsedSlots()))
end

function GameManager:OnUpdate(dt)
	self:EnsureInit()
	self.Clock = self.Clock + dt

	-- 플레이어 스크립트가 생기면 몇 프레임 뒤(플레이어 OnStart 이후) 장비를 한 번 적용
	if not self.EquipApplied then
		if self:GetPlayerScript() ~= nil then
			self.PlayerFoundFrames = self.PlayerFoundFrames + 1
			if self.PlayerFoundFrames >= 3 then
				self.EquipApplied = true
				self:ApplyEquipment()
			end
		end
	end

	-- 알림 수명
	for Index = #self.Toasts, 1, -1 do
		if self.Clock - self.Toasts[Index].Time > ToastDuration then
			table.remove(self.Toasts, Index)
		end
	end

	-- 자동 저장 (바뀐 뒤 1.5초)
	if self.SaveTimer >= 0 then
		self.SaveTimer = self.SaveTimer - dt
		if self.SaveTimer < 0 then
			self:Save()
		end
	end
end

function GameManager:OnDestroy()
	if self.bInit and self.SaveTimer >= 0 then
		self:Save()
	end
end

function GameManager:MarkChanged()
	self.Revision = self.Revision + 1
	if self.Properties.AutoSave then
		self.SaveTimer = 1.5
	end
end

-- ---------------------------------------------------------------- 플레이어
function GameManager:GetPlayer()
	if self.PlayerEntity ~= nil and self.PlayerEntity:IsValid() then
		return self.PlayerEntity
	end
	-- 없으면 가끔만 다시 찾는다 (Scene.Find는 선형 탐색)
	if self.NextPlayerSearch ~= nil and (self.Clock or 0) < self.NextPlayerSearch then
		return nil
	end
	self.NextPlayerSearch = (self.Clock or 0) + 1.0
	self.PlayerEntity = Scene.Find(self.Properties.PlayerName)
	return self.PlayerEntity
end

function GameManager:GetPlayerScript()
	local Player = self:GetPlayer()
	return Player and Player:GetScript() or nil
end

-- ---------------------------------------------------------------- 골드
function GameManager:GetGold()
	self:EnsureInit()
	return self.Gold
end

function GameManager:AddGold(Amount)
	self:EnsureInit()
	Amount = math.floor(tonumber(Amount) or 0)
	if Amount <= 0 then
		return
	end
	self.Gold = self.Gold + Amount
	self:MarkChanged()
end

function GameManager:SpendGold(Amount)
	self:EnsureInit()
	Amount = math.floor(tonumber(Amount) or 0)
	if Amount < 0 or self.Gold < Amount then
		return false
	end
	self.Gold = self.Gold - Amount
	self:MarkChanged()
	return true
end

-- ---------------------------------------------------------------- 아이템 정의
function GameManager:GetItemDef(Id)
	return Items[Id]
end

function GameManager:GetItemIds()
	local Ids = {}
	for Id in pairs(Items) do
		Ids[#Ids + 1] = Id
	end
	table.sort(Ids)
	return Ids
end

function GameManager:GetTypeName(Def)
	return Def and TypeNames[Def.Type] or ""
end

-- 툴팁용 능력치 한 줄
function GameManager:DescribeStats(Def)
	if Def == nil then
		return ""
	end
	local Parts = {}
	if Def.Damage > 0 then Parts[#Parts + 1] = "공격력 +" .. Def.Damage end
	if Def.Defense > 0 then Parts[#Parts + 1] = "방어력 +" .. Def.Defense end
	if Def.Heal > 0 then Parts[#Parts + 1] = "체력 회복 " .. Def.Heal end
	if Def.Mana > 0 then Parts[#Parts + 1] = "마나 회복 " .. Def.Mana end
	if Def.bTwoHanded then Parts[#Parts + 1] = "양손" end
	return table.concat(Parts, "   ")
end

function GameManager:GetSellPrice(Id)
	local Def = Items[Id]
	if Def == nil then
		return 0
	end
	return math.max(1, math.floor(Def.Price / 2))
end

-- ---------------------------------------------------------------- 가방
function GameManager:GetSlot(Index)
	self:EnsureInit()
	return self.Slots[Index]
end

function GameManager:GetSlotCount()
	return SlotCount
end

function GameManager:CountUsedSlots()
	local Used = 0
	for Index = 1, SlotCount do
		if self.Slots[Index] ~= nil then
			Used = Used + 1
		end
	end
	return Used
end

function GameManager:CountItem(Id)
	self:EnsureInit()
	if Id == "gold" then
		return self.Gold
	end
	local Total = 0
	for Index = 1, SlotCount do
		local Slot = self.Slots[Index]
		if Slot ~= nil and Slot.Id == Id then
			Total = Total + Slot.Count
		end
	end
	return Total
end

-- 이 수만큼 들어갈 자리가 있는가
function GameManager:CanAdd(Id, Count)
	local Def = Items[Id]
	if Def == nil then
		return false
	end
	if Def.bCurrency then
		return true
	end
	local Room = 0
	for Index = 1, SlotCount do
		local Slot = self.Slots[Index]
		if Slot == nil then
			Room = Room + Def.MaxStack
		elseif Slot.Id == Id then
			Room = Room + (Def.MaxStack - Slot.Count)
		end
		if Room >= Count then
			return true
		end
	end
	return Room >= Count
end

function GameManager:GiveItem(Id, Count)
	self:EnsureInit()
	Count = math.floor(tonumber(Count) or 1)
	local Def = Items[Id]
	if Def == nil then
		Log.Warn("GameManager: 알 수 없는 아이템", Id)
		return false
	end
	if Count <= 0 then
		return true
	end
	if Def.bCurrency then
		self:AddGold(Count)
		return true
	end
	if not self:CanAdd(Id, Count) then
		return false
	end
	-- 같은 아이템 칸부터 채우고 남으면 빈 칸
	local Left = Count
	for Index = 1, SlotCount do
		local Slot = self.Slots[Index]
		if Left > 0 and Slot ~= nil and Slot.Id == Id and Slot.Count < Def.MaxStack then
			local Add = math.min(Left, Def.MaxStack - Slot.Count)
			Slot.Count = Slot.Count + Add
			Left = Left - Add
		end
	end
	for Index = 1, SlotCount do
		if Left > 0 and self.Slots[Index] == nil then
			local Add = math.min(Left, Def.MaxStack)
			self.Slots[Index] = { Id = Id, Count = Add }
			Left = Left - Add
		end
	end
	self:MarkChanged()
	return true
end

function GameManager:RemoveItem(Id, Count)
	self:EnsureInit()
	Count = math.floor(tonumber(Count) or 1)
	if Id == "gold" then
		return self:SpendGold(Count)
	end
	if self:CountItem(Id) < Count then
		return false
	end
	-- 뒤 칸부터 뺀다 (앞쪽 칸 배치 유지)
	local Left = Count
	for Index = SlotCount, 1, -1 do
		local Slot = self.Slots[Index]
		if Left > 0 and Slot ~= nil and Slot.Id == Id then
			local Take = math.min(Left, Slot.Count)
			Slot.Count = Slot.Count - Take
			Left = Left - Take
			if Slot.Count <= 0 then
				self.Slots[Index] = nil
			end
		end
	end
	self:MarkChanged()
	return true
end

-- ---------------------------------------------------------------- 사용 / 장착
-- 가방 칸 클릭: 소모품은 사용, 무기/방패는 장착, 전리품은 안내
function GameManager:UseSlot(Index)
	self:EnsureInit()
	local Slot = self.Slots[Index]
	if Slot == nil then
		return false
	end
	local Def = Items[Slot.Id]
	if Def.Type == "Consumable" then
		return self:UseItem(Slot.Id)
	elseif Def.Type == "Weapon" or Def.Type == "Shield" then
		return self:EquipSlot(Index)
	end
	self:Notify(Def.Name .. ": 상인에게 팔 수 있습니다")
	return false
end

function GameManager:UseItem(Id)
	self:EnsureInit()
	local Def = Items[Id]
	if Def == nil or Def.Type ~= "Consumable" or self:CountItem(Id) <= 0 then
		return false
	end
	local PlayerScript = self:GetPlayerScript()
	if SafeCall(PlayerScript, "IsDead") then
		return false
	end
	local Stats = SafeCall(PlayerScript, "GetStats")
	if Stats ~= nil then
		local bHealthFull = Def.Heal <= 0 or (Stats.Health or 0) >= (Stats.MaxHealth or 0)
		local bManaFull   = Def.Mana <= 0 or (Stats.Mana or 0) >= (Stats.MaxMana or 0)
		if bHealthFull and bManaFull then
			self:Notify(Def.Heal > 0 and "이미 체력이 가득합니다" or "이미 마나가 가득합니다")
			self:PlaySound("Error")
			return false
		end
	end
	self:RemoveItem(Id, 1)
	if Def.Heal > 0 then
		SafeCall(PlayerScript, "RestoreHealth", Def.Heal)
	end
	if Def.Mana > 0 then
		SafeCall(PlayerScript, "RestoreMana", Def.Mana)
	end
	self:PlaySound("Potion")
	self:Notify(Def.Heal > 0 and string.format("체력 +%d", Def.Heal) or string.format("마나 +%d", Def.Mana))
	return true
end

-- 퀵 슬롯: 1 = 체력 물약 (작은 것 먼저), 2 = 마나 물약. 반환: 사용한 아이템 id 또는 nil
local QuickSlots = { { "potion_hp_small", "potion_hp_large" }, { "potion_mp" } }

function GameManager:GetQuickPotion(Index)
	self:EnsureInit()
	local Candidates = QuickSlots[Index] or {}
	for _, Id in ipairs(Candidates) do
		if self:CountItem(Id) > 0 then
			return Id
		end
	end
	return Candidates[1]
end

function GameManager:CountQuickPotion(Index)
	self:EnsureInit()
	local Total = 0
	for _, Id in ipairs(QuickSlots[Index] or {}) do
		Total = Total + self:CountItem(Id)
	end
	return Total
end

function GameManager:UseQuickPotion(Index)
	local Id = self:GetQuickPotion(Index)
	if Id == nil or self:CountItem(Id) <= 0 then
		local Def = Id and Items[Id]
		self:Notify((Def and Def.Name or "물약") .. "이 없습니다")
		self:PlaySound("Error")
		return nil
	end
	return self:UseItem(Id) and Id or nil
end

function GameManager:GetEquipped(Kind)
	self:EnsureInit()
	return self.Equipped[Kind]
end

-- 장비 합계 (툴팁/가방 표시용). 플레이어가 GetAttackPower를 주면 그 값을 함께
function GameManager:GetEquipmentStats()
	self:EnsureInit()
	local Damage, Defense = 0, 0
	for _, Id in pairs(self.Equipped) do
		local Def = Items[Id]
		if Def ~= nil then
			Damage  = Damage + Def.Damage
			Defense = Defense + Def.Defense
		end
	end
	return Damage, Defense, SafeCall(self:GetPlayerScript(), "GetAttackPower")
end

function GameManager:EquipSlot(Index)
	self:EnsureInit()
	local Slot = self.Slots[Index]
	local Def  = Slot and Items[Slot.Id]
	if Def == nil or (Def.Type ~= "Weapon" and Def.Type ~= "Shield") then
		return false
	end
	local Kind = Def.Type
	-- 양손 무기 ↔ 방패
	if Kind == "Shield" then
		local Weapon = Items[self.Equipped.Weapon or ""]
		if Weapon ~= nil and Weapon.bTwoHanded then
			self:Notify("양손 무기를 든 채로는 방패를 쓸 수 없습니다")
			self:PlaySound("Error")
			return false
		end
	elseif Def.bTwoHanded and self.Equipped.Shield ~= nil then
		-- 방패를 가방으로 (자리가 있어야 함: 지금 칸은 무기를 빼면 비므로 그 칸 외 하나 더 필요할 수 있다)
		if not self:Unequip("Shield", true) then
			self:Notify("가방이 가득 차 방패를 벗을 수 없습니다")
			self:PlaySound("Error")
			return false
		end
		Slot = self.Slots[Index]
	end
	local Previous = self.Equipped[Kind]
	self.Slots[Index] = nil
	if Previous ~= nil then
		self.Slots[Index] = { Id = Previous, Count = 1 }
	end
	self.Equipped[Kind] = Def.Id
	self:ApplyEquipment(Kind)
	self:MarkChanged()
	self:PlaySound("Equip")
	self:Notify(Def.Name .. " 장착")
	return true
end

function GameManager:Unequip(Kind, bSilent)
	self:EnsureInit()
	local Id = self.Equipped[Kind]
	if Id == nil then
		return true
	end
	for Index = 1, SlotCount do
		if self.Slots[Index] == nil then
			self.Slots[Index] = { Id = Id, Count = 1 }
			self.Equipped[Kind] = nil
			self:ApplyEquipment(Kind)
			self:MarkChanged()
			if not bSilent then
				self:PlaySound("Equip")
				self:Notify(Items[Id].Name .. " 해제")
			end
			return true
		end
	end
	if not bSilent then
		self:Notify("가방이 가득 찼습니다")
		self:PlaySound("Error")
	end
	return false
end

-- 플레이어에 장비 반영: EquipWeapon(itemDef) / EquipShield(itemDef), 해제는 nil
function GameManager:ApplyEquipment(OnlyKind)
	local PlayerScript = self:GetPlayerScript()
	if PlayerScript == nil then
		return
	end
	if OnlyKind == nil or OnlyKind == "Weapon" then
		SafeCall(PlayerScript, "EquipWeapon", Items[self.Equipped.Weapon or ""])
	end
	if OnlyKind == nil or OnlyKind == "Shield" then
		SafeCall(PlayerScript, "EquipShield", Items[self.Equipped.Shield or ""])
	end
end

-- ---------------------------------------------------------------- 상점
-- "potion_hp_small, dagger" → { "potion_hp_small", "dagger" } (모르는 id는 경고 후 제외)
function GameManager:ParseStock(Text)
	local Stock = {}
	for Id in tostring(Text or ""):gmatch("[^,%s]+") do
		if Items[Id] ~= nil and not Items[Id].bCurrency then
			Stock[#Stock + 1] = Id
		else
			Log.Warn("GameManager: 상점 목록에 알 수 없는 아이템", Id)
		end
	end
	return Stock
end

function GameManager:BuyItem(Id)
	self:EnsureInit()
	local Def = Items[Id]
	if Def == nil then
		return false
	end
	if self.Gold < Def.Price then
		self:Notify("골드가 부족합니다")
		self:PlaySound("Error")
		return false
	end
	if not self:CanAdd(Id, 1) then
		self:Notify("가방이 가득 찼습니다")
		self:PlaySound("Error")
		return false
	end
	self:SpendGold(Def.Price)
	self:GiveItem(Id, 1)
	self:PlaySound("Buy")
	self:Notify(string.format("%s 구매 (-%d 골드)", Def.Name, Def.Price))
	return true
end

function GameManager:SellSlot(Index)
	self:EnsureInit()
	local Slot = self.Slots[Index]
	if Slot == nil then
		return false
	end
	local Def   = Items[Slot.Id]
	local Price = self:GetSellPrice(Slot.Id)
	Slot.Count = Slot.Count - 1
	if Slot.Count <= 0 then
		self.Slots[Index] = nil
	end
	self.Gold = self.Gold + Price
	self:MarkChanged()
	self:PlaySound("Buy")
	self:Notify(string.format("%s 판매 (+%d 골드)", Def.Name, Price))
	return true
end

-- ---------------------------------------------------------------- 전리품
-- 위치 아래 바닥 높이 (Ground 레이어, 없으면 pos.Z 그대로)
local function FindGroundZ(Position)
	local bOk, Hit = pcall(Physics.Raycast, Position + Vector3(0, 0, 150), Vector3(0, 0, -1), 600, "Ground")
	if bOk and Hit ~= nil then
		return Hit.position.Z
	end
	return Position.Z
end

function GameManager:SpawnPickup(Id, Count, Position, Hop)
	local Overrides = string.format('{"ItemId":"%s","Count":%d,"HopX":%.1f,"HopY":%.1f}', Id, Count, Hop and Hop.X or 0, Hop and Hop.Y or 0)
	Scene.SpawnPrefab(self.Properties.PickupPrefab, Position, function(Root)
		local Script = Root:GetComponent("ScriptComponent")
		if Script ~= nil then
			Script.PropertyOverrides = Overrides
		end
	end)
end

function GameManager:SpawnLoot(Position, LootTableId)
	self:EnsureInit()
	if Position == nil then
		return
	end
	local Table = LootTables[NormalizeTableId(LootTableId)]
	if Table == nil then
		Log.Warn("GameManager: 알 수 없는 전리품 표", LootTableId)
		return
	end
	local Drops = {}
	if Table.Gold ~= nil then
		Drops[#Drops + 1] = { "gold", math.random(Table.Gold[1], Table.Gold[2]) }
	end
	for _, Entry in ipairs(Table.Drops or {}) do
		if math.random() < Entry[2] then
			Drops[#Drops + 1] = { Entry[1], math.random(Entry[3], Entry[4]) }
		end
	end
	local Ground = Vector3(Position.X, Position.Y, FindGroundZ(Position))
	-- 고르게 흩뿌린다 (각도는 균등 + 흔들림)
	local StartAngle = math.random() * math.pi * 2
	for Index, Drop in ipairs(Drops) do
		local Angle  = StartAngle + (Index - 1) * (math.pi * 2 / #Drops) + (math.random() - 0.5) * 0.6
		local Radius = 50 + math.random() * 50
		self:SpawnPickup(Drop[1], Drop[2], Ground, Vector3(math.cos(Angle) * Radius, math.sin(Angle) * Radius, 0))
	end
end

-- ---------------------------------------------------------------- 알림 / 상호작용 안내 / 데미지 숫자
function GameManager:Notify(Text)
	self:EnsureInit()
	Text = tostring(Text)
	self.Toasts[#self.Toasts + 1] = { Text = Text, Time = self.Clock }
	while #self.Toasts > MaxToasts do
		table.remove(self.Toasts, 1)
	end
	Log.Info("[RPG 알림] " .. Text)
end

-- { { Text, Age, Alpha } ... } 오래된 것부터
function GameManager:GetToasts()
	self:EnsureInit()
	local Result = {}
	for _, Toast in ipairs(self.Toasts) do
		local Age = self.Clock - Toast.Time
		Result[#Result + 1] = { Text = Toast.Text, Age = Age, Alpha = math.min(1, math.max(0, (ToastDuration - Age) / 0.6)) }
	end
	return Result
end

-- 상호작용 안내 ("E: 거래"): 가장 최근에 요청한 주인 하나만. Owner는 요청한 스크립트(self)
function GameManager:SetPrompt(Owner, Text, Key)
	self:EnsureInit()
	self.Prompt = { Owner = Owner, Text = Text, Key = Key or "E" }
end

function GameManager:ClearPrompt(Owner)
	self:EnsureInit()
	if self.Prompt ~= nil and self.Prompt.Owner == Owner then
		self.Prompt = nil
	end
end

function GameManager:GetPrompt()
	self:EnsureInit()
	return self.Prompt
end

-- 화면 표시는 HUD(HUDController:AddDamageNumber/AddEnemyBar)가 한다 — 위젯 복제 + Camera.WorldToScreen
function GameManager:GetHUDScript()
	if self.HUDScript == nil then
		local HUD = Scene.Find("HUD")
		self.HUDScript = HUD and HUD:GetScript() or nil
	end
	return self.HUDScript
end

function GameManager:ShowDamageNumber(WorldPos, Amount, Kind)
	local HUD = self:GetHUDScript()
	if HUD ~= nil and type(HUD.AddDamageNumber) == "function" then
		HUD:AddDamageNumber(WorldPos, Amount, Kind)
	end
end

-- 적 머리 위 체력바 등록 (적 스크립트 OnStart에서). 파괴되거나 죽으면 HUD가 알아서 치운다
function GameManager:TrackEnemy(Entity)
	local HUD = self:GetHUDScript()
	if HUD ~= nil and type(HUD.AddEnemyBar) == "function" then
		HUD:AddEnemyBar(Entity)
	end
end

-- ---------------------------------------------------------------- 창 (가방/상점) — 열린 동안 입력 모드 GameAndUI
function GameManager:OpenWindow(Name)
	self:EnsureInit()
	if next(self.OpenWindows) == nil then
		self.SavedInputMode = Game.GetInputMode()
		if self.SavedInputMode ~= "GameAndUI" then
			Game.SetInputMode("GameAndUI")
		end
	end
	self.OpenWindows[Name] = true
end

function GameManager:CloseWindow(Name)
	self:EnsureInit()
	self.OpenWindows[Name] = nil
	if next(self.OpenWindows) == nil and self.SavedInputMode ~= nil then
		if self.SavedInputMode ~= Game.GetInputMode() then
			Game.SetInputMode(self.SavedInputMode)
		end
		self.SavedInputMode = nil
	end
end

function GameManager:IsWindowOpen(Name)
	self:EnsureInit()
	return self.OpenWindows[Name] == true
end

-- 플레이어 스크립트가 UI가 열린 동안 공격을 막을 때 쓴다
function GameManager:IsAnyWindowOpen()
	self:EnsureInit()
	return next(self.OpenWindows) ~= nil
end

-- ---------------------------------------------------------------- 소리
local Sounds = {
	Click   = "Asset/Kenney_InterfaceSounds/click.wav",
	Open    = "Asset/Kenney_InterfaceSounds/open.wav",
	Close   = "Asset/Kenney_InterfaceSounds/close.wav",
	Error   = "Asset/Kenney_InterfaceSounds/error.wav",
	Potion  = "Asset/Kenney_InterfaceSounds/potion.wav",
	Pickup  = "Asset/Kenney_InterfaceSounds/pickup_item.wav",
	Confirm = "Asset/Kenney_InterfaceSounds/confirm.wav",
	Coins   = "Asset/Kenney_RPGAudio/coins_pickup.wav",
	Buy     = "Asset/Kenney_RPGAudio/coins_buy.wav",
	Equip   = "Asset/Kenney_RPGAudio/equip.wav",
	Drop    = "Asset/Kenney_RPGAudio/item_drop.wav",
}

function GameManager:PlaySound(Name)
	local Path = Sounds[Name]
	if Path ~= nil then
		Audio.PlayOneShot(Path)
	end
end

-- ---------------------------------------------------------------- 저장
function GameManager:Save()
	self.SaveTimer = -1
	local Data = { Version = 1, Gold = self.Gold, Slots = {}, Weapon = self.Equipped.Weapon or "", Shield = self.Equipped.Shield or "" }
	for Index = 1, SlotCount do
		local Slot = self.Slots[Index]
		if Slot ~= nil then
			Data.Slots[#Data.Slots + 1] = { Slot = Index, Id = Slot.Id, Count = Slot.Count }
		end
	end
	local bOk, Result = pcall(SaveGame.Save, self.Properties.SaveSlot, Data)
	if not bOk or not Result then
		Log.Warn("GameManager: 저장 실패", tostring(Result))
		return false
	end
	return true
end

function GameManager:Load()
	local bOk, Data = pcall(SaveGame.Load, self.Properties.SaveSlot)
	if not bOk or type(Data) ~= "table" or Data.Version ~= 1 then
		return false
	end
	self.Gold  = math.floor(tonumber(Data.Gold) or 0)
	self.Slots = {}
	for _, Entry in ipairs(Data.Slots or {}) do
		local Index = math.floor(tonumber(Entry.Slot) or 0)
		if Items[Entry.Id] ~= nil and Index >= 1 and Index <= SlotCount then
			self.Slots[Index] = { Id = Entry.Id, Count = math.max(1, math.floor(tonumber(Entry.Count) or 1)) }
		end
	end
	self.Equipped.Weapon = Items[Data.Weapon or ""] and Data.Weapon or nil
	self.Equipped.Shield = Items[Data.Shield or ""] and Data.Shield or nil
	self.Revision = self.Revision + 1
	Log.Info(string.format("[RPG] 저장 불러옴 (%s): 골드 %d", self.Properties.SaveSlot, self.Gold))
	return true
end

return GameManager
