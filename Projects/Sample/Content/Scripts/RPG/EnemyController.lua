-- 액션 RPG 적 (Phase 45 트랙 C): 스켈레톤 전사/졸개/도적/마법사 공용 두뇌. 프리팹 Prefabs/RPG/Enemy_*.eprefab가 값을 정한다.
--   구성: 루트(이 스크립트 + HealthComponent + NavAgentComponent + 키네마틱 캡슐 콜라이더, 레이어 Enemy)
--         └ Mesh(모델 + AnimGraph Animations/RPG/Skeleton*.eanimgraph, 파라미터 Speed/Combat/Dormant/Dead/DeathVariant)
--         └ 무기(소켓 부착 HandR/HandL)
--   상태: Spawning(등장 몽타주) / Dormant(바닥에 누워 대기) → Idle ↔ Patrol(집 주변) → Chase(내비 이동) → Attack(몽타주)
--         → Return(집에서 너무 멀어지면 돌아가며 체력 회복) / Dead(사망 애니메이션 → 땅으로 가라앉은 뒤 파괴)
--   이동은 AI 시스템(entity:MoveTo, NavAgent가 이동 방향으로 회전), 멈춰 있을 때 바라보기만 스크립트가 돌린다.
--   타격 시점은 모델 .emeta 노티파이: Hit(근접), Shoot(투사체), Cast(마법사 광역) → OnAnimNotify_<이름>
-- 공개 계약 (다른 스크립트가 GetScript()로 부른다): IsEnemy = true, GetDisplayName(), IsDead(), GetHealthFraction(), Alert(target)
local EnemyController = {
	Properties = {
		DisplayName     = "스켈레톤",
		Damage          = 10.0,   -- 근접 한 대 / 투사체 한 발
		AttackRange     = 140.0,  -- cm (원거리는 사거리)
		MinRange        = 0.0,    -- 원거리: 이보다 가까우면 물러난다 (0 = 안 물러남)
		AggroRange      = 800.0,  -- 플레이어 발견 거리 (cm)
		LoseRange       = 1400.0, -- 이보다 멀어지면 놓친다
		LeashRange      = 1600.0, -- 집에서 이보다 멀어지면 포기하고 돌아간다
		AlertRadius     = 700.0,  -- 발견/피격 시 주변 동료도 깨운다 (0 = 안 함)
		AttackCooldown  = 1.5,    -- 초 (공격 시작 간격)
		AttackClips     = "1H_Melee_Attack_Chop", -- 쉼표로 여러 개 (돌아가며)
		AttackSpeed     = 1.0,    -- 공격 몽타주 배속
		WalkSpeed       = 75.0,   -- 순찰 속도 (cm/s, 걷기 클립 보폭에 맞춤)
		RunSpeed        = 240.0,  -- 추적 속도
		TurnSpeed       = 540.0,  -- 제자리에서 대상을 바라보는 회전 속도 (도/초)
		PatrolRadius    = 350.0,  -- 집 주변 순찰 반경 (0 = 제자리 대기)
		Poise           = 0.0,    -- 0~1: 공격 중 피격 경직을 버틸 확률
		HitStun         = 0.45,   -- 피격 경직 시간 (초)
		Ranged          = false,
		Projectile      = "Bolt", -- "Bolt"(석궁, 빠름) / "Magic"(마법 구체, 느리고 약간 유도)
		ProjectileSpeed = 1200.0, -- cm/s
		AoeDamage       = 0.0,    -- > 0이면 광역 마법 (Spellcast_Long, Cast 노티파이에 폭발)
		AoeRadius       = 170.0,
		AoeCooldown     = 7.0,
		SpawnAnimation  = "Spawn_Ground_Skeletons", -- 시작 시 몽타주 클립 / "Dormant"(누워 있다가 깨어남) / ""(없음)
		DeathCorpseTime = 3.0,    -- 사망 후 가라앉기 시작까지 (초)
		LootTable       = "",     -- GameManager:SpawnLoot에 넘기는 전리품 표 ID
		ShowDamageNumbers = true, -- 받은 피해를 GameManager:ShowDamageNumber로 표시
		TargetName      = "Player",
	},
}

local WorldLayers = { "Default", "Ground", "Prop" } -- 시야/투사체를 막는 레이어

-- 몽타주 클립 길이 (초, KayKit 스켈레톤) — 끝 이벤트가 오지 않을 때의 안전 타이머용
local ClipLength = {
	["1H_Melee_Attack_Chop"] = 1.07, ["1H_Melee_Attack_Slice_Diagonal"] = 1.0, ["1H_Melee_Attack_Slice_Horizontal"] = 1.07,
	["1H_Melee_Attack_Stab"] = 1.6, ["1H_Ranged_Shoot"] = 1.07, ["Spellcast_Shoot"] = 0.93, ["Spellcast_Long"] = 2.53,
	["Spawn_Ground_Skeletons"] = 3.57, ["Skeletons_Awaken_Floor"] = 2.3, ["Skeletons_Awaken_Standing"] = 1.0,
	["Death_C_Skeletons_Resurrect"] = 2.7, ["Hit_A"] = 0.67, ["Hit_B"] = 0.87, ["Cheer"] = 1.67, ["Idle_B"] = 2.13,
}
local SpawnSpeed = 1.4 -- 등장 몽타주 배속 (원본은 느리다)

