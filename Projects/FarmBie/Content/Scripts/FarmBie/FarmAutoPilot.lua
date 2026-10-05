-- FarmBie 자동 조종 (자동 검증 — FarmGame.Properties.AutoPlay). 플레이어 입력 표를 대신 채운다 (FarmPlayer:GatherInput).
--   시나리오는 코루틴 하나(Run<이름>) — 도우미(GoTo/Press/Wait)가 프레임마다 입력을 채우고 yield 한다. 시간은 실제 시간(메뉴로 게임이 멈춰도 흐른다).
--   확인은 Expect로 쌓고 끝에 관리자 ReportAutoPlay → 로그 "[FarmBie] 결과: 실패 N건".
--   Basic: 이동(사방)·방향 플립북·구르기·카메라 추적·집/울타리 충돌
--   이 모듈은 상태를 갖지 않는다 (Script.Require 값은 공유) — 상태는 New가 만든 객체에.
local AutoPilot = {}
AutoPilot.__index = AutoPilot

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function AutoPilot.New(Scenario, Player, GM)
	local A = setmetatable({ Scenario = Scenario, Player = Player, GM = GM, Time = 0, Frame = 0, Failures = {}, Checks = 0 }, AutoPilot)
	local Runner = A["Run" .. Scenario]
	if not Runner then
		Log.Error("[FarmBie] 모르는 자동 시나리오: " .. tostring(Scenario))
		return A
	end
	A.Co = coroutine.create(function() Runner(A) end)
	return A
end

function AutoPilot:Note(Text)
	Log.Info(string.format("[FarmBie] 자동 %s %.1fs: %s", self.Scenario, self.Time, Text))
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
	self.In = { Move = Vector3(0, 0, 0) }
	if self.Co and coroutine.status(self.Co) ~= "dead" then
		local bOk, Err = coroutine.resume(self.Co)
		if not bOk then
			Log.Error("[FarmBie] 자동 조종 오류: " .. tostring(Err))
			self.Failures[#self.Failures + 1] = "자동 조종 오류"
			self.Co = nil
			self:Finish()
		end
	end
	return self.In
end

function AutoPilot:Finish()
	if self.bFinished then return end
	self.bFinished = true
	local P = self.Player
	self.GM:ReportAutoPlay(self.Failures, string.format("%.0f초, 확인 %d건, 이동 %.0fcm, 구르기 %d", self.Time, self.Checks, P.Stats.Distance, P.Stats.Dodges))
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

-- 목표 지점까지 똑바로 걷는다 (도착 반경 R). 막혀서 Timeout을 넘기면 false
function AutoPilot:GoTo(Target, R, Timeout)
	R = R or 40
	local Until = self.Time + (Timeout or 15)
	while true do
		local D = Flat(Target - self:Pos())
		if D:Length() < R then return true end
		if self.Time > Until then return false end
		self.In.Move = D:Normalized()
		self:Yield()
	end
end

-- 한 방향으로 Seconds 동안 걷는다
function AutoPilot:Walk(Dir, Seconds)
	local Until = self.Time + Seconds
	while self.Time < Until do
		self.In.Move = Dir
		self:Yield()
	end
end

-- ================================================================ 시나리오
function AutoPilot:RunBasic()
	local P = self.Player
	self:Wait(1.0)
	local Start = self:Pos()
	self:Expect(P.Anim == "Farmer_IdleDown", "시작 대기 자세 " .. P.Anim)
	-- 사방 걷기 + 방향 플립북
	for _, Case in ipairs({ { Vector3(1, 0, 0), "Farmer_WalkSide", "Right" }, { Vector3(0, 1, 0), "Farmer_WalkDown", "Down" },
	                        { Vector3(-1, 0, 0), "Farmer_WalkSide", "Left" }, { Vector3(0, -1, 0), "Farmer_WalkUp", "Up" } }) do
		local Before = self:Pos()
		self:Walk(Case[1], 0.8)
		local Moved = Flat(self:Pos() - Before)
		self:Expect(Moved:Dot(Case[1]) > 200, string.format("%s 이동 %.0fcm", Case[3], Moved:Dot(Case[1])))
		self:Expect(P.Anim == Case[2] and P.Facing == Case[3], "걷기 플립북 " .. Case[3] .. " " .. P.Anim)
	end
	self:Wait(0.4)
	self:Expect(string.find(P.Anim, "Idle") ~= nil, "멈추면 대기 " .. P.Anim)
	-- 카메라 추적: 시선이 발 높이 평면과 만나는 점이 플레이어 근처
	local Cam = Scene.Find("Camera")
	local Fwd = Cam:GetForward()
	local CamPos = Cam:GetWorldPosition()
	local T = (self:Pos().Z - CamPos.Z) / Fwd.Z
	local Hit = CamPos + Fwd * T
	self:Expect(Flat(Hit - self:Pos()):Length() < 150, string.format("카메라 초점 거리 %.0fcm", Flat(Hit - self:Pos()):Length()))
	-- 구르기: 짧은 시간에 멀리
	local Before = self:Pos()
	self.In.Move = Vector3(1, 0, 0)
	self:Press("Dodge")
	self:Wait(0.3)
	self:Expect(P.Stats.Dodges == 1 and Flat(self:Pos() - Before):Length() > 150, string.format("구르기 %.0fcm", Flat(self:Pos() - Before):Length()))
	-- 집 충돌: 집 쪽(위)으로 계속 걸어도 집 앞에서 멈춘다
	self:GoTo(Vector3(0, -800, 0), 40, 10)
	self:Walk(Vector3(0, -1, 0), 2.0)
	self:Expect(self:Pos().Y > -1200, string.format("집에 막힘 Y %.0f", self:Pos().Y))
	-- 울타리 충돌: 오른쪽 울타리(입구 아님)에서 멈춘다
	self:GoTo(Vector3(2000, 900, 0), 60, 15)
	self:Walk(Vector3(1, 0, 0), 2.5)
	self:Expect(self:Pos().X < 2460, string.format("울타리에 막힘 X %.0f", self:Pos().X))
	self:Expect(Flat(self:Pos() - Start):Length() > 500, "시작에서 멀어짐")
	self:Finish()
end

return AutoPilot
