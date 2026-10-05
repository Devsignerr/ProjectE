-- FarmBie 플레이어 (Prefabs/FarmBie/Player.eprefab — 캡슐 이동기 + Visual > Body 농부 도트 스프라이트(전체 빌보드)·Shadow).
--   조작 (입력 액션, Config/Input.json): Move 이동, Dodge 구르기, Interact 상호작용, UseTool 도구/공격, Slot1~9·SlotScroll 도구 칸, Build 건설, Pause 일시정지
--   이동은 이동기가 한다 (스크립트는 AddMovementInput만). 방향 = 마지막 입력의 주된 축 → 아래/위/옆 3방향 플립북, 왼쪽은 좌우 반전.
--   카메라: OnLateUpdate(물리 뒤)에 지정 카메라를 발 위 초점 + 시작 오프셋으로 부드럽게 따라 놓는다 (회전은 씬 값 — 거의 수직 탑뷰).
--   이동기가 루트를 이동 방향으로 돌리므로 Visual 회전을 매 프레임 상쇄한다. 자동 검증이면 입력을 FarmAutoPilot.lua가 채운다.
local AutoPilot = Script.Require("Scripts/FarmBie/FarmAutoPilot.lua")

local FarmPlayer = {
	Properties = {
		Camera         = "Camera",
		CameraDistance = 2400.0, -- 초점에서 카메라까지 (cm)
		FocusHeight    = 70.0,
		CameraLag      = 8.0,    -- 1/초
		MinX = -100000.0, MaxX = 100000.0, MinY = -100000.0, MaxY = 100000.0, -- 카메라 초점 범위
		DodgeSpeed     = 1200.0,
		DodgeTime      = 0.22,
		DodgeCooldown  = 0.6,
	},
}

local Sprites = "Sprites/FarmBie/"
-- 방향 이름 → (월드 방향, 플립북 방향, 옆모습 반전). 화면 아래 = +Y
local Dirs = {
	Down  = { V = Vector3(0, 1, 0),  Anim = "Down", Flip = false },
	Up    = { V = Vector3(0, -1, 0), Anim = "Up",   Flip = false },
	Right = { V = Vector3(1, 0, 0),  Anim = "Side", Flip = false },
	Left  = { V = Vector3(-1, 0, 0), Anim = "Side", Flip = true },
}

local function Flat(V) return Vector3(V.X, V.Y, 0) end

local function FacingFromMove(Move, Current)
	if math.abs(Move.X) < 0.1 and math.abs(Move.Y) < 0.1 then
		return Current
	end
	if math.abs(Move.X) >= math.abs(Move.Y) * 0.8 then
		return Move.X > 0 and "Right" or "Left"
	end
	return Move.Y > 0 and "Down" or "Up"
end

function FarmPlayer:OnStart()
	self.GM = Scene.Find("FarmGame"):GetScript()
	self.Visual = self.entity:FindChild("Visual")
	self.Body = self.Visual:FindChild("Body")
	self.Sprite = self.Body:GetComponent("SpriteComponent")
	self.Facing = "Down"
	self.Anim = ""
	self.DodgeTimer, self.DodgeCooldown = 0.0, 0.0
	self.Start = self.entity:GetWorldPosition()
	self.LastPos = self.Start
	self.Stats = { Distance = 0, Dodges = 0 }
	if self.GM.Properties.AutoPlay ~= "" then
		self.Pilot = AutoPilot.New(self.GM.Properties.AutoPlay, self, self.GM)
	end
	self.Camera = Scene.Find(self.Properties.Camera)
	if self.Camera then
		self.CamOffset = self.Camera:GetForward() * -self.Properties.CameraDistance
		self.CamPos = self:CameraTarget()
		self.Camera:SetPosition(self.CamPos)
	end
	self:UpdateAnimation(Vector3(0, 0, 0))
end

-- ---- 입력 (사람 또는 자동 조종)
function FarmPlayer:GatherInput()
	if self.Pilot then
		return self.Pilot:Step(Time.GetUnscaledDelta())
	end
	local MX, MY = Input.GetAction("Move")
	return { Move = Vector3(MX, -MY, 0), Dodge = Input.WasActionPressed("Dodge"), Interact = Input.WasActionPressed("Interact"),
	         UseTool = Input.WasActionPressed("UseTool"), Pause = Input.WasActionPressed("Pause") }
end

function FarmPlayer:OnUpdate(Dt)
	local E = self.entity
	local Pos = E:GetWorldPosition()
	local Step = Flat(Pos - self.LastPos)
	if Step:Length() < 500 then self.Stats.Distance = self.Stats.Distance + Step:Length() end
	self.LastPos = Pos

	local In = self:GatherInput()
	local Move = In.Move
	if Move:Length() > 1 then Move = Move:Normalized() end
	self.DodgeCooldown = math.max(0.0, self.DodgeCooldown - Dt)
	if self.DodgeTimer > 0 then
		self.DodgeTimer = self.DodgeTimer - Dt
	elseif In.Dodge and self.DodgeCooldown <= 0 then
		local Dir = Move:Length() > 0.1 and Move:Normalized() or Dirs[self.Facing].V
		self.Facing = FacingFromMove(Dir, self.Facing)
		E:AddKnockback(Dir * self.Properties.DodgeSpeed, self.Properties.DodgeTime)
		self.DodgeTimer, self.DodgeCooldown = self.Properties.DodgeTime, self.Properties.DodgeCooldown
		self.Stats.Dodges = self.Stats.Dodges + 1
	elseif Move:Length() > 0.05 then
		self.Facing = FacingFromMove(Move, self.Facing)
		E:AddMovementInput(Move)
	end
	self:UpdateAnimation(E:GetMovementVelocity())
end

function FarmPlayer:UpdateAnimation(Velocity)
	local Dd = Dirs[self.Facing]
	local Want
	if Velocity.X * Velocity.X + Velocity.Y * Velocity.Y > 40 * 40 then
		Want = "Farmer_Walk" .. Dd.Anim
	else
		Want = "Farmer_Idle" .. Dd.Anim
	end
	if Want ~= self.Anim then
		self.Anim = Want
		self.Body:PlayFlipbook(Sprites .. Want .. ".eflipbook")
	end
	self.Body:SetSpriteFlip(Dd.Flip, false)
end

-- 바라보는 방향 (월드 단위 벡터)
function FarmPlayer:GetFacingVector()
	return Dirs[self.Facing].V
end

function FarmPlayer:CameraTarget()
	local P = self.entity:GetWorldPosition()
	local Focus = Vector3(math.max(self.Properties.MinX, math.min(self.Properties.MaxX, P.X)),
	                      math.max(self.Properties.MinY, math.min(self.Properties.MaxY, P.Y)), P.Z - 85 + self.Properties.FocusHeight)
	return Focus + self.CamOffset
end

-- 물리 뒤: 스프라이트 정면 유지 + 카메라 (OnUpdate에서 놓으면 한 프레임 늦게 따라가 떨린다)
function FarmPlayer:OnLateUpdate(Dt)
	self.Visual:SetRotation(self.entity:GetRotation():Inverse())
	if self.Camera then
		local Target = self:CameraTarget()
		self.CamPos = Vector3.Lerp(self.CamPos, Target, 1.0 - math.exp(-self.Properties.CameraLag * Dt))
		self.Camera:SetPosition(self.CamPos)
	end
end

function FarmPlayer:Teleport(Pos)
	self.entity:SetPosition(Pos)
	self.LastPos = Pos
end

return FarmPlayer