local function Flat(V)
	return Vector3(V.X, V.Y, 0)
end

local function FlatDistance(A, B)
	local DX, DY = A.X - B.X, A.Y - B.Y
	return math.sqrt(DX * DX + DY * DY)
end

local function YawOf(Direction)
	return math.deg(math.atan(Direction.Y, Direction.X))
end

local function SplitList(Text)
	local Items = {}
	for Item in string.gmatch(Text or "", "[^,%s]+") do
		table.insert(Items, Item)
	end
	return Items
end

-- 다른 엔티티의 스크립트 메서드를 오류에 안전하게 부른다 (상대 스크립트 오류가 이 적을 멈추지 않게)
local function SafeCall(Target, Method, ...)
	if Target == nil or type(Target[Method]) ~= "function" then
		return nil
	end
	local Ok, Result = pcall(Target[Method], Target, ...)
	if not Ok then
		Log.Warn("적: " .. Method .. " 호출 실패 — " .. tostring(Result))
		return nil
	end
	return Result
end

-- ---------------------------------------------------------------- 공개 API

EnemyController.IsEnemy = true

function EnemyController:GetDisplayName()
	return self.Properties.DisplayName
end

function EnemyController:IsDead()
	return self.bDead == true or self.entity:IsDead()
end

function EnemyController:GetHealthFraction()
	local Health = self.entity:GetComponent("HealthComponent")
	if Health == nil or Health.MaxHealth <= 0 then
		return 0
	end
	return math.max(0, math.min(1, Health.Health / Health.MaxHealth))
end

-- 동료가 발견/피격을 알린다: 잠들어 있으면 깨우고 대상을 쫓는다
function EnemyController:Alert(Target)
	if self:IsDead() or Target == nil or not Target:IsValid() or self.Target ~= nil then
		return
	end
	if self.State == "Return" then
		return
	end
	self:Engage(Target, false)
end

-- ---------------------------------------------------------------- 수명

function EnemyController:OnStart()
	local P = self.Properties
	self.Home        = self.entity:GetWorldPosition()
	self.Agent       = self.entity:GetComponent("NavAgentComponent")
	self.State       = "Idle"
	self.StateTime   = 0
	self.Cooldown    = 0.5 + math.random() * 0.5
	self.AoeLeft     = P.AoeCooldown * 0.5
	self.StunTime    = 0
	self.AttackIndex = 0
	self.AttackClips = SplitList(P.AttackClips)
	self.Projectiles = {}
	self.Effects     = {}   -- { Entity, Expire } 잠깐 쓰는 효과 엔티티 (파괴 책임)
	self.Telegraph   = nil
	self.RingAngle   = math.random() * math.pi * 2 -- 근접 포위 위치 (여럿이 한 점에 겹치지 않게)
	self.LastPosition = self.Home
	self.SmoothedSpeed = 0
	self.IdleWait    = 0.5 + math.random() * 1.5
	self.FindTimer   = 0
	self:FindPlayer()

	if P.SpawnAnimation == "Dormant" then
		self.entity:SetAnimParam("Dormant", true)
		self:SetState("Dormant")
	elseif P.SpawnAnimation ~= nil and P.SpawnAnimation ~= "" then
		self:PlaySpawn(P.SpawnAnimation)
	end
end

function EnemyController:OnDestroy()
	for _, Projectile in ipairs(self.Projectiles or {}) do
		if Projectile.Entity:IsValid() then Projectile.Entity:Destroy() end
	end
	for _, Effect in ipairs(self.Effects or {}) do
		if Effect.Entity:IsValid() then Effect.Entity:Destroy() end
	end
	self:ClearTelegraph()
end

function EnemyController:SetState(State)
	self.State     = State
	self.StateTime = 0
end

function EnemyController:FindPlayer()
	local Found = Scene.Find(self.Properties.TargetName)
	if Found ~= nil then
		local Script = Found:GetScript()
		-- 계약: 플레이어 스크립트는 IsPlayer = true. 스크립트가 아직 없거나 다른 구현이어도 이름이 맞으면 대상
		if Script == nil or Script.IsPlayer ~= false then
			self.Player = Found
		end
	end
end

function EnemyController:IsTargetAlive(Target)
	if Target == nil or not Target:IsValid() then
		return false
	end
	if Target:IsDead() then
		return false
	end
	local Script = Target:GetScript()
	if Script ~= nil and type(Script.IsDead) == "function" then
		return not SafeCall(Script, "IsDead")
	end
	return true
end

