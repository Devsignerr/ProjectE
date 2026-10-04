-- 데모 맵 플레이어 (싱글, ExecutionLocation = Both): 이동은 CharacterMovementComponent, 이 스크립트는 입력 → 이동 방향과 카메라만.
--   V: 시점 전환 3인칭 → 1인칭 → 탑뷰 → 3인칭 (CameraMode 프로퍼티 = 시작 시점)
--   3인칭/1인칭: 마우스로 시점(런타임은 클릭 시 커서 잠금·ESC 해제, 에디터 플레이 뷰포트는 우클릭을 누른 채), WASD = 보는 방향 기준
--   탑뷰: 상공에서 내려다보는 직교 카메라(고정 각도, 화면 세로 = TopDownViewHeight), WASD = 화면 기준, 몸은 움직이는 방향을 본다.
--         마우스 시점은 카메라 방위만 돌린다. 벽 검사 레이캐스트를 하지 않는다(가파른 각도에서는 시작점이 캡슐 안이라 자기 몸에 맞았다)
--   1인칭에서는 몸(Body)을 숨긴다 (스케일 — 그림자도 함께 사라짐)
-- 몸 방향은 엔진이 ControlRotation yaw를 따른다(FaceControlYaw). 카메라는 OnLateUpdate에서 놓는다(물리 뒤 — 떨림 방지)
local DemoPlayer = {
	Properties = {
		CameraMode       = "ThirdPerson", -- ThirdPerson | FirstPerson | TopDown
		LookSensitivity  = 0.12,          -- 도/마우스 카운트
		ThirdDistance    = 380.0,         -- cm
		ThirdHeight      = 70.0,          -- cm (캡슐 중심 기준 시점 높이)
		FirstHeight      = 70.0,          -- cm (눈 높이, 캡슐 중심 기준)
		TopDownDistance  = 8000.0,        -- cm (직교라 화면 크기와 무관 — 지형·건물 위로 충분히 높게)
		TopDownPitch     = -60.0,         -- 도
		TopDownViewHeight = 1500.0,       -- cm (직교 화면 세로가 담는 월드 높이)
		MinPitch         = -75.0,
		MaxPitch         = 70.0,
	},
}

local Modes = { "ThirdPerson", "FirstPerson", "TopDown" }
local ModeNames = { ThirdPerson = "3인칭", FirstPerson = "1인칭", TopDown = "탑뷰" }
local CameraClearance = 40.0 -- cm, 카메라 벽 검사를 캡슐 바깥에서 시작
local HiddenScale = 0.001    -- 0이면 행렬이 퇴화하므로 아주 작게

local function DirectionFrom(Yaw, Pitch)
	local Y, P = math.rad(Yaw), math.rad(Pitch)
	return Vector3(math.cos(P) * math.cos(Y), math.cos(P) * math.sin(Y), math.sin(P))
end

function DemoPlayer:OnStart()
	local Forward = self.entity:GetForward()
	self.Yaw = math.deg(math.atan(Forward.Y, Forward.X))
	self.Pitch = -12.0
	self.BodyYaw = self.Yaw
	self.Mode = self.Properties.CameraMode
	self.IsLocalPlayer = false
end

function DemoPlayer:BeginLocalPlayer()
	self.IsLocalPlayer = true
	self.Camera = Scene.Create("PlayerCamera") -- 레벨 카메라(Priority 0)보다 우선하는 로컬 카메라
	local Camera = self.Camera:AddComponent("CameraComponent")
	self.CameraComponent = Camera
	Camera.Priority    = 10
	Camera.FovYDegrees = 70.0
	Camera.NearZ       = 5.0
	Camera.FarZ        = 200000.0
	self:ApplyMode()
	Log.Info("V: 시점 전환 (3인칭 → 1인칭 → 탑뷰), WASD 이동, 스페이스 점프, 클릭: 마우스 잠금")
end

function DemoPlayer:OnUpdate(dt)
	if not self.IsLocalPlayer and (not Net.IsClient() or self.entity:IsLocallyOwned()) then
		self:BeginLocalPlayer()
	end
	if not self.IsLocalPlayer then return end
	if Input.IsKeyPressed("V") then
		self:CycleMode()
	end
	self:UpdateView()
	self:UpdateMovementInput()
end

