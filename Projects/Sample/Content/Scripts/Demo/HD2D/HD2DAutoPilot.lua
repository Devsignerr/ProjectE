-- HD-2D 데모 자동 조종 (자동 검증 — HD2DGame.Properties.AutoPlay). 플레이어 입력 표를 대신 채운다 (HD2DPlayer:GatherInput).
--   시나리오는 코루틴 하나(Run*) — 도우미(GoTo/Press/Fight…)가 프레임마다 입력을 채우고 yield 한다. 시간은 실제 시간(메뉴로 게임이 멈춰도 흐른다),
--   막힘 판정만 게임 시간. 확인은 Expect로 쌓고 끝에 관리자 ReportAutoPlay → 로그 "[HD2D] 결과: 실패 N건".
--   Full      : 인벤토리 열기/닫기(시간 정지) → 촌장 대화(퀘스트 1) → 보물상자(창) → 인벤토리에서 창 장비 → 상인 대화 → 상점(활·회복약 구입)
--               → 야영지 보물상자(지팡이) → 회복약 사용 → 종류별 전투(슬라임=검, 고블린=창, 박쥐·해골 궁수=활, 독버섯=지팡이, R 교체)
--               → 촌장 보고(퀘스트 2) → 보스(골렘) 처치 → 보상 상자 → 촌장 보고(퀘스트 4) → 결과
--   Inventory / Shop / Dialog / Combat / Boss : 스크린샷용 (데모 소지품을 받고 그 화면에서 머문다)
--   이 모듈은 상태를 갖지 않는다 (Script.Require 값은 공유) — 상태는 New가 만든 객체에.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local AutoPilot = {}
AutoPilot.__index = AutoPilot

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function AutoPilot.New(Scenario, Player, GM)
	local A = setmetatable({ Scenario = Scenario, Player = Player, GM = GM, Time = 0, Frame = 0, Failures = {}, Checks = 0, Teleports = 0,
	                         DamageTakenScale = Scenario == "Full" and 0.35 or 0.25, PotionTimer = 0 }, AutoPilot)
	local Runner = A["Run" .. Scenario]
	if not Runner then
		Log.Error("[HD2D] 모르는 자동 시나리오: " .. tostring(Scenario))
		return A
	end
	A.Co = coroutine.create(function() Runner(A) end)
	return A
end

function AutoPilot:Note(Text)
	Log.Info(string.format("[HD2D] 자동 %s %.1fs: %s", self.Scenario, self.Time, Text))
end

