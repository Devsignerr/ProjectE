-- FarmBie 게임 관리자 (씬의 "FarmGame" 엔티티). 기능 모듈을 메서드로 합친다 — FarmTime(시간·달력·낮밤)부터 단계별로 (Plans.md FarmBie).
--   다른 스크립트는 Scene.Find("FarmGame"):GetScript()로 쓴다. 자동 검증이면 Properties.AutoPlay = 시나리오 이름 (FarmAutoPilot.lua).
--   상호작용: 모듈이 AddInteractable{Pos, Radius, Prompt = fn() → 글|nil, Act = fn()}로 등록 → 플레이어가 매 프레임 UpdateInteract(발 위치),
--             Interact 입력에 Interact(). 가장 가까운 "지금 가능한(Prompt가 글을 돌려준)" 것 하나만.
--   저장: 하루가 넘어갈 때(잠) 슬롯 "FarmBie_<Slot>"에 자동 저장. 모듈마다 Save<이름>(표)/Load<이름>(표) 쌍을 SaveParts에 적는다.
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local FarmGame = {
	Properties = {
		AutoPlay  = "",       -- 자동 검증 시나리오 (FarmAutoPilot.lua — 맵 이동으로 이어지면 Persistent "FarmBie_AutoPlay")
		Map       = "Farm",   -- Farm | Forest (밭·출하·보부상은 농장에서만)
		CameraBounds = "",    -- 카메라 초점 범위 "minx,miny,maxx,maxy"
		Slot      = "1",      -- 저장 슬롯 ("Test" = 자동 검증 전용 — 시작할 때 지운다)
		SleepSpot = "0,0",    -- 집 문 앞 "x,y" (잠자기 상호작용·기상 자리)
		ShipSpot  = "0,0",    -- 출하 상자 "x,y"
		MerchantSpot = "0,0", -- 보부상 자리 "x,y"
	},
}
for _, Module in ipairs({ "Scripts/FarmBie/FarmTime.lua", "Scripts/FarmBie/FarmInventory.lua", "Scripts/FarmBie/FarmField.lua",
                         "Scripts/FarmBie/FarmEconomy.lua", "Scripts/FarmBie/FarmMenu.lua", "Scripts/FarmBie/FarmVitals.lua",
                         "Scripts/FarmBie/FarmForage.lua" }) do
	for Name, Fn in pairs(Script.Require(Module)) do FarmGame[Name] = Fn end
end

local SaveParts = { "Time", "Inventory", "Field", "Economy", "Vitals", "Forage" }
local SessionSlot = "FarmBie_Session"
local SaveVersion = 1

local function Flat(V) return Vector3(V.X, V.Y, 0) end
local function Parse2(Text)
	local X, Y = string.match(Text, "([-%d%.]+),([-%d%.]+)")
	return Vector3(tonumber(X), tonumber(Y), 0)
end

function FarmGame:OnStart()
	self.Report = {}
	self.Interactables = {}
	self.MapId = self.Properties.Map
	-- 맵 이동으로 이어지는 자동 검증: 시나리오·슬롯을 Persistent로 받는다
	local Auto = Game.GetPersistent("FarmBie_AutoPlay", "")
	if self.Properties.AutoPlay == "" and Auto ~= "" then self.Properties.AutoPlay = Auto end
	if self.Properties.AutoPlay ~= "" then self.Properties.Slot = "Test" end
	self.SleepSpot = Parse2(self.Properties.SleepSpot)
	self:InitVitals()
	self:InitTime()
	self:InitInventory()
	self.RandState = 12345
	self:InitField()
	if self.MapId == "Farm" then self:InitEconomy() else self.Economy = Script.Require("Scripts/FarmBie/FarmData.lua").Values("Economy.edata"); self.Gold = 0; self.Shipped = {}; self.Stock = {}; self.StockDay = -1 end
	self:InitForage()
	self:AddInteractable({ Pos = self.SleepSpot, Radius = 170, Prompt = function()
		if self.Phase == "Night" then return "E  잠자기 (하루를 마친다)" end
		return nil
	end, Act = function() self:BeginSleep("Bed") end })
	if self:TryResumeSession() then
		-- 맵 이동으로 왔다 (상태는 세션에서)
	elseif self.Properties.Slot == "Test" and Game.GetPersistent("FarmBie_AutoPhase", "") == "" then
		SaveGame.Delete(self:SlotName())
	end
	if not self.bResumed and not (SaveGame.Exists(self:SlotName()) and self:LoadGame()) then
		self:GiveStartItems()
		self:GiveStartGold()
	end
	self:ApplyDayNight(true)
	self.bReady = true
