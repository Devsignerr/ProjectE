-- HD-2D 데모 관리자 확장 ① 일행·진행 (HD2DGame.lua가 Script.Require로 받아 메서드로 붙인다 — 이 모듈은 상태를 갖지 않는다, 상태는 관리자 self에).
--   일행: 골드·소지품(아이템 id → 개수)·장비(무기 Equipped / 방어구 Armor / 장신구 Accessory) — 능력치는 플레이어 RecalcStats가 장비에서 다시 계산.
--   진행: 메인 퀘스트 단계(Quests.etable), 서브 퀘스트(SubQuests.etable — 상태 nil → Active → Done, 진행 Count), 연 보물상자(맵:번호), 보스 처치.
--   저장(SaveGame 슬롯 Balance.SaveSlot = "HD2D"): BuildSave/ApplySave — 골드·소지품·장비·레벨/경험치/HP/MP/BP·퀘스트·서브 퀘스트·상자·보스·현재 씬·위치.
--   맵 이동(HD2DTravel.lua → TravelTo): 같은 저장 형식을 세션 슬롯 "HD2D_Session"에 쓰고 Game.SetPersistent("HD2D_Session", true) 후 Game.OpenScene.
--     도착한 씬의 관리자는 플래그가 있으면 세션을 불러와 "Spawn_<SpawnName>" 엔티티(위치 = 발 자리) 위에 플레이어를 둔다 (플래그는 바로 지움 —
--     프로세스 안에서만 유효하므로 지난 실행의 세션 파일이 남아 있어도 타이틀을 건너뛰지 않는다).
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local Party = {}
local SessionSlot = "HD2D_Session"

local function Flat(V) return Vector3(V.X, V.Y, 0) end

-- ================================================================ 골드·소지품·장비
function Party:InitParty()
	local B = D.Balance()
	self.Gold = B.StartGold
	self.Items = {}
	self:AddItem(B.StartWeapon, 1, false)
	if B.StartArmor ~= "" then self:AddItem(B.StartArmor, 1, false) end
	for _, Id in ipairs(B.StartItems) do self:AddItem(Id, 1, false) end
	self.Equipped = B.StartWeapon
	self.Armor = B.StartArmor ~= "" and B.StartArmor or nil
	self.Accessory = nil
	self.QuestStage, self.QuestKills = 0, 0
	self.Sub = {}          -- 서브 퀘스트 id → { State = "Active"|"Done", Count = n }
	self.Opened = {}       -- "맵:번호" → true (연 보물상자)
	self.BossDead = false
end

function Party:AddGold(N, bToast)
	self.Gold = self.Gold + N
	if bToast then
		self:Hud():Toast("UI/Demo/HD2D/Icons/Coin.png", string.format("%d 골드", N))
	end
end

function Party:Count(Id) return self.Items[Id] or 0 end

function Party:IsGear(Row) return Row.Kind == "Weapon" or Row.Kind == "Armor" or Row.Kind == "Accessory" end

function Party:AddItem(Id, N, bToast)
	local Row = D.Item(Id)
	if not Row then
		Log.Error("[HD2D] 아이템 표에 없음:", Id)
		return
	end
	if self:IsGear(Row) then
		self.Items[Id] = 1
	else
		self.Items[Id] = (self.Items[Id] or 0) + N
	end
	if bToast then
		self:Hud():Toast(Row.Icon, N > 1 and string.format("%s ×%d 획득", Row.DisplayName, N) or (Row.DisplayName .. " 획득"))
	end
	self:RefreshQuest()
end

function Party:RemoveItem(Id, N)
	self.Items[Id] = math.max(0, (self.Items[Id] or 0) - N)
	if self.Items[Id] == 0 then self.Items[Id] = nil end
end

function Party:GetWeapon()
	return D.Weapon(self.Equipped)
end

-- 장비 칸 (무기 / 방어구 / 장신구) 이름 → 지금 낀 아이템 id
function Party:GearSlot(Row)
	if Row.Kind == "Weapon" then return "Equipped" end
	if Row.Kind == "Armor" then return "Armor" end
	if Row.Kind == "Accessory" then return "Accessory" end
end

