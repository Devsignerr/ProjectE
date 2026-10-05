-- HD-2D 데모 자동 조종 (자동 검증 — HD2DGame.Properties.AutoPlay). 플레이어 입력 표를 대신 채운다 (HD2DPlayer:GatherInput).
--   시나리오는 코루틴 하나(Run*) — 도우미(GoTo/Press/Fight…)가 프레임마다 입력을 채우고 yield 한다. 시간은 실제 시간(메뉴로 게임이 멈춰도 흐른다),
--   막힘 판정만 게임 시간. 확인은 Expect로 쌓고 끝에 관리자 ReportAutoPlay → 로그 "[HD2D] 결과: 실패 N건".
--   Full: 씬을 세 번 연다 — 단계는 Game.SetPersistent("HD2D_AutoPhase")로 넘기고 실패 목록·확인 수·기대 상태도 Persistent로 잇는다.
--     ① Main     : 타이틀(이어하기 없음) → 처음부터 → 시작 연출 → 인벤토리(탭) → 촌장(퀘스트 1) → 창 상자 → 장비 탭에서 창 → 대장장이 의뢰 → 상점(활·회복약)
--                  → 리나 의뢰 → 야영지 상자(지팡이) → 회복약 → 종류별 전투(부스트 공격 포함) → 고양이 → 농부 의뢰 → 허수아비 사냥 → 농부 보고(부적 장비)
--                  → 리나 보고(목걸이) → 촌장 보고(퀘스트 2) → 대장장이 보고(판금 갑옷 장비) → 메타(일시정지 각 화면·설정·지도·도감·기록·대장간 — HD2DMetaPilot)
--                  → 게시판 저장(슬롯 2) → 맵 이동 트리거(같은 씬)
--     ② Travel   : 세션으로 도착(타이틀 없음, Spawn_Test 자리, 상태 일치) → 보스 → 보상 상자 → 촌장(퀘스트 완료) → 저장(슬롯 2 덮어쓰기 확인)
--                  → 예전 단일 슬롯 저장 만들기(슬롯 1 비움) → 같은 씬 다시 열기(세션 없음)
--     ③ Load     : 예전 저장 → 슬롯 1 이전 → 타이틀(이어하기 가능) → 슬롯 창(빈 칸 막힘) → 슬롯 2 → 저장 상태와 일치·자리 → 결과
--   Cave: 동굴 유적(_HD2DCaveAutoPlay.escene)에서 시작해 두 씬을 잇는다 — 단계 Persistent "HD2D_AutoPhase", 시나리오 "HD2D_AutoScenario"(마을 씬은 속성이 비어 있다)
--     ① Run    : 타이틀 없이 시작 → (메인 퀘스트 4단계 상태로 꾸밈) → 입구 홀·갈림길·보물 단(수정 검)·복도·호수의 적 정리 → 구덩이 막힘 → 가시 함정 건너기
--                → 보스 방(문 닫힘) → 수정 거미 여왕(패턴 3종 이상·2단계·잔상) → 문 열림·보상 상자·제단 상자(수정 부적) → 퀘스트 5단계 → 동굴 출구 트리거
--     ② Return : 메인 맵 Spawn_CaveExit 도착(세션 상태 일치) → 촌장 보고(6단계) → 엔딩·크레딧 → 결과
--   TitleShot / Inventory / Equip / Shop / Dialog / Combat / Boost / Boss / CaveCombat / CaveBoss / CaveTrap : 스크린샷·측정용 (그 화면에서 머문다)
--   이 모듈은 상태를 갖지 않는다 (Script.Require 값은 공유) — 상태는 New가 만든 객체에.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")

