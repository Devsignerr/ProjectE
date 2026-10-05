-- HD-2D 데모 관리자 확장 ④ 메타 시스템 핵심 (HD2DGame.lua가 Script.Require로 받아 메서드로 붙인다 — 상태는 관리자 self에).
--   설정(SaveGame 슬롯 "HD2D_Settings" — 게임 저장과 따로): 효과음 음량(0~10), 화면 흔들림, 피해·회복 숫자, 대화 글자 속도(1 느림 ~ 4 즉시), 미니맵.
--     효과음: 엔진에 마스터 음량 API가 없어 Lua 상태의 Audio.PlayOneShot을 한 번 감싸 음량 배율을 곱한다(모든 스크립트 공용 — 장면 소리 AudioSource는 그대로).
--   기록(Stats — 저장에 들어간다): 처치·보스·상자·서브 퀘스트·번 골드·걸은 거리·쓰러짐·강화·조합·저장 횟수 (관리자 Report·플레이어 Stats의 증가분을 매 프레임 더함).
--   도감: 만난 수(플레이어 11m 안에 처음 들어온 적 개체마다 1)·처치 수(종류별), 얻어 본 아이템. 약점 칸은 HD2DGame:BestiaryWeakness(종류) 훅(없으면 ???).
--   재료 드랍: MaterialDrops 표 — 별도 난수열(MetaSeed)로 굴려 기존 전리품 난수 순서를 바꾸지 않는다.
--   무기 강화: Upgrades[무기] = 단계 → GetWeapon이 공격력 보너스를 더한 대리 표(UpgradedWeapon)를 준다 (표 행은 읽기 전용이라 고치지 않는다).
--   저장 슬롯: "HD2D_1"~"HD2D_3" (예전 단일 슬롯 "HD2D"는 슬롯 1이 비어 있으면 그리로 옮긴다), 퀘스트 추적(Tracked = "Main" | 서브 퀘스트 id).
--   ESC: 프로젝트 입력 설정의 Pause 액션(Escape·게임패드 Start). Escape가 바인딩돼 있으므로 개발 실행도 ESC로 종료하지 않는다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")
local M = Script.Require("Scripts/Demo/HD2D/HD2DMetaData.lua")

local Meta = {}
local SettingsSlot = "HD2D_Settings"
local SlotCount = 3
local TextSpeeds = { 22.0, 42.0, 80.0, 100000.0 } -- 글자/초 (느림·보통·빠름·즉시)

local function Flat(V) return Vector3(V.X, V.Y, 0) end

Meta.SlotCount = SlotCount

function Meta.DefaultSettings()
	return { SfxVolume = 8, Shake = true, Numbers = true, TextSpeed = 2, Minimap = true }
end

-- ================================================================ 시작 · 매 프레임
function Meta:InitMeta()
	self.Upgrades = {}       -- 무기 id → 강화 단계
	self.Bestiary = { Seen = {}, Kills = {} }
	self.ItemsSeen = {}
	self.Stats = { Kills = 0, Bosses = 0, Chests = 0, SubDone = 0, GoldEarned = 0, Distance = 0, Deaths = 0, Upgrades = 0, Crafts = 0, Saves = 0,
	               Materials = 0 }
	self.Tracked = "Main"
	self.MetaSeed = 777
	self.MetaWatch = nil     -- 증가분 추적 (첫 갱신에서 기준값)
	self.SeenTimer = 0
	self:LoadSettings()
	self:InstallVolumeHook()
	self:MigrateLegacySave()
	for Id in pairs(self.Items) do self.ItemsSeen[Id] = true end
end

function Meta:MetaHud()
	if not self.MetaHudScript then
		local E = Scene.Find("MetaUI")
		self.MetaHudScript = E and E:GetScript() or nil
	end
	return self.MetaHudScript
end

