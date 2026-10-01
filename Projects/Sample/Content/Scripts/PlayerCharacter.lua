-- 멀티플레이 3인칭 캐릭터 (ExecutionLocation = Both)
--   서버: 소유 플레이어의 입력(WASD/스페이스)과 시점 방향으로 물리 캡슐을 움직인다 → 위치는 복제로 모두에게 보인다
--   소유 클라이언트(리슨 호스트 포함): 마우스로 시점을 돌리고(Net.SetControlRotation으로 서버에 보냄) 자기 카메라를 캐릭터 뒤에 둔다
-- 조작: WASD 이동(보는 방향 기준), 마우스 시점, 스페이스 점프.
--   런타임 창: 클릭하면 커서 잠금(마우스만 움직여도 시점), ESC로 잠금 해제. 에디터 플레이 뷰포트: 우클릭을 누른 채 마우스
local PlayerCharacter = {
	Properties = {
		WalkSpeed       = 450.0, -- cm/s
		JumpSpeed       = 520.0, -- cm/s (점프 순간 위쪽 속도, 약 1.4m 높이)
		LookSensitivity = 0.12,  -- 도/마우스 카운트
		CameraDistance  = 420.0, -- cm (0이면 1인칭)
		CameraHeight    = 110.0, -- cm (캡슐 중심 기준 시점 높이 — 머리 위를 넘겨 본다)
		MinPitch        = -70.0,
		MaxPitch        = 60.0,
	},
}

-- 플레이어 번호별 몸 색 (서버가 정하면 복제된다)
local BodyMaterials = { "Materials/Orange.emat", "Materials/Blue.emat", "Materials/Green.emat", "Materials/Purple.emat", "Materials/Yellow.emat" }

local CapsuleRadius     = 35.0 -- 프리팹 CapsuleColliderComponent와 같게
local CapsuleHalfHeight = 55.0

