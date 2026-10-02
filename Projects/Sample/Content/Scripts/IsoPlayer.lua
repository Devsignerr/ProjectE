-- 아이소메트릭(쿼터뷰) 플레이어 조작 (Demo_PixelArt): 레벨 카메라를 그대로 쓰고 캐릭터를 따라 평행 이동한다.
--   이동은 화면 기준 — W = 화면 위, D = 화면 오른쪽 (카메라 앞 방향을 바닥에 투영한 축).
--   이동/점프는 CharacterMovementComponent(FaceControlYaw 끔 = 이동 방향을 봄), 애니메이션은 자식 메시의 그래프.
-- 조작: WASD 이동, Space 점프, J 또는 마우스 왼쪽 공격
local IsoPlayer = {
	Properties = {
		Camera       = "Camera", -- 따라갈 카메라 엔티티 이름
		AttackClip   = "1H_Melee_Attack_Chop",
		AttackSpeed  = 1.2,
	},
}

function IsoPlayer:OnStart()
	self.Camera = Scene.Find(self.Properties.Camera)
	if not self.Camera then
		Log.Warn("IsoPlayer: 카메라를 찾지 못함", self.Properties.Camera)
		return
	end
	-- 시작 시 카메라 시선이 캐릭터 발 높이 평면과 만나는 점을 캐릭터에 맞추는 오프셋
	local Forward = self.Camera:GetForward()
	local Height  = self.Camera:GetWorldPosition().Z - self.entity:GetWorldPosition().Z
	self.CameraOffset = Forward * (Height / Forward.Z) -- Forward.Z < 0 → 시선 반대쪽 위
	-- 화면 기준 이동 축 (바닥 평면)
	self.ScreenUp    = Vector3(Forward.X, Forward.Y, 0):Normalized()
	self.ScreenRight = Vector3(-self.ScreenUp.Y, self.ScreenUp.X, 0) -- Z-up 왼손: 앞에서 +90도 = 오른쪽
	self.bMouseWasDown = false
	self:PlaceCamera()
end

function IsoPlayer:OnUpdate(dt)
	if not self.Camera then return end
	local MoveX, MoveY = Input.GetAction("Move")
	local Move = self.ScreenUp * MoveY + self.ScreenRight * MoveX
	if Move:LengthSquared() > 0 then
		self.entity:AddMovementInput(Move)
	end
	if Input.WasActionPressed("Jump") then
		self.entity:Jump()
	end

	local bMouseDown = Input.IsMouseDown("Left")
	local bAttack    = Input.IsKeyPressed("J") or (bMouseDown and not self.bMouseWasDown)
	self.bMouseWasDown = bMouseDown
	if bAttack and not self.entity:IsMontagePlaying() then
		self.entity:PlayMontage(self.Properties.AttackClip, { BlendIn = 0.1, BlendOut = 0.2, Speed = self.Properties.AttackSpeed })
	end
end

-- 카메라는 캐릭터 이동(물리)이 끝난 뒤에 놓는다 (OnUpdate에서 놓으면 한 프레임 늦게 따라가 떨린다)
function IsoPlayer:OnLateUpdate(dt)
	if self.Camera then
		self:PlaceCamera()
	end
end

function IsoPlayer:PlaceCamera()
	local Foot = self.entity:GetWorldPosition()
	self.Camera:SetPosition(Foot + self.CameraOffset)
end

return IsoPlayer
