-- 쇼케이스 플레이어 여우 (Demo_Showcase, 1인용 Standalone): PlayerCharacter.lua의 조작부만 가져온 버전
--   투어 중에는 아무것도 하지 않다가 ShowcaseDirector가 SetControlEnabled(true)를 부르면 조작 시작:
--   이 창 전용 카메라(Priority 10 — 레벨 카메라 0보다 위, 시퀀스 컷 카메라보다 아래)를 만들고 OnLateUpdate에서 따라간다.
--   이동/점프/계단은 CharacterMovementComponent, 애니메이션은 FoxCharacter 그래프(이동 상태 자동 입력), 발 IK는 Mesh의 FootIkComponent
-- 조작: WASD 이동, 마우스 시점(클릭하면 커서 잠금, ESC 해제), Space 점프, E 몽타주(Survey 전신 한 번)
local ShowcasePlayer = {
	Properties = {
		LookSensitivity = 0.12,  -- 도/마우스 카운트
		CameraDistance  = 380.0, -- cm
		CameraHeight    = 110.0, -- cm (캡슐 중심 기준)
		MinPitch        = -60.0,
		MaxPitch        = 50.0,
		StartPitch      = -20.0,
	},
}

local CameraClearance = 40.0 -- cm, 카메라 벽 검사를 캡슐 바깥에서 시작

function ShowcasePlayer:OnStart()
	local Forward = self.entity:GetForward()
	self.Yaw     = math.deg(math.atan(Forward.Y, Forward.X))
	self.Pitch   = self.Properties.StartPitch
	self.Enabled = false
end

-- 감독 스크립트가 투어가 끝나면 부른다
function ShowcasePlayer:SetControlEnabled(Enabled)
	if Enabled == self.Enabled then return end
	self.Enabled = Enabled
	if Enabled then
		self.SkipJumpFrame = true -- 투어를 건너뛴 Space가 같은 프레임 점프로 이어지지 않게
		self.Camera = Scene.Create("PlayerCamera")
		local Camera = self.Camera:AddComponent("CameraComponent")
		Camera.Priority    = 10
		Camera.FovYDegrees = 70.0
		Camera.FarZ        = 200000.0
		self:PlaceCamera()
		Log.Info("쇼케이스: 여우 조작 시작 — WASD/마우스/Space/E")
	elseif self.Camera then
		self.Camera:Destroy()
		self.Camera = nil
	end
end

function ShowcasePlayer:IsControlEnabled()
	return self.Enabled
end

function ShowcasePlayer:OnUpdate(dt)
	if not self.Enabled then return end
	self:UpdateView()
	self:UpdateMovementInput()
	if Input.IsKeyPressed("E") and not self.entity:IsMontagePlaying() then
		self.entity:PlayMontage("Survey", { BlendIn = 0.25, BlendOut = 0.35, Speed = 1.3 }) -- 슬롯 없음 = 몸 전체
	end
	self.SkipJumpFrame = false
end

-- 카메라는 이동(캐릭터 이동/물리)이 끝난 뒤에 놓는다 (OnUpdate에서 놓으면 한 프레임 늦게 따라가 떨린다)
function ShowcasePlayer:OnLateUpdate(dt)
	if self.Enabled and self.Camera then
		self:PlaceCamera()
	end
end

function ShowcasePlayer:UpdateView()
	if not Game.IsMouseLocked() and Input.IsMouseDown("Left") then
		Game.SetMouseLocked(true) -- 런타임만 (에디터는 우클릭을 누른 채 시점)
	end
	local LookX, LookY = Input.GetAction("Look")
	if not (Game.IsMouseLocked() or Input.IsMouseDown("Right")) then
		local MouseX, MouseY = Input.GetLookDelta()
		LookX, LookY = LookX - MouseX, LookY - MouseY -- 잠기지 않았으면 마우스 몫은 빼고 스틱만
	end
	local P = self.Properties
	self.Yaw   = (self.Yaw + LookX * P.LookSensitivity) % 360
	self.Pitch = math.max(P.MinPitch, math.min(P.MaxPitch, self.Pitch - LookY * P.LookSensitivity))
	Net.SetControlRotation(self.Yaw, self.Pitch) -- 몸 방향(FaceControlYaw)도 이것을 따른다
end

function ShowcasePlayer:UpdateMovementInput()
	local Radians = math.rad(self.Yaw)
	local Forward = Vector3(math.cos(Radians), math.sin(Radians), 0)
	local Right   = Vector3(-math.sin(Radians), math.cos(Radians), 0)
	local MoveX, MoveY = Input.GetAction("Move")
	local Move = Forward * MoveY + Right * MoveX
	if Move:LengthSquared() > 0 then
		self.entity:AddMovementInput(Move)
	end
	if Input.WasActionPressed("Jump") and not self.SkipJumpFrame then
		self.entity:Jump()
	end
end

local function DirectionFrom(Yaw, Pitch)
	local Y, P = math.rad(Yaw), math.rad(Pitch)
	return Vector3(math.cos(P) * math.cos(Y), math.cos(P) * math.sin(Y), math.sin(P))
end

function ShowcasePlayer:PlaceCamera()
	local P        = self.Properties
	local Eye      = self.entity:GetWorldPosition() + Vector3(0, 0, P.CameraHeight)
	local Look     = DirectionFrom(self.Yaw, self.Pitch)
	local Distance = P.CameraDistance
	local Start    = Eye - Look * CameraClearance
	local Hit      = Physics.Raycast(Start, -Look, Distance)
	if Hit then
		Distance = math.max(0, Hit.distance + CameraClearance - 15)
	end
	self.Camera:SetPosition(Eye - Look * Distance)
	self.Camera:SetRotation(Quat.LookRotation(Look))
end

function ShowcasePlayer:OnMontageEnded(clip, interrupted, slot)
	Log.Info("쇼케이스: 몽타주 끝", clip, interrupted and "(중단)" or "(완료)")
end

function ShowcasePlayer:OnDestroy()
	if self.Camera then
		self.Camera:Destroy()
		self.Camera = nil
	end
	if self.Enabled then
		Game.SetMouseLocked(false)
	end
end

return ShowcasePlayer
