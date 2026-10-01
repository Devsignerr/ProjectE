-- 게임플레이 예제 표적 (서버): HealthComponent가 있는 엔티티에 붙인다.
--   맞으면 잠깐 커지고, 죽으면 숨었다가 리스폰(DeathAction = RespawnInPlace, 지연은 게임 모드 RespawnDelay)하면 다시 보인다.
--   체력/크기/표시는 복제되므로 클라이언트는 결과만 본다
local Target = {
	Properties = {
		BaseScale = 0.8,
		HitScale  = 0.3, -- 맞았을 때 커지는 비율
	},
}

function Target:OnStart()
	self.Pulse = 0
	self.Mesh  = self.entity:GetComponent("StaticMeshComponent")
end

function Target:OnUpdate(dt)
	if self.Pulse > 0 then
		self.Pulse = math.max(0, self.Pulse - dt * 4)
	end
	local Scale = self.Properties.BaseScale * (1 + self.Properties.HitScale * self.Pulse)
	self.entity:SetScale(Vector3(Scale, Scale, Scale))
end

function Target:OnDamaged(amount, instigator)
	self.Pulse = 1
end

function Target:OnDeath(instigator)
	self.Mesh.Visible = false
end

function Target:OnRespawned()
	self.Mesh.Visible = true
	self.Pulse        = 1
end

return Target
