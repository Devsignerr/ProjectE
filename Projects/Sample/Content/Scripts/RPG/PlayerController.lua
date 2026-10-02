-- 액션 RPG 플레이어 (Phase 45 트랙 B): 쿼터뷰 이동 + 3연타 콤보 + 스킬 2개 + 구르기 + 막기 + 피격/사망.
--   엔티티 구성 (Prefabs/RPG/Player.eprefab): 루트 "Player" = CharacterMovementComponent(FaceControlYaw 끔) + HealthComponent + 이 스크립트,
--     자식 "PlayerMesh" = KnightBare.glb + AnimGraph(Animations/RPG/KnightRPG.eanimgraph, 캐릭터 이동 파라미터),
--     자식 "Weapon"/"Shield" = 무기 모델 (SocketAttachment → PlayerMesh 소켓 HandR / Shield, 소켓은 KnightBare.glb.emeta)
--   이동: 화면 기준 WASD (카메라 앞 방향을 바닥에 투영). 입력 크기를 지수 램프로 올리고 내려 가감속하고, 그래프 Speed 블렌드
--     (Idle 0 / Walking_A 100 / Running_A 345 / Running_A×1.5 520)가 실제 속도를 따른다 — 달리기·질주 속도는 클립 발 속도와 같다.
--   몸 방향: 캡슐(루트)은 엔진이 이동 방향으로 바로 돌리므로, 화면에 보이는 몸(PlayerMesh)을 보간한 방향으로 따로 돌린다
--     (Standalone에서는 소유자 없는 캐릭터가 ControlYaw를 쓰지 않는다 — 로컬 회전 = 보간 방향 − 루트 방향).
--   전투: 공격 = 몽타주(AttackHit 노티파이 시점에 앞쪽 OverlapSphere → IsEnemy 스크립트에 ApplyDamage), 입력 버퍼로 3연타.
--     달리는 중 공격은 상체 슬롯(UpperBody)으로 다리는 계속 달린다. 노티파이가 오지 않으면 예상 시각 + 여유에 대신 판정한다.
--   조작 (Config/Input.json 액션): Move, Attack(마우스 왼쪽/J), Skill1(Q 회전베기), Skill2(R 돌진 찌르기), Dodge(Space 구르기),
--     Sprint(왼쪽 Shift), Block(마우스 오른쪽/K)
-- 공개 메서드 (Phase 45 공통 계약): GetStats, GetSkillCooldowns, EquipWeapon, EquipShield, RestoreHealth, RestoreMana, IsDead,
--   GetAttackPower, Respawn([위치])
local PlayerController = {
	Properties = {
		Camera               = "Camera", -- 따라갈 카메라 엔티티 이름 (직교 쿼터뷰)
		RunSpeed             = 345.0,    -- cm/s, Running_A 발 속도
		SprintSpeed          = 520.0,    -- cm/s, Running_A ×1.5
		Acceleration         = 7.0,      -- 입력 크기 램프 (지수, 1/초)
		Deceleration         = 12.0,
		TurnSpeed            = 14.0,     -- 몸 방향 보간 (지수, 1/초)
		MaxTurnRate          = 1080.0,   -- 도/초
		MaxMana              = 100.0,
		MaxStamina           = 100.0,
		ManaRegen            = 4.0,      -- 초당
		StaminaRegen         = 32.0,     -- 초당
		StaminaRegenDelay    = 0.7,      -- 스태미나를 쓴 뒤 회복 시작까지 (초)
		SprintCost           = 16.0,     -- 초당
		BaseAttack           = 10.0,
		CritChance           = 0.12,
		DefaultWeapon        = "Asset/KayKit/Weapons/sword_1handed.gltf",
		DefaultWeaponDamage  = 8.0,
		DefaultShield        = "Asset/KayKit/Weapons/shield_badge.gltf",
		DefaultShieldDefense = 2.0,
		SpinCost             = 30.0,     -- 마나
		SpinCooldown         = 6.0,
		DashCost             = 20.0,     -- 마나
		DashCooldown         = 4.0,
		DodgeCost            = 22.0,     -- 스태미나
		DodgeCooldown        = 0.7,
		BlockCost            = 12.0,     -- 막을 때마다 스태미나
		Debug                = false,    -- 판정 구를 디버그 선으로 그린다
	},
}

-- ---------------------------------------------------------------- 데이터

local AudioDir    = "Audio/RPG/"
local ParticleDir = "Particles/RPG/"

-- 3연타: 클립, 배속, 노티파이 시각(클립 초 — Knight*.glb.emeta와 같게), 판정 반경/거리, 데미지 배율, 연타 가능 시점·끝 시점(실제 초)
local Combo = {
	{ Clip = "1H_Melee_Attack_Slice_Diagonal",   Speed = 1.35, HitTime = 0.38, Reach = 95, Radius = 105, Damage = 1.0, Chain = 0.34, End = 0.62, Sound = "Swing1.wav", Lunge = 0.35 },
	{ Clip = "1H_Melee_Attack_Slice_Horizontal", Speed = 1.35, HitTime = 0.24, Reach = 90, Radius = 115, Damage = 1.1, Chain = 0.26, End = 0.58, Sound = "Swing2.wav", Lunge = 0.35 },
	{ Clip = "1H_Melee_Attack_Chop",             Speed = 1.3,  HitTime = 0.55, Reach = 105, Radius = 110, Damage = 1.7, Chain = 99,   End = 0.80, Sound = "Swing3.wav", Lunge = 0.5, Heavy = true },
}
local ComboGrace      = 0.35 -- 공격이 끝난 뒤 이 시간 안에 누르면 다음 단계로 이어진다
local SpinClip        = "2H_Melee_Attack_Spin"
local SpinSpeed       = 1.35
local SpinHitTimes    = { 0.65, 0.92, 1.18 } -- 클립 초 (노티파이 SpinHit와 같게)
local DashClip        = "1H_Melee_Attack_Stab"
local DashSpeed       = 1.5
local DashStart       = 0.15 -- 찌르기 클립 시작 위치 (예비 동작 생략)
local DashHitTime     = 0.36 -- 클립 초 (노티파이 AttackHit)
local DashDuration    = 0.24 -- 돌진 이동 시간 (초)
local DashMoveSpeed   = 1450.0
local DodgeClip       = "Dodge_Forward"
local DodgeDuration   = 0.42
local DodgeMoveSpeed  = 720.0
local DodgeInvulnTime = 0.36
local DodgeBlendIn    = 0.05
local DodgeBlendOut   = 0.12
-- Dodge_Forward의 엉덩이 앞쪽 이동 (m, 0.05초 간격 — glTF 분석값). 캡슐이 직접 움직이므로 몸 메시를 그만큼 뒤로 당겨 이중 이동/끝 튐을 없앤다
local DodgeHipsCurve  = { 0.0, 0.07, 0.17, 0.23, 0.28, 0.32, 0.33, 0.33, 0.33 }
local HitClips        = { "Hit_A", "Hit_B" }
local HitStunTime     = 0.38
local MeshBaseZ       = -90.0

