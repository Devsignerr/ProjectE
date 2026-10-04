-- Tests/Character2D 확인 스크립트 (2D 캐릭터 이동기 — Physics/CharacterMovement2D.h). 실행 위치 Both.
-- 이 캐릭터를 조종하는 쪽(entity:IsLocallyOwned — Standalone/서버 소유는 서버, 플레이어 폰은 그 클라이언트)에서만 입력을 넣는다.
-- 스크립트는 이동을 계산하지 않고 입력(AddMovementInput/Jump/StopJumping/Dash/DropDown)만 넣는다.
--   AutoInput: 프레임 시간표대로 달리기 → 점프 → 2단 점프(원웨이 발판 착지) → 내려가기 → 오르막 → 대시 → 가파른 경사에 막힘 → 점프,
--              단계마다 위치를 [Character2D] 로그로 남기고 기대와 다르면 "확인 실패"
--   수동: Move(A/D), Jump(Space — 누를 때 Jump, 뗄 때 StopJumping), Dodge(대시), S + Jump = 원웨이 내려가기
-- 몸 방향(좌우 반전)은 이동기가 정하지 않는다 — 스프라이트가 있으면 이동 속도 X 부호로 SetSpriteFlip을 부른다.
local T = {
	Properties = {
		AutoInput = true,
		Label = "Hero",
	},
}

local function Fmt(V)
	return string.format("(%.1f, %.1f)", V.X, V.Z)
end

function T:OnStart()
	self.Frame = 0
	self.Failures = 0
	self.Events = {}
end

function T:Note(Text)
	Log.Info(string.format("[Character2D] %s f%d: %s", self.Properties.Label, self.Frame, Text))
end

function T:Expect(bOk, What)
	if bOk then
		self:Note("확인 " .. What)
	else
		self.Failures = self.Failures + 1
		self:Note("확인 실패 " .. What)
	end
end

function T:OnJumped(n)
	table.insert(self.Events, "J" .. n)
end

function T:OnLanded()
	table.insert(self.Events, "L")
end

function T:OnDashStarted()
	table.insert(self.Events, "D")
end

function T:Report(Stage)
	local e = self.entity
	local P = e:GetWorldPosition()
	local V = e:GetMovementVelocity()
	self:Note(string.format("%s 위치 %s 속도 %s 바닥 %s 남은 점프 %d 대시 %d 이벤트 [%s]", Stage, Fmt(P), Fmt(V), tostring(e:IsGrounded()),
		e:GetJumpsRemaining(), e:GetDashesRemaining(), table.concat(self.Events, ",")))
	return P
end

function T:Manual()
	local e = self.entity
	local Move = Input.GetAction("Move")
	if Move ~= nil and Move.X ~= nil then
		e:AddMovementInput(Vector3(Move.X, 0, 0))
	end
	if Input.WasActionPressed("Jump") then
		if Input.IsKeyDown("S") then
			e:DropDown()
		else
			e:Jump()
		end
	end
	if Input.WasActionReleased("Jump") then
		e:StopJumping()
	end
	if Input.WasActionPressed("Dodge") then
		e:Dash(Vector3(Move ~= nil and Move.X or 0, 0, 0))
	end
end

function T:Auto()
	local e = self.entity
	local F = self.Frame
	if F >= 20 and F < 58 then e:AddMovementInput(Vector3(1, 0, 0)) end
	if F == 40 then e:Jump() end
	if F == 58 then e:Jump() end -- 2단 점프 (상승 중)
	if F == 70 then e:StopJumping() end
	if F == 120 then
		local P = self:Report("발판")
		self:Expect(math.abs(P.Z - 360) < 3 and e:IsGrounded(), "원웨이 발판 위 (중심 Z 360)")
	end
	if F == 130 then e:DropDown() end
	if F == 170 then
		local P = self:Report("내려감")
		self:Expect(math.abs(P.Z - 60) < 3 and e:IsGrounded(), "내려가기 → 바닥 (중심 Z 60)")
	end
	if F >= 175 and F < 345 then e:AddMovementInput(Vector3(1, 0, 0)) end
	if F == 200 then e:Dash(Vector3(1, 0, 0)) end
	if F == 300 then
		local P = self:Report("오르막 뒤")
		self:Expect(P.X > 1200 and math.abs(P.Z - 260) < 4 and e:IsGrounded(), "45도 타일 오르막을 올라 고원 (중심 Z 260)")
	end
	if F == 345 then
		local P = self:Report("가파른 경사")
		self:Expect(P.X < 1600 and P.X > 1500 and math.abs(P.Z - 260) < 6, "65도 경사에 막힘 (X 1600 앞)")
		e:Jump()
	end
	if F == 365 then e:StopJumping() end
	if F == 395 then
		self:Report("끝")
		local Events = table.concat(self.Events, ",")
		self:Expect(Events:find("J1,J2") ~= nil and Events:find("D") ~= nil, "이벤트 (J1,J2,D,L)")
		self:Note(string.format("결과: 실패 %d건", self.Failures))
	end
end

function T:OnUpdate(dt)
	self.Frame = self.Frame + 1
	if not self.entity:IsLocallyOwned() then
		-- 서버가 시뮬레이션하는 원격 플레이어 폰: 서버가 본 위치 (클라이언트 예측 로그와 비교)
		if Net.IsServer() and self.Frame % 100 == 0 then
			self:Note(string.format("서버가 본 원격 폰 위치 %s 바닥 %s", Fmt(self.entity:GetWorldPosition()), tostring(self.entity:IsGrounded())))
		end
		return
	end
	if self.Properties.AutoInput then
		self:Auto()
	else
		self:Manual()
	end
end

return T