function Party:IsEquipped(Id)
	local Row = D.Item(Id)
	local Slot = Row and self:GearSlot(Row)
	return Slot ~= nil and self[Slot] == Id
end

-- 장착 (장신구는 이미 낀 것을 다시 고르면 뺀다)
function Party:Equip(Id, bToast)
	local Row = D.Item(Id)
	local Slot = Row and self:GearSlot(Row)
	if not Slot or self:Count(Id) <= 0 then return false end
	if self[Slot] == Id then
		if Slot ~= "Accessory" then return false end
		self[Slot] = nil
		if bToast then self:Hud():Toast(Row.Icon, Row.DisplayName .. " 해제") end
	else
		self[Slot] = Id
		if bToast then self:Hud():Toast(Row.Icon, Row.DisplayName .. " 장비") end
	end
	self.Report.Equips = self.Report.Equips + 1
	if Slot ~= "Equipped" then self.Report.GearEquips = self.Report.GearEquips + 1 end
	Audio.PlayOneShot(self.Sounds.Equip)
	local Player = self:GetPlayer()
	if Player then
		if Slot == "Equipped" then Player:OnWeaponChanged() else Player:RecalcStats() end
	end
	return true
end

-- 지금 장비의 능력치 합 (Defense / HealthBonus / SpeedBonus / CritBonus) — Swap = { Slot, Id }이면 그 칸을 바꿨다고 치고 (비교용)
function Party:GearStats(Swap)
	local Sum = { Defense = 0, HealthBonus = 0, SpeedBonus = 0, CritBonus = 0 }
	for _, Slot in ipairs({ "Armor", "Accessory" }) do
		local Id = self[Slot]
		if Swap and Swap.Slot == Slot then Id = Swap.Id end
		local Row = Id and D.Item(Id)
		if Row then
			for K in pairs(Sum) do Sum[K] = Sum[K] + (Row[K] or 0) end
		end
	end
	return Sum
end

function Party:CycleWeapon()
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

function Party:UseItem(Id)
	local Row = D.Item(Id)
	local Player = self:GetPlayer()
	if not Row or self:Count(Id) <= 0 or not Player then return false end
	if self:IsGear(Row) then return self:Equip(Id, true) end
	if Row.Kind == "Material" then return false end
	self:RemoveItem(Id, 1)
	if Row.Kind == "Heal" then
		Player:Heal(Row.Amount, 0)
	elseif Row.Kind == "Mana" then
		Player:Heal(0, Row.Amount)
	else
		Player:Heal(99999, 99999)
	end
	self.Report.Used[Id] = (self.Report.Used[Id] or 0) + 1
	Audio.PlayOneShot(self.Sounds.Potion)
	Log.Info("[HD2D] 아이템 사용: " .. Row.DisplayName)
	return true
end

function Party:QuickUse(Ids)
	for _, Id in ipairs(Ids) do
		if self:Count(Id) > 0 then return self:UseItem(Id) end
	end
	self:Hud():Toast(D.Item(Ids[1]).Icon, D.Item(Ids[1]).DisplayName .. "이(가) 없다")
	Audio.PlayOneShot(self.Sounds.Error)
	return false
end

-- 팡파르 (레벨 업·퀘스트 완료) — 효과음을 잇달아 (실제 시간: 메뉴로 멈춰도 울린다)
-- TODO(엔진 API): Audio.PlayOneShot(경로, 위치?, 음량?, 피치?)가 들어오면 피치를 올려 가며 화음처럼 (지금은 같은 높이)
function Party:Fanfare()
	Audio.PlayOneShot(self.Sounds.Confirm)
	Timer.After(0.14, function() Audio.PlayOneShot(self.Sounds.Buy) end, { Unscaled = true })
	Timer.After(0.30, function() Audio.PlayOneShot(self.Sounds.Potion) end, { Unscaled = true })
	Timer.After(0.46, function() Audio.PlayOneShot(self.Sounds.Confirm) end, { Unscaled = true })
end