-- ---------------------------------------------------------------- 수학 도우미

local function YawOf(V)
	return math.deg(math.atan(V.Y, V.X))
end

local function WrapAngle(A)
	return (A + 180.0) % 360.0 - 180.0
end

local function DirOfYaw(Yaw)
	local R = math.rad(Yaw)
	return Vector3(math.cos(R), math.sin(R), 0)
end

local function Smoothstep(T)
	T = math.max(0.0, math.min(1.0, T))
	return T * T * (3.0 - 2.0 * T)
end

local function Clamp(V, Lo, Hi)
	return math.max(Lo, math.min(Hi, V))
end

-- ---------------------------------------------------------------- 시작

function PlayerController:OnStart()
	self.IsPlayer  = true
	self.Movement  = self.entity:GetComponent("CharacterMovementComponent")
	self.Health    = self.entity:GetComponent("HealthComponent")
	self.Mesh      = self.entity:FindChild("PlayerMesh")
	if not self.Movement or not self.Health or not self.Mesh then
		Log.Error("PlayerController: CharacterMovementComponent / HealthComponent / 자식 PlayerMesh가 필요합니다")
	end
	self.MeshAnim  = self.Mesh and self.Mesh:GetComponent("AnimationComponent")

	local P = self.Properties
	self.Mana          = P.MaxMana
	self.Stamina       = P.MaxStamina
	self.StaminaIdle   = 0.0   -- 마지막 스태미나 사용 뒤 시간
	self.bExhausted    = false -- 스태미나를 다 쓰면 일정량 회복까지 질주 불가
	self.Cooldowns     = { Skill1 = 0.0, Skill2 = 0.0, Dodge = 0.0 }
	self.SmoothInput   = Vector3(0, 0, 0)
	self.FacingYaw     = YawOf(self.entity:GetForward())
	self.Action        = nil   -- 진행 중 행동 {Kind=..., Elapsed=...} (nil = 자유 이동)
	self.ComboStage    = 0     -- 마지막으로 낸 콤보 단계 (0 = 없음)
	self.ComboGraceLeft = 0.0
	self.Buffered      = {}    -- 액션 이름 → 남은 버퍼 시간
	self.BlockWeight   = 0.0
	self.bBlocking     = false
	self.HitStop       = 0.0
	self.Shake         = 0.0
	self.ShakeStrength = 0.0
	self.InvulnUntil   = 0.0
	self.HitClipIndex  = 1
	self.SpawnPosition = self.entity:GetPosition()
	self.CurrentSpeedSetting = -1

	-- 기본 장비: 프리팹 자식(Weapon/Shield)이 있으면 그대로 쓰고 수치만 맞춘다
	self.WeaponEntity  = self.entity:FindChild("Weapon")
	self.ShieldEntity  = self.entity:FindChild("Shield")
	self:EquipWeapon({ Id = "DefaultSword", Name = "기사의 검", Type = "Weapon", Model = P.DefaultWeapon, Damage = P.DefaultWeaponDamage })
	self:EquipShield({ Id = "DefaultShield", Name = "문장 방패", Type = "Shield", Model = P.DefaultShield, Defense = P.DefaultShieldDefense })

	-- 쿼터뷰 카메라 (IsoPlayer와 같은 방식): 시작 시 시선이 발 높이 평면과 만나는 점을 캐릭터에 맞춘다
	self.Camera = Scene.Find(P.Camera)
	if self.Camera then
		local Forward = self.Camera:GetForward()
		local Height  = self.Camera:GetWorldPosition().Z - self.entity:GetWorldPosition().Z
		self.CameraOffset = Forward * (Height / Forward.Z)
		self.ScreenUp     = Vector3(Forward.X, Forward.Y, 0):Normalized()
		self.ScreenRight  = Vector3(-self.ScreenUp.Y, self.ScreenUp.X, 0) -- Z-up 왼손: 앞에서 +90도 = 오른쪽
		self:PlaceCamera(0)
	else
		Log.Warn("PlayerController: 카메라를 찾지 못함 — 월드 축 기준으로 움직입니다", P.Camera)
		self.ScreenUp    = Vector3(1, 0, 0)
		self.ScreenRight = Vector3(0, 1, 0)
	end
end

function PlayerController:GetFootPosition()
	if self.Mesh then
		return self.Mesh:GetWorldPosition()
	end
	return self.entity:GetWorldPosition() + Vector3(0, 0, MeshBaseZ)
end

function PlayerController:GetGameManager()
	if self.GameManager == nil or (self.GameManagerEntity and not self.GameManagerEntity:IsValid()) then
		local Now = Time.TotalTime
		if self.NextGameManagerLookup and Now < self.NextGameManagerLookup then
			return nil
		end
		self.NextGameManagerLookup = Now + 1.0 -- 없으면 1초에 한 번만 다시 찾는다
		self.GameManagerEntity = Scene.Find("GameManager")
		self.GameManager       = self.GameManagerEntity and self.GameManagerEntity:GetScript() or nil
	end
	return self.GameManager
end

-- ---------------------------------------------------------------- 매 프레임

