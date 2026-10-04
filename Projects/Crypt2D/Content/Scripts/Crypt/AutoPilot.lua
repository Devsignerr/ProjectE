-- Crypt2D 자동 조종 (자동 검증 — GameManager.Properties.AutoPlay). 플레이어 입력 표를 대신 채우고(Player:GatherInput),
-- 실제 레벨에서 기능이 되는지 단계마다 확인해 "[Crypt2D] 자동 …" 로그와 마지막 "[Crypt2D] 결과: 실패 N건"을 남긴다.
--   Test    : 검증 코스(Dungeon.TestLayout — 시작 방 + 해골 하나 방 + 보물 + 상점). 바닥 대시 → 높은 단(두 번 점프로만) → 원웨이 발판 위로
--             (아래에서 통과) → S+Space 내려가기 → 오른쪽 방 입장 = 문 잠김 → 싸워서 전멸 = 문 열림 → 코인 → 보물 방 지나 상점에서 구매
--             (코인 차감·효과) → 일시정지(시간 배율 0)
--   Boss    : 보스 층. 오른쪽 보스 방으로 가서 싸운다 (플레이어 무적). 패턴 3종 이상·보스 체력 감소 확인
--   Explore : 생성된 층. 왼쪽/오른쪽/아래/위 문으로 이웃 방에 들어가 싸운다 (스크린샷용, 확인은 방 하나 정리)
--   Death   : 검증 코스에서 싸우지 않고 해골에게 맞아 죽는다 (체력을 낮춰 시작) → 사망 화면·결과 글·시간 배율 0·커서·기록 확인
--   Climb   : Dungeon.ClimbLayout(시작 + 위로 방 세 개) — 방마다 템플릿 UpPath(생성기가 검사한 발판 경로)를 따라 위 문으로 오른다.
--             꼭대기에서 공중 일시정지 = 떨어지지 않음(시간 배율 0) 확인
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
	if self.Mode == "Death" then
		if GM.bResultShown then self:CheckDeath() end
		return
	end
	if self.Mode == "Boss" then
		self:CheckBoss()
	else
		self:Expect(false, "플레이어 사망으로 판이 끝남")
	end
	self:Report()
end