-- ================================================================ 메인 퀘스트
function Party:SetQuestStage(Stage)
	self.QuestStage = Stage
	local Q = D.Quest(Stage)
	if Stage == 4 then
		local B = D.Balance()
		self:AddGold(B.QuestRewardGold, true)
		if B.QuestRewardItem ~= "" then self:AddItem(B.QuestRewardItem, 1, true) end
		self:Hud():Announce("퀘스트 완료!", Q.Title, 3.0)
		self:SpawnLevelFx(self:GetPlayer().entity:GetWorldPosition())
		self:Fanfare()
	elseif Stage == 2 then
		self:Hud():Announce("새 목표", Q.Objective, 2.5)
	end
	self:RefreshQuest()
	Log.Info(string.format("[HD2D] 퀘스트 단계 %d: %s", Stage, Q.Objective))
end

function Party:MainObjective()
	local Q = D.Quest(self.QuestStage)
	if Q.KillGoal > 0 then
		if self.QuestKills >= Q.KillGoal then return "마물 퇴치 완료! 촌장 바르톨로에게 보고하자" end
		return string.format("%s (%d/%d)", Q.Objective, self.QuestKills, Q.KillGoal)
	end
	return Q.Objective
end

function Party:RefreshQuest()
	local H = self:Hud()
	if H and self.QuestStage then H:SetQuest(D.Quest(self.QuestStage).Title, self:MainObjective()) end
	if self.RefreshMarkers then self:RefreshMarkers() end
end

-- ================================================================ 서브 퀘스트
function Party:SubState(Id)
	local S = self.Sub[Id]
	return S and S.State or nil
end

-- 받은 퀘스트를 보고할 수 있는가
function Party:SubReady(Id)
	local Q = D.SubQuest(Id)
	local S = self.Sub[Id]
	if not S or S.State ~= "Active" then return false end
	if Q.Kind == "Hunt" then return S.Count >= Q.Count end
	return self:Count(Q.Target) >= Q.Count
end

function Party:SubProgressText(Id)
	local Q = D.SubQuest(Id)
	local S = self.Sub[Id]
	if not S then return "" end
	if S.State == "Done" then return "완료" end
	local Have = Q.Kind == "Hunt" and S.Count or self:Count(Q.Target)
	if self:SubReady(Id) then return string.format("보고 가능 (%d/%d)", math.min(Have, Q.Count), Q.Count) end
	return string.format("진행 중 (%d/%d)", math.min(Have, Q.Count), Q.Count)
end

-- 의뢰인과 대화: 받기 → 진행 중 → 보고(보상) → 끝난 뒤
function Party:TalkSubQuest(Npc)
	local Id = Npc.Row.SubQuest
	local Q = D.SubQuest(Id)
	local Name, Portrait = Npc.Row.DisplayName, Npc.Row.Portrait
	local S = self.Sub[Id]
	if not S then
		self:StartDialog(Name, Q.AcceptLines, function()
			self.Sub[Id] = { State = "Active", Count = 0 }
			self:Hud():Announce("서브 퀘스트", Q.Title, 2.2)
			Audio.PlayOneShot(self.Sounds.Confirm)
			self:OnSubQuestChanged()
			Log.Info("[HD2D] 서브 퀘스트 받음: " .. Q.Title)
		end, Portrait)
	elseif S.State == "Active" and self:SubReady(Id) then
		self:StartDialog(Name, Q.DoneLines, function() self:CompleteSubQuest(Id) end, Portrait)
	elseif S.State == "Active" then
		self:StartDialog(Name, Q.ProgressLines, nil, Portrait)
	else
		self:StartDialog(Name, Q.AfterLines, nil, Portrait)
	end
end

function Party:CompleteSubQuest(Id)
	local Q = D.SubQuest(Id)
	if Q.Kind ~= "Hunt" then self:RemoveItem(Q.Target, Q.Count) end
	self.Sub[Id].State = "Done"
	if Q.RewardGold > 0 then self:AddGold(Q.RewardGold, true) end
	if Q.RewardItem ~= "" then self:AddItem(Q.RewardItem, 1, true) end
	local Player = self:GetPlayer()
	if Player and Q.RewardExp > 0 then Player:AddExp(Q.RewardExp) end
	self.Report.SubDone = self.Report.SubDone + 1
	self:Hud():Announce("서브 퀘스트 완료!", Q.Title, 2.6)
	if Player then self:SpawnLevelFx(Player.entity:GetWorldPosition()) end
	self:Fanfare()
	self:OnSubQuestChanged()
	Log.Info("[HD2D] 서브 퀘스트 완료: " .. Q.Title)