function PlayerController:OnUpdate(dt)
	local P = self.Properties
	self:TickResources(dt)
	self:TickHitStop(dt)

	if self:IsDead() then
		self:SetMoveSpeed(P.RunSpeed)
		self.SmoothInput = Vector3(0, 0, 0)
		return
	end

	-- 입력 (화면 기준 이동 방향)
	local MoveX, MoveY = Input.GetAction("Move")
	local Move = self.ScreenUp * MoveY + self.ScreenRight * MoveX
	local MoveMag = math.min(1.0, Move:Length())
	local MoveDir = MoveMag > 0.05 and Move:Normalized() or nil

	for _, Name in ipairs({ "Attack", "Skill1", "Skill2", "Dodge" }) do
		self.Buffered[Name] = math.max(0.0, (self.Buffered[Name] or 0.0) - dt)
		if Input.WasActionPressed(Name) then
			self.Buffered[Name] = Name == "Attack" and 0.45 or 0.25
		end
	end
	-- 공격을 누르고 있으면 계속 이어 친다 (버퍼를 짧게 유지)
	if Input.IsActionPressed("Attack") and self.Buffered.Attack < 0.1 then
		self.Buffered.Attack = 0.1
	end

	if self.ComboGraceLeft > 0.0 then
		self.ComboGraceLeft = self.ComboGraceLeft - dt
		if self.ComboGraceLeft <= 0.0 then
			self.ComboStage = 0
		end
	end

	-- 행동 진행 (취소 가능하면 새 행동이 끊는다)
	if self.Action then
		-- 히트 스톱 동안은 애니메이션이 거의 멈추므로 행동 시간도 같이 늦춘다 (노티파이 대체 판정 시각이 어긋나지 않게)
		self.Action.Elapsed = self.Action.Elapsed + (self.HitStop > 0.0 and dt * 0.05 or dt)
		self:TickAction(dt, MoveDir, MoveMag)
	end
	if not self.Action or self.Action.bCancelable then
		self:TryStartAction(MoveDir)
	end

	-- 자유 이동 (행동 중이면 행동이 이동을 정한다)
	if not self.Action then
		self:TickLocomotion(dt, MoveDir, MoveMag)
	end
	self:TickBlockWeight(dt)
	self:TickFacing(dt)
end

function PlayerController:TickResources(dt)
	local P = self.Properties
	for Name, Remaining in pairs(self.Cooldowns) do
		self.Cooldowns[Name] = math.max(0.0, Remaining - dt)
	end
	self.Mana = math.min(P.MaxMana, self.Mana + P.ManaRegen * dt)
	self.StaminaIdle = self.StaminaIdle + dt
	if self.StaminaIdle >= P.StaminaRegenDelay then
		self.Stamina = math.min(P.MaxStamina, self.Stamina + P.StaminaRegen * dt)
	end
	if self.bExhausted and self.Stamina >= P.MaxStamina * 0.3 then
		self.bExhausted = false
	end
	if self.InvulnUntil > 0.0 and Time.TotalTime >= self.InvulnUntil then
		self.InvulnUntil = 0.0
		self:SetInvulnerable(false)
	end
end

function PlayerController:UseStamina(Amount)
	self.Stamina     = math.max(0.0, self.Stamina - Amount)
	self.StaminaIdle = 0.0
	if self.Stamina <= 0.0 then
		self.bExhausted = true
	end
end

function PlayerController:SetMoveSpeed(Speed)
	if self.Movement and self.CurrentSpeedSetting ~= Speed then
		self.Movement.MaxWalkSpeed = Speed
		self.CurrentSpeedSetting   = Speed
	end
end

-- 자유 이동: 입력 크기·방향을 지수 램프로 따라가 가감속을 만든다 (엔진 이동은 입력 × 최고 속도를 그대로 쓴다)
function PlayerController:TickLocomotion(dt, MoveDir, MoveMag)
	local P = self.Properties
	self.bBlocking = Input.IsActionPressed("Block") and self.ShieldEntity ~= nil
	local bSprint = Input.IsActionPressed("Sprint") and MoveDir ~= nil and not self.bExhausted and not self.bBlocking
	if bSprint then
		self:UseStamina(P.SprintCost * dt)
	end
	self:SetMoveSpeed(bSprint and P.SprintSpeed or P.RunSpeed)

	local Scale  = self.bBlocking and 0.35 or 1.0
	local Target = MoveDir and (MoveDir * (MoveMag * Scale)) or Vector3(0, 0, 0)
	local Rate   = Target:LengthSquared() >= self.SmoothInput:LengthSquared() and P.Acceleration or P.Deceleration
	self.SmoothInput = Vector3.Lerp(self.SmoothInput, Target, 1.0 - math.exp(-Rate * dt))
	if self.SmoothInput:LengthSquared() > 0.0004 then
		self.entity:AddMovementInput(self.SmoothInput)
	else
		self.SmoothInput = Vector3(0, 0, 0)
	end
	if MoveDir then
		self.TargetYaw = YawOf(MoveDir)
	end
end

function PlayerController:TickBlockWeight(dt)
	local Target = (self.bBlocking and not self.Action) and 1.0 or 0.0
	local Rate   = Target > self.BlockWeight and 14.0 or 10.0
	self.BlockWeight = self.BlockWeight + (Target - self.BlockWeight) * (1.0 - math.exp(-Rate * dt))
	if math.abs(self.BlockWeight - (self.LastBlockWeight or -1)) > 0.002 then
		self.entity:SetAnimParam("BlockWeight", self.BlockWeight)
		self.LastBlockWeight = self.BlockWeight
	end
end

function PlayerController:TickFacing(dt)
	if not self.TargetYaw then
		return
	end
	local P     = self.Properties
	local Delta = WrapAngle(self.TargetYaw - self.FacingYaw)
	local Boost = self.Action and 2.5 or 1.0 -- 공격/스킬은 빨리 돌아선다
	local Step  = Delta * (1.0 - math.exp(-P.TurnSpeed * Boost * dt))
	local Max   = P.MaxTurnRate * Boost * dt
	self.FacingYaw = WrapAngle(self.FacingYaw + Clamp(Step, -Max, Max))
end

function PlayerController:GetFacing()
	return DirOfYaw(self.FacingYaw)
end

-- ---------------------------------------------------------------- 행동 시작

function PlayerController:TryStartAction(MoveDir)
	local B = self.Buffered
	local Current = self.Action and self.Action.Kind
	if B.Dodge > 0.0 and self:CanDodge() then
		B.Dodge = 0.0
		self:StartDodge(MoveDir)
	elseif B.Skill1 > 0.0 and Current ~= "Skill1" and self:CanUseSkill("Skill1") then
		B.Skill1 = 0.0
		self:StartSpin()
	elseif B.Skill2 > 0.0 and Current ~= "Skill2" and self:CanUseSkill("Skill2") then
		B.Skill2 = 0.0
		self:StartDash(MoveDir)
	elseif B.Attack > 0.0 and (Current == nil or Current == "Attack") then
		B.Attack = 0.0
		self:StartAttack(MoveDir)
	end