-- 사망 화면 (GM:ShowResult — 결과 글, 뒤 세계 정지, 커서 보임·게임 입력도 받음)
function AutoPilot:CheckDeath()
	local GM = self.GM
	local Hud = GM.Hud
	self:Expect(GM.bWin ~= true, "패배로 판이 끝남")
	self:Expect(Hud ~= nil and Hud:W("DeathScreen").Visible == true, "사망 화면 표시")
	local Text = Hud and Hud:W("DeathResult").Text or ""
	self:Expect(Text:find("도달 층") ~= nil and Text:find("처치") ~= nil, "결과 글 (" .. Text:gsub("\n", " / ") .. ")")
	self:Expect(Game.GetTimeScale() == 0, "결과 화면 뒤 세계 정지 (시간 배율 0)")
	self:Expect(Game.GetInputMode() == "GameAndUI" and Game.IsCursorVisible() and not Game.IsMouseLocked(), "커서 보임 + 다시 시작 입력")
	self:Expect(SaveGame.Load("Crypt2D") ~= nil, "기록 저장")
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
	elseif self.Mode == "Death" then
		self:DeathStep(Player, In, P)
	elseif self.Mode == "Climb" then
		self:ClimbStep(Player, In, P)
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
			self:SetPhase("ToShop")
		end
	elseif Phase == "ToShop" then
		-- 보물 방을 지나 오른쪽 상점으로 (적 없음 — 들어가면 정리된 방)
		In.MoveX = 1
		In.AimX, In.AimZ = P.X + 300, P.Z
		local Room = GM.CurrentRoom
		if Room and Room.Kind == "Shop" and P.X > Room.Rect[1] + 6 * U.Cell then
			self:Note(string.format("상점 입장: 물건 [%s], 코인 %d", table.concat(Room.ShopOffers or {}, ", "), GM.Gold))
			self:Expect(Room.State == "Cleared" and #(Room.ShopOffers or {}) == 3, "상점 방 (적 없음, 진열대 3자리)")
			self:SetPhase("Buy")
		elseif T > 20 then
			self:Expect(false, Pos("상점 도착 시간 초과"))
			self:SetPhase("Report")
		end
	elseif Phase == "Buy" then
		-- 살 수 있는 가장 싼 물건 앞으로 가서 F
		if self.Offer == nil then
			for _, I in ipairs(GM.Interactables) do
				if I.Kind == "Shop" and not I.bUsed and I.Price <= GM.Gold and (self.Offer == nil or I.Price < self.Offer.Price) then self.Offer = I end
			end
			if self.Offer == nil then
				self:Expect(false, string.format("살 수 있는 물건 없음 (코인 %d)", GM.Gold))
				self:SetPhase("Pause")
				return
			end
			self.GoldBefore, self.HealthBefore, self.MaxBefore = GM.Gold, Player.Health, Player.MaxHealth
		end
		local Offer = self.Offer
		In.AimX, In.AimZ = Offer.X, Offer.Z
		if self.Step == 0 and math.abs(P.X - Offer.X) > 24 then
			In.MoveX = U.Clamp((Offer.X - P.X) / 100, -1, 1)
		elseif self.Step == 0 and GM.Focus == Offer then
			In.InteractPressed = true
			self.Step = 1
		elseif self.Step == 1 then
			local Item = GM.Data.ShopItem(Offer.ItemId)
			local bEffect
			if Item.Kind == "Heal" then bEffect = Player.Health == math.min(Player.MaxHealth, self.HealthBefore + Item.Amount)
			elseif Item.Kind == "MaxHealth" then bEffect = Player.MaxHealth == self.MaxBefore + Item.Amount
			else bEffect = GM.Weapons[GM.WeaponSlot] == Item.Weapon end
			self:Expect(Offer.bUsed == true and GM.Gold == self.GoldBefore - Item.Price,
				string.format("상점 구매 %s: 코인 %d → %d (가격 %d)", Offer.ItemId, self.GoldBefore, GM.Gold, Item.Price))
			self:Expect(bEffect, "구매 효과 (" .. Item.Kind .. ")")
			self:SetPhase("Pause")
		end
		if self.Phase == "Buy" and T > 8 then
			self:Expect(false, Pos("구매 시간 초과"))
			self:SetPhase("Pause")
		end
	elseif Phase == "Pause" then
		-- 일시정지 메뉴 (멈춘 뒤에는 플레이어 입력이 불리지 않으므로 같은 프레임에 확인·보고 — 마지막 스크린샷은 메뉴 화면)
		GM:SetPaused(true)
		self:Expect(GM.Hud ~= nil and GM.Hud:W("PauseScreen").Visible == true, "일시정지 메뉴 표시")
		self:Expect(Game.GetTimeScale() == 0 and Game.IsCursorVisible(), "일시정지 = 시간 배율 0 + 커서 보임")
		self:Report()
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
		if (Boss and self.PhaseTime > 55) or (Boss == nil and GM.bVictory) or self.PhaseTime > 70 then
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
			for _, Side in ipairs({ "R", "L", "D", "U" }) do
				if Room.Doors[Side] then
					local DX = (Side == "R" and 1) or (Side == "L" and -1) or 0
					local DY = (Side == "D" and -1) or (Side == "U" and 1) or 0
					local N = GM.Layout.ByKey[(Room.SY + DY) * 100 + Room.SX + DX]
					if N and not N.Visited then Target = Side break end
				end
			end
			if Target == nil then
				if self.PhaseTime > 4 then self:SetPhase("Report") end
				return
			end
			if Target == "U" then
				self:ClimbRoom(Player, In, P, Room) -- 템플릿 UpPath를 따라 위 문으로
			elseif Target == "D" then
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

-- ================================================================ 위 문 오르기 (템플릿 UpPath — Crypt2DRooms.FindUpPath가 생성 때 검사한 면 목록)
-- 면 k에 서 있으면 면 k+1로: 오르기는 가장자리(단단한 단·틈) 또는 그 아래(원웨이 — 뚫고 오름)에서 뛰고, 높으면 정점에서 한 번 더,
-- 공중에서는 다음 면 가운데로. 마지막 면(착지 발판) 위에서는 문 가운데에서 두 번 뛰어 위 방으로 (위 방 원웨이 바닥에 착지)
local FeetOffset = 80 -- 캡슐 중심 → 발 (높이 160의 절반)

function AutoPilot:ClimbRoom(Player, In, P, Room)
	local Path = Room.Template.UpPath
	if Path == nil then return false end
	local E = Player.entity
	local Cell = U.Cell
	local OX, OZ = Room.Rect[1], Room.Rect[2]
	local Feet = P.Z - FeetOffset
	if self.ClimbRoomRef ~= Room then
		self.ClimbRoomRef, self.ClimbIndex = Room, 1 -- 점프 횟수는 착지할 때만 (아래 방에서 뛰어 올라오는 중에도 이어서 센다)
		self.ClimbJumps = self.ClimbJumps or 0
		self.ClimbLanded = E:IsGrounded() -- 아래 방에서 위 문으로 들어오는 중이면 이 방에 처음 설 때까지 아래 문 원웨이가 목표
	end
	local function Range(S) return OX + S[1] * Cell, OX + (S[2] + 1) * Cell end
	-- 면 높이: '#' 윗면 타일은 아래 절반 다각형(Y + 0.5칸), '=' 원웨이는 Full(Y + 1칸) — Dungeon.lua 자동 타일·FindUpPath와 같다
	local function Top(S) return OZ + (S[3] + (S[4] and 0.5 or 1.0)) * Cell end
	local bGrounded = E:IsGrounded()
	if bGrounded then
		self.ClimbJumps = 0
		self.ClimbLanded = true
		-- 지금 선 면 (경로 안에서 발 높이·가로 범위가 맞는 것 중 가장 높은 번호)
		for I = #Path, 1, -1 do
			local X0, X1 = Range(Path[I])
			if math.abs(Feet - Top(Path[I])) < 14 and P.X > X0 - 40 and P.X < X1 + 40 then
				if I ~= self.ClimbIndex then self:Note(string.format("오르기 면 %d/%d (방 %d, %d)", I, #Path, Room.SX, Room.SY)) end
				self.ClimbIndex = I
				break
			end
		end
	end
	local A = Path[self.ClimbIndex]
	local AX0, AX1 = Range(A)
	if self.ClimbIndex >= #Path then
		-- 착지 발판: 위 문 가운데(18~21열 → 20열 경계)에서 두 번 뛴다
		local DoorX = OX + 20 * Cell
		In.MoveX = U.Clamp((DoorX - P.X) / 60, -1, 1)
		if bGrounded and math.abs(P.X - DoorX) < 20 then
			In.JumpPressed = true
			self.ClimbJumps = 1
		elseif not bGrounded and self.ClimbJumps == 1 and E:GetMovementVelocity().Z < 150 then
			In.JumpPressed = true
			self.ClimbJumps = 2
		end
		return true
	end
	local B = Path[self.ClimbIndex + 1]
	local BX0, BX1 = Range(B)
	local BMid = (BX0 + BX1) * 0.5
	local Rise = (B[3] + (B[4] and 0.5 or 1.0)) - (A[3] + (A[4] and 0.5 or 1.0))
	if not bGrounded then
		-- 공중: 다음 면 가운데로, 정점 근처에서 아직 아래면 한 번 더. 아래 방에서 위 문으로 올라오는 중이면(발이 이 방 바닥 아래)
		-- 목표는 이 방 바닥의 원웨이(아래 문 18~21열) — 문 가운데로
		local bFromBelow = self.ClimbIndex == 1 and not self.ClimbLanded
		local GoalX = bFromBelow and OX + 20 * Cell or BMid
		if not bFromBelow and B[4] and Feet < Top(B) + 8 then
			-- 단단한 단: 윗면보다 높아질 때까지 옆에 머문다 (아래로 파고들면 밑면에 머리를 박는다)
			GoalX = P.X > BMid and BX1 + 36 or BX0 - 36
		end
		local GoalTop = bFromBelow and OZ + 2 * Cell or Top(B) -- 아래 문 원웨이(1행, Full) 윗면
		In.MoveX = U.Clamp((GoalX - P.X) / 80, -1, 1)
		-- 정점에서도 목표 아래이거나, 단단한 단 옆에서 아직 그 위로 들어가지 못했으면 한 번 더
		local bBesideSolid = not bFromBelow and B[4] and (P.X > BX1 or P.X < BX0)
		if self.ClimbJumps == 1 and E:GetMovementVelocity().Z < 150 and (Feet < GoalTop + 20 or bBesideSolid) then
			In.JumpPressed = true
			self.ClimbJumps = 2
		end
		return true
	end
	if Rise <= 0 then
		-- 내려가기: 아래 면 쪽으로 걸어 나가고, 원웨이 위에서 바로 아래면 내려가기
		In.MoveX = U.Clamp((BMid - P.X) / 80, -1, 1)
		if not A[4] and P.X >= BX0 and P.X <= BX1 and (self.JumpCooldown or 0) <= 0 then
			In.MoveY = -1
			In.JumpPressed = true
			self.JumpCooldown = 0.6
		end
		return true
	end
	-- 오르기: 뛸 자리 (원웨이가 머리 위에 겹치면 겹친 곳 가운데, 아니면 다음 면 쪽 가장자리 반 칸 안)
	local TakeoffX
	local Overlap0, Overlap1 = math.max(AX0, BX0), math.min(AX1, BX1)
	if not B[4] and Overlap1 - Overlap0 > Cell then
		TakeoffX = (Overlap0 + Overlap1) * 0.5
	elseif BMid > (AX0 + AX1) * 0.5 then
		TakeoffX = U.Clamp(math.min(AX1, BX0) - Cell * 0.5, AX0 + Cell * 0.5, AX1 - Cell * 0.5)
	else
		TakeoffX = U.Clamp(math.max(AX0, BX1) + Cell * 0.5, AX0 + Cell * 0.5, AX1 - Cell * 0.5)
	end
	if math.abs(P.X - TakeoffX) > 18 then
		In.MoveX = U.Clamp((TakeoffX - P.X) / 80, -1, 1)
	elseif (self.JumpCooldown or 0) <= 0 then
		In.JumpPressed = true
		In.MoveX = U.Sign(BMid - P.X)
		self.ClimbJumps = 1
		self.JumpCooldown = 0.3
	end
	return true
end

-- ================================================================ 사망 화면 검증: 체력을 낮추고 싸우지 않고 해골 쪽으로 걸어간다
function AutoPilot:DeathStep(Player, In, P)
	local GM = self.GM
	if self.Phase == "Settle" then
		if self.PhaseTime > 0.5 then
			Player.Health = 12
			GM:RefreshHud()
			self:SetPhase("Walk")
		end
	elseif self.Phase == "Walk" then
		local Room = GM.CurrentRoom
		local Target = nil
		if Room and Room.State == "Active" then
			for _, E in ipairs(Room.Enemies) do
				if E:IsValid() then Target = E:GetWorldPosition() break end
			end
		end
		if Target then
			In.MoveX = math.abs(Target.X - P.X) > 30 and U.Sign(Target.X - P.X) or 0
			In.AimX, In.AimZ = Target.X, Target.Z
		else
			In.MoveX = 1
			In.AimX, In.AimZ = P.X + 300, P.Z
		end
		if self.PhaseTime > 40 then
			self:Expect(false, "사망 시간 초과")
			self:Report()
		end
	end
end

-- ================================================================ 위 문 경로 검증 (ClimbLayout)
function AutoPilot:ClimbStep(Player, In, P)
	local GM = self.GM
	local Room = GM.CurrentRoom
	if Room == nil then return end
	In.AimX, In.AimZ = P.X + 300, P.Z + 200
	if self.Phase == "Settle" then
		if self.PhaseTime > 0.5 then
			self.StartRoom = Room
			self.TopRoom = Room
			self:SetPhase("Climb")
		end
	elseif self.Phase == "Climb" then
		-- 방 판정은 캡슐 중심 기준이라 구멍을 지나는 순간 아래 방으로 잠깐 돌아갈 수 있다 — 새로 오른 방만 센다
		if Room.SY > self.TopRoom.SY then
			self:Expect(Room.SY == self.TopRoom.SY + 1 and Room.SX == self.TopRoom.SX,
				string.format("위 문으로 %s → %s (%d, %d)", self.TopRoom.Template.Name, Room.Template.Name, Room.SX, Room.SY))
			self.TopRoom = Room
			self.PhaseTime = 0
		end
		if Room.Doors.U == nil then
			if Player.entity:IsGrounded() then
				self:Expect(Room.SY - self.StartRoom.SY == 3, string.format("꼭대기 방 도착 (%s, 오른 방 %d)", Room.Template.Name, Room.SY - self.StartRoom.SY))
				self:SetPhase("AirPause")
			end
			return
		end
		self:ClimbRoom(Player, In, P, Room)
		-- 3초마다 상태 (막히면 원인을 볼 수 있게)
		self.ClimbLog = (self.ClimbLog or 0) + Time.DeltaTime
		if self.ClimbLog > 3 then
			self.ClimbLog = 0
			local V = Player.entity:GetMovementVelocity()
			self:Note(string.format("오르는 중 %s 면 %d: 칸 (%.1f, %.1f) 속도 (%.0f, %.0f) 바닥 %s", Room.Template.Name, self.ClimbIndex or 0,
				(P.X - Room.Rect[1]) / U.Cell, (P.Z - 80 - Room.Rect[2]) / U.Cell, V.X, V.Z, tostring(Player.entity:IsGrounded())))
		end
		if self.PhaseTime > 25 then
			self:Expect(false, string.format("오르기 시간 초과: %s 면 %d", Room.Template.Name, self.ClimbIndex or 0))
			self:SetPhase("Report")
		end
	elseif self.Phase == "AirPause" then
		-- 공중에서 일시정지 → 실제 0.5초 동안 위치·속도가 그대로 (예전: 스크립트만 멈춰 이동기가 떨어뜨렸다)
		if self.Step == 0 then
			In.JumpPressed = true
			self.Step = 1
		elseif self.Step == 1 and self.PhaseTime > 0.25 then
			-- 배율은 다음 틱부터 — 멈춘 뒤(실제 0.1초) 자리를 재고, 다시 실제 0.5초 뒤 비교 (실제 시간 타이머)
			local E = Player.entity
			GM:SetPaused(true)
			Timer.After(0.1, function()
				self.FrozenAt = E:GetWorldPosition()
				self.FrozenVel = E:GetMovementVelocity()
				Timer.After(0.5, function()
					local Now = E:GetWorldPosition()
					local Vel = E:GetMovementVelocity()
					self:Expect(not E:IsGrounded() and Now.Z == self.FrozenAt.Z and Vel.Z == self.FrozenVel.Z,
						string.format("공중 일시정지: 높이 %.1f → %.1f, 속도 %.1f → %.1f (떨어지지 않음)", self.FrozenAt.Z, Now.Z, self.FrozenVel.Z, Vel.Z))
					GM:SetPaused(false)
					self:Report()
				end, { Unscaled = true })
			end, { Unscaled = true })
			self.Step = 2
		end
	elseif self.Phase == "Report" then
		self:Report()
	end
end

return AutoPilot