function AutoPilot:Expect(bOk, What)
	self.Checks = self.Checks + 1
	if bOk then
		self:Note("확인 " .. What)
	else
		self.Failures[#self.Failures + 1] = What
		self:Note("확인 실패 " .. What)
	end
	return bOk
end

-- 프레임마다 플레이어가 부른다 → 이번 프레임 입력
function AutoPilot:Step(UDt)
	self.Time = self.Time + UDt
	self.Frame = self.Frame + 1
	self.GameDt = Time.GetDelta()
	self.In = { Move = Vector3(0, 0, 0) }
	if self.Co and coroutine.status(self.Co) ~= "dead" then
		local bOk, Err = coroutine.resume(self.Co)
		if not bOk then
			Log.Error("[HD2D] 자동 조종 오류: " .. tostring(Err))
			self.Failures[#self.Failures + 1] = "자동 조종 오류"
			self.Co = nil
			self:Finish()
		end
	end
	local In = self.In
	In.Confirm = In.Confirm or In.Interact or In.Attack
	In.Cancel = In.Cancel or In.Dash
	return In
end

function AutoPilot:Finish()
	if self.bFinished then return end
	self.bFinished = true
	local P = self.Player
	self.GM:ReportAutoPlay(self.Failures, string.format("%.0f초, 확인 %d건, 순간이동 %d회, 이동 %.0fcm, 공격 %d(명중 %d), 대시 %d, 피격 %d, 쓰러짐 %d, Lv %d",
		self.Time, self.Checks, self.Teleports, P.Stats.Distance, P.Stats.Attacks, P.Stats.Hits, P.Stats.Dashes, P.Stats.Damaged, P.Stats.Deaths, P.Level))
end

-- ================================================================ 도우미 (코루틴 안에서만)
function AutoPilot:Yield()
	coroutine.yield()
end

function AutoPilot:Wait(Seconds)
	local Until = self.Time + Seconds
	while self.Time < Until do self:Yield() end
end

function AutoPilot:WaitUntil(Fn, Timeout)
	local Until = self.Time + Timeout
	while not Fn() do
		if self.Time > Until then return false end
		self:Yield()
	end
	return true
end

-- 버튼 한 번 (한 프레임 누름 + 한 프레임 뗌)
function AutoPilot:Press(Field)
	self.In[Field] = true
	self:Yield()
	self:Yield()
end

function AutoPilot:Pos()
	return self.Player.entity:GetWorldPosition()
end

-- 길 따라 가기: 마을 ↔ 들판처럼 멀면 관리자 Path(마을 동쪽 문을 지나는 길)의 점들을 거친다
function AutoPilot:Route(From, To)
	local Path = {}
	for _, P in ipairs(self.GM.Path) do
		if P.X > -1400 then Path[#Path + 1] = P end -- 광장 가운데(우물)는 건너뛴다
	end
	if #Path == 0 or math.abs(From.X - To.X) < 900 then return { To } end
	local function Nearest(V)
		local Best, BestD = 1, 1.0e9
		for I, P in ipairs(Path) do
			local L = Flat(P - V):Length()
			if L < BestD then Best, BestD = I, L end
		end
		return Best
	end
	local I, J = Nearest(From), Nearest(To)
	local Points = {}
	local Step = I <= J and 1 or -1
	for K = I, J, Step do Points[#Points + 1] = Path[K] end
	Points[#Points + 1] = To
	return Points
end

-- 한 프레임 이동 입력 (막히면 옆으로 비켜 보고, 오래 막히면 순간이동)
function AutoPilot:MoveToward(Target, Scale)
	local Pos = self:Pos()
	local To = Flat(Target - Pos)
	if To:Length() < 1 then return end
	local Dir = To:Normalized()
	self.StuckClock = (self.StuckClock or 0) + self.GameDt
	if self.StuckClock > 0.75 then
		local Moved = self.StuckFrom and Flat(Pos - self.StuckFrom):Length() or 999
		if Moved < 35 and self.GameDt > 0 then
			self.StuckTotal = (self.StuckTotal or 0) + self.StuckClock
			self.SideTime = 0.55
			self.SideSign = -(self.SideSign or 1)
		else
			self.StuckTotal = 0
		end
		self.StuckClock = 0
		self.StuckFrom = Pos
	end
	if (self.StuckTotal or 0) > 4.5 then
		self:Note(string.format("막힘 → 순간이동 (%.0f, %.0f)", Target.X, Target.Y))
		self.Teleports = self.Teleports + 1
		self.Player:Teleport(Vector3(Target.X, Target.Y, Pos.Z + 30))
		self.StuckTotal = 0
		return
	end
	if (self.SideTime or 0) > 0 then
		self.SideTime = self.SideTime - self.GameDt
		Dir = (Dir + Vector3(-Dir.Y, Dir.X, 0) * (1.6 * self.SideSign)):Normalized()
	end
	self.In.Move = Dir * (Scale or 1.0)
end

function AutoPilot:GoTo(Target, Radius, Timeout, Label)
	local Deaths = self.Player.Stats.Deaths
	local Until = self.Time + (Timeout or 40)
	local Route = self:Route(self:Pos(), Target)
	local Index = 1
	self.StuckTotal, self.StuckClock, self.StuckFrom = 0, 0, nil
	while Index <= #Route do
		local WP = Route[Index]
		local Rad = Index == #Route and (Radius or 120) or 140
		if Flat(WP - self:Pos()):Length() <= Rad then
			Index = Index + 1
		else
			if self.Player.Stats.Deaths ~= Deaths then
				-- 쓰러져 마을로 돌아왔다 → 길을 다시
				Deaths = self.Player.Stats.Deaths
				Route, Index = self:Route(self:Pos(), Target), 1
			end
			if self.Time > Until then
				return self:Expect(false, "도착 " .. (Label or "?"))
			end
			if not self.GM:IsMenuOpen() then self:MoveToward(WP) end
			self:Survive()
			self:Yield()
		end
	end
	return true
end

-- 대화가 끝날 때까지 확인 버튼
function AutoPilot:TalkThrough(Timeout)
	local Until = self.Time + (Timeout or 30)
	while self.GM.Menu == "Dialog" do
		if self.Time > Until then return false end
		self:Wait(0.3)
		if self.GM.Menu == "Dialog" then self:Press("Confirm") end
	end
	return true
end

-- 메뉴(인벤토리·상점)에서 아이템 줄 고르기
function AutoPilot:SelectRow(Id)
	for _ = 1, 12 do
		if self.GM:SelectedId() == Id then return true end
		self:Press("MenuDown")
		self:Wait(0.08)
	end
	return self.GM:SelectedId() == Id
end

-- R로 원하는 무기가 나올 때까지 돌림
function AutoPilot:EquipBySwitch(Id)
	for _ = 1, 6 do
		if self.GM.Equipped == Id then return true end
		self:Press("Switch")
		self:Wait(0.1)
	end
	return self:Expect(self.GM.Equipped == Id, "R 무기 교체 → " .. Id)
end

-- 체력·마나 관리 (단축키 1/2)
function AutoPilot:Survive()
	local P = self.Player
	self.PotionTimer = self.PotionTimer - (self.GameDt or 0)
	if self.PotionTimer > 0 or self.GM:IsMenuOpen() then return end
	if P.Health < P.MaxHealth * 0.4 and (self.GM:Count("Potion") + self.GM:Count("HiPotion") + self.GM:Count("Elixir")) > 0 then
		self.In.Use1 = true
		self.PotionTimer = 1.5
	elseif P.Weapon.ManaCost > 0 and P.Mana < P.Weapon.ManaCost and (self.GM:Count("Ether") + self.GM:Count("Elixir")) > 0 then
		self.In.Use2 = true
		self.PotionTimer = 1.5
	end
end

local PreferredRange = { Slash = 105, Thrust = 210, Arrow = 560, Bolt = 480 }

-- 한 프레임 교전: 무기에 맞는 거리로 다가가거나 물러나고, 닿으면 공격
function AutoPilot:Engage(Target)
	local P = self.Player
	local W = P.Weapon
	local Pos = self:Pos()
	local To = Flat(Target.entity:GetWorldPosition() - Pos)
	local L = To:Length()
	local Dir = L > 1 and To * (1.0 / L) or Vector3(0, 1, 0)
	local Want = PreferredRange[W.Kind] + (Target.bBoss and 90 or 0)
	-- 보스가 내리치려 하면 물러난다
	if Target.bBoss and (Target.State == "SlamWindup" or Target.State == "ChargeWindup") and L < 520 then
		self.In.Move = Dir * -1
		if Target.State == "ChargeWindup" or L < 330 then self.In.Dash = P.DashCooldown <= 0 and self.Frame % 2 == 0 end
		return
	end
	if L > Want + 50 then
		self:MoveToward(Target.entity:GetWorldPosition())
	elseif (W.Kind == "Arrow" or W.Kind == "Bolt") and L < Want - 220 then
		self.In.Move = Dir * -1
	else
		self.In.Move = Dir * 0.25
	end
	if L < Want + (W.Kind == "Slash" and 70 or 120) and P.AttackTimer <= 0 and self.Frame % 3 == 0 then
		self.In.Attack = true
		self.In.Move = Dir * 0.25
	end
end

-- 종류 Kind를 Count마리 더 쓰러뜨린다 (무기 WeaponId)
function AutoPilot:Fight(Kind, WeaponId, Count, Timeout)
	local GM = self.GM
	if WeaponId then self:EquipBySwitch(WeaponId) end
	local Start = GM.Report.Kills[Kind] or 0
	local Until = self.Time + Timeout
	self.StuckTotal, self.StuckClock, self.StuckFrom = 0, 0, nil
	while (GM.Report.Kills[Kind] or 0) < Start + Count do
		if self.Time > Until then
			return self:Expect(false, string.format("%s 처치 (%s, %.0f초 안)", Kind, WeaponId or "-", Timeout))
		end
		if not GM:IsMenuOpen() then
			local Target = GM:NearestEnemy(self:Pos(), 2200, function(S) return S.Kind == Kind end)
			if Target then
				self:Engage(Target)
			else
				-- 그 종류의 가장 가까운 자리로
				local Best, BestD = nil, 1.0e9
				for _, Slot in ipairs(GM.Slots) do
					if Slot.Kind == Kind then
						local L = Flat(Slot.Pos - self:Pos()):Length()
						if L < BestD then Best, BestD = Slot, L end
					end
				end
				if Best and BestD > 200 then self:MoveToward(Best.Pos) end
			end
			self:Survive()
		end
		self:Yield()
	end
	return self:Expect(true, string.format("%s 처치 (%s)", Kind, WeaponId or "-"))
end

function AutoPilot:FindNpc(Id)
	for _, N in ipairs(self.GM.Npcs) do
		if N.Id == Id then return N end
	end
end

function AutoPilot:TalkToNpc(Id)
	local N = self:FindNpc(Id)
	if not N then return self:Expect(false, "마을 사람 " .. Id) end
	self:GoTo(N.Pos + Vector3(0, 120, 0), 60, 40, Id)
	self:Wait(0.2)
	self:Press("Interact")
	self:WaitUntil(function() return self.GM.Menu ~= nil end, 1.0)
	return self:Expect(self.GM.Menu == "Dialog", Id .. " 대화 시작")
end

function AutoPilot:OpenChestAt(Index)
	local C = self.GM.Chests[Index]
	if not C then return self:Expect(false, "보물상자 " .. Index) end
	self:GoTo(C.Pos + Vector3(0, 115, 0), 50, 60, "보물상자 " .. Index)
	self:Wait(0.2)
	self:Press("Interact")
	self:Wait(0.3)
	return self:Expect(C.bOpened, "보물상자 " .. Index .. " 열림")
end

function AutoPilot:Idle()
	while true do self:Yield() end
end

-- ================================================================ 시나리오
function AutoPilot:RunFull()
	local GM, P = self.GM, self.Player
	self:Wait(1.0)
	-- 인벤토리 열기 → 시간 정지 → 닫기
	self:Press("Inventory")
	self:Wait(0.4)
	self:Expect(GM.Menu == "Inventory" and Game.GetTimeScale() == 0, "인벤토리 열림 + 시간 정지")
	self:Press("MenuDown")
	self:Wait(0.2)
	self:Press("Inventory")
	self:Wait(0.3)
	self:Expect(GM.Menu == nil and Game.GetTimeScale() == 1, "인벤토리 닫힘 + 시간 재개")

	-- 촌장 → 퀘스트 시작
	self:TalkToNpc("Elder")
	self:TalkThrough()
	self:Expect(GM.QuestStage == 1, "퀘스트 1단계")

	-- 대장간 옆 보물상자 → 창, 인벤토리에서 장비
	self:OpenChestAt(1)
	self:Expect(GM:Count("Spear") == 1, "창 획득")
	self:Press("Inventory")
	self:Wait(0.3)
	self:Expect(self:SelectRow("Spear"), "인벤토리 창 고르기")
	self:Press("Confirm")
	self:Wait(0.3)
	self:Expect(GM.Equipped == "Spear", "인벤토리에서 창 장비")
	self:Press("Inventory")
	self:Wait(0.2)

	-- 상인 → 상점에서 활·회복약
	local Gold0 = GM.Gold
	self:TalkToNpc("Merchant")
	self:TalkThrough()
	self:Expect(GM.Menu == "Shop", "상점 열림")
	self:SelectRow("Bow")
	self:Press("Confirm")
	self:Wait(0.25)
	self:SelectRow("Potion")
	self:Press("Confirm")
	self:Wait(0.25)
	self:Expect(GM:Count("Bow") == 1 and GM.Gold == Gold0 - D.Item("Bow").Price - D.Item("Potion").Price, "상점 구입 (활 + 회복약, 골드 차감)")
	self:Press("Cancel")
	self:Wait(0.3)
	self:Expect(GM.Menu == nil, "상점 닫힘")

	-- 경비병 (안내 대사)
	self:TalkToNpc("Guard")
	self:TalkThrough()

	-- 야영지 보물상자 → 지팡이
	self:OpenChestAt(3)
	self:Expect(GM:Count("Staff") == 1, "지팡이 획득")
	-- 회복약 (인벤토리에서 사용)
	local Potions = GM:Count("Potion")
	self:Press("Inventory")
	self:Wait(0.3)
	self:SelectRow("Potion")
	self:Press("Confirm")
	self:Wait(0.3)
	self:Press("Inventory")
	self:Wait(0.2)
	self:Expect(GM:Count("Potion") == Potions - 1, "인벤토리에서 회복약 사용")

	-- 종류별 전투 (무기를 R로 바꿔 가며)
	self:Fight("Slime", "Sword", 2, 45)
	self:Fight("Goblin", "Spear", 1, 45)
	self:Fight("Mushroom", "Staff", 1, 50)
	self:Fight("Bat", "Bow", 1, 50)
	self:Fight("Archer", "Bow", 1, 55)
	if GM.QuestKills < D.Quest(1).KillGoal then
		self:Fight("Slime", "Spear", D.Quest(1).KillGoal - GM.QuestKills, 45)
	end
	self:Expect(GM.QuestKills >= D.Quest(1).KillGoal, "퀘스트 처치 목표")

	-- 촌장 보고 → 2단계
	self:TalkToNpc("Elder")
	self:TalkThrough()
	self:Expect(GM.QuestStage == 2, "퀘스트 2단계 (보스)")

	-- 보스
	self:EquipBySwitch("Spear")
	self:GoTo(GM.BossPos + Vector3(-700, 250, 0), 150, 60, "보스 앞")
	local Until = self.Time + 120
	local Swap = self.Time + 10
	while not GM.bBossDead and self.Time < Until do
		local Boss = GM:NearestEnemy(self:Pos(), 3000, function(S) return S.bBoss end)
		if not GM:IsMenuOpen() then
			if Boss then
				self:Engage(Boss)
			else
				self:MoveToward(GM.BossPos) -- 잠든 보스는 목록에 없다 → 다가가 깨운다
			end
			self:Survive()
			if self.Time > Swap then
				Swap = self.Time + 10
				self.In.Switch = true -- 가끔 무기를 바꿔 본다
			end
		end
		self:Yield()
	end
	self:Expect(GM.bBossDead, "보스 처치")
	local Patterns = 0
	for _, N in pairs(GM.Report.BossPatterns) do if N > 0 then Patterns = Patterns + 1 end end
	self:Expect(Patterns >= 3, "보스 패턴 3종 이상 (" .. Patterns .. ")")
	self:Expect(GM.QuestStage == 3, "퀘스트 3단계")
	self:Wait(1.5)
	self:OpenChestAt(#GM.Chests)

	-- 촌장 보고 → 완료
	self:TalkToNpc("Elder")
	self:TalkThrough()
	self:Expect(GM.QuestStage == 4, "퀘스트 완료")

	-- 결과 확인
	local R = GM.Report
	for _, Kind in ipairs({ "Slime", "Goblin", "Mushroom", "Bat", "Archer" }) do
		self:Expect((R.Kills[Kind] or 0) >= 1, "처치 기록 " .. Kind)
	end
	for _, Id in ipairs({ "Sword", "Spear", "Bow", "Staff" }) do
		self:Expect((R.WeaponHits[Id] or 0) >= 1, "무기 명중 " .. Id)
	end
	self:Expect(P.Level >= 2, "레벨 업 (Lv " .. P.Level .. ")")
	self:Expect(R.Pickups >= 3, "전리품 줍기 (" .. R.Pickups .. ")")
	self:Expect(R.Chests >= 3, "보물상자 3개 이상 (" .. R.Chests .. ")")
	self:Expect(R.InventoryOpened >= 3, "인벤토리 사용")
	self:Finish()
	self:Idle()
end

-- 스크린샷용 공통: 데모 소지품 (무기 넷·물약·골드·레벨)
function AutoPilot:DemoLoadout(Level)
	local GM, P = self.GM, self.Player
	for _, Id in ipairs({ "Spear", "Bow", "Staff" }) do GM:AddItem(Id, 1, false) end
	GM:AddItem("Potion", 3, false)
	GM:AddItem("HiPotion", 2, false)
	GM:AddItem("Ether", 2, false)
	GM:AddItem("Elixir", 1, false)
	GM.Gold = 482
	P:AddExp(D.Balance().ExpTable[Level or 3] or 0)
	P.Health = math.floor(P.MaxHealth * 0.82)
	P.Mana = math.floor(P.MaxMana * 0.7)
end

function AutoPilot:RunInventory()
	self:Wait(0.5)
	self:DemoLoadout(3)
	self:Wait(1.5)
	self:Press("Inventory")
	self:Wait(0.3)
	self:SelectRow("Spear")
	self:Note("인벤토리 화면에서 대기")
	self:Idle()
end

function AutoPilot:RunShop()
	self:Wait(0.5)
	self:DemoLoadout(2)
	self.GM.Items.Bow = nil
	self.GM.Items.Staff = nil
	self:TalkToNpc("Merchant")
	self:TalkThrough()
	self:SelectRow("Bow")
	self:Note("상점 화면에서 대기")
	self:Idle()
end

function AutoPilot:RunDialog()
	self:Wait(0.8)
	self:TalkToNpc("Elder")
	self:Wait(1.2)
	self:Press("Confirm")
	self:Wait(0.3)
	self:Press("Confirm")
	self:Note("대화 화면에서 대기")
	self:Idle()
end

function AutoPilot:RunCombat()
	self:Wait(0.3)
	self:DemoLoadout(3)
	self.GM.QuestStage = 1
	self.GM:RefreshQuest()
	local Order = { "Sword", "Spear", "Staff", "Bow" }
	local I = 1
	while true do
		self:EquipBySwitch(Order[I])
		local Until = self.Time + 4.0
		while self.Time < Until do
			local Target = self.GM:NearestEnemy(self:Pos(), 1500)
			if Target and not self.GM:IsMenuOpen() then self:Engage(Target) end
			self:Survive()
			self:Yield()
		end
		I = I % #Order + 1
	end
end

function AutoPilot:RunBoss()
	self:Wait(0.3)
	self:DemoLoadout(5)
	self.GM.QuestStage = 2
	self.GM:RefreshQuest()
	self:EquipBySwitch("Spear")
	while true do
		local Boss = self.GM:NearestEnemy(self:Pos(), 3000, function(S) return S.bBoss end)
		if not self.GM:IsMenuOpen() then
			if Boss then self:Engage(Boss) else self:MoveToward(self.GM.BossPos) end
			self:Survive()
		end
		self:Yield()
	end
end

return AutoPilot