function EnemyController:PlaySpawn(Clip)
	self.entity:PlayMontage(Clip, { BlendIn = 0.0, BlendOut = 0.35, Speed = SpawnSpeed })
	self.SpawnClip = Clip
	self.SpawnEnd  = (ClipLength[Clip] or 2.0) / SpawnSpeed
	self:SetState("Spawning")
	self:AddEffect("Particles/RPG/EnemyBoneDust.eparticle", self.entity:GetWorldPosition() + Vector3(0, 0, 10), 2.0)
end

-- ---------------------------------------------------------------- 갱신

function EnemyController:OnUpdate(dt)
	self.StateTime = self.StateTime + dt
	self:UpdateProjectiles(dt)
	self:UpdateEffects()
	self:UpdateTelegraph(dt)
	if self.bDead then
		self:UpdateCorpse(dt)
		return
	end

	self.Cooldown = self.Cooldown - dt
	self.AoeLeft  = self.AoeLeft - dt
	if self.Player == nil or not self.Player:IsValid() then
		self.Player    = nil
		self.FindTimer = self.FindTimer - dt
		if self.FindTimer <= 0 then
			self.FindTimer = 1.0
			self:FindPlayer()
		end
	end

	if self.State == "Spawning" then
		if self.StateTime >= self.SpawnEnd then
			self:SetState("Idle")
		end
		return
	end
	if self.State == "Dormant" then
		if self:CanSee(self.Player, self.Properties.AggroRange * 0.75) then
			self:Awaken(self.Player)
		end
		return
	end
	if self.StunTime > 0 then
		self.StunTime = self.StunTime - dt
		return
	end

	-- 대상이 죽었거나 사라졌으면 집으로 (플레이어가 쓰러지면 한 번 환호)
	if self.Target ~= nil and not self:IsTargetAlive(self.Target) then
		local bPlayerDied = self.Target:IsValid()
		self.Target = nil
		self.entity:StopMove()
		self:ClearTelegraph()
		if bPlayerDied then
			-- 제자리에서 환호한 뒤 대기 → 순찰(집 주변 지점으로 걸어서 돌아간다)
			self.entity:PlayMontage("Cheer", { BlendIn = 0.2, BlendOut = 0.3 })
			self:SetState("Idle")
			self.IdleWait = 2.2
		else
			self:BeginReturn(false)
		end
	end

	local Handler = self["Update" .. self.State]
	if Handler then
		Handler(self, dt)
	end
end

-- 애니메이션 파라미터는 이동(AI)·물리·트랜스폼 갱신이 끝난 뒤의 실제 위치 변화로 (미끄러짐 없게 보폭 블렌드)
function EnemyController:OnLateUpdate(dt)
	if dt <= 0 then return end
	local Position = self.entity:GetWorldPosition()
	local Speed    = FlatDistance(Position, self.LastPosition) / dt
	self.LastPosition = Position
	if Speed > 2000 then Speed = 0 end -- 순간 이동(리스폰 등)은 무시
	local Blend = math.min(1, dt * 12)
	self.SmoothedSpeed = self.SmoothedSpeed + (Speed - self.SmoothedSpeed) * Blend
	if self.SmoothedSpeed < 3 then self.SmoothedSpeed = 0 end
	self.entity:SetAnimParam("Speed", self.SmoothedSpeed)
	local bCombat = not self.bDead and self.Target ~= nil and (self.State == "Chase" or self.State == "Attack" or self.State == "Retreat")
	if bCombat ~= self.bCombatParam then
		self.bCombatParam = bCombat
		self.entity:SetAnimParam("Combat", bCombat)
	end
end

function EnemyController:CanSee(Target, Range)
	if not self:IsTargetAlive(Target) then
		return false
	end
	return FlatDistance(Target:GetWorldPosition(), self.entity:GetWorldPosition()) <= Range
end

function EnemyController:SetMoveSpeed(Speed)
	if self.Agent ~= nil then
		self.Agent.MaxSpeed = Speed
	end
end

-- 대상 지정 → 추적 (bAlertOthers면 주변 동료에게도 알린다)
function EnemyController:Engage(Target, bAlertOthers)
	self.Target = Target
	if self.State == "Dormant" then
		self:Awaken(Target)
		return
	end
	if self.State == "Spawning" then
		return -- 등장이 끝나면 Idle에서 바로 대상을 본다
	end
	if self.entity:IsMontagePlaying() and self.State ~= "Attack" then
		self.entity:StopMontage(nil, 0.2) -- 대기 동작(Idle_B, 환호) 끊기
	end
	self:SetState("Chase")
	self.RepathTimer = 0
	if bAlertOthers then
		self:AlertAllies(Target)
	end
end

function EnemyController:AlertAllies(Target)
	local Radius = self.Properties.AlertRadius
	if Radius <= 0 then return end
	for _, Other in ipairs(Physics.OverlapSphere(self.entity:GetWorldPosition() + Vector3(0, 0, 80), Radius, self.entity)) do
		local Script = Other:GetScript()
		if Script ~= nil and Script.IsEnemy and Script ~= self then
			SafeCall(Script, "Alert", Target)
		end
	end