function Meta:UpdateMeta(Dt)
	local Player = self:GetPlayer()
	-- 증가분 기록 (씬마다 새로 시작하는 Report·플레이어 Stats → 저장되는 Stats)
	local Now = { Chests = self.Report.Chests, SubDone = self.Report.SubDone, Gold = self.Gold,
	              Distance = (Player and Player.Stats) and Player.Stats.Distance or 0, Deaths = (Player and Player.Stats) and Player.Stats.Deaths or 0 }
	local W = self.MetaWatch
	if W then
		self.Stats.Chests = self.Stats.Chests + math.max(0, Now.Chests - W.Chests)
		self.Stats.SubDone = self.Stats.SubDone + math.max(0, Now.SubDone - W.SubDone)
		self.Stats.GoldEarned = self.Stats.GoldEarned + math.max(0, Now.Gold - W.Gold - (self.MetaSpent or 0))
		self.Stats.Distance = self.Stats.Distance + math.max(0, Now.Distance - W.Distance)
		self.Stats.Deaths = self.Stats.Deaths + math.max(0, Now.Deaths - W.Deaths)
	end
	self.MetaWatch, self.MetaSpent = Now, 0
	-- 도감: 가까이 온 적 (0.25초마다)
	self.SeenTimer = self.SeenTimer - Dt
	if self.SeenTimer <= 0 and Player then
		self.SeenTimer = 0.25
		local PP = Player.entity:GetWorldPosition()
		for _, S in pairs(self.Enemies) do
			if not S.bMetaSeen and S.entity:IsValid() and Flat(S.entity:GetWorldPosition() - PP):Length() < 1100 then
				S.bMetaSeen = true
				self.Bestiary.Seen[S.Kind] = (self.Bestiary.Seen[S.Kind] or 0) + 1
			end
		end
		for Id in pairs(self.Items) do self.ItemsSeen[Id] = true end
	end
	-- 추적 중인 서브 퀘스트가 끝났으면 메인으로
	if self.Tracked ~= "Main" and self:SubState(self.Tracked) ~= "Active" then
		self.Tracked = "Main"
		self:RefreshQuest()
	end
end

-- 관리자 OnEnemyKilled가 부른다 (Ground = 떨어진 자리 지면)
function Meta:OnMetaEnemyKilled(S, Ground)
	self.Bestiary.Kills[S.Kind] = (self.Bestiary.Kills[S.Kind] or 0) + 1
	if not S.bMetaSeen then
		S.bMetaSeen = true
		self.Bestiary.Seen[S.Kind] = (self.Bestiary.Seen[S.Kind] or 0) + 1
	end
	self.Stats.Kills = self.Stats.Kills + 1
	if S.bBoss then self.Stats.Bosses = self.Stats.Bosses + 1 end
	local Drop = M.Drop(S.Kind)
	if Drop and Drop.Material ~= "" and self:MetaRandom() < Drop.Chance then
		local N = Drop.Min + math.floor(self:MetaRandom() * (Drop.Max - Drop.Min + 1))
		-- 줍는 것의 튀는 방향도 메타 난수열로 (관리자 Seed를 잠깐 바꿔 끼운다 — 기존 전리품 난수 순서 유지)
		local Saved = self.Seed
		self.Seed = self.MetaSeed
		self:SpawnPickup(Ground, Drop.Material, N)
		self.MetaSeed, self.Seed = self.Seed, Saved
		self.Report.MaterialDrops = (self.Report.MaterialDrops or 0) + 1
		self.Stats.Materials = self.Stats.Materials + N
	end
end

function Meta:MetaRandom()
	self.MetaSeed = (self.MetaSeed * 1103515245 + 12345) % 2147483648
	return (self.MetaSeed % 100000) / 100000.0
end

-- 약점 칸 (전투 쪽이 덮어쓴다 — 공개된 약점 문자열 또는 nil)
function Meta:BestiaryWeakness(Kind)
	return nil
end