local AutoPilot = {}
AutoPilot.__index = AutoPilot
-- 메타 시스템 확인·스크린샷 시나리오 (일시정지 메뉴·슬롯·설정·일지·지도·대장간·도감 — HD2DMetaPilot.lua)
for Name, Fn in pairs(Script.Require("Scripts/Demo/HD2D/HD2DMetaPilot.lua")) do AutoPilot[Name] = Fn end
for Name, Fn in pairs(Script.Require("Scripts/Demo/HD2D/HD2DAutoCombat.lua")) do AutoPilot[Name] = Fn end -- 전투 깊이: 스킬 쓰기·확인·스크린샷

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function AutoPilot.New(Scenario, Player, GM)
	local A = setmetatable({ Scenario = Scenario, Player = Player, GM = GM, Time = 0, Frame = 0, Failures = {}, Checks = 0, Teleports = 0,
	                         DamageTakenScale = 1.0, PotionTimer = 0 }, AutoPilot) -- 받는 피해 그대로 (2026-10-05 전투 깊이: 자동 조종도 사람과 같은 난이도로)
	local Name = Scenario
	if Scenario == "Full" or Scenario == "Cave" then
		A.Phase = Game.GetPersistent("HD2D_AutoPhase", Scenario == "Full" and "Main" or "Run")
		Name = Scenario .. A.Phase
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
	Log.Info(string.format("[HD2D] 자동 %s %.1fs: %s", self.Phase and (self.Scenario .. "/" .. self.Phase) or self.Scenario, self.Time, Text))
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
	In.Cancel = In.Cancel or In.Dash or In.Pause
	return In
end

