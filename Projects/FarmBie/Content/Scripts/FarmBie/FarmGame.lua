-- FarmBie 게임 관리자 (씬의 "FarmGame" 엔티티). 기능 모듈을 메서드로 합친다 — FarmTime(시간·달력·낮밤)부터 단계별로 (Plans.md FarmBie).
--   다른 스크립트는 Scene.Find("FarmGame"):GetScript()로 쓴다. 자동 검증이면 Properties.AutoPlay = 시나리오 이름 (FarmAutoPilot.lua).
--   상호작용: 모듈이 AddInteractable{Pos, Radius, Prompt = fn() → 글|nil, Act = fn()}로 등록 → 플레이어가 매 프레임 UpdateInteract(발 위치),
--             Interact 입력에 Interact(). 가장 가까운 "지금 가능한(Prompt가 글을 돌려준)" 것 하나만.
--   저장: 하루가 넘어갈 때(잠) 슬롯 "FarmBie_<Slot>"에 자동 저장. 모듈마다 Save<이름>(표)/Load<이름>(표) 쌍을 SaveParts에 적는다.
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local FarmGame = {
	Properties = {
		AutoPlay  = "",       -- 자동 검증 시나리오 (FarmAutoPilot.lua)
		Slot      = "1",      -- 저장 슬롯 ("Test" = 자동 검증 전용 — 시작할 때 지운다)
		SleepSpot = "0,0",    -- 집 문 앞 "x,y" (잠자기 상호작용·기상 자리)
	},
}
for _, Module in ipairs({ "Scripts/FarmBie/FarmTime.lua" }) do
	for Name, Fn in pairs(Script.Require(Module)) do FarmGame[Name] = Fn end
end

local SaveParts = { "Time" }
local SaveVersion = 1

local function Flat(V) return Vector3(V.X, V.Y, 0) end
local function Parse2(Text)
	local X, Y = string.match(Text, "([-%d%.]+),([-%d%.]+)")
	return Vector3(tonumber(X), tonumber(Y), 0)
end

function FarmGame:OnStart()
	self.Report = {}
	self.Interactables = {}
	self.SleepSpot = Parse2(self.Properties.SleepSpot)
	self:InitTime()
	self:AddInteractable({ Pos = self.SleepSpot, Radius = 170, Prompt = function()
		if self.Phase == "Night" then return "E  잠자기 (하루를 마친다)" end
		return nil
	end, Act = function() self:BeginSleep("Bed") end })
	if self.Properties.Slot == "Test" then
		SaveGame.Delete(self:SlotName())
	elseif SaveGame.Exists(self:SlotName()) then
		self:LoadGame()
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
	self:UpdateTime(Dt)
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
	if self.Focus and self.Phase ~= "Sleep" then
		self.Focus.Act()
		return true
	end
	return false
end

-- 잠에서 깸: 집 문 앞으로
function FarmGame:WakePlayer()
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
