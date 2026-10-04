-- HD-2D 데모 플레이어 (Prefabs/Demo/HD2D/Player.eprefab — 캡슐 이동기 + Visual > Body 도트 스프라이트·Shadow·Heart0~4).
--   조작: WASD/왼쪽 스틱 이동(화면 기준 — W = 화면 안쪽 -Y), J/마우스 왼쪽/패드 A 공격, Space/패드 B 대시 (입력 액션 Move/Attack/Dodge).
--   이동은 이동기가 한다 (스크립트는 AddMovementInput만). 방향 = 마지막 입력의 주된 축 → 아래/위/옆 3방향 플립북, 왼쪽은 좌우 반전.
--   공격: 바라보는 쪽 반원 안의 슬라임(관리자 목록 거리 판정)에 피해 + 넉백, 베기 호 효과(가산), 맞히면 히트스톱·흔들림. 연타하면 호가 위/아래 번갈아.
--   대시: entity:AddKnockback(방향 × DashSpeed, DashTime) — 경직 시간 동안 수평 속도를 덮어써 미끄러지고 그동안 무적 + 잔상.
--   피격: 무적 깜빡임 + 넉백, 머리 위 하트가 잠깐 보인다. 체력 0 → 시작 자리에서 부활.
--   카메라: OnLateUpdate(물리 뒤)에 지정 카메라를 발 위치 + 시작 오프셋으로 부드럽게 따라 놓는다 (카메라 회전은 씬 값 그대로 — 고정 시점 디오라마).
--   이동기가 루트를 이동 방향으로 돌리므로 Visual 회전을 매 프레임 상쇄한다.
local HD2DPlayer = {
	Properties = {
		Camera         = "Camera",
		CameraDistance = 2600.0, -- 초점(발 위 FocusHeight)에서 카메라까지 (cm)
		FocusHeight    = 70.0,
		CameraLag      = 7.0,    -- 1/초 (클수록 바짝 따라감)
		MinX = -100000.0, MaxX = 100000.0, MinY = -100000.0, MaxY = 100000.0, -- 카메라 초점 범위 (맵 가장자리가 보이지 않게)
		MaxHealth      = 5,
		AttackDamage   = 1,
		AttackRange    = 150.0,  -- 몸에서 호 중심까지 + 판정 반경
		AttackCooldown = 0.26,
		DashSpeed      = 1700.0, -- cm/s
		DashTime       = 0.2,
		DashCooldown   = 0.5,
	},
}

local Books = {}
for _, Pose in ipairs({ "Idle", "Walk", "Attack", "Dash" }) do
	for _, Dir in ipairs({ "Down", "Up", "Side" }) do
		Books[Pose .. Dir] = "Sprites/HD2D/Hero_" .. Pose .. Dir .. ".eflipbook"
	end
end
local HeroSprite = "Sprites/HD2D/Hero.esprite"
-- 방향 이름 → (월드 방향, 화면 각: 화면 반시계 + 도, 옆모습 반전)
local Dirs = {
	Down  = { V = Vector3(0, 1, 0),  Angle = 270, Anim = "Down", Flip = false },
	Up    = { V = Vector3(0, -1, 0), Angle = 90,  Anim = "Up",   Flip = false },
	Right = { V = Vector3(1, 0, 0),  Angle = 0,   Anim = "Side", Flip = false },
	Left  = { V = Vector3(-1, 0, 0), Angle = 180, Anim = "Side", Flip = true },
}

