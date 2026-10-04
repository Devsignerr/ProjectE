-- Crypt2D 자동 조종 (자동 검증 — GameManager.Properties.AutoPlay). 플레이어 입력 표를 대신 채우고(Player:GatherInput),
-- 실제 레벨에서 기능이 되는지 단계마다 확인해 "[Crypt2D] 자동 …" 로그와 마지막 "[Crypt2D] 결과: 실패 N건"을 남긴다.
--   Test    : 검증 코스(Dungeon.TestLayout — 시작 방 + 해골 하나 방). 바닥 대시 → 높은 단(두 번 점프로만) → 원웨이 발판 위로(아래에서 통과)
--             → S+Space 내려가기 → 오른쪽 방 입장 = 문 잠김 → 싸워서 전멸 = 문 열림
--   Boss    : 보스 층. 오른쪽 보스 방으로 가서 싸운다 (플레이어 무적). 패턴 3종 이상·보스 체력 감소 확인
--   Explore : 생성된 층. 왼쪽/오른쪽 문으로 이웃 방에 들어가 싸운다 (스크린샷용, 확인은 방 하나 정리)
local U = Script.Require("Scripts/Crypt/Util.lua")

local AutoPilot = {}
AutoPilot.__index = AutoPilot

function AutoPilot.New(Mode, GM)
	return setmetatable({ Mode = Mode, GM = GM, Phase = "Settle", PhaseTime = 0, Time = 0, Failures = 0, Checks = 0, Events = {},
	                      bReported = false, Frame = 0 }, AutoPilot)
end

function AutoPilot:Note(Text)
	Log.Info(string.format("[Crypt2D] 자동 %s f%d %.2fs: %s", self.Mode, self.Frame, self.Time, Text))
end

function AutoPilot:Expect(bOk, What)
	self.Checks = self.Checks + 1
	if bOk then
		self:Note("확인 " .. What)
	else
		self.Failures = self.Failures + 1
		self:Note("확인 실패 " .. What)
	end
end

function AutoPilot:SetPhase(Phase)
	self.Phase = Phase
	self.PhaseTime = 0
	self.Step = 0
	self:Note("단계 → " .. Phase)
end

function AutoPilot:Report()
	if self.bReported then return end
	self.bReported = true
	self:Note(string.format("결과: 실패 %d건 (확인 %d개)", self.Failures, self.Checks))
	Log.Info(string.format("[Crypt2D] 결과: 실패 %d건", self.Failures))
end