end

function PlayerController:CanDodge()
	return self.Cooldowns.Dodge <= 0.0 and self.Stamina >= self.Properties.DodgeCost * 0.5
end

function PlayerController:CanUseSkill(Name)
	local P    = self.Properties
	local Cost = Name == "Skill1" and P.SpinCost or P.DashCost
	if self.Cooldowns[Name] > 0.0 then
		return false
	end
	if self.Mana < Cost then
		if not self.LastNoManaLog or Time.TotalTime - self.LastNoManaLog > 1.0 then
			self.LastNoManaLog = Time.TotalTime
			Log.Info("[Player] 마나 부족", Name, string.format("%.0f/%.0f", self.Mana, Cost))
			local GM = self:GetGameManager()
			if GM and GM.Notify then GM:Notify("마나가 부족합니다") end
		end
		return false
	end
	return true
end

-- 지금 공격할 방향: 입력이 있으면 입력 방향, 없으면 보고 있는 방향
function PlayerController:AimDirection(MoveDir)
	return MoveDir or self:GetFacing()
end

function PlayerController:BeginAction(Kind)
	-- 이전 행동 정리 (무적/속도/메시 위치)
	if self.Action and self.Action.Kind == "Dodge" then
		self:EndDodgeMesh()
	end
	self.Action = { Kind = Kind, Elapsed = 0.0, bCancelable = false }
	self.bBlocking = false
	return self.Action
end

function PlayerController:EndAction()
	if self.Action and self.Action.Kind == "Dodge" then
		self:EndDodgeMesh()
	end
	if self.Action and self.Action.Kind == "Attack" then
		self.ComboGraceLeft = self.ComboStage < #Combo and ComboGrace or 0.0
		if self.ComboStage >= #Combo then
			self.ComboStage = 0
		end
	end
	self.Action = nil
	self:SetMoveSpeed(self.Properties.RunSpeed)
end

-- ---- 콤보 공격
function PlayerController:StartAttack(MoveDir)
	local Stage = self.ComboStage % #Combo + 1
	if self.Action == nil and self.ComboGraceLeft <= 0.0 then
		Stage = 1
	end
	local Def = Combo[Stage]
	-- 달리는 중이면 상체만 휘두르고 다리는 계속 달린다
	local Speed    = self.SmoothInput:Length()
	local bRunning = Speed > 0.55 and MoveDir ~= nil
	local Slot     = bRunning and "UpperBody" or ""
	local Previous = self.Action
	if Previous and Previous.Slot ~= nil and Previous.Slot ~= Slot then
		self.entity:StopMontage(Previous.Slot, 0.12)
	end
	local A = self:BeginAction("Attack")
	A.Stage    = Stage
	A.Def      = Def
	A.Slot     = Slot
	A.bRunning = bRunning
	A.HitDone  = false
	A.Dir      = self:AimDirection(MoveDir)
	self.ComboStage     = Stage
	self.ComboGraceLeft = 0.0
	self.TargetYaw      = YawOf(A.Dir)
	self.entity:PlayMontage(Def.Clip, { Slot = Slot, BlendIn = 0.08, BlendOut = 0.22, Speed = Def.Speed })
	Audio.PlayOneShot(AudioDir .. Def.Sound)
end

-- ---- 회전베기 (스킬 1): 주변 원형 판정 3회, 이동은 느리게 가능, 피격에 끊기지 않는다
function PlayerController:StartSpin()
	local P = self.Properties
	self.Mana = self.Mana - P.SpinCost
	self.Cooldowns.Skill1 = P.SpinCooldown
	if self.Action and self.Action.Slot and self.Action.Slot ~= "" then
		self.entity:StopMontage(self.Action.Slot, 0.1)
	end
	local A = self:BeginAction("Skill1")
	A.Slot      = ""
	A.HitsDone  = 0
	A.Duration  = 2.4 / SpinSpeed - 0.15
	A.HitSet    = nil
	self.ComboStage = 0
	self.entity:PlayMontage(SpinClip, { BlendIn = 0.1, BlendOut = 0.25, Speed = SpinSpeed })
	Audio.PlayOneShot(AudioDir .. "Spin.wav")
	Log.Info("[Player] 스킬: 회전베기")
end

-- ---- 돌진 찌르기 (스킬 2): 앞으로 빠르게 미끄러지며 지나간 적을 한 번씩 찌른다
function PlayerController:StartDash(MoveDir)
	local P = self.Properties
	self.Mana = self.Mana - P.DashCost
	self.Cooldowns.Skill2 = P.DashCooldown
	if self.Action and self.Action.Slot and self.Action.Slot ~= "" then
		self.entity:StopMontage(self.Action.Slot, 0.1)
	end
	local A = self:BeginAction("Skill2")
	A.Slot     = ""
	A.Dir      = self:AimDirection(MoveDir)
	A.HitSet   = {}
	A.HitDone  = false
	A.Duration = 0.72
	self.ComboStage = 0
	self.TargetYaw  = YawOf(A.Dir)
	self.FacingYaw  = self.TargetYaw -- 돌진은 바로 그 방향
	self.entity:PlayMontage(DashClip, { BlendIn = 0.06, BlendOut = 0.25, Speed = DashSpeed, StartTime = DashStart })
	Audio.PlayOneShot(AudioDir .. "Dash.wav")
	self:SpawnEffect("DustPuff", self:GetFootPosition() + Vector3(0, 0, 10), 1.2)
	Log.Info("[Player] 스킬: 돌진 찌르기")
end