function AutoPilot:Finish()
	if self.bFinished then return end
	self.bFinished = true
	Game.SetPersistent("HD2D_AutoPhase", nil)
	Game.SetPersistent("HD2D_AutoScenario", nil)
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
	-- 출발점이 내비메시 가장자리 밖(소품·적에 밀려남)이면 조금 옆에서 다시
	for _, Off in ipairs({ Vector3(80, 0, 0), Vector3(-80, 0, 0), Vector3(0, 80, 0), Vector3(0, -80, 0) }) do
		if Nav and #Nav >= 2 then break end
		Nav = AI.FindPath(Foot + Off, Vector3(To.X, To.Y, Foot.Z))
	end
	if Nav and #Nav >= 2 and Flat(Nav[#Nav] - To):Length() < 200 then
		local Points = {}
		for I = 2, #Nav do Points[#Points + 1] = Nav[I] end
		Points[#Points] = To
		self.NavRoutes = (self.NavRoutes or 0) + 1
		return Points
	end
	local Path = {}
	for _, P in ipairs(self.GM.Path) do
		if P.X > -1400 or self.GM.Properties.Map ~= "Village" then Path[#Path + 1] = P end -- 마을: 광장 가운데(우물)는 건너뛴다
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

-- 순간이동 자리의 바닥 높이 (위에서 아래로 레이캐스트 — 단·벼랑처럼 높이가 다른 곳에 지금 높이로 놓으면 지형 속에 끼어 떨어졌다)
function AutoPilot:GroundZ(Target, FallbackZ)
	local Hit = Physics.Raycast(Vector3(Target.X, Target.Y, FallbackZ + 1500), Vector3(0, 0, -1), 4000)
	if Hit then return Hit.position.Z end
	return FallbackZ - 60
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
		self.Player:Teleport(Vector3(Target.X, Target.Y, self:GroundZ(Target, Pos.Z) + 90))
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
	local Nav0 = self.NavRoutes or 0
	local Route = self:Route(self:Pos(), Target)
	if self.Scenario == "Cave" and Label then
		local From = self:Pos()
		self:Note(string.format("길 %s: %d점 (%s, 출발 %.0f, %.0f, %.0f)", Label, #Route, (self.NavRoutes or 0) > Nav0 and "내비메시" or "직선/Path", From.X, From.Y, From.Z))
	end
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
	-- 사람처럼: 보스 패턴 세 번에 한 번은 욕심내 계속 때리다 맞는다 (자동 조종이 모든 패턴을 완벽히 피하면 난이도 확인이 안 된다 — 결정적)
	if Target.bBoss then
		local N = 0
		for _, C in pairs(self.GM.Report.BossPatterns) do N = N + C end
		self.bGreedy = N % 3 == 2
	else
		self.bGreedy = false
	end
	-- 보스가 내리치려 하면 물러난다
	if not self.bGreedy and Target.bBoss and (Target.State == "SlamWindup" or Target.State == "ChargeWindup") and L < 520 then
		self.In.Move = Dir * -1
		if Target.State == "ChargeWindup" or L < 330 then self.In.Dash = P.DashCooldown <= 0 and self.Frame % 2 == 0 end
		return
	end
	-- 수정 거미 여왕: 덮치기 예고면 옆으로 비켜 대시, 수정 가시 경고 원 안이면 밖으로
	if not self.bGreedy and Target.bBoss and Target.State == "PounceWindup" and Target.Timer < 0.3 then
		self.In.Move = Vector3(-Dir.Y, Dir.X, 0)
		self.In.Dash = P.DashCooldown <= 0
		return
	end
	if self:DodgeEruptions() or self:DodgeProjectiles() then return end
	if L > Want + 50 then
		self:MoveToward(Target.entity:GetWorldPosition())
	elseif (W.Kind == "Arrow" or W.Kind == "Bolt") and L < Want - 220 then
		self.In.Move = Dir * -1
	else
		self.In.Move = Dir * 0.25
	end
	-- 보스·정예에는 BP를 아껴 두었다가 브레이크(또는 실드 마지막 두 칸)에 몰아 쓴다
	if bBoost and (Target.bBoss or Target.Row.Elite) and not Target.bBroken and (Target.Shield or 0) > 2 then bBoost = false end
	if not (bBoost and P.BP >= 2) and self:UseSkills(Target, L) then return end -- 약점 속성 스킬·치유 (HD2DAutoCombat.lua — 부스트를 모을 땐 평타)
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

-- 곧 솟을 수정 가시 경고 원 안이면 바깥으로 (이번 프레임 이동을 정했으면 true)
function AutoPilot:DodgeEruptions()
	local Pos = self:Pos()
	if self.bGreedy then return false end
	for _, R in ipairs(self.GM.Eruptions or {}) do
		local Away = Flat(Pos - R.Pos)
		if Away:Length() < R.Radius + 50 and R.Delay < 0.55 then -- 반응 시간: 경고가 뜨고 0.3초쯤 지나서야 움직인다
			self.In.Move = Away:Length() > 1 and Away:Normalized() or Vector3(1, 0, 0)
			return true
		end
	end
	return false
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
	self:GoTo(Pos + Vector3(0, 115, 0), 30, 70, Label) -- 상자 상호작용 160cm 안 (115 + 30)
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
	return self:SaveAtBoardSlot(2) -- 슬롯 창 (HD2DMetaPilot)
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
	self:MetaResetSaves() -- 슬롯 1~3도
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
	self:MetaTrackCheck("Cat")
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
	self:Expect(GM.Report.Reveals >= 1, "약점 공개 (슬라임 ← 검 = 베기)")
	self:Fight("Goblin", "Spear", 1, 50, true)
	self:Fight("Mushroom", "Staff", 1, 55)
	self:PoisonAndCure()
	self:Fight("Bat", "Bow", 1, 55, true)
	self:Fight("Archer", "Bow", 1, 60)
	if GM.QuestKills < D.Quest(1).KillGoal then
		self:Fight("Slime", "Spear", D.Quest(1).KillGoal - GM.QuestKills, 45)
	end
	self:Expect(GM.QuestKills >= D.Quest(1).KillGoal, "퀘스트 처치 목표")
	self:Fight("EliteGoblin", "Spear", 1, 90, true)
	self:Expect(GM.Report.EliteKills >= 1, "정예 고블린 도적 처치")
	self:CheckBreaks("들판 (고블린·정예)")
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
	self:RecruitCompanionCheck() -- 들판에서 돌아와 광장의 엘라를 영입 (들판 시험은 혼자 — 독 시험을 엘라가 대신 끝내지 않게)
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
	self:CheckSkills("들판", 2)
	self:Press("Inventory")
	self:Wait(0.2)
	self:MetaChecksMain()

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
	self:Expect(GM.Companion ~= nil and GM.Companion.Mode == "Follow", "맵 이동 뒤 동료 동행")
	local Expect = Game.GetPersistent("HD2D_AutoExpect", "")
	local Now = GM:StateSignature()
	if not self:Expect(Now == Expect, "세션 상태 유지") then
		self:Note("기대 " .. Expect)
		self:Note("실제 " .. Now)
	end

	-- 보스
	self:EquipBySwitch("Spear")
	self:GoTo(GM.BossPos + Vector3(-700, 250, 0), 150, 70, "보스 앞")
	local Until = self.Time + 240 -- 보스는 브레이크를 노려야 잡힌다 (HD2DCombatGen 보스 수치 근거)
	local Dash0, Fight0, Potion0 = P.Stats.Dashes, self.Time, self:PotionsUsed()
	self.LowHp = 1
	local Swap = self.Time + 10
	-- 일반 전투 흐름만 (예비 동작을 보고 피하며 계속 공격 — 패턴 3종은 보스의 패턴 순환이 보장한다)
	while not GM.bBossDead and self.Time < Until do
		local Boss = GM:NearestEnemy(self:Pos(), 3000, function(S) return S.bBoss end)
		if not GM:IsMenuOpen() then
			if Boss then
				self:Engage(Boss, true)
			else
				self:MoveToward(GM.BossPos) -- 잠든 보스는 목록에 없다 → 다가가 깨운다
			end
			self:Survive()
			self:TrackLowHp()
			if self.Time > Swap then
				Swap = self.Time + 2
				if Boss then self:SwitchToWeakWeapon(Boss) end -- R로 약점 속성 무기로 (2단계에 약점이 바뀌면 다시 — HD2DAutoCombat.lua)
			end
		end
		self:Yield()
	end
	self:Expect(GM.bBossDead, string.format("보스 처치 (%.0f초)", self.Time - Fight0))
	self:Expect(P.Stats.Dashes > Dash0, "골렘 패턴 대시 회피 (" .. (P.Stats.Dashes - Dash0) .. ")")
	self:Note(string.format("골렘전 난이도: 최저 HP %.0f%%, 회복약 %d개, 쓰러짐 %d", self.LowHp * 100, self:PotionsUsed() - Potion0, P.Stats.Deaths))
	self:CheckCompanion("골렘전")
	self:Expect(GM.Report.BossBreaks >= 1, "골렘 브레이크 (" .. GM.Report.BossBreaks .. ")")
	local Patterns = 0
	for _, N in pairs(GM.Report.BossPatterns) do if N > 0 then Patterns = Patterns + 1 end end
	self:Expect(Patterns >= 3, "보스 패턴 3종 이상 (" .. Patterns .. ")")
	self:Expect(GM.QuestStage == 3, "퀘스트 3단계")
	self:Wait(1.5)
	self:OpenChestAt(#GM.Chests)

	-- 촌장 보고 → 완료 → 저장 → 같은 씬 다시 열기 (세션 없음 = 타이틀)
	self:TalkToNpc("Elder")
	self:TalkThrough()
	self:Expect(GM.QuestStage == 4 and D.Quest(4).BossGoal == "Cave", "골렘 보고 → 퀘스트 4단계 (동굴 유적 탐사)")
	self:SaveAtBoard()
	local Board = self:FindProp("SavePoint")
	Game.SetPersistent("HD2D_AutoExpect", GM:StateSignature())
	Game.SetPersistent("HD2D_AutoPos", Board.Pos)
	self:MetaPrepareMigration(2)
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
	self:MetaLoadChecks(2) -- 예전 저장 이전 확인 → 이어하기 슬롯 창 → 슬롯 2
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
	self:Expect(GM.CompanionRecruited and GM.Companion ~= nil and GM.Companion.Mode == "Follow", "이어하기 → 동료 동행")
	-- 전체 기록 (마지막 씬의 보고는 이번 씬 것뿐이라 처치·명중 같은 누적은 앞 단계에서 확인했다)
	self:Expect(self.Player.Level >= 3, "레벨 (Lv " .. self.Player.Level .. ")")
	self:Finish()
	self:Idle()
end

-- ================================================================ Cave ① Run (동굴 유적 — 입구에서 보스·출구까지)
-- 메인 퀘스트에서 동굴로 온 상태로 꾸민다: 골렘 보고 뒤(4단계), 레벨·장비·회복약
function AutoPilot:CaveLoadout()
	local GM, P = self.GM, self.Player
	for _, Id in ipairs({ "Spear", "Bow", "Staff", "KnightPlate", "SwiftCharm" }) do GM:AddItem(Id, 1, false) end
	GM:AddItem("Potion", 4, false)
	GM:AddItem("HiPotion", 3, false)
	GM:AddItem("Ether", 3, false)
	GM.Armor, GM.Accessory = "KnightPlate", "SwiftCharm"
	GM.QuestStage, GM.BossDead = 4, true
	GM:RefreshQuest()
	P:AddExp(D.Balance().ExpTable[5] or 0)
	P:RecalcStats()
	P.Health, P.Mana, P.BP = P.MaxHealth, P.MaxMana, 3
	GM:RecruitCompanion() -- 마을에서 영입한 동료와 함께 (HD2DCombat.lua)
	self:Note(string.format("동굴 시작 상태: 퀘스트 4단계, Lv %d, HP %d", P.Level, P.MaxHealth))
end

function AutoPilot:TotalKills()
	local N = 0
	for _, K in pairs(self.GM.Report.Kills) do N = N + K end
	return N
end

-- Center 둘레 Radius 안 적(보스 제외)을 모두 쓰러뜨린다 (먼저 Center로 간다)
function AutoPilot:ClearArea(Center, Radius, Timeout, WeaponId, Label)
	local GM = self.GM
	if WeaponId then self:EquipBySwitch(WeaponId) end
	local Kills0 = self:TotalKills()
	local Until = self.Time + Timeout
	self:GoTo(Center, 220, Timeout * 0.5, Label)
	self.StuckTotal, self.StuckClock, self.StuckFrom = 0, 0, nil
	while true do
		local Target = GM:NearestEnemy(Center, Radius, function(S) return not S.bBoss end)
		if not Target then break end
		if self.Time > Until then
			return self:Expect(false, string.format("%s 적 정리 (%.0f초 안, 남은 %s)", Label, Timeout, Target.Kind))
		end
		if not GM:IsMenuOpen() then
			local Pick = self:NearestPickup(700)
			local TargetDist = Flat(Target.entity:GetWorldPosition() - self:Pos()):Length()
			if Pick and TargetDist > 300 then self:MoveToward(Pick.Pos) else self:Engage(Target, true) end
			self:Survive()
		end
		self:Yield()
	end
	-- 떨어진 전리품 줍기 (잠깐)
	local PickUntil = self.Time + 4.0
	while self.Time < PickUntil do
		local Pick = self:NearestPickup(600)
		if not Pick then break end
		if not GM:IsMenuOpen() then self:MoveToward(Pick.Pos) end
		self:Yield()
	end
	return self:Expect(true, string.format("%s 적 정리 (처치 %d)", Label, self:TotalKills() - Kills0))
end

-- 구덩이: 다리 밖으로 걸어 들어가려 하면 막힌다 (떨어지지 않음)
function AutoPilot:CheckPitBlocked()
	self:GoTo(Vector3(470, 240, 0), 60, 40, "구덩이 앞 (다리 밖)")
	local Until = self.Time + 1.6
	while self.Time < Until do
		self.In.Move = Vector3(1, 0, 0)
		self:Yield()
	end
	local Pos = self:Pos()
	self:Expect(Pos.X < 680 and Pos.Z > -60, string.format("구덩이 낙하 막힘 (x %.0f, z %.0f)", Pos.X, Pos.Z))
end

-- 가시 함정 줄 건너기: 앞에서 솟는 것을 한 번 본 뒤, 마지막 판까지 막 내려간 때 달려 건넌다 (Dir = 1 동쪽, -1 서쪽)
function AutoPilot:CrossTraps(Dir)
	local GM = self.GM
	local First, Last = GM.Traps[1], GM.Traps[#GM.Traps]
	local Near, Far = Dir > 0 and First or Last, Dir > 0 and Last or First
	local StandX = Near.Pos.X - Dir * (Near.HX + 110)
	local GoalX = Far.Pos.X + Dir * (Far.HX + 160)
	self:GoTo(Vector3(StandX, 0, 0), 50, 40, "함정 앞")
	local Rises0, Hits0 = GM.Report.TrapRises, GM.Report.TrapHits
	self:WaitUntil(function() return GM.Report.TrapRises > Rises0 end, 8)
	self:WaitUntil(function()
		local State, Age = GM:TrapState(Last)
		return State == "Down" and Age < 0.25
	end, 8)
	self:GoTo(Vector3(GoalX, 0, 0), 60, 10, "함정 건너편")
	self:Expect(GM.Report.TrapRises > Rises0 and GM.Report.TrapHits == Hits0,
		string.format("가시 함정 회피 (%s쪽, 솟음 %d번 관찰, 건너는 동안 찔림 %d)", Dir > 0 and "동" or "서", GM.Report.TrapRises - Rises0, GM.Report.TrapHits - Hits0))
end

function AutoPilot:PatternsSeen()
	local Seen = 0
	for Name, N in pairs(self.GM.Report.BossPatterns) do
		if N > 0 and Name ~= "Summon" then Seen = Seen + 1 end
	end
	return Seen
end

function AutoPilot:RunCaveRun()
	local GM, P = self.GM, self.Player
	self:Wait(0.6)
	self:Expect(GM.Properties.Map == "Cave" and GM.Menu == nil and GM.Mode == "Play" and Game.GetTimeScale() == 1, "동굴 직접 시작: 타이틀 없이 바로 플레이")
	self:Expect(#GM.Slots == 12 and #GM.Chests == 3 and #GM.Traps == 4 and GM.Gate ~= nil and GM.BossPos ~= nil,
		string.format("동굴 배치 (적 %d, 상자 %d, 함정 %d, 문 %s)", #GM.Slots, #GM.Chests, #GM.Traps, tostring(GM.Gate ~= nil)))
	self:Expect(GM.QuestStage == 0 and not GM:IsBossDefeated(), "세션 없이 열면 기본 상태")
	self:CaveLoadout()

	-- 1 입구 홀 (약한 적) → 2 갈림길 (궁수·독버섯) → 보물 단 (궁수 + 숨은 상자: 수정 검)
	self:ClearArea(Vector3(-2400, -150, 0), 900, 70, "Sword", "입구 홀")
	self:ClearArea(Vector3(-850, -80, 0), 850, 60, "Spear", "갈림길")
	self:ClearArea(Vector3(-850, -1180, 0), 650, 60, "Bow", "보물 단")
	self:OpenChestAt(1)
	self:Expect(GM:Count("CrystalSword") == 1, "수정 검 획득 (보물 단 상자)")
	self:EquipFromMenu("CrystalSword")

	-- 3 함정 복도: 다리 밖 구덩이 막힘 → 박쥐(활) → 다리 → 가시 함정
	self:CheckPitBlocked()
	self:ClearArea(Vector3(200, 0, 0), 900, 60, "Bow", "복도 서쪽")
	self:GoTo(Vector3(1250, 0, 0), 80, 40, "다리 건너편")
	self:ClearArea(Vector3(1250, 0, 0), 1100, 60, "Bow", "함정 앞 박쥐")
	self:CrossTraps(1)

	-- 4 수정 호수 (강한 혼합) + 물가 상자
	self:ClearArea(Vector3(2450, 0, 0), 900, 80, "CrystalSword", "수정 호수")
	self:Expect(GM.Report.EliteKills >= 1, "정예 해골 궁수 처치 (수정 호수)")
	self:OpenChestAt(2)

	-- 5 보스 방: 들어서면 문이 닫힌다 → 수정 거미 여왕
	self:GoTo(Vector3(Scene.Find("Spawn_BossDoor"):GetWorldPosition().X, -150, 0), 120, 40, "보스 방 앞")
	self:GoTo(Vector3(3520, -320, 0), 80, 30, "보스 방 안")
	self:WaitUntil(function() return GM:IsGateClosed() end, 3)
	self:Expect(GM:IsGateClosed(), "보스 방 입장 → 문 닫힘")
	self:EquipBySwitch("Spear")
	local Until = self.Time + 260
	local Dash0, Fight0, Potion0 = P.Stats.Dashes, self.Time, self:PotionsUsed()
	self.LowHp = 1
	-- 일반 전투 흐름만 (관찰 대기 없이 — 여왕은 붙어 있어도 뒤로 뛰어 덮친다)
	while not GM.bBossDead and self.Time < Until do
		local Boss = GM:NearestEnemy(self:Pos(), 3000, function(S) return S.bBoss end)
		if not GM:IsMenuOpen() then
			if Boss then
				if self.Frame % 120 == 0 then self:SwitchToWeakWeapon(Boss) end -- R로 약점 속성 무기로 (HD2DAutoCombat.lua)
				self:Engage(Boss, true)
			else
				self:MoveToward(GM.BossPos) -- 잠든 보스는 목록에 없다 → 다가가 깨운다
			end
			self:Survive()
			self:TrackLowHp()
		end
		self:Yield()
	end
	self:Expect(GM.bBossDead and GM:IsBossDefeated("Cave"), string.format("수정 거미 여왕 처치 (%.0f초)", self.Time - Fight0))
	self:Expect(P.Stats.Dashes > Dash0, "여왕 패턴 대시 회피 (" .. (P.Stats.Dashes - Dash0) .. ")")
	self:Note(string.format("여왕전 난이도: 최저 HP %.0f%%, 회복약 %d개, 쓰러짐 %d", self.LowHp * 100, self:PotionsUsed() - Potion0, P.Stats.Deaths))
	self:Expect(GM.Report.BossBreaks >= 1, "여왕 브레이크 (" .. GM.Report.BossBreaks .. ")")
	self:CheckSkills("동굴", 3)
	self:CheckCompanion("동굴")
	local PlayerStatus = 0
	for _, N in pairs(GM.Report.StatusOnPlayer) do PlayerStatus = PlayerStatus + N end
	self:Expect(PlayerStatus >= 1, "동굴 적·여왕의 상태 이상 (" .. PlayerStatus .. ")")
	self:Expect(self:PatternsSeen() >= 3, "보스 패턴 3종 이상 (" .. self:PatternsSeen() .. ")")
	self:Expect((GM.Report.BossPatterns.Summon or 0) >= 1, "보스 2단계 (격노·소환)")
	self:Expect(GM.Report.Afterimages > 0 and GM.Report.Eruptions > 0, string.format("덮치기 잔상 %d, 수정 가시 %d", GM.Report.Afterimages, GM.Report.Eruptions))
	self:Expect(GM.QuestStage == 5, "퀘스트 5단계 (마을로 보고)")
	self:WaitUntil(function() return GM.Gate.State == "Open" end, 4)
	self:Expect(GM.Gate.State == "Open" and GM.Report.GateOpens >= 1, "보스 처치 → 문 열림")
	self:Wait(1.2)
	-- 남은 소환 슬라임 정리 → 보상 상자 → 제단 옆 상자 (수정 부적)
	self:ClearArea(GM.BossPos, 900, 40, "CrystalSword", "보스 방")
	self:OpenChestAt(#GM.Chests)
	self:Expect(GM.Opened["Cave:Boss"] == true, "보스 보상 상자")
	self:OpenChestAt(3)
	self:Expect(GM:Count("CrystalCharm") == 1, "수정 부적 획득 (제단 옆 상자)")
	self:EquipFromMenu("CrystalCharm")
	self:Expect(GM.Report.Chests == 4, "동굴 상자 4개 (보물 3 + 보상)")
	self:MetaCaveChecks() -- 동굴 지도·미니맵 (앞 구간 길찾기 타이밍을 바꾸지 않게 보스 뒤에)

	-- 6 출구로: 함정을 다시 건너 입구 홀 서쪽 이동 트리거 → 메인 맵
	self:CrossTraps(-1)
	self:HandOff("Return")
	Game.SetPersistent("HD2D_AutoScenario", "Cave")
	local Trigger = Scene.Find("CaveExit_Travel")
	local Target = Trigger:GetWorldPosition() + Vector3(-60, 0, 0)
	local Route = self:Route(self:Pos(), Target)
	local Index, Until = 1, self.Time + 80
	while GM.Mode ~= "Travel" and self.Time < Until do
		-- 트리거 상자에 들어서는 순간 맵 이동이 시작된다 (도착 반경을 기다리지 않는다)
		local WP = Route[math.min(Index, #Route)]
		if Index < #Route and Flat(WP - self:Pos()):Length() < 90 then Index = Index + 1 end
		if not GM:IsMenuOpen() then self:MoveToward(WP) end
		self:Survive()
		self:Yield()
	end
	self:Expect(GM.Mode == "Travel", "동굴 출구 → 맵 이동 시작")
	Game.SetPersistent("HD2D_AutoExpect", GM:StateSignature())
	Game.SetPersistent("HD2D_AutoFailures", table.concat(self.Failures, "|"))
	Game.SetPersistent("HD2D_AutoChecks", self.Checks)
	Game.SetPersistent("HD2D_AutoTime", self.Time)
	self:Idle()
end

-- ================================================================ Cave ② Return (메인 맵 도착 → 촌장 보고 → 엔딩)
function AutoPilot:RunCaveReturn()
	local GM = self.GM
	self:Wait(0.6)
	self:Expect(GM.Properties.Map == "Village" and GM.Menu == nil and GM.Mode == "Play", "메인 맵 도착: 타이틀 없음")
	local Spawn = Scene.Find("Spawn_CaveExit")
	self:Expect(Spawn ~= nil and Flat(self:Pos() - Spawn:GetWorldPosition()):Length() < 150, "도착 자리 = Spawn_CaveExit (폭포 옆 동굴 입구 앞)")
	local Expect = Game.GetPersistent("HD2D_AutoExpect", "")
	local Now = GM:StateSignature()
	if not self:Expect(Now == Expect, "세션 상태 유지 (동굴 → 마을)") then
		self:Note("기대 " .. Expect)
		self:Note("실제 " .. Now)
	end
	self:Expect(GM.QuestStage == 5 and GM:IsBossDefeated("Cave") and GM:IsEquipped("CrystalSword"), "동굴 진행 유지 (퀘스트 5단계, 여왕 처치, 수정 검)")
	self:TalkToNpc("Elder")
	self:TalkThrough()
	self:Expect(GM.QuestStage == D.FinalQuestStage(), "촌장 보고 → 마지막 단계")
	self:WaitUntil(function() return GM.Menu == "Ending" end, 8)
	self:Expect(GM.Menu == "Ending" and GM:Hud():W("EndingScreen").Visible, "엔딩·크레딧 화면")
	local Until = self.Time + 40
	while GM.Menu == "Ending" and self.Time < Until do
		self:Wait(1.3)
		if GM.Menu == "Ending" then self:Press("Confirm") end
	end
	self:Wait(0.5)
	self:Expect(GM.Menu == nil and GM.Mode == "Play" and Game.GetTimeScale() == 1 and GM.Report.Endings == 1, "엔딩 끝 → 자유 탐험")
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

-- 수정 호수 전투가 이어지게 (측정용): 둘레 적이 셋보다 적으면 동굴 적을 돌아가며 더 부른다
function AutoPilot:RunCaveCombat()
	self:Wait(0.3)
	self:CaveLoadout()
	self:EquipBySwitch("Spear")
	local Kinds, Next, SpawnTimer = { "CrystalSlime", "CaveBat", "Goblin", "Archer" }, 1, 0
	local Home = self:Pos()
	while true do
		local GM = self.GM
		SpawnTimer = SpawnTimer - (self.GameDt or 0)
		local Near = 0
		for _, S in ipairs(GM:AliveEnemies()) do
			if not S.bBoss and Flat(S.entity:GetWorldPosition() - Home):Length() < 1300 then Near = Near + 1 end
		end
		if Near < 3 and SpawnTimer <= 0 then
			local A = Next * 2.1
			GM:SpawnEnemy(Kinds[(Next - 1) % #Kinds + 1], Vector3(Home.X + math.cos(A) * 450, Home.Y - 250 + math.sin(A) * 200, Home.Z + 10))
			Next, SpawnTimer = Next + 1, 0.6
		end
		local Target = GM:NearestEnemy(self:Pos(), 1500, function(S) return not S.bBoss end)
		if Target and not GM:IsMenuOpen() then self:Engage(Target, true) end
		if self.Player.Health < self.Player.MaxHealth * 0.5 then self.Player.Health = self.Player.MaxHealth end
		self:Yield()
	end
end

function AutoPilot:RunCaveBoss()
	self:Wait(0.3)
	self:CaveLoadout()
	self:EquipBySwitch("Spear")
	while true do
		local Boss = self.GM:NearestEnemy(self:Pos(), 3000, function(S) return S.bBoss end)
		if not self.GM:IsMenuOpen() then
			if Boss then
				if Boss.Health < Boss.Row.MaxHealth * 0.3 then Boss.Health = Boss.Row.MaxHealth * 0.45 end -- 오래 싸우는 장면 유지
				self:Engage(Boss, true)
			else
				self:MoveToward(self.GM.BossPos)
			end
			if self.Player.Health < self.Player.MaxHealth * 0.5 then self.Player.Health = self.Player.MaxHealth end
		end
		self:Yield()
	end
end

function AutoPilot:RunCaveEnding()
	self:Wait(0.3)
	self:CaveLoadout()
	self.GM.QuestStage = 5
	self.GM:SetQuestStage(6)
	self:Note("엔딩·크레딧 화면에서 대기")
	self:Idle()
end

function AutoPilot:RunCaveTrap()
	self:Wait(0.3)
	self:CaveLoadout()
	self:Note("함정 앞에서 대기")
	self:Idle()
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