function PlayerCharacter:OnStart()
	self.Body = self.entity:FindChild("Body")
	local Forward = self.entity:GetForward() -- 생성 위치(PlayerStart)의 방향으로 시작
	self.Yaw = math.deg(math.atan(Forward.Y, Forward.X))
	self.Pitch = -15.0
	self.JumpCooldown = 0.0

	if Net.IsServer() then
		local Owner = self.entity:GetOwner()
		local Mesh = self.Body and self.Body:FindChild("Mesh")
		if Mesh and Owner >= 0 then
			Mesh:GetComponent("StaticMeshComponent").MaterialAsset = BodyMaterials[(Owner % #BodyMaterials) + 1]
		end
	end

	self.IsLocalPlayer = false
end

-- 클라이언트에서는 소유자(ReplicatedComponent)가 복제로 늦게 올 수 있어 OnStart가 아니라 확정되는 순간에 시작한다
function PlayerCharacter:BeginLocalPlayer()
	self.IsLocalPlayer = true
	-- 이 창에서만 쓰는 카메라 (복제 안 됨). 레벨 카메라(Priority 0)보다 우선
	self.Camera = Scene.Create("PlayerCamera")
	local Camera = self.Camera:AddComponent("CameraComponent")
	Camera.Priority    = 10
	Camera.FovYDegrees = 75.0
	Net.SetControlRotation(self.Yaw, self.Pitch)
	Log.Info("내 캐릭터:", self.entity:GetName(), "— 클릭: 마우스 잠금, ESC: 해제, WASD/마우스/스페이스")
end

function PlayerCharacter:OnUpdate(dt)
	if not self.IsLocalPlayer and Net.IsClient() and self.entity:GetOwner() >= 0 and self.entity:IsLocallyOwned() then
		self:BeginLocalPlayer()
	end
	if self.IsLocalPlayer then
		self:UpdateView()
	end
	if Net.IsServer() then
		self:ServerMove(dt)
	end
end

-- 카메라는 물리/복제 보간이 캐릭터를 옮긴 뒤에 (OnUpdate에서 놓으면 한 프레임 늦게 따라가 떨려 보인다)
function PlayerCharacter:OnLateUpdate(dt)
	if self.IsLocalPlayer then
		self:PlaceCamera()
	end
end

-- ---- 소유 클라이언트: 시점
function PlayerCharacter:UpdateView()
	if not Game.IsMouseLocked() and Input.IsMouseDown("Left") then
		Game.SetMouseLocked(true) -- 런타임만 (에디터는 아무 일 없음 → 우클릭 시점)
	end
	if Game.IsMouseLocked() or Input.IsMouseDown("Right") then
		local DeltaX, DeltaY = Input.GetLookDelta()
		local P = self.Properties
		self.Yaw = (self.Yaw + DeltaX * P.LookSensitivity) % 360
		self.Pitch = math.max(P.MinPitch, math.min(P.MaxPitch, self.Pitch - DeltaY * P.LookSensitivity))
	end
	Net.SetControlRotation(self.Yaw, self.Pitch) -- 매 틱 입력과 함께 서버로
end

local function DirectionFrom(Yaw, Pitch)
	local Y, P = math.rad(Yaw), math.rad(Pitch)
	return Vector3(math.cos(P) * math.cos(Y), math.cos(P) * math.sin(Y), math.sin(P))
end

function PlayerCharacter:PlaceCamera()
	local P      = self.Properties
	local Eye    = self.entity:GetWorldPosition() + Vector3(0, 0, P.CameraHeight)
	local Look   = DirectionFrom(self.Yaw, self.Pitch)
	local Distance = P.CameraDistance
	if Distance > 0 then
		-- 벽 뒤로 들어가지 않게: 캡슐 밖에서 카메라 쪽으로 레이캐스트
		local Start = Eye - Look * (CapsuleRadius + 5)
		local Hit = Physics.Raycast(Start, -Look, Distance)
		if Hit then
			Distance = math.max(0, Hit.distance + CapsuleRadius + 5 - 15)
		end
	end
	local Position = Eye - Look * Distance
	self.Camera:SetPosition(Position)
	self.Camera:SetRotation(Quat.LookRotation(Look))
end

-- ---- 서버: 이동/점프 (Input = 이 캐릭터를 소유한 플레이어의 입력)
function PlayerCharacter:IsGrounded()
	local Feet = self.entity:GetWorldPosition() - Vector3(0, 0, CapsuleHalfHeight + CapsuleRadius + 1)
	return Physics.Raycast(Feet, Vector3(0, 0, -1), 12) ~= nil
end

function PlayerCharacter:ServerMove(dt)
	local Yaw = self.entity:GetControlRotation() -- 소유 플레이어가 보는 방향 (도)
	local Radians = math.rad(Yaw)
	local Forward = Vector3(math.cos(Radians), math.sin(Radians), 0)
	local Right   = Vector3(-math.sin(Radians), math.cos(Radians), 0)

	local Move = Vector3.Zero()
	if Input.IsKeyDown("W") then Move = Move + Forward end
	if Input.IsKeyDown("S") then Move = Move - Forward end
	if Input.IsKeyDown("D") then Move = Move + Right end
	if Input.IsKeyDown("A") then Move = Move - Right end
	if Move:LengthSquared() > 0 then
		Move = Move:Normalized() * self.Properties.WalkSpeed
	end

	local Velocity = self.entity:GetVelocity()
	local VerticalSpeed = Velocity.Z
	self.JumpCooldown = math.max(0, self.JumpCooldown - dt)
	if Input.IsKeyDown("Space") and self.JumpCooldown <= 0 and self:IsGrounded() then
		VerticalSpeed = self.Properties.JumpSpeed
		self.JumpCooldown = 0.3
	end
	self.entity:SetVelocity(Vector3(Move.X, Move.Y, VerticalSpeed))

	-- 몸(자식)만 보는 방향으로 — 캡슐 바디는 회전 고정
	if self.Body then
		self.Body:SetRotation(Quat.FromEuler(0, Yaw, 0))
	end
end

function PlayerCharacter:OnDestroy()
	if self.Camera then
		self.Camera:Destroy()
		self.Camera = nil
	end
	if self.IsLocalPlayer then
		Game.SetMouseLocked(false)
	end
end

return PlayerCharacter