end

-- 사냥 퀘스트: 구역 안에서 쓰러뜨린 적 수
function Party:CountSubQuestKill(Pos)
	for _, Id in ipairs(D.SubQuestOrder) do
		local Q = D.SubQuest(Id)
		local S = self.Sub[Id]
		if Q.Kind == "Hunt" and S and S.State == "Active" and S.Count < Q.Count then
			if Flat(Pos - Vector3(Q.AreaX, Q.AreaY, 0)):Length() <= Q.AreaRadius then
				S.Count = S.Count + 1
				self:Hud():Toast("UI/Demo/HD2D/Icons/Charm.png", string.format("%s %d/%d", Q.Title, S.Count, Q.Count))
				self:OnSubQuestChanged()
			end
		end
	end
end

-- 서브 퀘스트 상태가 바뀌면: 표시(머리 위 !)·고양이 보이기
function Party:OnSubQuestChanged()
	if self.RefreshMarkers then self:RefreshMarkers() end
	if self.RefreshProps then self:RefreshProps() end
end

-- ================================================================ 저장 · 불러오기 · 맵 이동 세션
function Party:BuildSave(Pos)
	local Player = self:GetPlayer()
	local P = Pos or (Player and Player.entity:GetWorldPosition()) or Vector3(0, 0, 0)
	local Items = {}
	for Id, N in pairs(self.Items) do Items[Id] = N end
	local Sub = {}
	for Id, S in pairs(self.Sub) do Sub[Id] = { State = S.State, Count = S.Count } end
	local Opened = {}
	for K, V in pairs(self.Opened) do Opened[K] = V end
	return {
		Version = 1, Scene = Game.GetCurrentScene(), Map = self.Properties.Map, X = P.X, Y = P.Y, Z = P.Z,
		Gold = self.Gold, Items = Items, Equipped = self.Equipped, Armor = self.Armor or "", Accessory = self.Accessory or "",
		Level = Player and Player.Level or 1, Exp = Player and Player.Exp or 0, Health = Player and Player.Health or 1, Mana = Player and Player.Mana or 0,
		BP = Player and Player.BP or 1, QuestStage = self.QuestStage, QuestKills = self.QuestKills, Sub = Sub, Opened = Opened,
		BossDead = self.BossDead, PlayTime = (self.PlayTime or 0),
	}
end

function Party:ApplySave(Data)
	self.Gold = Data.Gold or 0
	self.Items = {}
	for Id, N in pairs(Data.Items or {}) do self.Items[Id] = math.floor(N) end
	self.Equipped = Data.Equipped or "Sword"
	self.Armor = (Data.Armor ~= nil and Data.Armor ~= "") and Data.Armor or nil
	self.Accessory = (Data.Accessory ~= nil and Data.Accessory ~= "") and Data.Accessory or nil
	self.QuestStage, self.QuestKills = math.floor(Data.QuestStage or 0), math.floor(Data.QuestKills or 0)
	self.Sub = {}
	for Id, S in pairs(Data.Sub or {}) do self.Sub[Id] = { State = S.State, Count = math.floor(S.Count or 0) } end
	self.Opened = {}
	for K, V in pairs(Data.Opened or {}) do self.Opened[K] = V end
	self.BossDead = Data.BossDead == true
	self.PlayTime = Data.PlayTime or 0
	self.PendingPlayer = Data -- 플레이어 OnStart가 아직이면 거기서 적용
	local Player = self:GetPlayer()
	if Player and Player.bStarted then Player:ApplySave(Data) end
	self:RefreshQuest()
end

function Party:SaveGame()
	local Slot = D.Balance().SaveSlot
	local Data = self:BuildSave()
	local bOk = SaveGame.Save(Slot, Data)
	self.Report.Saves = self.Report.Saves + (bOk and 1 or 0)
	Log.Info(string.format("[HD2D] 저장 %s: 슬롯 %s, %s", bOk and "성공" or "실패", Slot, self:StateSignature()))
	return bOk
end

