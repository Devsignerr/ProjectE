-- 멀티플레이 3인칭 캐릭터 (ExecutionLocation = Both)
--   이동/점프/중력/계단/밀기는 엔진 CharacterMovementComponent가 한다 (소유 클라이언트는 즉시 예측, 서버가 같은 입력으로 보정).
--   이 스크립트는 "조종하는 쪽"(소유 클라이언트 또는 리슨 호스트)에서 입력을 이동 방향으로 바꿔 넘기고, 시점과 카메라만 맡는다.
--   서버는 플레이어 번호별 발밑 표식(Marker 판) 색만 정한다.
-- 몸은 스켈레탈 모델(Fox.glb, Body/Mesh)이고 애니메이션 그래프(Animations/FoxCharacter.eanimgraph)가 캐릭터 이동 상태
--   (Speed/VerticalSpeed/Grounded — 엔진이 자동으로 넣는다)로 대기·걷기·뛰기 블렌드와 점프/낙하 상태를 고른다. 원격 플레이어도
--   각 클라이언트가 복제된 움직임으로 같은 파라미터를 계산하므로 스크립트는 애니메이션에 손대지 않는다.
-- 체력(HealthComponent): 죽어 있는 동안 입력을 막고, 서버가 OnDeath/OnRespawned에서 몸을 숨기고/보인다 (Body 스케일 — 트랜스폼 복제로 모두에게)
-- 조작은 입력 액션으로 읽는다 (프로젝트 설정 → 입력, Config/Input.json — 플레이어 재지정은 Input.Rebind):
--   Move = WASD / 왼쪽 스틱 (보는 방향 기준), Look = 마우스 / 오른쪽 스틱, Jump = 스페이스 / 게임패드 A
--   마우스 시점: 런타임 창은 클릭하면 커서 잠금(마우스만 움직여도 시점), ESC로 잠금 해제. 에디터 플레이 뷰포트는 우클릭을 누른 채.
--   게임패드 스틱 시점은 잠금과 관계없이 항상
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

-- 플레이어 번호별 표식 색 (서버가 정하면 복제된다)
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
		local Marker = Body and Body:FindChild("Marker")
		if Marker and Owner >= 0 then
			Marker:GetComponent("StaticMeshComponent").MaterialAsset = BodyMaterials[(Owner % #BodyMaterials) + 1]
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
	Log.Info("내 캐릭터:", self.entity:GetName(), "— 클릭: 마우스 잠금, ESC: 해제, WASD/마우스/스페이스 또는 게임패드(스틱/A)")
end

function PlayerCharacter:OnUpdate(dt)
	if not self.IsLocalPlayer and Net.IsClient() and self.entity:GetOwner() >= 0 and self.entity:IsLocallyOwned() then
		self:BeginLocalPlayer()
	end
	if self.entity:IsDead() then return end -- 죽어 있는 동안 조작 없음 (리스폰까지)
	if self.IsLocalPlayer then
		self:UpdateView()
		self:UpdateMovementInput()
	end
end

-- ---- 체력 (서버에서만 불린다): 몸을 숨기고/보인다. Body는 ReplicatedComponent가 있어 스케일이 클라이언트로 복제된다
local HiddenScale = 0.001 -- 0이면 행렬이 퇴화하므로 아주 작게

function PlayerCharacter:SetBodyVisible(Visible)
	local Body = self.entity:FindChild("Body")
	if Body then
		local S = Visible and 1.0 or HiddenScale
		Body:SetScale(Vector3(S, S, S))
	end
end

function PlayerCharacter:OnDeath(instigator)
	Log.Info(self.entity:GetName(), "사망 — 리스폰 대기")
	self:SetBodyVisible(false)
end

function PlayerCharacter:OnRespawned()
	self:SetBodyVisible(true)
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
	-- Look 액션 = 마우스 원시 이동 + 오른쪽 스틱(마우스 카운트 단위로 맞춰 둠). 마우스가 잡혀 있지 않으면 마우스 몫은 빼고 스틱만
	-- (기본 바인딩의 MouseXY는 수정자가 없으므로 원시 이동량을 그대로 빼면 된다)
	local LookX, LookY = Input.GetAction("Look")
	if not (Game.IsMouseLocked() or Input.IsMouseDown("Right")) then
		local MouseX, MouseY = Input.GetLookDelta()
		LookX, LookY = LookX - MouseX, LookY - MouseY
	end
	local P = self.Properties
	self.Yaw = (self.Yaw + LookX * P.LookSensitivity) % 360
	self.Pitch = math.max(P.MinPitch, math.min(P.MaxPitch, self.Pitch - LookY * P.LookSensitivity))
	Net.SetControlRotation(self.Yaw, self.Pitch) -- 몸 방향(yaw)도 이것을 따른다
end

-- ---- 조종하는 쪽: 이동 입력 (실제 이동은 엔진이 이번 프레임에 — 클라이언트는 예측)
function PlayerCharacter:UpdateMovementInput()
	local Radians = math.rad(self.Yaw)
	local Forward = Vector3(math.cos(Radians), math.sin(Radians), 0)
	local Right   = Vector3(-math.sin(Radians), math.cos(Radians), 0)
	local MoveX, MoveY = Input.GetAction("Move") -- X = 오른쪽, Y = 앞 (스틱은 기울인 만큼 — 길이 1 넘는 대각선은 엔진이 자른다)
	local Move = Forward * MoveY + Right * MoveX
	if Move:LengthSquared() > 0 then
		self.entity:AddMovementInput(Move)
	end
	if Input.WasActionPressed("Jump") then
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