function DemoPlayer:CycleMode()
	local Index = 1
	for I, Name in ipairs(Modes) do
		if Name == self.Mode then Index = I end
	end
	self.Mode = Modes[(Index % #Modes) + 1]
	self:ApplyMode()
	Log.Info("시점:", ModeNames[self.Mode] or self.Mode)
end

function DemoPlayer:ApplyMode()
	local Body = self.entity:FindChild("Body")
	if Body then
		local S = (self.Mode == "FirstPerson") and HiddenScale or 1.0
		Body:SetScale(Vector3(S, S, S))
	end
	if self.Mode == "TopDown" then
		self.BodyYaw = self.Yaw
	end
	if self.CameraComponent then
		self.CameraComponent.Orthographic = (self.Mode == "TopDown")
		self.CameraComponent.OrthoHeight  = self.Properties.TopDownViewHeight
	end
end

function DemoPlayer:UpdateView()
	if not Game.IsMouseLocked() and Input.IsMouseDown("Left") then
		Game.SetMouseLocked(true) -- 런타임만 (에디터는 아무 일 없음 → 우클릭 시점)
	end
	local LookX, LookY = Input.GetAction("Look")
	if not (Game.IsMouseLocked() or Input.IsMouseDown("Right")) then
		local MouseX, MouseY = Input.GetLookDelta()
		LookX, LookY = LookX - MouseX, LookY - MouseY
	end
	local P = self.Properties
	self.Yaw = (self.Yaw + LookX * P.LookSensitivity) % 360
	if self.Mode ~= "TopDown" then
		self.Pitch = math.max(P.MinPitch, math.min(P.MaxPitch, self.Pitch - LookY * P.LookSensitivity))
		Net.SetControlRotation(self.Yaw, self.Pitch)
	else
		Net.SetControlRotation(self.BodyYaw, 0) -- 탑뷰: 몸은 움직이는 방향 (UpdateMovementInput이 정함)
	end
end

function DemoPlayer:UpdateMovementInput()
	local Radians = math.rad(self.Yaw)
	local Forward = Vector3(math.cos(Radians), math.sin(Radians), 0)
	local Right   = Vector3(-math.sin(Radians), math.cos(Radians), 0)
	local MoveX, MoveY = Input.GetAction("Move")
	local Move = Forward * MoveY + Right * MoveX
	if Move:LengthSquared() > 0 then
		self.entity:AddMovementInput(Move)
		if self.Mode == "TopDown" then
			self.BodyYaw = math.deg(math.atan(Move.Y, Move.X))
		end
	end
	if Input.WasActionPressed("Jump") then
		self.entity:Jump()
	end
end

function DemoPlayer:OnLateUpdate(dt)
	if self.IsLocalPlayer then
		self:PlaceCamera()
	end
end

function DemoPlayer:PlaceCamera()
	local P = self.Properties
	local Center = self.entity:GetWorldPosition()
	local Eye, Look, Distance
	if self.Mode == "FirstPerson" then
		Eye, Look, Distance = Center + Vector3(0, 0, P.FirstHeight), DirectionFrom(self.Yaw, self.Pitch), 0
	elseif self.Mode == "TopDown" then
		Eye, Look, Distance = Center, DirectionFrom(self.Yaw, P.TopDownPitch), P.TopDownDistance
	else
		Eye, Look, Distance = Center + Vector3(0, 0, P.ThirdHeight), DirectionFrom(self.Yaw, self.Pitch), P.ThirdDistance
	end
	if Distance > 0 and self.Mode ~= "TopDown" then
		-- 벽·지형 뒤로 들어가지 않게: 캡슐 밖에서 카메라 쪽으로 레이캐스트
		local Start = Eye - Look * CameraClearance
		local Hit = Physics.Raycast(Start, -Look, Distance)
		if Hit then
			Distance = math.max(0, Hit.distance + CameraClearance - 15)
		end
	end
	self.Camera:SetPosition(Eye - Look * Distance)
	self.Camera:SetRotation(Quat.LookRotation(Look))
end

function DemoPlayer:OnDestroy()
	if self.Camera then
		self.Camera:Destroy()
		self.Camera = nil
	end
	if self.IsLocalPlayer then
		Game.SetMouseLocked(false)
	end
end

return DemoPlayer