-- 상태 요약 문자열 (저장·불러오기 일치 확인 — 키 정렬)
function Party:StateSignature()
	local Player = self:GetPlayer()
	local function Sorted(T, Fmt)
		local Keys = {}
		for K in pairs(T) do Keys[#Keys + 1] = K end
		table.sort(Keys)
		local Parts = {}
		for _, K in ipairs(Keys) do Parts[#Parts + 1] = Fmt(K, T[K]) end
		return table.concat(Parts, ",")
	end
	return string.format("G=%d;L=%d;E=%d;W=%s;A=%s;C=%s;Q=%d/%d;Boss=%s;I=%s;S=%s;O=%s", self.Gold, Player and Player.Level or 0, Player and Player.Exp or 0,
		self.Equipped, self.Armor or "-", self.Accessory or "-", self.QuestStage, self.QuestKills, tostring(self.BossDead),
		Sorted(self.Items, function(K, V) return K .. ":" .. V end), Sorted(self.Sub, function(K, V) return K .. ":" .. V.State .. ":" .. V.Count end),
		Sorted(self.Opened, function(K) return K end))
end

-- 맵 이동 (HD2DTravel.lua가 부른다): 페이드 아웃 → 세션 저장 → 씬 열기
function Party:TravelTo(SceneAsset, SpawnName)
	if self.Mode == "Travel" then return end
	self.Mode = "Travel"
	self.Menu = "Travel"
	self.TravelTarget, self.TravelSpawn, self.TravelTimer = SceneAsset, SpawnName, 0.45
	self:SetPaused(true)
	self:Hud():ShowPrompt(nil)
	self:Hud():FadeTo(1.0, 0.4)
	self.Report.Travels = (self.Report.Travels or 0) + 1
	Log.Info(string.format("[HD2D] 맵 이동 → %s (Spawn_%s)", SceneAsset, SpawnName))
end

function Party:UpdateTravel(UDt)
	self.TravelTimer = self.TravelTimer - UDt
	if self.TravelTimer > 0 then return end
	local Data = self:BuildSave()
	Data.Spawn = self.TravelSpawn
	SaveGame.Save(SessionSlot, Data)
	Game.SetPersistent("HD2D_Session", true)
	Game.SetTimeScale(1.0)
	Game.OpenScene(self.TravelTarget)
	self.TravelTimer = 1.0e9
end

-- 시작할 때: 맵 이동으로 왔으면 세션을 불러온다 (true)
function Party:TryResumeSession()
	if Game.GetPersistent("HD2D_Session", false) ~= true then return false end
	Game.SetPersistent("HD2D_Session", nil)
	local Data = SaveGame.Load(SessionSlot)
	if not Data then return false end
	self:ApplySave(Data)
	local Spawn = Data.Spawn and Scene.Find("Spawn_" .. Data.Spawn)
	if Spawn then
		self.ArrivePos = Spawn:GetWorldPosition() + Vector3(0, 0, 89)
	elseif Data.X then
		self.ArrivePos = Vector3(Data.X, Data.Y, Data.Z)
	end
	self.Report.Arrivals = (self.Report.Arrivals or 0) + 1
	Log.Info(string.format("[HD2D] 맵 도착: Spawn_%s (세션 불러옴, 골드 %d, 퀘스트 %d단계)", tostring(Data.Spawn), self.Gold, self.QuestStage))
	return true
end

-- 이어하기: 저장한 씬이 지금 씬이면 그 자리에서, 아니면 세션으로 넘겨 그 씬을 연다
function Party:ContinueGame()
	local Slot = D.Balance().SaveSlot
	local Data = SaveGame.Load(Slot)
	if not Data then return false end
	self.Report.Loads = (self.Report.Loads or 0) + 1
	if Data.Scene and Data.Scene ~= "" and Data.Scene ~= Game.GetCurrentScene() then
		Data.Spawn = nil
		SaveGame.Save(SessionSlot, Data)
		Game.SetPersistent("HD2D_Session", true)
		Game.SetTimeScale(1.0)
		Game.OpenScene(Data.Scene)
		return true
	end
	self:ApplySave(Data)
	local Player = self:GetPlayer()
	if Player and Data.X then Player:Teleport(Vector3(Data.X, Data.Y, Data.Z + 4)) end
	Log.Info("[HD2D] 불러오기: " .. self:StateSignature())
	return true
end

return Party