function HD2DPlayer:OnStart()
	self.GM = Scene.Find("HD2DGame"):GetScript()
	self.Visual = self.entity:FindChild("Visual")
	self.Body = self.Visual:FindChild("Body")
	self.Sprite = self.Body:GetComponent("SpriteComponent")
	self.Hearts = {}
	for I = 0, 4 do
		local H = self.Visual:FindChild("Heart" .. I)
		if H then self.Hearts[#self.Hearts + 1] = H end
	end
	self.Health = self.Properties.MaxHealth
	self.Facing = "Down"
	self.Anim = ""
	self.AttackTimer, self.AttackCooldown, self.bHitDone, self.SwingSide = 0.0, 0.0, true, 1
	self.DashTimer, self.DashCooldown, self.AfterimageTimer = 0.0, 0.0, 0.0
	self.Invuln, self.HurtFlash, self.HeartTimer = 0.0, 0.0, 0.0
	self.bDead = false
	self.Start = self.entity:GetWorldPosition()
	self.LastPos = self.Start
	self.Stats = { Distance = 0, Attacks = 0, Hits = 0, Dashes = 0, Damaged = 0 }
	self.Auto = self.GM.Properties.AutoPlay and { Time = 0, DashTimer = 1.5 } or nil
	self.Camera = Scene.Find(self.Properties.Camera)
	if self.Camera then
		self.CamOffset = self.Camera:GetForward() * -self.Properties.CameraDistance
		self.CamPos = self:CameraTarget()
		self.Camera:SetPosition(self.CamPos)
	end
	self:ShowHearts(0)
	self:UpdateAnimation(Vector3(0, 0, 0))
end

-- ---- 입력 (사람 또는 자동 플레이)
function HD2DPlayer:GatherInput(Dt)
	if self.Auto then
		return self:AutoInput(Dt)
	end
	local MX, MY = Input.GetAction("Move")
	return { Move = Vector3(MX, -MY, 0), Attack = Input.WasActionPressed("Attack"), Dash = Input.WasActionPressed("Dodge") }
end

-- 자동 플레이: 가장 가까운 슬라임에게 걸어가 때리고, 멀면 대시로 거리를 좁힌다
function HD2DPlayer:AutoInput(Dt)
	local A = self.Auto
	A.Time = A.Time + Dt
	A.DashTimer = A.DashTimer - Dt
	local Pos = self.entity:GetWorldPosition()
	local Target, Dist = self.GM:NearestSlime(Pos)
	local In = { Move = Vector3(0, 0, 0), Attack = false, Dash = false }
	if not Target then
		-- 슬라임이 없으면 들판 가운데로
		In.Move = (Vector3(1800, -300, Pos.Z) - Pos)
		In.Move.Z = 0
		if In.Move:Length() > 50 then In.Move = In.Move:Normalized() else In.Move = Vector3(0, 0, 0) end
		return In
	end
	local To = Target.entity:GetWorldPosition() - Pos
	To.Z = 0
	if Dist > 120 then
		In.Move = To:Normalized()
	end
	if Dist < 170 then
		-- 바라보는 방향을 맞추고 때린다
		In.Move = To:Normalized() * 0.3
		In.Attack = self.AttackCooldown <= 0
	elseif Dist > 450 and A.DashTimer <= 0 then
		In.Dash = true
		A.DashTimer = 2.5
	end
	return In
end

local function FacingFromMove(Move, Current)
	if math.abs(Move.X) < 0.1 and math.abs(Move.Y) < 0.1 then
		return Current
	end
	-- 대각선은 옆모습 (도트 RPG 관례)
	if math.abs(Move.X) >= math.abs(Move.Y) * 0.8 then
		return Move.X > 0 and "Right" or "Left"
	end
	return Move.Y > 0 and "Down" or "Up"
end

function HD2DPlayer:OnUpdate(Dt)
	local E = self.entity
	local Pos = E:GetWorldPosition()
	local Step = Pos - self.LastPos
	Step.Z = 0
	if Step:Length() < 500 then self.Stats.Distance = self.Stats.Distance + Step:Length() end
	self.LastPos = Pos

	self.AttackCooldown = math.max(0.0, self.AttackCooldown - Dt)
	self.DashCooldown = math.max(0.0, self.DashCooldown - Dt)
	self.Invuln = math.max(0.0, self.Invuln - Dt)
	self.HeartTimer = math.max(0.0, self.HeartTimer - Dt)
	if self.HeartTimer <= 0 then self:ShowHearts(0) end

	local In = self:GatherInput(Dt)
	local Move = In.Move
	if Move:Length() > 1 then Move = Move:Normalized() end

	-- 대시 중: 이동기가 넉백 경직으로 미끄러뜨린다. 잔상만 남긴다
	if self.DashTimer > 0 then
		self.DashTimer = self.DashTimer - Dt
		self.AfterimageTimer = self.AfterimageTimer - Dt
		if self.AfterimageTimer <= 0 then
			self.AfterimageTimer = 0.035
			local D = Dirs[self.Facing]
			self.GM:SpawnAfterimage(HeroSprite, "Dash" .. D.Anim .. "0", self.Body:GetWorldPosition() + Vector3(0, -2, 0), D.Flip)
		end
	elseif self.AttackTimer > 0 then
		-- 공격 중: 이동 입력 무시, 두 번째 프레임에 판정
		self.AttackTimer = self.AttackTimer - Dt
		if not self.bHitDone and self.AttackTimer <= 0.2 then
			self.bHitDone = true
			self:DoAttackHit()
		end
	else
		if In.Dash and self.DashCooldown <= 0 then
			self:StartDash(Move)
		elseif In.Attack and self.AttackCooldown <= 0 then
			self.Facing = FacingFromMove(Move, self.Facing)
			self:StartAttack()
		elseif Move:Length() > 0.05 then
			self.Facing = FacingFromMove(Move, self.Facing)
			E:AddMovementInput(Move)
		end
	end

	-- 무적 깜빡임 + 피격 붉은빛
	if self.HurtFlash > 0 then
		self.HurtFlash = self.HurtFlash - Dt
	end
	self.Sprite.Color = self.HurtFlash > 0 and Vector4(1, 0.4, 0.4, 1) or Vector4(1, 1, 1, 1)
	self.Sprite.Visible = not (self.Invuln > 0 and self.DashTimer <= 0 and math.floor(self.Invuln * 16) % 2 == 1)

	self:UpdateAnimation(E:GetMovementVelocity())
end

function HD2DPlayer:StartDash(Move)
	local Dir = Move:Length() > 0.1 and Move:Normalized() or Dirs[self.Facing].V
	self.Facing = FacingFromMove(Dir, self.Facing)
	self.DashTimer = self.Properties.DashTime
	self.DashCooldown = self.Properties.DashCooldown
	self.AfterimageTimer = 0.0
	self.entity:AddKnockback(Dir * self.Properties.DashSpeed, self.Properties.DashTime)
	self.GM:SpawnFx("Dust", self.entity:GetWorldPosition() + Vector3(-Dir.X * 40, -Dir.Y * 40 + 4, -82), { FlipX = Dir.X < 0 })
	Audio.PlayOneShot("Audio/RPG/Dash.wav")
	self.Stats.Dashes = self.Stats.Dashes + 1
end

function HD2DPlayer:StartAttack()
	self.AttackTimer = 0.29
	self.AttackCooldown = self.Properties.AttackCooldown
	self.bHitDone = false
	self.SwingSide = -self.SwingSide
	self.Anim = "" -- 같은 방향 연타도 처음부터
	self.Stats.Attacks = self.Stats.Attacks + 1
	Audio.PlayOneShot(self.SwingSide > 0 and "Audio/RPG/Swing1.wav" or "Audio/RPG/Swing2.wav")
	-- 반 걸음 내딛기
	self.entity:AddKnockback(Dirs[self.Facing].V * 260, 0.08)
end

function HD2DPlayer:DoAttackHit()
	local D = Dirs[self.Facing]
	local Pos = self.entity:GetWorldPosition()
	local Center = Pos + D.V * (self.Properties.AttackRange * 0.55)
	-- 베기 호: 화면 평면에서 바라보는 쪽으로 회전, 몸보다 살짝 앞(카메라 쪽)에
	local FxPos = Center + Vector3(0, 12, -5)
	self.GM:SpawnFx("Slash", FxPos, { Rotation = D.Angle, FlipY = self.SwingSide < 0, Blend = 2, Scale = 1.1 })
	local Hit = 0
	for _, S in ipairs(self.GM:FindSlimes(Center, self.Properties.AttackRange * 0.75)) do
		local Away = S.entity:GetWorldPosition() - Pos
		Away.Z = 0
		Away = Away:Length() > 1 and Away:Normalized() or D.V
		if S:TakeHit(self.Properties.AttackDamage, Away) then
			Hit = Hit + 1
		end
	end
	if Hit > 0 then
		self.Stats.Hits = self.Stats.Hits + Hit
		Game.HitStop(0.06)
		self.GM:AddShake(6, 0.15)
		Audio.PlayOneShot("Audio/RPG/Hit.wav")
	end
end

-- 슬라임이 부른다. 피해를 받았으면 true
function HD2DPlayer:TakeDamage(Amount, From)
	if self.bDead or self.Invuln > 0 or self.DashTimer > 0 then
		return false
	end
	self.Health = self.Health - Amount
	self.Stats.Damaged = self.Stats.Damaged + 1
	self.Invuln = 1.0
	self.HurtFlash = 0.15
	self.AttackTimer = 0.0
	local Away = self.entity:GetWorldPosition() - From
	Away.Z = 0
	Away = Away:Length() > 1 and Away:Normalized() or Vector3(0, 1, 0)
	self.entity:AddKnockback(Away * 650, 0.18)
	self.GM:AddShake(9, 0.2)
	self.GM:SpawnFx("Spark", self.entity:GetWorldPosition() + Vector3(0, 30, 0), { Blend = 2, Color = { 1, 0.45, 0.45, 1 } })
	Audio.PlayOneShot("Audio/RPG/Hurt.wav")
	self:ShowHearts(2.0)
	if self.Health <= 0 then
		self:Respawn()
	end
	return true
end

function HD2DPlayer:Respawn()
	self.GM:SpawnFx("Poof", self.entity:GetWorldPosition() + Vector3(0, 10, -60), { Scale = 1.3 })
	self.entity:SetPosition(self.Start)
	self.Health = self.Properties.MaxHealth
	self.Invuln = 2.0
	self:ShowHearts(2.0)
	Log.Info("[HD2D] 플레이어 쓰러짐 → 시작 자리에서 부활")
end

function HD2DPlayer:ShowHearts(Seconds)
	self.HeartTimer = Seconds
	for I, H in ipairs(self.Hearts) do
		local S = H:GetComponent("SpriteComponent")
		S.Visible = Seconds > 0 and I <= self.Properties.MaxHealth
		S.Slice = I <= self.Health and "HeartFull" or "HeartEmpty"
	end
end

function HD2DPlayer:UpdateAnimation(Velocity)
	local D = Dirs[self.Facing]
	local Pose = "Idle"
	if self.DashTimer > 0 then
		Pose = "Dash"
	elseif self.AttackTimer > 0 then
		Pose = "Attack"
	elseif Velocity.X * Velocity.X + Velocity.Y * Velocity.Y > 40 * 40 then
		Pose = "Walk"
	end
	local Want = Pose .. D.Anim
	if Want ~= self.Anim then
		self.Anim = Want
		self.Body:PlayFlipbook(Books[Want])
	end
	self.Body:SetSpriteFlip(D.Flip, false)
end

function HD2DPlayer:CameraTarget()
	local P = self.entity:GetWorldPosition()
	local Focus = Vector3(math.max(self.Properties.MinX, math.min(self.Properties.MaxX, P.X)),
	                      math.max(self.Properties.MinY, math.min(self.Properties.MaxY, P.Y)), P.Z - 85 + self.Properties.FocusHeight)
	return Focus + self.CamOffset
end

-- 물리 뒤: 스프라이트 정면 유지 + 카메라 (OnUpdate에서 놓으면 한 프레임 늦게 따라가 떨린다)
function HD2DPlayer:OnLateUpdate(Dt)
	self.Visual:SetRotation(self.entity:GetRotation():Inverse())
	if self.Camera then
		local Target = self:CameraTarget()
		self.CamPos = Vector3.Lerp(self.CamPos, Target, 1.0 - math.exp(-self.Properties.CameraLag * Dt))
		self.Camera:SetPosition(self.CamPos + self.GM:GetShakeOffset())
	end
end

return HD2DPlayer