end

function FarmGame:Hud()
	if not self.HudScript then
		local E = Scene.Find("Hud")
		self.HudScript = E and E:GetScript()
	end
	return self.HudScript
end

function FarmGame:Player()
	if not self.PlayerScript then
		local E = Scene.Find("Player")
		self.PlayerScript = E and E:GetScript()
	end
	return self.PlayerScript
end

function FarmGame:OnUpdate(Dt)
	if self.ArrivePos then
		-- 맵 도착 자리 (플레이어 OnStart 뒤 첫 갱신)
		local P = self:Player()
		if P then
			P:Teleport(self.ArrivePos)
			self.ArrivePos = nil
		end
	end
	if self.TravelTarget then
		self:UpdateTravel()
		return
	end
	self:UpdateTime(Dt)
	if self.MapId == "Farm" then self:UpdateMerchant() end
	self:UpdateVitals(Dt)
end

-- ---- 상호작용
function FarmGame:AddInteractable(Item)
	self.Interactables[#self.Interactables + 1] = Item
	return Item
end

function FarmGame:RemoveInteractable(Item)
	for I, It in ipairs(self.Interactables) do
		if It == Item then
			table.remove(self.Interactables, I)
			return
		end
	end
end

function FarmGame:UpdateInteract(Pos)
	local Best, BestD, BestText = nil, math.huge, nil
	if self.Phase ~= "Sleep" then
		for _, It in ipairs(self.Interactables) do
			local Dist = Flat(It.Pos - Pos):Length()
			if Dist < It.Radius and Dist < BestD then
				local Text = It.Prompt()
				if Text then Best, BestD, BestText = It, Dist, Text end
			end
		end
	end
	self.Focus = Best
	self:Hud():ShowPrompt(BestText)
end

function FarmGame:Interact()
	if self.Phase == "Sleep" then return false end
	if self.Focus then
		self.Focus.Act()
		return true
	end
	-- 다 자란 작물 수확 (발 앞 칸)
	if self.CursorAction == "Harvest" then
		local P = self:Player()
		local TX, TY = self:TargetTile(P.entity:GetWorldPosition(), P:GetFacingVector())
		return self:ApplyUse(nil, TX, TY)
	end
	return false
end

-- ---- 시간 훅 (FarmTime이 부른다)
-- 잠 전환이 끝났을 때 (FarmTime UpdateSleep) — 다른 맵에서 잠들었으면 집 침대로
function FarmGame:OnWake(Title, Sub)
	if self.bWakeTravel then
		self.bWakeTravel = false
		self.TravelBanner = { Title, Sub }
		self:TravelTo("Scenes/Farm.escene", "Bed")
		return true
	end
	return false
end

function FarmGame:OnDayStart(bNewSeason)
	-- 아침 알림 띠 부제에 붙일 글 (FarmTime이 잠 전환 끝에 쓴다)
	self.MorningNotes = {}
	self:ApplySleepRecovery(self.MorningNotes)
	local Grown = self:GrowField()
	local Income = self:SettleShipping()
	self:RefreshNodes()
	if Income > 0 then self.MorningNotes[#self.MorningNotes + 1] = string.format("출하 수입 +%d", Income) end
	if self:IsMerchantDay() then self.MorningNotes[#self.MorningNotes + 1] = "보부상이 남쪽 천막에 왔다" end
	Log.Info(string.format("[FarmBie] 아침: 자란 작물 %d, 출하 수입 %d, 돈 %d", Grown, Income, self.Gold))
end

function FarmGame:OnSeasonChanged(OldSeason)
	local Count = self:WitherField()
	if Count > 0 then Log.Info(string.format("[FarmBie] 계절이 바뀌어 작물 %d개가 시듦", Count)) end
end

-- ---- 맵 이동 (FarmTravel.lua 트리거 · 다른 맵에서 잠들면 집으로)
function FarmGame:TravelTo(SceneAsset, SpawnName)
	if self.TravelTarget then return end
	self.TravelTarget, self.TravelSpawn, self.TravelTimer = SceneAsset, SpawnName, 0
	self:Hud():ShowPrompt(nil)
	self.Report.Travels = (self.Report.Travels or 0) + 1
	Log.Info(string.format("[FarmBie] 맵 이동 → %s (Spawn_%s)", SceneAsset, SpawnName))
end

function FarmGame:UpdateTravel()
	self.TravelTimer = self.TravelTimer + Time.GetUnscaledDelta()
	self:Hud():SetFade(self.TravelTimer / 0.4)
	if self.TravelTimer < 0.45 or self.bTravelSent then return end
	self.bTravelSent = true
	local Data = self:BuildSave()
	Data.Session = { Hour = self.Hour, Phase = self.Phase == "Sleep" and "Day" or self.Phase, Spawn = self.TravelSpawn, Slot = self.Properties.Slot,
	                 Banner = self.TravelBanner }
	SaveGame.Save(SessionSlot, Data)
	Game.SetPersistent("FarmBie_Session", true)
	if self.Properties.AutoPlay ~= "" then Game.SetPersistent("FarmBie_AutoPlay", self.Properties.AutoPlay) end
	Game.SetTimeScale(1.0)
	Game.OpenScene(self.TravelTarget)
end

function FarmGame:TryResumeSession()
	if Game.GetPersistent("FarmBie_Session", false) ~= true then return false end
	Game.SetPersistent("FarmBie_Session", nil)
	local Data = SaveGame.Load(SessionSlot)
	if type(Data) ~= "table" or not Data.Session then return false end
	local S = Data.Session
	self.Properties.Slot = S.Slot or self.Properties.Slot
	for _, Part in ipairs(SaveParts) do self["Load" .. Part](self, Data) end
	self.Hour = S.Hour or self.Hour
	self.Phase = S.Phase or "Day"
	local Spawn = S.Spawn and Scene.Find("Spawn_" .. S.Spawn)
	if Spawn then self.ArrivePos = Spawn:GetWorldPosition() + Vector3(0, 0, 89) end
	if S.Banner then self:Hud():Announce(S.Banner[1], S.Banner[2], 3.5) end
	self.bResumed = true
	self.Report.Arrivals = (self.Report.Arrivals or 0) + 1
	Log.Info(string.format("[FarmBie] 맵 도착: %s Spawn_%s (%s %s)", self.MapId, tostring(S.Spawn), self:DateText(), self:ClockText()))
	return true
end

-- 잠에서 깸: 집 문 앞으로 (다른 맵이면 집으로 이동)
function FarmGame:WakePlayer()
	if self.MapId ~= "Farm" then
		self.bWakeTravel = true
		return
	end
	local P = self:Player()
	if P then
		local Pos = P.entity:GetWorldPosition()
		P:Teleport(Vector3(self.SleepSpot.X, self.SleepSpot.Y + 60, Pos.Z))
		P.Facing = "Down"
	end
end

-- ---- 저장
function FarmGame:SlotName()
	return "FarmBie_" .. self.Properties.Slot
end

function FarmGame:BuildSave()
	local T = { Version = SaveVersion }
	for _, Part in ipairs(SaveParts) do self["Save" .. Part](self, T) end
	return T
end

function FarmGame:SaveGame()
	local bOk = SaveGame.Save(self:SlotName(), self:BuildSave())
	self.Report.Saves = (self.Report.Saves or 0) + 1
	if not bOk then Log.Error("[FarmBie] 저장 실패: " .. self:SlotName()) end
	return bOk
end

function FarmGame:LoadGame()
	local T = SaveGame.Load(self:SlotName())
	if type(T) ~= "table" then return false end
	for _, Part in ipairs(SaveParts) do self["Load" .. Part](self, T) end
	self.Report.Loads = (self.Report.Loads or 0) + 1
	Log.Info(string.format("[FarmBie] 불러옴: %s — %d년차 %s", self:SlotName(), self.Year, self:DateText()))
	return true
end

-- 자동 검증 결과 (FarmAutoPilot:Finish가 부른다) — 로그 "[FarmBie] 결과: 실패 N건"
function FarmGame:ReportAutoPlay(Failures, Summary)
	Log.Info("[FarmBie] 자동 플레이 요약: " .. (Summary or ""))
	if #Failures == 0 then
		Log.Info("[FarmBie] 결과: 실패 0건")
	else
		Log.Error("[FarmBie] 결과: 실패 " .. #Failures .. "건 — " .. table.concat(Failures, ", "))
	end
end

return FarmGame
