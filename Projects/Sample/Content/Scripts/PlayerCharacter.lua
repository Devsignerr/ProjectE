-- 멀티플레이 3인칭 캐릭터 (ExecutionLocation = Both)
--   이동/점프/중력/계단/밀기는 엔진 CharacterMovementComponent가 한다 (소유 클라이언트는 즉시 예측, 서버가 같은 입력으로 보정).
--   이 스크립트는 "조종하는 쪽"(소유 클라이언트 또는 리슨 호스트)에서 입력을 이동 방향으로 바꿔 넘기고, 시점과 카메라만 맡는다.
--   서버는 플레이어 번호별 몸 색만 정한다.
-- 조작: WASD 이동(보는 방향 기준), 마우스 시점, 스페이스 점프.
--   런타임 창: 클릭하면 커서 잠금(마우스만 움직여도 시점), ESC로 잠금 해제. 에디터 플레이 뷰포트: 우클릭을 누른 채 마우스
-- 속도/점프 높이/계단 높이 등은 프리팹의 CharacterMovementComponent 속성에서 바꾼다
local PlayerCharacter = {
	Properties = {
		LookSensitivity = 0.12,  -- 도/마우스 카운트
		CameraDistance  = 420.0, -- cm (0이면 1인칭)
		CameraHeight    = 110.0, -- cm (캡슐 중심 기준 시점 높이 — 머리 위를 넘겨 본다)
		MinPitch        = -70.0,
		MaxPitch        = 60.0,
	},
}

-- 플레이어 번호별 몸 색 (서버가 정하면 복제된다)
local BodyMaterials = { "Materials/Orange.emat", "Materials/Blue.emat", "Materials/Green.emat", "Materials/Purple.emat", "Materials/Yellow.emat" }

local CameraClearance = 40.0 -- cm, 카메라 벽 검사를 캡슐 바깥에서 시작

function PlayerCharacter:OnStart()
	local Forward = self.entity:GetForward() -- 생성 위치(PlayerStart)의 방향으로 시작
	self.Yaw = math.deg(math.atan(Forward.Y, Forward.X))
	self.Pitch = -15.0
	self.IsLocalPlayer = false

	if Net.IsServer() then
		local Owner = self.entity:GetOwner()
		local Body = self.entity:FindChild("Body")
		local Mesh = Body and Body:FindChild("Mesh")
		if Mesh and Owner >= 0 then
			Mesh:GetComponent("StaticMeshComponent").MaterialAsset = BodyMaterials[(Owner % #BodyMaterials) + 1]
		end
	end
end

-- 클라이언트에서는 소유자(ReplicatedComponent)가 복제로 늦게 올 수 있어 OnStart가 아니라 확정되는 순간에 시작한다
function PlayerCharacter:BeginLocalPlayer()
	self.IsLocalPlayer = true
	-- 이 창에서만 쓰는 카메라 (복제 안 됨). 레벨 카메라(Priority 0)보다 우선
	self.Camera = Scene.Create("PlayerCamera")
	local Camera = self.Camera:AddComponent("CameraComponent")
	Camera.Priority    = 10
	Camera.FovYDegrees = 75.0
	Log.Info("내 캐릭터:", self.entity:GetName(), "— 클릭: 마우스 잠금, ESC: 해제, WASD/마우스/스페이스")
end

function PlayerCharacter:OnUpdate(dt)
	if not self.IsLocalPlayer and Net.IsClient() and self.entity:GetOwner() >= 0 and self.entity:IsLocallyOwned() then
		self:BeginLocalPlayer()
	end
	if self.IsLocalPlayer then
		self:UpdateView()
		self:UpdateMovementInput()
	end
end

-- 카메라는 이동(캐릭터 이동 컴포넌트/물리/복제 보간)이 끝난 뒤에 (OnUpdate에서 놓으면 한 프레임 늦게 따라가 떨려 보인다)
function PlayerCharacter:OnLateUpdate(dt)
	if self.IsLocalPlayer then
		self:PlaceCamera()
	end
end

-- ---- 조종하는 쪽: 시점
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
	Net.SetControlRotation(self.Yaw, self.Pitch) -- 몸 방향(yaw)도 이것을 따른다
end

-- ---- 조종하는 쪽: 이동 입력 (실제 이동은 엔진이 이번 프레임에 — 클라이언트는 예측)
function PlayerCharacter:UpdateMovementInput()
	local Radians = math.rad(self.Yaw)
	local Forward = Vector3(math.cos(Radians), math.sin(Radians), 0)
	local Right   = Vector3(-math.sin(Radians), math.cos(Radians), 0)
	local Move = Vector3.Zero()
	if Input.IsKeyDown("W") then Move = Move + Forward end
	if Input.IsKeyDown("S") then Move = Move - Forward end
	if Input.IsKeyDown("D") then Move = Move + Right end
	if Input.IsKeyDown("A") then Move = Move - Right end
	if Move:LengthSquared() > 0 then
		self.entity:AddMovementInput(Move)
	end
	if Input.IsKeyPressed("Space") then
		self.entity:Jump() -- 바닥에 있을 때만 뛴다
	end
end

local function DirectionFrom(Yaw, Pitch)
	local Y, P = math.rad(Yaw), math.rad(Pitch)
	return Vector3(math.cos(P) * math.cos(Y), math.cos(P) * math.sin(Y), math.sin(P))
end

function PlayerCharacter:PlaceCamera()
	local P        = self.Properties
	local Eye      = self.entity:GetWorldPosition() + Vector3(0, 0, P.CameraHeight)
	local Look     = DirectionFrom(self.Yaw, self.Pitch)
	local Distance = P.CameraDistance
	if Distance > 0 then
		-- 벽 뒤로 들어가지 않게: 캡슐 밖에서 카메라 쪽으로 레이캐스트
		local Start = Eye - Look * CameraClearance
		local Hit = Physics.Raycast(Start, -Look, Distance)
		if Hit then
			Distance = math.max(0, Hit.distance + CameraClearance - 15)
		end
	end
	self.Camera:SetPosition(Eye - Look * Distance)
	self.Camera:SetRotation(Quat.LookRotation(Look))
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
