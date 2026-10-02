-- 치유 지대 (능력 표 "HealZone"): 마나 30, 쿨다운 10초. 서버가 발밑에 지대를 깔고 5초 동안 1초마다
-- 반경 안의 능력 시스템 엔티티에 HealZoneTick(IncomingHeal = 대상 MaxHealth × 8% + 2)을 건다. 발동 중 State.Channeling
local HealZone = { Properties = { Radius = 350.0, Duration = 5.0, Interval = 1.0 } }

function HealZone:OnActivate(ctx)
	if not ctx:HasAuthority() then
		return
	end
	local P      = self.Properties
	local Center = ctx.Owner:GetWorldPosition()
	self.Zone    = Scene.Create("HealZone")
	local Mesh   = self.Zone:AddComponent("StaticMeshComponent")
	Mesh.MeshAsset     = "primitive:sphere"
	Mesh.MaterialAsset = "Materials/Green.emat"
	self.Zone:AddComponent("ReplicatedComponent")
	self.Zone:SetPosition(Center - Vector3(0, 0, 88))
	self.Zone:SetScale(Vector3(P.Radius / 50, P.Radius / 50, 0.06)) -- 기본 구 반지름 50cm

	for _ = 1, math.floor(P.Duration / P.Interval + 0.5) do
		for _, Target in ipairs(Abilities.FindInRadius(Center, P.Radius)) do
			ctx:ApplyEffectToTarget(Target, "HealZoneTick")
		end
		ctx:Wait(P.Interval)
	end
end

function HealZone:OnEnd(ctx, cancelled)
	if self.Zone and self.Zone:IsValid() then
		self.Zone:Destroy()
	end
end

return HealZone