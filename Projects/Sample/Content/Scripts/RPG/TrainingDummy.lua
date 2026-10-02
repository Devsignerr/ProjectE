-- 플레이어 전투 검증용 표적 (Phase 45 트랙 B 테스트 씬 RPG_Test_Player). 적 계약의 최소 구현: IsEnemy + GetDisplayName/IsDead/GetHealthFraction.
--   엔티티: 스켈레탈 모델 루트(ModelComponent + AnimationComponent) + CapsuleColliderComponent(정적 바디 — OverlapSphere에 잡힘) + HealthComponent.
--   맞으면 Hit_A 몽타주, 죽으면 Death_A에서 멈췄다가 ReviveTime 뒤 체력을 채워 일어난다.
--   AttackPlayer = true면 플레이어가 가까울 때 주기적으로 내려친다 (플레이어 피격/막기/구르기 무적 확인용)
local TrainingDummy = {
	Properties = {
		DisplayName    = "훈련용 해골",
		ReviveTime     = 3.0,
		AttackPlayer   = false,
		AttackDamage   = 8.0,
		AttackInterval = 2.2,
		AttackRange    = 200.0,
	},
}

function TrainingDummy:OnStart()
	self.IsEnemy     = true
	self.Health      = self.entity:GetComponent("HealthComponent")
	self.Animation   = self.entity:GetComponent("AnimationComponent")
	self.Player      = Scene.Find("Player")
	self.AttackTimer = self.Properties.AttackInterval
end

function TrainingDummy:GetDisplayName()
	return self.Properties.DisplayName
end

function TrainingDummy:IsDead()
	return self.entity:IsDead()
end

function TrainingDummy:GetHealthFraction()
	return self.Health and self.Health.MaxHealth > 0 and self.Health.Health / self.Health.MaxHealth or 0
end

function TrainingDummy:OnUpdate(dt)
	if not self.Properties.AttackPlayer or self:IsDead() or not self.Player or not self.Player:IsValid() then
		return
	end
	local ToPlayer = self.Player:GetWorldPosition() - self.entity:GetWorldPosition()
	ToPlayer = Vector3(ToPlayer.X, ToPlayer.Y, 0)
	if ToPlayer:Length() > self.Properties.AttackRange then
		self.AttackTimer = math.max(self.AttackTimer, 0.6)
		return
	end
	self.AttackTimer = self.AttackTimer - dt
	if self.AttackTimer > 0 then
		return
	end
	self.AttackTimer = self.Properties.AttackInterval
	-- 모델은 -X를 보므로 180도 더 돌린다
	self.entity:SetRotation(Quat.FromEuler(0, math.deg(math.atan(ToPlayer.Y, ToPlayer.X)) + 180.0, 0))
	self.entity:PlayMontage("1H_Melee_Attack_Chop", { BlendIn = 0.1, BlendOut = 0.2 })
	Timer.After(0.5, function()
		if self:IsDead() or not self.Player:IsValid() then return end
		local D = self.Player:GetWorldPosition() - self.entity:GetWorldPosition()
		if Vector3(D.X, D.Y, 0):Length() <= self.Properties.AttackRange + 30 then
			self.Player:ApplyDamage(self.Properties.AttackDamage, self.entity)
		end
	end)
end

function TrainingDummy:OnDamaged(Amount, Instigator)
	Log.Info(string.format("[Dummy] %s 피해 %.0f → 체력 %.0f/%.0f", self.entity:GetName(), Amount, self.Health.Health, self.Health.MaxHealth))
	if not self:IsDead() then
		self.entity:PlayMontage("Hit_A", { BlendIn = 0.04, BlendOut = 0.15, Speed = 1.3 })
	end
end

function TrainingDummy:OnDeath(Instigator)
	Log.Info("[Dummy]", self.entity:GetName(), "쓰러짐")
	self.entity:StopMontage(nil, 0.05)
	self.Animation.Loop = false
	self.entity:PlayAnimation("Death_A", 0.1)
	Timer.After(self.Properties.ReviveTime, function()
		self.Health.Health = self.Health.MaxHealth
		self.Animation.Loop = true
		self.entity:PlayAnimation("Idle", 0.3)
		Log.Info("[Dummy]", self.entity:GetName(), "다시 일어남")
	end)
end

return TrainingDummy
