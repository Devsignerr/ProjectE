-- HD-2D 데모 자동 조종 (자동 검증 — HD2DGame.Properties.AutoPlay). 플레이어 입력 표를 대신 채운다 (HD2DPlayer:GatherInput).
--   시나리오는 코루틴 하나(Run*) — 도우미(GoTo/Press/Fight…)가 프레임마다 입력을 채우고 yield 한다. 시간은 실제 시간(메뉴로 게임이 멈춰도 흐른다),
--   막힘 판정만 게임 시간. 확인은 Expect로 쌓고 끝에 관리자 ReportAutoPlay → 로그 "[HD2D] 결과: 실패 N건".
--   Full: 씬을 세 번 연다 — 단계는 Game.SetPersistent("HD2D_AutoPhase")로 넘기고 실패 목록·확인 수·기대 상태도 Persistent로 잇는다.
--     ① Main     : 타이틀(이어하기 없음) → 처음부터 → 시작 연출 → 인벤토리(탭) → 촌장(퀘스트 1) → 창 상자 → 장비 탭에서 창 → 대장장이 의뢰 → 상점(활·회복약)
--                  → 리나 의뢰 → 야영지 상자(지팡이) → 회복약 → 종류별 전투(부스트 공격 포함) → 고양이 → 농부 의뢰 → 허수아비 사냥 → 농부 보고(부적 장비)
--                  → 리나 보고(목걸이) → 촌장 보고(퀘스트 2) → 대장장이 보고(판금 갑옷 장비) → 게시판 저장 → 맵 이동 트리거(같은 씬)
--     ② Travel   : 세션으로 도착(타이틀 없음, Spawn_Test 자리, 상태 일치) → 보스 → 보상 상자 → 촌장(퀘스트 완료) → 저장 → 같은 씬 다시 열기(세션 없음)
--     ③ Load     : 타이틀(이어하기 가능) → 이어하기 → 저장 상태와 일치·자리 → 결과
--   TitleShot / Inventory / Equip / Shop / Dialog / Combat / Boost / Boss : 스크린샷용 (그 화면에서 머문다)
--   이 모듈은 상태를 갖지 않는다 (Script.Require 값은 공유) — 상태는 New가 만든 객체에.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local AutoPilot = {}
AutoPilot.__index = AutoPilot

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function AutoPilot.New(Scenario, Player, GM)
	local A = setmetatable({ Scenario = Scenario, Player = Player, GM = GM, Time = 0, Frame = 0, Failures = {}, Checks = 0, Teleports = 0,
	                         DamageTakenScale = Scenario == "Full" and 0.35 or 0.25, PotionTimer = 0 }, AutoPilot)
	local Name = Scenario
	if Scenario == "Full" then
		A.Phase = Game.GetPersistent("HD2D_AutoPhase", "Main")
		Name = "Full" .. A.Phase
		-- 앞 단계의 실패·확인 수를 잇는다
		local Prev = Game.GetPersistent("HD2D_AutoFailures", "")
		for Item in string.gmatch(Prev, "[^|]+") do A.Failures[#A.Failures + 1] = Item end
		A.Checks = Game.GetPersistent("HD2D_AutoChecks", 0)
		A.Time = Game.GetPersistent("HD2D_AutoTime", 0)
	end
	local Runner = A["Run" .. Name]
	if not Runner then
		Log.Error("[HD2D] 모르는 자동 시나리오: " .. tostring(Name))
		return A
	end
	A.Co = coroutine.create(function() Runner(A) end)
	return A
end

function AutoPilot:Note(Text)
	Log.Info(string.format("[HD2D] 자동 %s %.1fs: %s", self.Phase and ("Full/" .. self.Phase) or self.Scenario, self.Time, Text))
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
	Game.SetPersistent("HD2D_AutoPhase", nil)
	local P = self.Player
	self.GM:ReportAutoPlay(self.Failures, string.format("%.0f초, 확인 %d건, 순간이동 %d회, 이동 %.0fcm, 공격 %d(명중 %d), 대시 %d, 피격 %d, 쓰러짐 %d, Lv %d, 경로 %d",
		self.Time, self.Checks, self.Teleports, P.Stats.Distance, P.Stats.Attacks, P.Stats.Hits, P.Stats.Dashes, P.Stats.Damaged, P.Stats.Deaths, P.Level,
		self.GM.Report.Paths or 0))
end

-- 다음 단계로: Persistent에 넘기고 씬을 연다 (Main → Travel은 트리거가 열고, Travel → Load는 여기서)
function AutoPilot:HandOff(NextPhase)
	self.GM:ReportAutoPlay(nil, string.format("단계 %s 끝 %.0f초, 확인 %d건", self.Phase or "-", self.Time, self.Checks))
	Game.SetPersistent("HD2D_AutoPhase", NextPhase)
	Game.SetPersistent("HD2D_AutoFailures", table.concat(self.Failures, "|"))
	Game.SetPersistent("HD2D_AutoChecks", self.Checks)
	Game.SetPersistent("HD2D_AutoTime", self.Time)
	self:Note("단계 넘김 → " .. NextPhase)
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

-- 길: 내비메시 경로(지면 점)가 있으면 그것, 없으면 관리자 Path(마을 동쪽 문을 지나는 길)의 점들을 거친다
function AutoPilot:Route(From, To)
	local Foot = Vector3(From.X, From.Y, From.Z - 85)
	local Nav = AI.FindPath(Foot, Vector3(To.X, To.Y, Foot.Z))
	if Nav and #Nav >= 2 and Flat(Nav[#Nav] - To):Length() < 200 then
		local Points = {}
		for I = 2, #Nav do Points[#Points + 1] = Nav[I] end
		Points[#Points] = To
		self.NavRoutes = (self.NavRoutes or 0) + 1
		return Points
	end
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
	local Until = self.Time + (Timeout or 50)
	local Route = self:Route(self:Pos(), Target)
	local Index = 1
	self.StuckTotal, self.StuckClock, self.StuckFrom = 0, 0, nil
	while Index <= #Route do
		local WP = Route[Index]
		local Rad = Index == #Route and (Radius or 120) or 90
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

-- 메뉴(인벤토리·상점)에서 줄 고르기
function AutoPilot:SelectRow(Id)
	for _ = 1, 14 do
		if self.GM:SelectedId() == Id then return true end
		self:Press("MenuDown")
		self:Wait(0.06)
	end
	return self.GM:SelectedId() == Id
end

-- 인벤토리 탭으로 (1 도구, 2 장비, 3 퀘스트)
function AutoPilot:SelectTab(Tab)
	for _ = 1, 3 do
		if self.GM.MenuTab == Tab then return true end
		self:Press("MenuRight")
		self:Wait(0.06)
	end
	return self.GM.MenuTab == Tab
end

-- 인벤토리 장비 탭에서 Id를 고르고 장비 (비교 줄이 보였는가도)
function AutoPilot:EquipFromMenu(Id)
	self:Press("Inventory")
	self:Wait(0.25)
	self:SelectTab(2)
	self:SelectRow(Id)
	self:Wait(0.15)
	local bCompare = self.GM:Hud():W("InvCmp0").Visible
	self:Press("Confirm")
	self:Wait(0.25)
	self:Press("Inventory")
	self:Wait(0.2)
	self:Expect(self.GM:IsEquipped(Id), "장비 탭에서 " .. Id .. " 장비")
	return bCompare
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

-- 한 프레임 교전: 무기에 맞는 거리로 다가가거나 물러나고, 닿으면 공격 (bBoost = BP가 있으면 단계를 올려 공격)
function AutoPilot:Engage(Target, bBoost)
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
	if L < Want + (W.Kind == "Slash" and 70 or 120) and P.AttackTimer <= 0 then
		if bBoost and P.BP >= 2 and P.BoostLevel < 2 then
			self.In.Boost = true -- 프레임마다 한 단계 (BP 2개 이상 모이면 2단계 부스트 공격)
		elseif self.Frame % 3 == 0 then
			self.In.Attack = true
			self.In.Move = Dir * 0.25
		end
	end
end

-- 종류 Kind를 Count마리 더 쓰러뜨린다 (무기 WeaponId, Until = 추가 종료 조건)
function AutoPilot:Fight(Kind, WeaponId, Count, Timeout, bBoost, Near, Done)
	local GM = self.GM
	if WeaponId then self:EquipBySwitch(WeaponId) end
	local Start = GM.Report.Kills[Kind] or 0
	local Until = self.Time + Timeout
	self.StuckTotal, self.StuckClock, self.StuckFrom = 0, 0, nil
	while (GM.Report.Kills[Kind] or 0) < Start + Count or (Done and not Done()) do
		if self.Time > Until then
			return self:Expect(false, string.format("%s 처치 (%s, %.0f초 안)", Kind, WeaponId or "-", Timeout))
		end
		if not GM:IsMenuOpen() then
			local Center = Near or self:Pos()
			local Target = GM:NearestEnemy(Center, Near and 800 or 2200, function(S) return Kind == "*" or S.Kind == Kind end)
			local Pick = self:NearestPickup(900)
			local TargetDist = Target and Flat(Target.entity:GetWorldPosition() - self:Pos()):Length() or 1.0e9
			if Pick and TargetDist > 260 then
				self:MoveToward(Pick.Pos) -- 떨어진 전리품(젤리 등)부터 줍는다
			elseif Target then
				self:Engage(Target, bBoost)
			else
				-- 그 종류의 가장 가까운 자리로
				local Best, BestD = Near, 1.0e9
				if not Near then
					for _, Slot in ipairs(GM.Slots) do
						if Slot.Kind == Kind then
							local L = Flat(Slot.Pos - self:Pos()):Length()
							if L < BestD then Best, BestD = Slot.Pos, L end
						end
					end
				end
				if Best and Flat(Best - self:Pos()):Length() > 200 then self:MoveToward(Best) end
			end
			self:Survive()
		end
		self:Yield()
	end
	return self:Expect(true, string.format("%s 처치 (%s)", Kind, WeaponId or "-"))
end

function AutoPilot:NearestPickup(MaxDist)
	local Best, BestD = nil, MaxDist
	for _, P in ipairs(self.GM.Pickups) do
		if P.Age > 0.6 then
			local L = Flat(P.Pos - self:Pos()):Length()
			if L < BestD then Best, BestD = P, L end
		end
	end
	return Best
end

function AutoPilot:FindNpc(Id)
	for _, N in ipairs(self.GM.Npcs) do
		if N.Id == Id then return N end
	end
end

function AutoPilot:FindProp(Id)
	for _, P in ipairs(self.GM.Props_) do
		if P.Id == Id then return P end
	end
end

function AutoPilot:TalkToNpc(Id)
	local N = self:FindNpc(Id)
	if not N then return self:Expect(false, "마을 사람 " .. Id) end
	-- 다가가 말 걸기 (적에게 밀리거나 안내 대상이 바뀌면 다시 — 최대 3번)
	for Try = 1, 3 do
		self:GoTo(N.Pos + Vector3(0, 105, 0), 40, 60, Id)
		self:Wait(0.2)
		self:Press("Interact")
		if self:WaitUntil(function() return self.GM.Menu ~= nil end, 1.0) then break end
		local Pp = self:Pos()
		self:Note(string.format("%s 말 걸기 다시 (%d, 대상 %s, 나 (%.0f, %.0f, %.0f), %s (%.0f, %.0f, %.0f), 엔티티 %s, 메뉴 %s)", Id, Try,
			self.GM.Target and (self.GM.Target.Id or "상자") or "없음", Pp.X, Pp.Y, Pp.Z, Id, N.Pos.X, N.Pos.Y, N.Pos.Z, tostring(N.Entity ~= nil),
			tostring(self.GM.Menu)))
	end
	return self:Expect(self.GM.Menu == "Dialog", Id .. " 대화 시작")
end

function AutoPilot:InteractAt(Pos, Label)
	self:GoTo(Pos + Vector3(0, 115, 0), 50, 70, Label)
	self:Wait(0.2)
	self:Press("Interact")
	self:Wait(0.3)
end

function AutoPilot:OpenChestAt(Index)
	local C = self.GM.Chests[Index]
	if not C then return self:Expect(false, "보물상자 " .. Index) end
	self:InteractAt(C.Pos, "보물상자 " .. Index)
	return self:Expect(C.bOpened, "보물상자 " .. Index .. " 열림")
end

function AutoPilot:SaveAtBoard()
	local Board = self:FindProp("SavePoint")
	local Saves = self.GM.Report.Saves
	self:InteractAt(Board.Pos, "게시판")
	self:TalkThrough()
	self:Wait(0.2)
	return self:Expect(self.GM.Report.Saves == Saves + 1 and SaveGame.Exists(D.Balance().SaveSlot), "게시판 저장")
end

function AutoPilot:Idle()
	while true do self:Yield() end
end

-- 타이틀: Index 줄 고르고 결정
function AutoPilot:TitleChoose(Index)
	self:WaitUntil(function() return self.GM.Menu == "Title" end, 5)
	self:Wait(0.6)
	while self.GM.MenuIndex ~= Index do
		self:Press("MenuDown")
		self:Wait(0.1)
	end
	self:Press("Confirm")
	self:Wait(0.3)
end

-- ================================================================ Full ① Main
function AutoPilot:RunFullMain()
	local GM, P = self.GM, self.Player
	-- 타이틀 (처음 실행: 이어하기 없음) → 처음부터 → 시작 연출
	SaveGame.Delete(D.Balance().SaveSlot)
	self:WaitUntil(function() return GM.Menu == "Title" end, 5)
	self:Wait(0.5)
	self:Expect(GM.Menu == "Title" and Game.GetTimeScale() == 0, "타이틀 화면 + 시간 정지")
	self:Expect(GM:Hud():W("TitleScreen").Visible, "타이틀 UI 보임")
	-- 이어하기를 고르면 막힌다 (저장 없음)
	self:Press("MenuDown")
	self:Press("Confirm")
	self:Wait(0.2)
	self:Expect(GM.Menu == "Title", "저장 없을 때 이어하기 막힘")
	self:TitleChoose(1)
	self:Expect(GM.Mode == "Intro" and GM.Menu == "Dialog", "새 게임 시작 연출")
	self:TalkThrough()
	self:Expect(GM.Mode == "Play" and GM.Menu == nil, "연출 끝 → 플레이")

	-- 인벤토리 열기 → 시간 정지 → 탭 이동 → 닫기
	self:Press("Inventory")
	self:Wait(0.4)
	self:Expect(GM.Menu == "Inventory" and Game.GetTimeScale() == 0, "인벤토리 열림 + 시간 정지")
	self:SelectTab(2)
	self:Expect(GM.MenuTab == 2 and GM:SelectedId() == "Sword", "장비 탭 (검)")
	self:SelectTab(3)
	self:Expect(GM.MenuTab == 3 and GM:SelectedId() == "#Main", "퀘스트 탭")
	self:SelectTab(1)
	self:Press("Inventory")
	self:Wait(0.3)
	self:Expect(GM.Menu == nil and Game.GetTimeScale() == 1, "인벤토리 닫힘 + 시간 재개")

	-- 촌장 → 퀘스트 시작
	self:TalkToNpc("Elder")
	self:TalkThrough()
	self:Expect(GM.QuestStage == 1, "퀘스트 1단계")

	-- 대장간 옆 보물상자 → 창, 장비 탭에서 비교 보고 장비
	self:OpenChestAt(1)
	self:Expect(GM:Count("Spear") == 1, "창 획득")
	self:Expect(self:EquipFromMenu("Spear"), "장비 비교 줄 표시")

	-- 대장장이 의뢰 (젤리)
	self:TalkToNpc("Smith")
	self:TalkThrough()
	self:Expect(GM:SubState("Smith") == "Active", "서브 퀘스트 받음: 대장간")

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
	self:Expect(GM.ShopSay == D.Balance().ShopLines[2], "상점 주인 말 (구입)")
	self:SelectRow("ChainMail")
	self:Press("Confirm")
	self:Wait(0.2)
	self:Expect(GM.ShopSay == D.Balance().ShopLines[3] and GM:Count("ChainMail") == 0, "골드 부족 (사슬 갑옷)")
	self:Press("Cancel")
	self:Wait(0.3)
	self:Expect(GM.Menu == nil, "상점 닫힘")

	-- 리나 의뢰 (고양이) → 야영지 상자 (지팡이)
	self:TalkToNpc("Girl")
	self:TalkThrough()
	self:Expect(GM:SubState("Cat") == "Active", "서브 퀘스트 받음: 고양이")
	self:OpenChestAt(3)
	self:Expect(GM:Count("Staff") == 1, "지팡이 획득")
	-- 회복약 (인벤토리 도구 탭에서)
	local Potions = GM:Count("Potion")
	self:Press("Inventory")
	self:Wait(0.3)
	self:SelectTab(1)
	self:SelectRow("Potion")
	self:Press("Confirm")
	self:Wait(0.3)
	self:Press("Inventory")
	self:Wait(0.2)
	self:Expect(GM:Count("Potion") == Potions - 1, "인벤토리에서 회복약 사용")

	-- 종류별 전투 (무기를 R로, 슬라임은 부스트로 — 젤리 3개까지)
	self:Fight("Slime", "Sword", 2, 70, true, nil, function() return GM:Count("Jelly") >= 3 end)
	self:Expect(GM.Report.Boosts >= 1 and GM.Report.BoostMax >= 2, string.format("부스트 공격 (%d회, 최대 %d단계)", GM.Report.Boosts, GM.Report.BoostMax))
	self:Expect(GM:Count("Jelly") >= 3, "슬라임 젤리 3개")
	self:Fight("Goblin", "Spear", 1, 50, true)
	self:Fight("Mushroom", "Staff", 1, 55)
	self:Fight("Bat", "Bow", 1, 55, true)
	self:Fight("Archer", "Bow", 1, 60)
	if GM.QuestKills < D.Quest(1).KillGoal then
		self:Fight("Slime", "Spear", D.Quest(1).KillGoal - GM.QuestKills, 45)
	end
	self:Expect(GM.QuestKills >= D.Quest(1).KillGoal, "퀘스트 처치 목표")
	self:Expect((GM.Report.Paths or 0) > 0, "적 내비메시 길찾기 (" .. (GM.Report.Paths or 0) .. ")")

	-- 고양이 찾기
	local Cat = self:FindProp("Cat")
	self:InteractAt(Cat.Pos, "고양이")
	self:TalkThrough()
	self:Expect(GM:Count("LostCat") == 1 and GM:SubReady("Cat"), "고양이 찾음")

	-- 농부 의뢰 → 허수아비 근처 사냥 → 보고 → 부적 장비
	self:TalkToNpc("Farmer")
	self:TalkThrough()
	self:Expect(GM:SubState("Scarecrow") == "Active", "서브 퀘스트 받음: 허수아비")
	local Q = D.SubQuest("Scarecrow")
	self:EquipBySwitch("Spear")
	self:Fight("*", nil, 0, 90, true, Vector3(Q.AreaX, Q.AreaY + 150, 0), function() return GM:SubReady("Scarecrow") end)
	self:Expect(GM:SubReady("Scarecrow"), "허수아비 근처 사냥")
	self:TalkToNpc("Farmer")
	self:TalkThrough()
	self:Expect(GM:SubState("Scarecrow") == "Done" and GM:Count("SwiftCharm") == 1, "서브 퀘스트 완료: 허수아비 (부적)")
	local Speed0 = P.Mover.MaxWalkSpeed
	self:EquipFromMenu("SwiftCharm")
	self:Expect(P.Mover.MaxWalkSpeed > Speed0 + 1, string.format("부적으로 이동 속도 증가 (%.0f → %.0f)", Speed0, P.Mover.MaxWalkSpeed))

	-- 마을로: 리나 보고(목걸이) → 촌장 보고(2단계) → 대장장이 보고(판금 갑옷 장비)
	self:TalkToNpc("Girl")
	self:TalkThrough()
	self:Expect(GM:SubState("Cat") == "Done" and GM:Count("LifeAmulet") == 1, "서브 퀘스트 완료: 고양이 (목걸이)")
	self:TalkToNpc("Elder")
	self:TalkThrough()
	self:Expect(GM.QuestStage == 2, "퀘스트 2단계 (보스)")
	self:TalkToNpc("Smith")
	self:TalkThrough()
	self:Expect(GM:SubState("Smith") == "Done" and GM:Count("KnightPlate") == 1, "서브 퀘스트 완료: 대장간 (판금 갑옷)")
	local Def0, Hp0 = P.Defense, P.MaxHealth
	self:EquipFromMenu("KnightPlate")
	self:Expect(P.Defense > Def0 and P.MaxHealth > Hp0, string.format("판금 갑옷: 방어 %d → %d, 최대 HP %d → %d", Def0, P.Defense, Hp0, P.MaxHealth))
	self:Press("Inventory")
	self:Wait(0.25)
	self:SelectTab(3)
	self:Expect(#GM:BuildRows() == 4, "퀘스트 탭 (메인 + 서브 3)")
	self:Press("Inventory")
	self:Wait(0.2)

	-- 게시판 저장 → 기대 상태 기록 → 맵 이동 트리거 (같은 씬)
	self:SaveAtBoard()
	Game.SetPersistent("HD2D_AutoExpect", GM:StateSignature())
	self:HandOff("Travel")
	local Trigger = Scene.Find("Travel_Test")
	self:GoTo(Trigger:GetWorldPosition(), 40, 40, "맵 이동 트리거")
	self:WaitUntil(function() return GM.Mode == "Travel" end, 5)
	self:Expect(GM.Mode == "Travel", "맵 이동 시작 (페이드)")
	Game.SetPersistent("HD2D_AutoFailures", table.concat(self.Failures, "|"))
	Game.SetPersistent("HD2D_AutoChecks", self.Checks)
	Game.SetPersistent("HD2D_AutoTime", self.Time)
	self:Idle()
end

-- ================================================================ Full ② Travel (세션으로 도착)
function AutoPilot:RunFullTravel()
	local GM, P = self.GM, self.Player
	self:Wait(0.6)
	self:Expect(GM.Menu == nil and GM.Mode == "Play", "맵 이동 도착: 타이틀 없음")
	local Spawn = Scene.Find("Spawn_Test")
	self:Expect(Spawn and Flat(self:Pos() - Spawn:GetWorldPosition()):Length() < 150, "도착 자리 = Spawn_Test")
	local Expect = Game.GetPersistent("HD2D_AutoExpect", "")
	local Now = GM:StateSignature()
	if not self:Expect(Now == Expect, "세션 상태 유지") then
		self:Note("기대 " .. Expect)
		self:Note("실제 " .. Now)
	end

	-- 보스
	self:EquipBySwitch("Spear")
	self:GoTo(GM.BossPos + Vector3(-700, 250, 0), 150, 70, "보스 앞")
	local Until = self.Time + 120
	local Swap = self.Time + 10
	local HoldUntil = nil -- 분노(2단계) 뒤 세 번째 패턴(돌진)을 볼 때까지 거리를 두고 공격을 멈춘다 (최대 15초 — 화력이 세면 돌진 전에 쓰러져 검증이 흔들렸다)
	while not GM.bBossDead and self.Time < Until do
		local Boss = GM:NearestEnemy(self:Pos(), 3000, function(S) return S.bBoss end)
		if not GM:IsMenuOpen() then
			local Seen = 0
			for _, N in pairs(GM.Report.BossPatterns) do if N > 0 then Seen = Seen + 1 end end
			if Boss and Boss.Phase == 2 and Seen < 3 and HoldUntil == nil then
				HoldUntil = self.Time + 15
				Log.Info(string.format("[HD2D] 자동: 보스 패턴 관찰 대기 시작 (본 패턴 %d, 보스 HP %.0f)", Seen, Boss.Health or -1))
			end
			if Boss and HoldUntil and Seen < 3 and self.Time < HoldUntil then
				local L = Flat(self:Pos() - Boss.entity:GetWorldPosition()):Length()
				if L < 900 then
					self:MoveToward(GM.BossPos + Vector3(-1100, 350, 0))
					if (Boss.State == "ChargeWindup" or Boss.State == "SlamWindup") and self.Player.DashCooldown <= 0 then self.In.Dash = true end
				end
			elseif Boss then
				self:Engage(Boss, true)
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

	-- 촌장 보고 → 완료 → 저장 → 같은 씬 다시 열기 (세션 없음 = 타이틀)
	self:TalkToNpc("Elder")
	self:TalkThrough()
	self:Expect(GM.QuestStage == 4, "퀘스트 완료")
	self:SaveAtBoard()
	local Board = self:FindProp("SavePoint")
	Game.SetPersistent("HD2D_AutoExpect", GM:StateSignature())
	Game.SetPersistent("HD2D_AutoPos", Board.Pos)
	self:HandOff("Load")
	Game.OpenScene(Game.GetCurrentScene())
	self:Idle()
end

-- ================================================================ Full ③ Load (타이틀 → 이어하기)
function AutoPilot:RunFullLoad()
	local GM = self.GM
	self:WaitUntil(function() return GM.Menu == "Title" end, 5)
	self:Wait(0.4)
	self:Expect(GM.Menu == "Title" and GM.bCanContinue, "다시 연 씬: 타이틀 + 이어하기 가능")
	self:TitleChoose(2)
	self:Wait(0.5)
	local Expect = Game.GetPersistent("HD2D_AutoExpect", "")
	local Now = GM:StateSignature()
	if not self:Expect(Now == Expect, "이어하기 = 저장 상태") then
		self:Note("기대 " .. Expect)
		self:Note("실제 " .. Now)
	end
	local Board = Game.GetPersistent("HD2D_AutoPos", Vector3(0, 0, 0))
	self:Expect(Flat(self:Pos() - Board):Length() < 300, "이어하기 자리 = 저장한 게시판 앞")
	self:Expect(GM.Mode == "Play" and GM.Menu == nil and Game.GetTimeScale() == 1, "이어하기 → 플레이")
	self:Expect(GM.BossDead and GM.QuestStage == 4, "보스·퀘스트 진행 유지")
	-- 전체 기록 (마지막 씬의 보고는 이번 씬 것뿐이라 처치·명중 같은 누적은 앞 단계에서 확인했다)
	self:Expect(self.Player.Level >= 3, "레벨 (Lv " .. self.Player.Level .. ")")
	self:Finish()
	self:Idle()
end

-- ================================================================ 스크린샷용
function AutoPilot:DemoLoadout(Level)
	local GM, P = self.GM, self.Player
	for _, Id in ipairs({ "Spear", "Bow", "Staff", "ChainMail", "LuckyRing", "SwiftCharm" }) do GM:AddItem(Id, 1, false) end
	GM:AddItem("Potion", 3, false)
	GM:AddItem("HiPotion", 2, false)
	GM:AddItem("Ether", 2, false)
	GM:AddItem("Elixir", 1, false)
	GM:AddItem("Jelly", 2, false)
	GM.Gold = 482
	GM.Sub.Smith = { State = "Active", Count = 0 }
	GM.Sub.Cat = { State = "Done", Count = 0 }
	GM.QuestStage = 1
	GM:RefreshQuest()
	P:AddExp(D.Balance().ExpTable[Level or 3] or 0)
	P.Health = math.floor(P.MaxHealth * 0.82)
	P.Mana = math.floor(P.MaxMana * 0.7)
	P.BP = 3
end

function AutoPilot:RunTitleShot()
	self:WaitUntil(function() return self.GM.Menu == "Title" end, 5)
	self:Note("타이틀 화면에서 대기")
	self:Idle()
end

function AutoPilot:RunInventory()
	self:Wait(0.5)
	self:DemoLoadout(3)
	self:Wait(1.5)
	self:Press("Inventory")
	self:Wait(0.3)
	self:SelectRow("HiPotion")
	self:Note("인벤토리 화면에서 대기")
	self:Idle()
end

function AutoPilot:RunEquip()
	self:Wait(0.5)
	self:DemoLoadout(3)
	self:Wait(1.5)
	self:Press("Inventory")
	self:Wait(0.3)
	self:SelectTab(2)
	self:SelectRow("ChainMail")
	self:Note("장비 탭에서 대기")
	self:Idle()
end

function AutoPilot:RunShop()
	self:Wait(0.5)
	self:DemoLoadout(2)
	self.GM.Items.Bow = nil
	self.GM.Items.ChainMail = nil
	self:TalkToNpc("Merchant")
	self:TalkThrough()
	self:SelectRow("ChainMail")
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

function AutoPilot:RunBoost()
	self:Wait(0.3)
	self:DemoLoadout(3)
	self.Player.BP = 5
	self:EquipBySwitch("Sword")
	while true do
		local Target = self.GM:NearestEnemy(self:Pos(), 1500)
		if Target and not self.GM:IsMenuOpen() then
			if self.Player.BP < 3 then self.Player.BP = 5 end
			self:Engage(Target, true)
		end
		self:Yield()
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
			if Boss then self:Engage(Boss, true) else self:MoveToward(self.GM.BossPos) end
			self:Survive()
		end
		self:Yield()
	end
end

return AutoPilot