-- ---- 구르기: 입력 방향으로, 잠깐 무적
function PlayerController:StartDodge(MoveDir)
	local P = self.Properties
	if self.Action and self.Action.Slot and self.Action.Slot ~= "" then
		self.entity:StopMontage(self.Action.Slot, 0.08)
	end
	self:UseStamina(P.DodgeCost)
	self.Cooldowns.Dodge = P.DodgeCooldown
	local A = self:BeginAction("Dodge")
	A.Slot = ""
	A.Dir  = self:AimDirection(MoveDir)
	self.ComboStage = 0
	self.ComboGraceLeft = 0.0
	self.TargetYaw = YawOf(A.Dir)
	self.FacingYaw = self.TargetYaw -- 구르는 방향과 몸 방향이 같아야 한다
	self.SmoothInput = A.Dir * 0.6  -- 끝나고 이어 달릴 때 갑자기 서지 않게
	self:SetInvulnerable(true)
	self.InvulnUntil = Time.TotalTime + DodgeInvulnTime
	self.entity:PlayMontage(DodgeClip, { BlendIn = DodgeBlendIn, BlendOut = DodgeBlendOut, Speed = 1.0 })
	Audio.PlayOneShot(AudioDir .. "Dodge.wav")
	self:SpawnEffect("DustPuff", self:GetFootPosition() + Vector3(0, 0, 10), 1.2)
end

-- ---------------------------------------------------------------- 행동 진행

function PlayerController:TickAction(dt, MoveDir, MoveMag)
	local A = self.Action
	local P = self.Properties
	if A.Kind == "Attack" then
		local Def = A.Def
		local HitAt = Def.HitTime / Def.Speed
		if A.bRunning then
			-- 달리며 베기: 감속해서 계속 달린다
			self:SetMoveSpeed(P.RunSpeed)
			local Dir = MoveDir or A.Dir
			self.SmoothInput = Vector3.Lerp(self.SmoothInput, Dir * 0.75, 1.0 - math.exp(-8.0 * dt))
			self.entity:AddMovementInput(self.SmoothInput)
			self.TargetYaw = YawOf(Dir)
		else
			-- 제자리 베기: 판정 직전에 반 걸음 내딛는다
			self:SetMoveSpeed(P.RunSpeed)
			local Lunge = (A.Elapsed > HitAt - 0.14 and A.Elapsed < HitAt + 0.02) and Def.Lunge or 0.0
			self.SmoothInput = A.Dir * Lunge
			if Lunge > 0.0 then
				self.entity:AddMovementInput(self.SmoothInput)
			end
		end
		if not A.HitDone and A.Elapsed > HitAt + 0.25 then
			self:WarnMissingNotify("AttackHit", Def.Clip)
			self:DoAttackHit()
		end
		-- 판정 뒤: 다음 타는 Chain 시점부터, 구르기/스킬은 바로 끊을 수 있다
		local B = self.Buffered
		A.bCancelable = A.HitDone and (A.Elapsed >= Def.Chain or B.Dodge > 0.0 or B.Skill1 > 0.0 or B.Skill2 > 0.0)
		if A.Elapsed >= Def.End then
			-- 이동 입력이 있으면 남은 회복 동작을 끊고 바로 움직인다
			if A.Slot == "" and MoveDir then
				self.entity:StopMontage(nil, 0.18)
			end
			self:EndAction()
		end
	elseif A.Kind == "Skill1" then
		self:SetMoveSpeed(P.RunSpeed * 0.45)
		local Target = MoveDir and MoveDir * MoveMag or Vector3(0, 0, 0)
		self.SmoothInput = Vector3.Lerp(self.SmoothInput, Target, 1.0 - math.exp(-6.0 * dt))
		if self.SmoothInput:LengthSquared() > 0.0004 then
			self.entity:AddMovementInput(self.SmoothInput)
		end
		local NextHit = SpinHitTimes[A.HitsDone + 1]
		if NextHit and A.Elapsed > NextHit / SpinSpeed + 0.25 then
			self:WarnMissingNotify("SpinHit", SpinClip)
			self:DoSpinHit()
		end
		A.bCancelable = A.HitsDone >= #SpinHitTimes -- 마지막 타 뒤에는 구르기/공격으로 끊을 수 있다
		if A.Elapsed >= A.Duration then
			self:EndAction()
		end
	elseif A.Kind == "Skill2" then
		if A.Elapsed < DashDuration then
			-- 돌진: 빠른 속도로 미끄러지며 지나가는 적을 판정
			local T = A.Elapsed / DashDuration
			self:SetMoveSpeed(DashMoveSpeed * (1.0 - 0.5 * T * T))
			self.SmoothInput = A.Dir
			self.entity:AddMovementInput(A.Dir)
			self:DamageInSphere(self.entity:GetWorldPosition() + A.Dir * 60.0, 85.0, nil, 2.2, A.HitSet, false, "Dash")
		else
			self:SetMoveSpeed(P.RunSpeed)
			self.SmoothInput = Vector3(0, 0, 0)
		end
		local HitAt = (DashHitTime - DashStart) / DashSpeed
		if not A.HitDone and A.Elapsed > math.max(HitAt, DashDuration) + 0.25 then
			self:WarnMissingNotify("AttackHit", DashClip)
			self:DoDashThrust()
		end
		A.bCancelable = A.Elapsed > DashDuration + 0.2
		if A.Elapsed >= A.Duration then
			if MoveDir then
				self.entity:StopMontage(nil, 0.18)
			end
			self:EndAction()
		end
	elseif A.Kind == "Dodge" then
		local T = A.Elapsed / DodgeDuration
		self:SetMoveSpeed(DodgeMoveSpeed * (1.0 - 0.55 * T * T))
		self.entity:AddMovementInput(A.Dir)
		self:UpdateDodgeMesh(A.Elapsed)
		A.bCancelable = A.Elapsed > DodgeDuration - 0.1
		if A.Elapsed >= DodgeDuration then
			self:EndAction()
		end
	elseif A.Kind == "Hit" then
		self:SetMoveSpeed(P.RunSpeed)
		self.SmoothInput = Vector3(0, 0, 0)
		A.bCancelable = A.Elapsed > HitStunTime * 0.6 and self.Buffered.Dodge > 0.0 -- 피격 경직은 구르기로만 빨리 끊는다
		if A.Elapsed >= HitStunTime then
			self:EndAction()
		end
	end
end

function PlayerController:WarnMissingNotify(Name, Clip)
	if not self.bWarnedNotify then
		self.bWarnedNotify = true
		Log.Warn("[Player] 노티파이", Name, "가 오지 않아 시간으로 판정합니다 (클립", Clip, ") — KnightBare.glb.emeta 확인")
	end
end