end

function EnemyController:Awaken(Target)
	self.entity:SetAnimParam("Dormant", false)
	self.Target = Target
	self:PlaySpawn("Skeletons_Awaken_Floor")
	Log.Info("적: " .. self.Properties.DisplayName .. " 깨어남")
end

-- ---- 대기 / 순찰

function EnemyController:UpdateIdle(dt)
	if self.Target ~= nil then
		self:Engage(self.Target, true)
		return
	end
	if self:CanSee(self.Player, self.Properties.AggroRange) then
		Log.Info("적: " .. self.Properties.DisplayName .. " 플레이어 발견")
		self:Engage(self.Player, true)
		return
	end
	if self.StateTime < self.IdleWait or self.Properties.PatrolRadius <= 0 then
		return
	end
	-- 집 주변 임의 지점으로 걷기
	local Angle  = math.random() * math.pi * 2
	local Radius = self.Properties.PatrolRadius * (0.35 + math.random() * 0.65)
	local Goal   = self.Home + Vector3(math.cos(Angle) * Radius, math.sin(Angle) * Radius, 0)
	self:SetMoveSpeed(self.Properties.WalkSpeed)
	if self.entity:MoveTo(Goal, 25) == "Moving" then
		self:SetState("Patrol")
	else
		self.IdleWait = self.StateTime + 1.0 -- 갈 수 없는 지점: 잠시 뒤 다시
	end
end

function EnemyController:UpdatePatrol(dt)
	if self:CanSee(self.Player, self.Properties.AggroRange) then
		Log.Info("적: " .. self.Properties.DisplayName .. " 플레이어 발견")
		self:Engage(self.Player, true)
		return
	end
	if self.entity:GetMoveStatus() ~= "Moving" or self.StateTime > 12 then
		self.entity:StopMove()
		self:SetState("Idle")
		self.IdleWait = 2.0 + math.random() * 3.0
		if math.random() < 0.35 then
			self.entity:PlayMontage("Idle_B", { BlendIn = 0.3, BlendOut = 0.3 }) -- 두리번거리기 (대기 시간보다 짧다)
		end
	end
end

-- ---- 추적 / 공격

function EnemyController:UpdateChase(dt)
	local P      = self.Properties
	local Target = self.Target
	local MyPos  = self.entity:GetWorldPosition()
	local TPos   = Target:GetWorldPosition()
	local Dist   = FlatDistance(MyPos, TPos)

	if FlatDistance(MyPos, self.Home) > P.LeashRange or Dist > P.LoseRange then
		Log.Info("적: " .. P.DisplayName .. " 추적 포기 — 집으로")
		self:BeginReturn(true)
		return
	end

	local bInRange = Dist <= P.AttackRange
	if bInRange and P.Ranged then
		bInRange = self:HasLineOfSight(TPos)
	end
	-- 근접: 동료와 겹쳐 서 있으면 고리의 다른 자리로 비켜 선다 (비키는 동안은 멈추지 않는다)
	if self.ShuffleTime ~= nil then
		self.ShuffleTime = self.ShuffleTime - dt
		if self.ShuffleTime > 0 and self.entity:GetMoveStatus() == "Moving" then
			return
		end
		self.ShuffleTime = nil
	end
	local Neighbor = (bInRange and not P.Ranged) and self:FindCrowdingAlly() or nil
	if Neighbor ~= nil then
		-- 대상 기준으로 동료와 반대쪽으로 고리를 따라 비켜 선다
		local MyAngle    = math.atan(MyPos.Y - TPos.Y, MyPos.X - TPos.X)
		local OtherPos   = Neighbor:GetWorldPosition()
		local OtherAngle = math.atan(OtherPos.Y - TPos.Y, OtherPos.X - TPos.X)
		local Delta      = (MyAngle - OtherAngle + math.pi * 3) % (math.pi * 2) - math.pi
		self.RingAngle   = MyAngle + (Delta >= 0 and 0.8 or -0.8)
		self.ShuffleTime = 0.8
		self:SetMoveSpeed(P.WalkSpeed * 1.5)
		self.entity:MoveTo(self:RingPoint(TPos, MyPos.Z), 10)
		return
	end
	if bInRange then
		if self.entity:GetMoveStatus() == "Moving" then
			self.entity:StopMove()
		end
		-- 원거리: 너무 가까우면 쿨다운 동안 물러난다
		if P.Ranged and P.MinRange > 0 and Dist < P.MinRange and self.Cooldown > 0.4 then
			if self:BeginRetreat(TPos) then return end
		end
		local bFacing = self:FaceTowards(TPos, dt)
		if self.Cooldown <= 0 and bFacing then
			self:StartAttack()
		end
		return
	end

	-- 사거리 밖: 다시 길 찾기 (대상이 움직이면 목표 갱신)
	self.RepathTimer = (self.RepathTimer or 0) - dt
	if self.RepathTimer <= 0 or self.entity:GetMoveStatus() ~= "Moving" then
		self.RepathTimer = 0.3
		self:SetMoveSpeed(P.RunSpeed)
		local Goal
		if P.Ranged then
			-- 사거리 80% 지점까지 (시야가 막혔으면 대상 쪽으로 계속)
			local Away = Flat(MyPos - TPos)
			local Len  = Away:Length()
			local Want = P.AttackRange * 0.8
			if Len > 1 and Dist > Want then
				Goal = TPos + Away * (Want / Len)
			else
				Goal = TPos
			end
		else
			Goal = self:RingPoint(TPos, MyPos.Z)
		end
		Goal = Vector3(Goal.X, Goal.Y, MyPos.Z)
		if self.entity:MoveTo(Goal, 15) == "Failed" then
			self.entity:MoveTo(Vector3(TPos.X, TPos.Y, MyPos.Z), P.AttackRange * 0.6) -- 자리가 막혔으면 대상에게 직접
		end
	end