-- ================================================================ 설정
function Meta:LoadSettings()
	local S = Meta.DefaultSettings()
	local Saved = SaveGame.Load(SettingsSlot)
	if type(Saved) == "table" then
		if type(Saved.SfxVolume) == "number" then S.SfxVolume = math.max(0, math.min(10, math.floor(Saved.SfxVolume + 0.5))) end
		if type(Saved.TextSpeed) == "number" then S.TextSpeed = math.max(1, math.min(#TextSpeeds, math.floor(Saved.TextSpeed + 0.5))) end
		for _, K in ipairs({ "Shake", "Numbers", "Minimap" }) do
			if type(Saved[K]) == "boolean" then S[K] = Saved[K] end
		end
	end
	self.Settings = S
	self:ApplySettings()
end

function Meta:SaveSettings()
	local bOk = SaveGame.Save(SettingsSlot, self.Settings)
	Log.Info(string.format("[HD2D] 설정 저장 %s: %s", bOk and "성공" or "실패", self:SettingsSignature()))
	return bOk
end

function Meta:SettingsSignature()
	local S = self.Settings
	return string.format("Vol=%d;Shake=%s;Num=%s;Text=%d;Mini=%s", S.SfxVolume, tostring(S.Shake), tostring(S.Numbers), S.TextSpeed, tostring(S.Minimap))
end

function Meta:ApplySettings()
	Audio.HD2DVolume = self.Settings.SfxVolume / 10.0
	local H = self:MetaHud()
	if H then H:RefreshMinimapVisibility() end
end

function Meta:TextSpeed()
	return TextSpeeds[self.Settings and self.Settings.TextSpeed or 2] / 42.0
end

-- Audio.PlayOneShot(경로[, 위치], [음량], [피치])을 한 번 감싼다 — 음량 배율 Audio.HD2DVolume (Lua 상태 공용)
function Meta:InstallVolumeHook()
	if Audio.HD2DRawPlayOneShot then return end
	local Raw = Audio.PlayOneShot
	Audio.HD2DRawPlayOneShot = Raw
	Audio.PlayOneShot = function(Path, A, B, C)
		local Scale = Audio.HD2DVolume or 1.0
		if Scale <= 0.001 then return end
		if A == nil or type(A) == "number" then
			return Raw(Path, (A or 1.0) * Scale, B or 1.0)
		end
		return Raw(Path, A, (B or 1.0) * Scale, C or 1.0)
	end
end

-- ================================================================ 무기 강화
function Meta:UpgradeLevel(Weapon) return (self.Upgrades and self.Upgrades[Weapon]) or 0 end

-- 강화 보너스를 더한 무기 행 (원래 행은 고치지 않는다 — 없는 값은 원래 행에서)
function Meta:UpgradedWeapon(W, Id)
	local Level = self:UpgradeLevel(Id)
	if not W or Level <= 0 then return W end
	local Row = M.Upgrade(Id, Level)
	if not Row then return W end
	return setmetatable({ Damage = W.Damage + Row.Damage, DisplayName = W.DisplayName .. " +" .. Level, UpgradeLevel = Level }, { __index = W })
end

function Meta:GetWeaponById(Id)
	return self:UpgradedWeapon(D.Weapon(Id), Id)
end

-- 아이템 표시 이름 (강화한 무기는 +N)
function Meta:ItemDisplayName(Id)
	local Row = D.Item(Id)
	if not Row then return Id end
	if Row.Kind == "Weapon" and self:UpgradeLevel(Row.Weapon) > 0 then
		return Row.DisplayName .. " +" .. self:UpgradeLevel(Row.Weapon)
	end
	return Row.DisplayName
end

-- 인벤토리 설명 칸의 종류 이름: 강화·조합에 쓰이는 재료면 "대장간 재료", 아니면 귀중품
function Meta:MaterialKindName(Id)
	for _, Row in ipairs(M.Recipes()) do
		for _, C in ipairs(M.ParseCost(Row.Materials)) do
			if C.Id == Id then return "대장간 재료" end
		end
	end
	for Weapon in pairs(D.WeaponOrder) do
		for Level = 1, M.MaxUpgrade do
			local Row = M.Upgrade(D.WeaponOrder[Weapon], Level)
			for _, C in ipairs(Row and M.ParseCost(Row.Materials) or {}) do
				if C.Id == Id then return "대장간 재료" end
			end
		end
	end
	return "귀중품"
end

-- 비용 (재료 { Id, N } 목록 + 골드)을 낼 수 있는가
function Meta:CanPay(Cost, Gold)
	if self.Gold < (Gold or 0) then return false end
	for _, C in ipairs(Cost) do
		if self:Count(C.Id) < C.N then return false end
	end
	return true
end

function Meta:Pay(Cost, Gold)
	for _, C in ipairs(Cost) do self:RemoveItem(C.Id, C.N) end
	self.Gold = self.Gold - (Gold or 0)
	self.MetaSpent = (self.MetaSpent or 0) - (Gold or 0) -- 이번 프레임에 쓴 골드 (번 골드 증가분 계산에서 되돌린다 — 같은 프레임에 번 것이 묻히지 않게)
end

-- ================================================================ 저장 · 슬롯
function Meta:SlotName(Index) return "HD2D_" .. tostring(Index) end

function Meta:SlotData(Index)
	local Data = SaveGame.Load(self:SlotName(Index))
	return type(Data) == "table" and Data or nil
end

function Meta:AnySaveExists()
	for I = 1, SlotCount do
		if SaveGame.Exists(self:SlotName(I)) then return true end
	end
	return false
end

-- 예전 단일 슬롯 "HD2D" → 슬롯 1 (비어 있을 때만, 옮긴 뒤 예전 슬롯은 지운다)
function Meta:MigrateLegacySave()
	local Legacy = D.Balance().SaveSlot
	if not SaveGame.Exists(Legacy) then return false end
	if SaveGame.Exists(self:SlotName(1)) then
		Log.Warn("[HD2D] 예전 저장(" .. Legacy .. ")이 있지만 슬롯 1이 차 있어 옮기지 않음")
		return false
	end
	local Data = SaveGame.Load(Legacy)
	if type(Data) ~= "table" then return false end
	Data.Meta = Data.Meta or {}
	Data.Meta.Slot = 1
	if SaveGame.Save(self:SlotName(1), Data) then
		SaveGame.Delete(Legacy)
		self.Report.Migrated = (self.Report.Migrated or 0) + 1
		Log.Info("[HD2D] 예전 저장 " .. Legacy .. " → 슬롯 1로 옮김")
		return true
	end
	return false
end

local function CopyTable(T)
	local Out = {}
	for K, V in pairs(T or {}) do Out[K] = type(V) == "table" and CopyTable(V) or V end
	return Out
end

function Meta:BuildMetaSave()
	local H = self:MetaHud()
	return { Version = 1, Slot = self.CurrentSlot or 0, Upgrades = CopyTable(self.Upgrades), Bestiary = CopyTable(self.Bestiary),
	         ItemsSeen = CopyTable(self.ItemsSeen), Stats = CopyTable(self.Stats), Tracked = self.Tracked or "Main",
	         MapName = H and H.Properties.MapTitle or "" }
end

function Meta:ApplyMetaSave(Data)
	Data = Data or {}
	self.Upgrades = {}
	for K, V in pairs(Data.Upgrades or {}) do self.Upgrades[K] = math.floor(V) end
	self.Bestiary = { Seen = {}, Kills = {} }
	for _, Key in ipairs({ "Seen", "Kills" }) do
		for K, V in pairs((Data.Bestiary or {})[Key] or {}) do self.Bestiary[Key][K] = math.floor(V) end
	end
	self.ItemsSeen = {}
	for K, V in pairs(Data.ItemsSeen or {}) do self.ItemsSeen[K] = V == true end
	for K in pairs(self.Stats) do
		local V = (Data.Stats or {})[K]
		if type(V) == "number" then self.Stats[K] = V end
	end
	self.Tracked = Data.Tracked or "Main"
	if Data.Slot and Data.Slot > 0 then self.CurrentSlot = math.floor(Data.Slot) end
	self.MetaWatch = nil -- 불러온 값에서 다시 증가분 기준
end

-- 상태 요약에 붙는 메타 부분 (저장·불러오기 일치 확인)
function Meta:MetaSignature()
	local function Sorted(T)
		local Keys = {}
		for K in pairs(T or {}) do Keys[#Keys + 1] = K end
		table.sort(Keys)
		local Parts = {}
		for _, K in ipairs(Keys) do Parts[#Parts + 1] = K .. ":" .. tostring(T[K]) end
		return table.concat(Parts, ",")
	end
	return string.format(";U=%s;BK=%s;T=%s;SK=%d;SC=%d", Sorted(self.Upgrades), Sorted(self.Bestiary.Kills), self.Tracked or "Main", self.Stats.Kills,
		self.Stats.Crafts)
end

-- 슬롯에 저장 (Party:SaveGame — 로그·보고·실패 처리)
function Meta:SaveToSlot(Index)
	self.CurrentSlot = Index
	local bOk = self:SaveGame(Index)
	if bOk then
		self.Stats.Saves = self.Stats.Saves + 1
		-- 저장 횟수까지 맞게 한 번 더 (횟수는 저장 뒤에 오르므로)
		SaveGame.Save(self:SlotName(Index), self:BuildSave())
	end
	return bOk
end

-- ================================================================ 퀘스트 추적
function Meta:SetTracked(Id)
	self.Tracked = Id or "Main"
	self:RefreshQuest()
	Log.Info("[HD2D] 추적 퀘스트: " .. self.Tracked)
end

-- HUD 퀘스트 칸 (Party:RefreshQuest가 부른다) — 서브 퀘스트를 추적 중이면 그 제목·목표
function Meta:TrackedQuest(Title, Text)
	local Id = self.Tracked
	if Id == nil or Id == "Main" or self:SubState(Id) ~= "Active" then return Title, Text end
	local Q = D.SubQuest(Id)
	return Q.Title, self:SubObjective(Id)
end

-- 서브 퀘스트 한 줄 목표 (일지 표 QuestJournal의 Objective/Report — 없으면 표 요약)
function Meta:SubObjective(Id)
	local Q = D.SubQuest(Id)
	local Row = M.Journal("Sub_" .. Id)
	local Giver = D.Npc(Q.Giver)
	if self:SubReady(Id) then
		return (Row and Row.Report ~= "") and Row.Report or (Giver.DisplayName .. "에게 돌아가 보고하자")
	end
	local Have = Q.Kind == "Hunt" and (self.Sub[Id] and self.Sub[Id].Count or 0) or self:Count(Q.Target)
	local Goal = (Row and Row.Objective ~= "") and Row.Objective or Q.Summary
	return string.format("%s (%d/%d)", Goal, math.min(Have, Q.Count), Q.Count)
end

-- ================================================================ 플레이 시간 문자열
function Meta.FormatTime(Seconds)
	local S = math.floor(Seconds or 0)
	return string.format("%d:%02d:%02d", math.floor(S / 3600), math.floor(S / 60) % 60, S % 60)
end

return Meta