function AutoPilot:OnPlayerEvent(Name, N)
	self.Events[#self.Events + 1] = Name .. (N > 0 and tostring(N) or "")
end

-- GameManager가 매 프레임 부른다 (입력은 GetInput — 플레이어가 죽거나 판이 끝나면 GetInput이 불리지 않으므로 마무리는 여기서)
function AutoPilot:Update(Dt)
	local GM = self.GM
	if not GM.bGameOver or self.bReported then return end
	if self.Mode == "Boss" then
		self:CheckBoss()
	else
		self:Expect(false, "플레이어 사망으로 판이 끝남")
	end
	self:Report()
end

function AutoPilot:CheckBoss()
	local GM = self.GM
	local Boss = GM.Boss
	local Used = 0
	local Names = {}
	local Counts = (Boss and Boss.PatternCount) or GM.BossPatternCount or {}
	for Name, Count in pairs(Counts) do
		Used = Used + 1
		Names[#Names + 1] = Name .. "×" .. Count
	end
	table.sort(Names)
	self:Expect(Used >= 3, "보스 패턴 3종 이상 [" .. table.concat(Names, ", ") .. "]")
	self:Expect(GM.bVictory or (Boss and Boss.Health < Boss.MaxHealth), string.format("보스 체력 감소 (%s)",
		GM.bVictory and "처치" or (Boss and string.format("%.0f / %.0f", Boss.Health, Boss.MaxHealth) or "?")))
	if GM.bVictory then
		self:Expect(true, string.format("보스 처치 → 승리 (시간 %.1f초)", GM.RunTime))
	end
end

local function Blank(Player)
	return { MoveX = 0, MoveY = 0, AimX = Player.AimX, AimZ = Player.AimZ }
end

-- ---- 싸움 공용: 방의 가장 가까운 적에게 다가가 조준·공격
function AutoPilot:Fight(Player, In, P, bAllowSwap)
	local GM = self.GM
	local Room = GM.CurrentRoom
	local Target, Best = nil, 1e9
	if Room then
		for _, E in ipairs(Room.Enemies) do
			if E:IsValid() then
				local S = E:GetScript()
				if S and not S.bDead then
					local EP = E:GetWorldPosition()
					local D = U.Length(EP.X - P.X, EP.Z - P.Z)
					if D < Best then Target, Best = { X = EP.X, Z = EP.Z, Script = S }, D end
				end
			end
		end
	end
	if Target == nil then return false end
	-- 5초마다 남은 적 상태 (막히면 원인을 볼 수 있게)
	self.FightLog = (self.FightLog or 0) + Time.DeltaTime
	if self.FightLog > 5 then
		self.FightLog = 0
		local Parts = {}
		for _, E in ipairs(Room.Enemies) do
			if E:IsValid() then
				local S = E:GetScript()
				local EP = E:GetWorldPosition()
				Parts[#Parts + 1] = string.format("%s(%.0f, %.0f) %s 체력 %.0f", S and S.Properties.Kind or "?", EP.X - Room.Rect[1], EP.Z - Room.Rect[2],
					S and S.State or "?", S and S.Health or 0)
			end
		end
		self:Note(string.format("전투 중 플레이어 (%.0f, %.0f) 적: %s", P.X - Room.Rect[1], P.Z - Room.Rect[2], table.concat(Parts, " / ")))
	end
	In.AimX, In.AimZ = Target.X, Target.Z
	local DX, DZ = Target.X - P.X, Target.Z - P.Z
	local Weapon = GM:CurrentWeapon()
	-- 높이 있는 적은 원거리, 땅의 적은 근접
	if bAllowSwap and Weapon then
		local bWantRanged = DZ > 160 or Target.Script.Behavior == "Boss" and (self.Frame % 600) < 360
		if (Weapon.Kind == "Ranged") ~= bWantRanged and (self.SwapCooldown or 0) <= 0 then
			In.SwapPressed = true
			self.SwapCooldown = 0.8
		end
	end
	local Reach = (Weapon and Weapon.Kind == "Ranged") and 700 or 90
	if math.abs(DX) > Reach then In.MoveX = U.Sign(DX) elseif math.abs(DX) < 40 and Weapon and Weapon.Kind == "Ranged" then In.MoveX = -U.Sign(DX) end
	if DZ > 140 and Player.entity:IsGrounded() and (self.JumpCooldown or 0) <= 0 then
		In.JumpPressed = true
		self.JumpCooldown = 0.5
	elseif DZ < -150 and Player.entity:IsGrounded() then
		-- 적이 아래: 원웨이면 아래 + 점프로 내려가고, 단단한 단이면 가장자리로 걸어 나간다
		if (self.JumpCooldown or 0) <= 0 then
			In.MoveY = -1
			In.JumpPressed = true
			self.JumpCooldown = 0.6
		end
		-- 가까운 가장자리 (발밑 아래로 땅이 없는 쪽)
		local Dir = 0
		for D = 64, 768, 64 do
			if Physics2D.Raycast(Vector3(P.X + D, 0, P.Z - 40), Vector3(0, -1, 0), 120, "Terrain") == nil then Dir = 1 break end
			if Physics2D.Raycast(Vector3(P.X - D, 0, P.Z - 40), Vector3(0, -1, 0), 120, "Terrain") == nil then Dir = -1 break end
		end
		if Dir ~= 0 then In.MoveX = Dir end
	end
	In.AttackHeld = Best < ((Weapon and Weapon.Kind == "Ranged") and 1400 or 260)
	return true
end

function AutoPilot:GetInput(Player)
	local Dt = Time.DeltaTime
	self.Time = self.Time + Dt
	self.PhaseTime = self.PhaseTime + Dt
	self.Frame = self.Frame + 1
	self.SwapCooldown = math.max(0, (self.SwapCooldown or 0) - Dt)
	self.JumpCooldown = math.max(0, (self.JumpCooldown or 0) - Dt)
	local In = Blank(Player)
	local P = Player.entity:GetWorldPosition()
	if self.Mode == "Test" then
		self:TestStep(Player, In, P)
	elseif self.Mode == "Boss" then
		self:BossStep(Player, In, P)
	else
		self:ExploreStep(Player, In, P)
	end
	return In
end

-- ================================================================ 검증 코스
local function DoorCell(Room)
	-- 방 왼쪽 문 가운데 칸 (전역 셀)
	return { Room.SX * 40 + 1, Room.SY * 24 + 3 }
end

function AutoPilot:TestStep(Player, In, P)
	local GM = self.GM
	local E = Player.entity
	local Start = GM.Layout.Start
	local OX, OZ = Start.Rect[1], Start.Rect[2]
	local FloorZ, BlockZ, PlatZ = OZ + 96 + 80, OZ + 480 + 80, OZ + 320 + 80 -- 캡슐 중심 기대값
	local T = self.PhaseTime
	local Phase = self.Phase
	local function Pos(Text)
		local V = E:GetMovementVelocity()
		return string.format("%s 위치 (%.0f, %.0f) 속도 (%.0f, %.0f) 바닥 %s 점프 남음 %d 이벤트 [%s]", Text, P.X - OX, P.Z - OZ, V.X, V.Z,
			tostring(E:IsGrounded()), E:GetJumpsRemaining(), table.concat(self.Events, ","))
	end

	if Phase == "Settle" then
		if T > 0.8 and E:IsGrounded() then
			self:Note(Pos("시작"))
			self:Expect(math.abs(P.Z - FloorZ) < 12, "시작 방 바닥 위 (캡슐 중심 Z 176)")
			self:SetPhase("Dash")
		end
	elseif Phase == "Dash" then
		In.AimX, In.AimZ = P.X + 600, P.Z
		if self.Step == 0 then
			self.X0 = P.X
			self.Charges0 = Player.DashCharges
			In.DashPressed = true
			self.Step = 1
		elseif T > 0.45 then
			self:Note(Pos("대시 뒤"))
			self:Expect(P.X - self.X0 > 200, string.format("바닥 대시 (조준 방향) %.0fcm ≥ 200", P.X - self.X0))
			self:Expect(Player.DashCharges == self.Charges0 - 1, "대시 충전 칸 1개 소모")
			self:SetPhase("GoBlock")
		end
	elseif Phase == "GoBlock" then
		local TX = OX + 1600
		In.AimX, In.AimZ = P.X + 300, P.Z
		if math.abs(P.X - TX) > 18 then In.MoveX = U.Clamp((TX - P.X) / 120, -1, 1) end
		if (math.abs(P.X - TX) <= 18 and E:IsGrounded() and math.abs(E:GetMovementVelocity().X) < 30) or T > 5 then
			self:SetPhase("DoubleJump")
		end
	elseif Phase == "DoubleJump" then
		In.AimX, In.AimZ = P.X + 300, P.Z
		if self.Step == 0 then
			In.JumpPressed = true
			self.Step = 1
		elseif self.Step == 1 and T > 0.36 then
			In.JumpPressed = true -- 상승 끝 무렵 공중 점프
			self.Step = 2
		end
		if self.Step >= 1 and P.X < OX + 1950 then In.MoveX = 1 end
		if T > 0.7 and self.Step == 2 then
			In.JumpReleased = true
			self.Step = 3
		end
		if T > 1.4 and E:IsGrounded() then
			self:Note(Pos("단 위"))
			local Ev = table.concat(self.Events, ",")
			self:Expect(math.abs(P.Z - BlockZ) < 14, string.format("2단 점프로 높은 단 위 (Z %.0f, 기대 560 — 한 번 점프 높이 %.0fcm로는 못 오름)",
				P.Z - OZ, 1260 * 1260 / (2 * 2941.995)))
			self:Expect(Ev:find("Jumped2") ~= nil, "2단 점프 이벤트 (OnJumped(2))")
			self:SetPhase("GoPlatform")
		elseif T > 3 then
			self:Expect(false, Pos("2단 점프 시간 초과"))
			self:SetPhase("GoPlatform")
		end
	elseif Phase == "GoPlatform" then
		local TX = OX + 1056
		In.AimX, In.AimZ = P.X - 300, P.Z
		if math.abs(P.X - TX) > 20 then In.MoveX = U.Clamp((TX - P.X) / 120, -1, 1) end
		if (math.abs(P.X - TX) <= 20 and E:IsGrounded() and P.Z < OZ + 260 and math.abs(E:GetMovementVelocity().X) < 30) or T > 6 then
			self:SetPhase("JumpPlatform")
		end
	elseif Phase == "JumpPlatform" then
		In.AimX, In.AimZ = P.X - 300, P.Z
		if self.Step == 0 then
			In.JumpPressed = true
			self.Step = 1
		end
		if T > 1.1 and E:IsGrounded() then
			self:Note(Pos("발판 위"))
			self:Expect(math.abs(P.Z - PlatZ) < 14, "원웨이 발판 위로 (아래에서 뛰어 통과, Z 400)")
			self:SetPhase("DropDown")
		end
	elseif Phase == "DropDown" then
		In.AimX, In.AimZ = P.X - 300, P.Z
		if T < 0.25 then In.MoveY = -1 end
		if self.Step == 0 and T > 0.1 then
			In.JumpPressed = true -- 아래 + 점프 = 내려가기
			self.Step = 1
		end
		if T > 1.0 and E:IsGrounded() then
			self:Note(Pos("내려간 뒤"))
			self:Expect(math.abs(P.Z - FloorZ) < 14, "S+Space 원웨이 내려가기 → 바닥 (Z 176)")
			self:SetPhase("ToArena")
		end
	elseif Phase == "ToArena" then
		In.MoveX = 1
		In.AimX, In.AimZ = P.X + 300, P.Z
		local Room = GM.CurrentRoom
		if Room and Room ~= Start and Room.State == "Active" then
			local Door = DoorCell(Room)
			self.Arena = Room
			self:Note(Pos("전투 방 입장"))
			self:Expect(GM.Terrain:GetTile(Door[1], Door[2]) ~= nil, string.format("입장 시 문 잠김 (문 칸 (%d, %d)에 돌)", Door[1], Door[2]))
			self:SetPhase("Fight")
		elseif T > 10 then
			self:Expect(false, Pos("전투 방 입장 시간 초과"))
			self:SetPhase("Report")
		end
	elseif Phase == "Fight" then
		local Room = self.Arena
		if Room.State == "Cleared" then
			local Door = DoorCell(Room)
			self:Note(Pos("전투 끝"))
			self:Expect(true, string.format("적 전멸 (처치 %d)", GM.Kills))
			self:Expect(GM.Terrain:GetTile(Door[1], Door[2]) == nil, "전멸 → 문 열림 (문 칸 빔)")
			self:SetPhase("Loot")
		elseif T > 30 then
			self:Expect(false, Pos("전투 시간 초과"))
			self:SetPhase("Report")
		else
			self:Fight(Player, In, P, false)
		end
	elseif Phase == "Loot" then
		-- 보상 코인을 줍는 동안 잠깐 서성인다 (스크린샷 장면)
		In.MoveX = (math.floor(T / 0.8) % 2 == 0) and 0.6 or -0.6
		if T > 2.5 then
			self:Expect(GM.Gold > 0, string.format("코인 줍기 (%d)", GM.Gold))
			-- 일시정지 메뉴 (멈춘 뒤에는 플레이어 입력이 불리지 않으므로 같은 프레임에 확인·보고 — 마지막 스크린샷은 메뉴 화면)
			GM:SetPaused(true)
			self:Expect(GM.Hud ~= nil and GM.Hud:W("PauseScreen").Visible == true, "일시정지 메뉴 표시")
			self:Report()
		end
	elseif Phase == "Report" then
		self:Report()
	end
end

-- ================================================================ 보스전
function AutoPilot:BossStep(Player, In, P)
	local GM = self.GM
	local Room = GM.CurrentRoom
	if self.Phase == "Settle" then
		if self.PhaseTime > 0.5 then self:SetPhase("ToBoss") end
	elseif self.Phase == "ToBoss" then
		In.MoveX = 1
		In.AimX, In.AimZ = P.X + 300, P.Z + 100
		if Room and Room.Kind == "Boss" and Room.State == "Active" then
			self:SetPhase("Fight")
		end
	elseif self.Phase == "Fight" then
		self:Fight(Player, In, P, true)
		-- 가끔 조준 반대쪽으로 대시해 탄을 피한다
		if self.PhaseTime % 2.7 < Time.DeltaTime and Player.DashCharges > 0 then
			In.DashPressed = true
			In.AimX = P.X + (P.X < (Room.Rect[1] + Room.Rect[3]) * 0.5 and 400 or -400)
			In.AimZ = P.Z + 150
		end
		local Boss = GM.Boss
		if (Boss and self.PhaseTime > 40) or (Boss == nil and GM.bVictory) or self.PhaseTime > 70 then
			self:SetPhase("Check")
		end
	elseif self.Phase == "Check" then
		self:CheckBoss()
		self:SetPhase("Report")
	elseif self.Phase == "Report" then
		self:Report()
	end
end

-- ================================================================ 탐험
function AutoPilot:ExploreStep(Player, In, P)
	local GM = self.GM
	local Room = GM.CurrentRoom
	if Room == nil then return end
	local E = Player.entity
	In.AimX, In.AimZ = P.X + 300, P.Z
	local function Drop()
		-- 원웨이 위에서 아래 + 점프 = 내려가기
		if E:IsGrounded() and (self.JumpCooldown or 0) <= 0 then
			In.MoveY = -1
			In.JumpPressed = true
			self.JumpCooldown = 0.6
		end
	end
	if self.Phase == "Settle" then
		if self.PhaseTime > 0.5 then self:SetPhase("Walk") end
	elseif self.Phase == "Walk" then
		if Room.State == "Active" then
			self:SetPhase("Fight")
			return
		end
		local MidX = (Room.Rect[1] + Room.Rect[3]) * 0.5
		if Room.State == "Idle" and Room.Kind ~= "Start" then
			-- 새 전투 방: 가운데·아래로 들어가 잠기게 한다
			if math.abs(P.X - MidX) > 200 then In.MoveX = U.Sign(MidX - P.X) end
			if P.Z > Room.Rect[2] + 6 * U.Cell then Drop() end
		else
			-- 아직 안 가 본 이웃: 왼쪽/오른쪽 문(걸어서) → 아래 문(구멍 원웨이로 내려감)
			local Target = nil
			for _, Side in ipairs({ "R", "L", "D" }) do
				if Room.Doors[Side] then
					local DX = (Side == "R" and 1) or (Side == "L" and -1) or 0
					local DY = (Side == "D" and -1) or 0
					local N = GM.Layout.ByKey[(Room.SY + DY) * 100 + Room.SX + DX]
					if N and not N.Visited then Target = Side break end
				end
			end
			if Target == nil then
				if self.PhaseTime > 4 then self:SetPhase("Report") end
				return
			end
			if Target == "D" then
				if math.abs(P.X - MidX) > 40 then
					In.MoveX = U.Clamp((MidX - P.X) / 150, -1, 1)
				else
					Drop()
				end
				if P.Z > Room.Rect[2] + 3 * U.Cell and math.abs(P.X - MidX) <= 40 then Drop() end
			else
				local Dir = Target == "R" and 1 or -1
				In.MoveX = Dir
				In.AimX = P.X + Dir * 300
				-- 높은 곳에 있으면 내려가고, 낮은 턱은 점프로
				if P.Z > Room.Rect[2] + 4 * U.Cell then Drop() end
				if Physics2D.Raycast(Vector3(P.X, 0, P.Z - 40), Vector3(Dir, 0, 0), 80, "Terrain") and E:IsGrounded() then
					In.JumpPressed = true
				end
			end
		end
		if self.PhaseTime > 25 then self:SetPhase("Report") end
	elseif self.Phase == "Fight" then
		if Room.State == "Cleared" then
			self:Expect(true, string.format("생성된 층 전투 방 정리 (%s, 처치 %d)", Room.Template.Name, GM.Kills))
			self:SetPhase("Loot")
		elseif self.PhaseTime > 50 then
			self:Expect(false, "탐험 전투 시간 초과")
			self:SetPhase("Report")
		else
			self:Fight(Player, In, P, true)
		end
	elseif self.Phase == "Loot" then
		In.MoveX = (math.floor(self.PhaseTime / 0.8) % 2 == 0) and 0.5 or -0.5
		if self.PhaseTime > 2 then self:SetPhase("Report") end
	elseif self.Phase == "Report" then
		self:Report()
	end
end

return AutoPilot