end

-- 근접 포위 고리의 자기 자리 (여럿이 한 점에 겹치지 않게 엔티티마다 각도가 다르다)
function EnemyController:RingPoint(TargetPosition, Z)
	local Ring = math.max(self.Properties.AttackRange * 0.8, 110)
	return Vector3(TargetPosition.X + math.cos(self.RingAngle) * Ring, TargetPosition.Y + math.sin(self.RingAngle) * Ring, Z)
end

-- 겹칠 만큼 가까이 선 살아 있는 동료 (없으면 nil, 0.4초마다만 질의)
function EnemyController:FindCrowdingAlly()
	self.CrowdTimer = (self.CrowdTimer or 0) - Time.DeltaTime
	if self.CrowdTimer > 0 then return nil end
	self.CrowdTimer = 0.4
	for _, Other in ipairs(Physics.OverlapSphere(self.entity:GetWorldPosition() + Vector3(0, 0, 74), 50, self.entity)) do
		local Script = Other:GetScript()
		if Script ~= nil and Script.IsEnemy and not Script:IsDead() then
			return Other
		end
	end
	return nil
end

function EnemyController:BeginRetreat(TargetPosition)
	local MyPos = self.entity:GetWorldPosition()
	local Away  = Flat(MyPos - TargetPosition)
	if Away:LengthSquared() < 1 then return false end
	local Goal = MyPos + Away:Normalized() * 300
	if FlatDistance(Goal, self.Home) > self.Properties.LeashRange * 0.8 then return false end
	self:SetMoveSpeed(self.Properties.RunSpeed)
	if self.entity:MoveTo(Goal, 20) ~= "Moving" then return false end
	self:SetState("Retreat")
	return true
end

function EnemyController:UpdateRetreat(dt)
	if self.entity:GetMoveStatus() ~= "Moving" or self.StateTime > 1.2 then
		self.entity:StopMove()
		self:SetState("Chase")
	end
end

-- 바라보기: 남은 각이 작으면 true (멈춰 있을 때만 — 이동 중에는 NavAgent가 돌린다)
function EnemyController:FaceTowards(Point, dt)
	local Direction = Flat(Point - self.entity:GetWorldPosition())
	if Direction:LengthSquared() < 1 then return true end
	local Current = YawOf(self.entity:GetForward())
	local Wanted  = YawOf(Direction)
	local Delta   = (Wanted - Current + 540) % 360 - 180
	local Step    = math.max(-self.Properties.TurnSpeed * dt, math.min(self.Properties.TurnSpeed * dt, Delta))
	if math.abs(Delta) > 0.5 then
		self.entity:SetRotation(Quat.FromEuler(0, Current + Step, 0))
	end
	return math.abs(Delta - Step) < 25
end

-- 시야 (0.25초마다만 레이캐스트, 그 사이는 직전 결과)
function EnemyController:HasLineOfSight(TargetPosition)
	if self.SightCheckTime ~= nil and Time.TotalTime - self.SightCheckTime < 0.25 then
		return self.bLastSight
	end
	self.SightCheckTime = Time.TotalTime
	self.bLastSight     = self:TraceLineOfSight(TargetPosition)
	return self.bLastSight
end

function EnemyController:TraceLineOfSight(TargetPosition)
	local Eye  = self.entity:GetWorldPosition() + Vector3(0, 0, 110)
	local Aim  = TargetPosition + Vector3(0, 0, 20)
	local Ray  = Aim - Eye
	local Dist = Ray:Length()
	if Dist < 1 then return true end
	local Hit = Physics.Raycast(Eye, Ray / Dist, Dist, WorldLayers)
	return Hit == nil or Hit.distance >= Dist - 40
end

