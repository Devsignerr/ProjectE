-- 적 트랙 테스트용 대역 플레이어 (Scenes/RPG_Test_Enemies.escene 전용 — 실제 플레이어는 트랙 B의 PlayerController.lua)
--   계약 최소한: IsPlayer = true, IsDead(). 받은 피해를 로그로 남긴다.
--   조작은 IsoPlayer와 같다(WASD 화면 기준 이동, 카메라 평행 추적). AutoAttack이면 주기적으로 주변 적을
--   Physics.OverlapSphere로 찾아 피해를 준다 (적 콜라이더/레이어 Enemy와 enemy:ApplyDamage 경로 검증)
local EnemyTestPlayer = {
	Properties = {
		Camera         = "Camera",
		AutoAttack     = true,
		AttackInterval = 0.8,   -- 초
		AttackRadius   = 170.0, -- cm (몸 주변)
		AttackDamage   = 18.0,
		AttackClip     = "1H_Melee_Attack_Chop",
	},
}

EnemyTestPlayer.IsPlayer = true

function EnemyTestPlayer:IsDead()
	return self.entity:IsDead()
end

function EnemyTestPlayer:OnStart()
	self.AttackTimer = self.Properties.AttackInterval
	self.Camera = Scene.Find(self.Properties.Camera)
	if self.Camera then
		local Forward = self.Camera:GetForward()
		local Height  = self.Camera:GetWorldPosition().Z - self.entity:GetWorldPosition().Z
		self.CameraOffset = Forward * (Height / Forward.Z)
		self.ScreenUp     = Vector3(Forward.X, Forward.Y, 0):Normalized()
		self.ScreenRight  = Vector3(-self.ScreenUp.Y, self.ScreenUp.X, 0)
		self.Camera:SetPosition(self.entity:GetWorldPosition() + self.CameraOffset)
	end
end

function EnemyTestPlayer:OnUpdate(dt)
	if self.entity:IsDead() then return end
	if self.ScreenUp then
		local MoveX, MoveY = Input.GetAction("Move")
		local Move = self.ScreenUp * MoveY + self.ScreenRight * MoveX
		if Move:LengthSquared() > 0 then
			self.entity:AddMovementInput(Move)
		end
	end
	if not self.Properties.AutoAttack then return end
	self.AttackTimer = self.AttackTimer - dt
	if self.AttackTimer > 0 then return end
	self.AttackTimer = self.Properties.AttackInterval

	local Hits = 0
	for _, Other in ipairs(Physics.OverlapSphere(self.entity:GetWorldPosition(), self.Properties.AttackRadius, self.entity)) do
		local Script = Other:GetScript()
		if Script ~= nil and Script.IsEnemy and not Script:IsDead() then
			local Dealt = Other:ApplyDamage(self.Properties.AttackDamage, self.entity)
			Hits = Hits + 1
			Log.Info(string.format("[테스트 플레이어] %s 타격 %.0f (남은 체력 %.0f%%)", Script:GetDisplayName(), Dealt,
				Script:GetHealthFraction() * 100))
		end
	end
	if Hits > 0 and not self.entity:IsMontagePlaying() then
		self.entity:PlayMontage(self.Properties.AttackClip, { BlendIn = 0.1, BlendOut = 0.2, Speed = 1.3 })
	end
end

function EnemyTestPlayer:OnLateUpdate(dt)
	if self.Camera and self.CameraOffset then
		self.Camera:SetPosition(self.entity:GetWorldPosition() + self.CameraOffset)
	end
end

function EnemyTestPlayer:OnDamaged(Amount, Instigator)
	local Health = self.entity:GetComponent("HealthComponent")
	Log.Info(string.format("[테스트 플레이어] 피해 %.0f ← %s (체력 %.0f)", Amount,
		Instigator and Instigator:IsValid() and Instigator:GetName() or "?", Health and Health.Health or -1))
end

function EnemyTestPlayer:OnDeath(Instigator)
	Log.Info("[테스트 플레이어] 사망")
end

return EnemyTestPlayer