-- 구르기 클립은 엉덩이가 앞으로 33cm 나가므로 몸 메시를 그만큼 뒤로 당긴다 (몽타주 가중치까지 반영 — 끝에서 튀지 않음)
function PlayerController:UpdateDodgeMesh(Elapsed)
	if not self.Mesh then return end
	local F     = Clamp(Elapsed / 0.05, 0.0, #DodgeHipsCurve - 1)
	local Index = math.floor(F)
	local Frac  = F - Index
	local A0    = DodgeHipsCurve[Index + 1]
	local A1    = DodgeHipsCurve[math.min(Index + 2, #DodgeHipsCurve)]
	local Hips  = (A0 + (A1 - A0) * Frac) * 100.0
	local Weight = Smoothstep(Elapsed / DodgeBlendIn) * Smoothstep((0.4 - Elapsed) / DodgeBlendOut)
	self.DodgeMeshOffset = Hips * Weight
end

function PlayerController:EndDodgeMesh()
	self.DodgeMeshOffset = 0.0
end

-- ---------------------------------------------------------------- 판정

function PlayerController:FindEnemyScript(Entity)
	local Current = Entity
	for _ = 1, 4 do
		if not Current then return nil end
		local Script = Current:GetScript()
		if Script and Script.IsEnemy then
			return Script, Current
		end
		Current = Current:GetParent()
	end
	return nil
end

local function IsScriptDead(Script, Entity)
	if Script.IsDead then
		local Ok, Dead = pcall(Script.IsDead, Script)
		if Ok then return Dead end
	end
	return Entity:IsDead()
end

-- 구 안의 살아 있는 적에게 데미지. Forward가 있으면 그 반대쪽(등 뒤)은 뺀다. HitSet에 이미 있는 적은 건너뛴다
function PlayerController:DamageInSphere(Center, Radius, Forward, Multiplier, HitSet, bHeavy, Label)
	if self.Properties.Debug then
		Debug.DrawSphere(Center, Radius, Vector3(1.0, 0.3, 0.2), 0.3)
	end
	local Count = 0
	local Seen  = {}
	local MyPos = self.entity:GetWorldPosition()
	for _, Hit in ipairs(Physics.OverlapSphere(Center, Radius, self.entity)) do
		local Script, Target = self:FindEnemyScript(Hit)
		if Script and not Seen[Target.Id] and not (HitSet and HitSet[Target.Id]) and not IsScriptDead(Script, Target) then
			Seen[Target.Id] = true
			local ToTarget = Target:GetWorldPosition() - MyPos
			ToTarget = Vector3(ToTarget.X, ToTarget.Y, 0)
			local bInFront = true
			if Forward and ToTarget:LengthSquared() > 400.0 then
				bInFront = Vector3.Dot(ToTarget:Normalized(), Forward) > -0.25
			end
			if bInFront then
				if HitSet then HitSet[Target.Id] = true end
				self:DealDamage(Script, Target, Multiplier, bHeavy, Label)
				Count = Count + 1
			end
		end
	end
	return Count
end

function PlayerController:DealDamage(Script, Target, Multiplier, bHeavy, Label)
	local P = self.Properties
	local Amount = self:GetAttackPower() * Multiplier * (0.9 + math.random() * 0.2)
	local bCrit  = math.random() < P.CritChance
	if bCrit then
		Amount = Amount * 1.5
	end
	Amount = math.floor(Amount + 0.5)
	local Dealt = Target:ApplyDamage(Amount, self.entity)
	if Dealt <= 0 then
		return
	end
	local MyPos     = self.entity:GetWorldPosition()
	local TargetPos = Target:GetWorldPosition()
	local Away      = Vector3(TargetPos.X - MyPos.X, TargetPos.Y - MyPos.Y, 0)
	Away = Away:LengthSquared() > 1.0 and Away:Normalized() or self:GetFacing()
	local HitPos = Vector3(TargetPos.X, TargetPos.Y, MyPos.Z + 15.0) - Away * 30.0
	Log.Info(string.format("[Player] %s 명중: %s 피해 %d%s", Label or "공격", Target:GetName(), Dealt, bCrit and " (치명타)" or ""))

	self:SpawnEffect((bHeavy or bCrit) and "HeavyHit" or "HitSpark", HitPos, 1.0)
	Audio.PlayOneShot(AudioDir .. ((bHeavy or bCrit) and "HitHeavy.wav" or "Hit.wav"))
	self:StartHitStop((bHeavy or bCrit) and 0.09 or 0.05)
	self:AddShake((bHeavy or bCrit) and 7.0 or 3.5, 0.15)
	-- 적이 넉백을 지원하면 (트랙 C 선택 메서드)
	if Script.ApplyKnockback then
		pcall(Script.ApplyKnockback, Script, Away, (bHeavy or bCrit) and 320.0 or 150.0)
	end
	local GM = self:GetGameManager()
	if GM and GM.ShowDamageNumber then
		GM:ShowDamageNumber(HitPos + Vector3(0, 0, 40), Dealt, bCrit and "Crit" or "Normal")
	end
end

function PlayerController:DoAttackHit()
	local A = self.Action
	if not A or A.HitDone then return end
	A.HitDone = true
	local Def    = A.Def
	local Facing = A.Dir -- 몸이 아직 다 돌지 않았어도 공격 방향 기준
	local Center = self.entity:GetWorldPosition() + Facing * Def.Reach
	self:DamageInSphere(Center, Def.Radius, Facing, Def.Damage, nil, Def.Heavy, string.format("%d타", A.Stage))
end

function PlayerController:DoSpinHit()
	local A = self.Action
	if not A or A.Kind ~= "Skill1" or A.HitsDone >= #SpinHitTimes then return end
	A.HitsDone = A.HitsDone + 1
	local Center = self.entity:GetWorldPosition()
	self:DamageInSphere(Center, 230.0, nil, 1.15, nil, A.HitsDone == #SpinHitTimes, "회전베기")
	self:SpawnEffect("SpinWave", self:GetFootPosition() + Vector3(0, 0, 45), 1.0)
	if A.HitsDone > 1 then
		Audio.PlayOneShot(AudioDir .. "Swing2.wav")
	end
end

function PlayerController:DoDashThrust()
	local A = self.Action
	if not A or A.Kind ~= "Skill2" or A.HitDone then return end
	A.HitDone = true
	local Facing = self:GetFacing()
	self:DamageInSphere(self.entity:GetWorldPosition() + Facing * 100.0, 100.0, Facing, 2.2, A.HitSet, true, "돌진 찌르기")
end

-- 애니메이션 노티파이 (Knight*.glb.emeta) — 모델 루트에 스크립트가 없으므로 조상인 이 스크립트가 받는다
function PlayerController:OnAnimNotify_AttackHit()
	if not self.Action then return end
	if self.Action.Kind == "Attack" then
		self:DoAttackHit()
	elseif self.Action.Kind == "Skill2" then
		self:DoDashThrust()
	end
end

function PlayerController:OnAnimNotify_SpinHit()
	if self.Action and self.Action.Kind == "Skill1" then
		self:DoSpinHit()
	end
end

function PlayerController:OnMontageEnded(Clip, bInterrupted, Slot)
	-- 행동 끝은 시간으로 정한다 (몽타주가 일찍 끊겨도 상태가 남지 않게 회전베기/피격만 여기서도 정리)
	if self.Action and not bInterrupted then
		if (self.Action.Kind == "Skill1" and Clip == SpinClip) then
			self:EndAction()
		end
	end
end

-- ---------------------------------------------------------------- 피격 / 사망

function PlayerController:SetInvulnerable(bValue)
	if self.Health then
		self.Health.Invulnerable = bValue
	end
end

function PlayerController:OnDamaged(Amount, Instigator)
	local P = self.Properties
	if self:IsDead() then
		-- 이번 피해로 죽었다: 숫자만 띄우고 연출은 OnDeath가 한다
		local GM = self:GetGameManager()
		if GM and GM.ShowDamageNumber then
			GM:ShowDamageNumber(self.entity:GetWorldPosition() + Vector3(0, 0, 90), math.floor(Amount + 0.5), "Player")
		end
		return
	end
	-- 막기: 앞에서 온 공격이면 대부분 되돌린다 (ApplyDamage는 이미 깎았으므로 회복으로 보정)
	local Refund   = 0.0
	local bBlocked = false
	local FromDir  = nil
	if Instigator and Instigator:IsValid() then
		local D = Instigator:GetWorldPosition() - self.entity:GetWorldPosition()
		D = Vector3(D.X, D.Y, 0)
		if D:LengthSquared() > 1.0 then FromDir = D:Normalized() end
	end
	if self.bBlocking and self.BlockWeight > 0.5 and self.Stamina >= P.BlockCost * 0.5 and (FromDir == nil or Vector3.Dot(FromDir, self:GetFacing()) > 0.2) then
		bBlocked = true
		Refund   = Amount * 0.8
		self:UseStamina(P.BlockCost)
	end
	Refund = Refund + math.min(self.ShieldDefense or 0.0, math.max(0.0, Amount - Refund - 1.0))
	if Refund > 0.0 then
		self.entity:Heal(Refund)
	end
	local Taken = math.floor(Amount - Refund + 0.5)
	Log.Info(string.format("[Player] 피격: %s 피해 %d%s (체력 %.0f/%.0f)", Instigator and Instigator:IsValid() and Instigator:GetName() or "?",
		Taken, bBlocked and " (막음)" or "", self.Health.Health, self.Health.MaxHealth))

	local GM = self:GetGameManager()
	if GM and GM.ShowDamageNumber then
		GM:ShowDamageNumber(self.entity:GetWorldPosition() + Vector3(0, 0, 90), Taken, "Player")
	end
	local ChestPos = self.entity:GetWorldPosition() + Vector3(0, 0, 20) + (FromDir or self:GetFacing()) * 30.0
	if bBlocked then
		self.entity:PlayMontage("Block_Hit", { Slot = "UpperBody", BlendIn = 0.05, BlendOut = 0.2, Speed = 1.6 })
		self:SpawnEffect("BlockSpark", ChestPos, 0.8)
		Audio.PlayOneShot(AudioDir .. "Block.wav")
		self:AddShake(2.5, 0.1)
		return
	end
	Audio.PlayOneShot(AudioDir .. "Hurt.wav")
	self:SpawnEffect("HitSpark", ChestPos, 0.8)
	self:AddShake(6.0, 0.2)
	-- 회전베기/돌진 중에는 경직되지 않는다 (슈퍼 아머)
	local Kind = self.Action and self.Action.Kind
	if Kind == "Skill1" or Kind == "Skill2" or Kind == "Dodge" then
		return
	end
	if self.Action and self.Action.Slot and self.Action.Slot ~= "" then
		self.entity:StopMontage(self.Action.Slot, 0.08)
	end
	self:BeginAction("Hit")
	self.ComboStage = 0
	if FromDir then
		self.TargetYaw = YawOf(FromDir) -- 때린 쪽을 돌아본다
	end
	local Clip = HitClips[self.HitClipIndex]
	self.HitClipIndex = self.HitClipIndex % #HitClips + 1
	self.entity:PlayMontage(Clip, { BlendIn = 0.05, BlendOut = 0.2, Speed = 1.25 })
end

function PlayerController:OnDeath(Instigator)
	Log.Info("[Player] 사망", Instigator and Instigator:IsValid() and Instigator:GetName() or "")
	if self.Action and self.Action.Kind == "Dodge" then
		self:EndDodgeMesh()
	end
	self.Action = nil
	self.ComboStage = 0
	self.bBlocking = false
	self.entity:StopMontage(nil, 0.1)
	self.entity:StopMontage("UpperBody", 0.1)
	self.entity:SetAnimParam("Dead", true)
	Audio.PlayOneShot(AudioDir .. "HitHeavy.wav")
	self:AddShake(8.0, 0.3)
	local GM = self:GetGameManager()
	if GM and GM.Notify then GM:Notify("쓰러졌습니다") end
end

-- 되살리기 (통합 단계의 리스폰 규칙이 부른다). 위치를 주면 그곳으로 순간이동, 2초 무적
function PlayerController:Respawn(Position)
	local P = self.Properties
	if Position then
		self.entity:SetPosition(Position)
	end
	self.Health.Health = self.Health.MaxHealth
	self.Mana    = P.MaxMana
	self.Stamina = P.MaxStamina
	self.bExhausted = false
	self.Action = nil
	self.ComboStage = 0
	self.SmoothInput = Vector3(0, 0, 0)
	self.entity:SetAnimParam("Dead", false)
	self:SetInvulnerable(true)
	self.InvulnUntil = Time.TotalTime + 2.0
	Log.Info("[Player] 부활")
end

-- ---------------------------------------------------------------- 연출

function PlayerController:SpawnEffect(Name, Position, Lifetime)
	local Effect = Scene.Create("FX_" .. Name)
	Effect:SetPosition(Position)
	local Particles = Effect:AddComponent("ParticleSystemComponent")
	Particles.Asset = ParticleDir .. Name .. ".eparticle"
	Timer.After(Lifetime or 1.0, function()
		if Effect:IsValid() then
			Effect:Destroy()
		end
	end)
end

-- 히트 스톱: 짧은 순간 몸 애니메이션을 거의 멈춰 타격감을 준다
function PlayerController:StartHitStop(Duration)
	self.HitStop = math.max(self.HitStop, Duration)
	if self.MeshAnim then
		self.MeshAnim.Speed = 0.05
	end
end

function PlayerController:TickHitStop(dt)
	if self.HitStop > 0.0 then
		self.HitStop = self.HitStop - dt
		if self.HitStop <= 0.0 and self.MeshAnim then
			self.MeshAnim.Speed = 1.0
		end
	end
end

function PlayerController:AddShake(Strength, Duration)
	self.ShakeStrength = math.max(self.ShakeStrength * (self.Shake > 0 and 1 or 0), Strength)
	self.Shake         = math.max(self.Shake, Duration)
end

-- 물리 이동이 끝난 뒤: 몸 방향(메시 로컬 회전)과 카메라를 놓는다
function PlayerController:OnLateUpdate(dt)
	if self.Mesh then
		local _, RootYaw = self.entity:GetRotation():ToEuler()
		local Relative   = WrapAngle(self.FacingYaw - RootYaw)
		self.Mesh:SetRotation(Quat.FromEuler(0, Relative + 180.0, 0)) -- 모델은 -X를 본다 (기본 180도)
		local Back = self.DodgeMeshOffset or 0.0
		local R    = math.rad(Relative)
		self.Mesh:SetPosition(Vector3(-math.cos(R) * Back, -math.sin(R) * Back, MeshBaseZ))
	end
	if self.Camera then
		self:PlaceCamera(dt)
	end
end

function PlayerController:PlaceCamera(dt)
	local Offset = Vector3(0, 0, 0)
	if self.Shake > 0.0 then
		self.Shake = self.Shake - dt
		local S = self.ShakeStrength * Clamp(self.Shake / 0.15, 0.0, 1.0)
		Offset = self.ScreenRight * ((math.random() * 2 - 1) * S) + Vector3(0, 0, (math.random() * 2 - 1) * S)
	end
	self.Camera:SetPosition(self.entity:GetWorldPosition() + self.CameraOffset + Offset)
end

-- ---------------------------------------------------------------- 장비

-- 모델을 소켓에 붙인 새 엔티티를 만든다 (같은 모델이면 기존 엔티티 유지)
function PlayerController:AttachItem(Old, Model, Socket, Name)
	if Old and Old:IsValid() then
		local Existing = Old:GetComponent("ModelComponent")
		if Model and Existing and Existing.AssetPath == Model then
			return Old
		end
		Old:Destroy()
	end
	if not Model or Model == "" or not self.Mesh then
		return nil
	end
	local Item = Scene.Create(Name)
	Item:SetParent(self.entity) -- 계층은 정리용 (소켓 부착이 위치를 정한다)
	Item:AddComponent("ModelComponent").AssetPath = Model
	local Attachment = Item:AddComponent("SocketAttachmentComponent")
	Attachment.Target = self.Mesh
	Attachment.Socket = Socket
	return Item
end

-- itemDef = { Model = Content 기준 모델 경로, Damage = 공격력 } (nil이면 맨손)
function PlayerController:EquipWeapon(ItemDef)
	self.WeaponDef    = ItemDef
	self.WeaponDamage = ItemDef and (ItemDef.Damage or 0.0) or 0.0
	self.WeaponEntity = self:AttachItem(self.WeaponEntity, ItemDef and ItemDef.Model, "HandR", "Weapon")
	Log.Info("[Player] 무기 장착:", ItemDef and (ItemDef.Name or ItemDef.Model) or "없음", "공격력", self:GetAttackPower())
end

-- itemDef = { Model = Content 기준 모델 경로, Defense = 받는 피해 감소 } (nil이면 방패 없음 — 막기 불가)
function PlayerController:EquipShield(ItemDef)
	self.ShieldDef     = ItemDef
	self.ShieldDefense = ItemDef and (ItemDef.Defense or 0.0) or 0.0
	self.ShieldEntity  = self:AttachItem(self.ShieldEntity, ItemDef and ItemDef.Model, "Shield", "Shield")
end

-- ---------------------------------------------------------------- 공개 상태

function PlayerController:GetStats()
	local P = self.Properties
	return {
		Health     = self.Health and self.Health.Health or 0,
		MaxHealth  = self.Health and self.Health.MaxHealth or 0,
		Mana       = self.Mana or P.MaxMana,
		MaxMana    = P.MaxMana,
		Stamina    = self.Stamina or P.MaxStamina,
		MaxStamina = P.MaxStamina,
		Attack     = self:GetAttackPower(),
		Defense    = self.ShieldDefense or 0,
	}
end

function PlayerController:GetSkillCooldowns()
	local P = self.Properties
	local C = self.Cooldowns or { Skill1 = 0, Skill2 = 0, Dodge = 0 }
	return {
		Skill1 = { Remaining = C.Skill1, Duration = P.SpinCooldown, Name = "회전베기", ManaCost = P.SpinCost },
		Skill2 = { Remaining = C.Skill2, Duration = P.DashCooldown, Name = "돌진 찌르기", ManaCost = P.DashCost },
		Dodge  = { Remaining = C.Dodge, Duration = P.DodgeCooldown, Name = "구르기", StaminaCost = P.DodgeCost },
	}
end

function PlayerController:RestoreHealth(Amount)
	return self.entity:Heal(Amount)
end

function PlayerController:RestoreMana(Amount)
	local Before = self.Mana
	self.Mana = math.min(self.Properties.MaxMana, self.Mana + Amount)
	return self.Mana - Before
end

function PlayerController:IsDead()
	return self.entity:IsDead()
end

function PlayerController:GetAttackPower()
	return self.Properties.BaseAttack + (self.WeaponDamage or 0.0)
end

return PlayerController