function EnemyController:StartAttack()
	local P = self.Properties
	local Clip
	self.bAoe = P.AoeDamage > 0 and self.AoeLeft <= 0
	if self.bAoe then
		Clip = "Spellcast_Long"
		self.AoeLeft = P.AoeCooldown
		self:PlaceTelegraph(self.Target:GetWorldPosition())
	else
		self.AttackIndex = self.AttackIndex % math.max(1, #self.AttackClips) + 1
		Clip = self.AttackClips[self.AttackIndex] or "1H_Melee_Attack_Chop"
	end
	self.AttackClip = Clip
	self.bHitDone   = false
	self.entity:PlayMontage(Clip, { BlendIn = 0.12, BlendOut = 0.25, Speed = P.AttackSpeed })
	self.AttackEnd = (ClipLength[Clip] or 1.2) / P.AttackSpeed + 0.3
	self.Cooldown  = P.AttackCooldown
	self:SetState("Attack")
end

function EnemyController:UpdateAttack(dt)
	-- 타격 전까지는 대상을 따라 돈다 (타격 뒤에는 휘두른 방향 유지)
	if not self.bHitDone and self.Target ~= nil and not self.bAoe then
		self:FaceTowards(self.Target:GetWorldPosition(), dt)
	end
	if self.StateTime >= self.AttackEnd then
		self:EndAttack()
	end
end

function EnemyController:EndAttack()
	if self.State == "Attack" then
		self:SetState(self.Target ~= nil and "Chase" or "Idle")
		self.RepathTimer = 0
	end
	self:ClearTelegraph()
end

function EnemyController:OnMontageEnded(Clip, bInterrupted, Slot)
	if self.bDead then return end
	if self.State == "Attack" and Clip == self.AttackClip then
		self:EndAttack()
	elseif self.State == "Spawning" and Clip == self.SpawnClip then
		self:SetState("Idle")
	end
end

-- 근접 타격 (모델 노티파이 Hit): 사거리 + 앞쪽 부채꼴 안이면 피해
function EnemyController:OnAnimNotify_Hit()
	if self.bDead or self.State ~= "Attack" or self.bHitDone then return end
	self.bHitDone = true
	local Target = self.Target
	if not self:IsTargetAlive(Target) then return end
	local MyPos     = self.entity:GetWorldPosition()
	local Direction = Flat(Target:GetWorldPosition() - MyPos)
	local Dist      = Direction:Length()
	if Dist > self.Properties.AttackRange + 45 then return end
	local Forward = Flat(self.entity:GetForward()):Normalized()
	if Dist > 1 and Forward:Dot(Direction / Dist) < 0.35 then return end -- 약 70도 밖
	local Dealt = Target:ApplyDamage(self.Properties.Damage, self.entity)
	Log.Info("적: " .. self.Properties.DisplayName .. " 근접 타격 " .. tostring(Dealt))
end

-- 투사체 발사 (노티파이 Shoot)
function EnemyController:OnAnimNotify_Shoot()
	if self.bDead or self.State ~= "Attack" or self.bHitDone then return end
	self.bHitDone = true
	local Target = self.Target
	if not self:IsTargetAlive(Target) then return end
	local Forward = Flat(self.entity:GetForward()):Normalized()
	local Origin  = self.entity:GetWorldPosition() + Forward * 45 + Vector3(0, 0, 105)
	local Aim     = Target:GetWorldPosition() + Vector3(0, 0, 10)
	self:SpawnProjectile(Origin, (Aim - Origin):Normalized())
end

-- 광역 마법 폭발 (노티파이 Cast): 예고 원 안이면 피해
function EnemyController:OnAnimNotify_Cast()
	if self.bDead or self.Telegraph == nil then return end
	local Center = self.Telegraph.Center
	self:AddEffect("Particles/RPG/EnemyMagicBurst.eparticle", Center + Vector3(0, 0, 10), 1.5)
	local Target = self.Target
	if self:IsTargetAlive(Target) and FlatDistance(Target:GetWorldPosition(), Center) <= self.Properties.AoeRadius + 30 then
		local Dealt = Target:ApplyDamage(self.Properties.AoeDamage, self.entity)
		Log.Info("적: " .. self.Properties.DisplayName .. " 광역 마법 적중 " .. tostring(Dealt))
	end
	self.bHitDone = true
	self:ClearTelegraph()
end

-- ---------------------------------------------------------------- 투사체

function EnemyController:SpawnProjectile(Origin, Direction)
	local P      = self.Properties
	local bMagic = P.Projectile == "Magic"
	local Entity = Scene.Create(bMagic and "EnemyMagicBolt" or "EnemyBolt")
	Entity:SetPosition(Origin)
	local Pitch = math.deg(math.asin(math.max(-1, math.min(1, Direction.Z))))
	Entity:SetRotation(Quat.FromEuler(Pitch, YawOf(Direction), 0))
	local Mesh = Entity:AddComponent("StaticMeshComponent")
	if bMagic then
		Entity:SetScale(Vector3(0.32, 0.32, 0.32))
		Mesh.MeshAsset     = "primitive:sphere"
		Mesh.MaterialAsset = "Materials/RPG/EnemyMagic.emat"
		local Trail = Entity:AddComponent("ParticleSystemComponent")
		Trail.Asset = "Particles/RPG/EnemyMagicTrail.eparticle"
	else
		Entity:SetScale(Vector3(0.45, 0.07, 0.07))
		Mesh.MeshAsset     = "primitive:cube"
		Mesh.MaterialAsset = "Materials/RPG/EnemyBolt.emat"
	end
	table.insert(self.Projectiles, {
		Entity   = Entity,
		Velocity = Direction * P.ProjectileSpeed,
		Life     = bMagic and 4.0 or 1.5,
		Radius   = bMagic and 28 or 14,
		bHoming  = bMagic,
		bMagic   = bMagic,
		Damage   = P.Damage,
	})
end

function EnemyController:UpdateProjectiles(dt)
	if #self.Projectiles == 0 then return end
	local Alive = {}
	for _, Projectile in ipairs(self.Projectiles) do
		if self:StepProjectile(Projectile, dt) then
			table.insert(Alive, Projectile)
		elseif Projectile.Entity:IsValid() then
			Projectile.Entity:Destroy()
		end
	end
	self.Projectiles = Alive
end

-- 투사체 한 번 진행. 계속 날아가면 true
function EnemyController:StepProjectile(Projectile, dt)
	local Entity = Projectile.Entity
	if not Entity:IsValid() then return false end
	Projectile.Life = Projectile.Life - dt
	if Projectile.Life <= 0 then return false end

	local Position = Entity:GetPosition()
	local Target   = self.Target or self.Player
	if Projectile.bHoming and self:IsTargetAlive(Target) then
		-- 느린 유도: 초당 최대 50도
		local Speed  = Projectile.Velocity:Length()
		local Wanted = (Target:GetWorldPosition() + Vector3(0, 0, 10) - Position):Normalized()
		local Current = Projectile.Velocity / Speed
		local Turn   = math.min(1, dt * 0.9)
		Projectile.Velocity = (Current + (Wanted - Current) * Turn):Normalized() * Speed
		local Dir = Projectile.Velocity / Speed
		Entity:SetRotation(Quat.FromEuler(math.deg(math.asin(math.max(-1, math.min(1, Dir.Z)))), YawOf(Dir), 0))
	end
	local NewPosition = Position + Projectile.Velocity * dt
	Entity:SetPosition(NewPosition)

	-- 겹침: 플레이어면 피해, 적이 아닌 다른 물체(벽/소품/바닥)면 부딪혀 사라짐
	for _, Other in ipairs(Physics.OverlapSphere(NewPosition, Projectile.Radius, self.entity)) do
		if Other == self.Player or (Other:GetScript() ~= nil and Other:GetScript().IsPlayer) then
			if self:IsTargetAlive(Other) then
				local Dealt = Other:ApplyDamage(Projectile.Damage, self.entity)
				Log.Info("적: " .. self.Properties.DisplayName .. " 투사체 적중 " .. tostring(Dealt))
			end
			self:ProjectileImpact(Projectile, NewPosition)
			return false
		end
		local Script = Other:GetScript()
		if not (Script ~= nil and Script.IsEnemy) then
			self:ProjectileImpact(Projectile, NewPosition)
			return false
		end
	end
	return true
end

function EnemyController:ProjectileImpact(Projectile, Position)
	if Projectile.bMagic then
		self:AddEffect("Particles/RPG/EnemyMagicBurst.eparticle", Position, 1.5)
	end
end

-- ---------------------------------------------------------------- 효과 엔티티 (파티클 한 번, 예고 원)

function EnemyController:AddEffect(Asset, Position, Lifetime)
	local Entity = Scene.Create("EnemyEffect")
	Entity:SetPosition(Position)
	local Particles = Entity:AddComponent("ParticleSystemComponent")
	Particles.Asset = Asset
	table.insert(self.Effects, { Entity = Entity, Expire = Time.TotalTime + Lifetime })
end

function EnemyController:UpdateEffects()
	if #self.Effects == 0 then return end
	local Keep = {}
	for _, Effect in ipairs(self.Effects) do
		if Time.TotalTime >= Effect.Expire then
			if Effect.Entity:IsValid() then Effect.Entity:Destroy() end
		else
			table.insert(Keep, Effect)
		end
	end
	self.Effects = Keep
end

function EnemyController:PlaceTelegraph(Center)
	self:ClearTelegraph()
	local Ground = Vector3(Center.X, Center.Y, self.entity:GetWorldPosition().Z + 2)
	local Entity = Scene.Create("EnemyTelegraph")
	Entity:SetPosition(Ground)
	Entity:SetScale(Vector3(0.1, 0.1, 0.02))
	local Mesh = Entity:AddComponent("StaticMeshComponent")
	Mesh.MeshAsset     = "primitive:sphere"
	Mesh.MaterialAsset = "Materials/RPG/EnemyTelegraph.emat"
	self.Telegraph = { Entity = Entity, Center = Ground, Time = 0 }
end

function EnemyController:UpdateTelegraph(dt)
	local Telegraph = self.Telegraph
	if Telegraph == nil then return end
	if not Telegraph.Entity:IsValid() then
		self.Telegraph = nil
		return
	end
	-- 시전 시간(약 1.5초) 동안 원이 반경까지 커진다
	Telegraph.Time = Telegraph.Time + dt
	local Size = self.Properties.AoeRadius * 2 / 100 * math.min(1, 0.25 + Telegraph.Time / 1.3)
	Telegraph.Entity:SetScale(Vector3(Size, Size, 0.02))
end

function EnemyController:ClearTelegraph()
	if self.Telegraph ~= nil then
		if self.Telegraph.Entity:IsValid() then self.Telegraph.Entity:Destroy() end
		self.Telegraph = nil
	end
end

-- ---------------------------------------------------------------- 귀환

function EnemyController:BeginReturn(bHeal)
	self.Target = nil
	self.bHealOnReturn = bHeal
	self:SetMoveSpeed(self.Properties.RunSpeed)
	self:ClearTelegraph()
	if self.entity:MoveTo(self.Home, 30) == "Moving" then
		self:SetState("Return")
	else
		self:SetState("Idle")
	end
end

function EnemyController:UpdateReturn(dt)
	if self.entity:GetMoveStatus() ~= "Moving" or self.StateTime > 15 then
		if self.bHealOnReturn then
			local Health = self.entity:GetComponent("HealthComponent")
			if Health ~= nil and Health.Health > 0 then
				self.entity:Heal(Health.MaxHealth - Health.Health)
			end
		end
		self:SetState("Idle")
		self.IdleWait = 1.0 + math.random() * 2.0
	end
end

-- ---------------------------------------------------------------- 피격 / 사망

function EnemyController:OnDamaged(Amount, Instigator)
	if self.bDead then return end
	local P = self.Properties
	if P.ShowDamageNumbers and Amount > 0 then
		local Manager = Scene.Find("GameManager")
		Manager = Manager and Manager:GetScript()
		SafeCall(Manager, "ShowDamageNumber", self.entity:GetWorldPosition() + Vector3(0, 0, 170), Amount, "Normal")
	end
	if self.entity:IsDead() then
		return -- OnDeath가 처리
	end
	-- 맞으면 가해자를 쫓는다 (잠들어 있었으면 깨어난다)
	if Instigator ~= nil and Instigator:IsValid() and self.Target == nil then
		self:Engage(Instigator, true)
	end
	if self.State == "Spawning" or self.State == "Dormant" then
		return
	end
	-- 경직: 공격 중에는 Poise 확률로 버틴다
	if self.State == "Attack" and math.random() < P.Poise then
		return
	end
	self.entity:StopMove()
	self:ClearTelegraph()
	self.entity:PlayMontage(math.random() < 0.5 and "Hit_A" or "Hit_B", { BlendIn = 0.05, BlendOut = 0.2, Speed = 1.3 })
	self.StunTime = P.HitStun
	if self.State == "Attack" then
		self:SetState("Chase")
	end
end

function EnemyController:OnDeath(Instigator)
	if self.bDead then return end
	self.bDead = true
	local P = self.Properties
	self:SetState("Dead")
	self.Target = nil
	self.entity:StopMove()
	self.entity:StopMontage(nil, 0.1)
	self:ClearTelegraph()
	self.entity:SetAnimParam("DeathVariant", math.random() < 0.65 and 1 or 0)
	self.entity:SetAnimParam("Dead", true)
	self.entity:SetAnimParam("Combat", false)
	-- 시체는 길을 막지 않고 공격 질의에도 걸리지 않는다
	if self.entity:HasComponent("RigidBodyComponent") then self.entity:RemoveComponent("RigidBodyComponent") end
	if self.entity:HasComponent("CapsuleColliderComponent") then self.entity:RemoveComponent("CapsuleColliderComponent") end

	local Position = self.entity:GetWorldPosition()
	Log.Info("적: " .. P.DisplayName .. " 사망 (가해자 " .. (Instigator and Instigator:IsValid() and Instigator:GetName() or "없음") .. ")")
	if P.LootTable ~= "" then
		local Manager = Scene.Find("GameManager")
		Manager = Manager and Manager:GetScript()
		SafeCall(Manager, "SpawnLoot", Position, P.LootTable)
	end
end

-- 사망 애니메이션 → DeathCorpseTime 뒤 먼지와 함께 땅으로 가라앉고 파괴
function EnemyController:UpdateCorpse(dt)
	local SinkStart = self.Properties.DeathCorpseTime
	if self.StateTime < SinkStart then return end
	if not self.bSinking then
		self.bSinking = true
		self:AddEffect("Particles/RPG/EnemyBoneDust.eparticle", self.entity:GetWorldPosition() + Vector3(0, 0, 10), 2.0)
	end
	self.entity:SetPosition(self.entity:GetPosition() - Vector3(0, 0, 60 * dt))
	if self.StateTime > SinkStart + 1.6 and #self.Projectiles == 0 then
		self.entity:Destroy()
	end
end

return EnemyController
