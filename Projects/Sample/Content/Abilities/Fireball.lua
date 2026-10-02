-- 화염구 (능력 표 "Fireball"): 마나 15, 쿨다운 1초. 투사체·데미지는 서버만 (클라이언트는 비용·쿨다운·발동 태그 State.Casting만 예측).
-- 명중: FireballHit(SetByCaller Damage → IncomingDamage, 방어막 DamageTaken 배율 적용) + Burn(1초마다 화상, 대상에 최대 3스택, 지속 갱신)
local Fireball = { Properties = { Speed = 1800.0, Range = 2200.0, HitRadius = 90.0, Damage = 20.0, AimAngle = 30.0 } }

-- 약한 자동 조준: 앞쪽 AimAngle도 안의 가장 가까운 능력 시스템 엔티티 쪽으로 (없으면 정면)
local function Aim(Owner, Forward, Range, AimAngle)
	local Origin = Owner:GetWorldPosition()
	local Cos    = math.cos(math.rad(AimAngle))
	for _, Target in ipairs(Abilities.FindInRadius(Origin, Range, Owner)) do
		local To = Target:GetWorldPosition() - Origin
		To       = Vector3(To.X, To.Y, 0)
		if To:Length() > 1 and To:Normalized():Dot(Forward) >= Cos then
			return To:Normalized()
		end
	end
	return Forward
end

function Fireball:OnActivate(ctx)
	if not ctx:HasAuthority() then
		return -- 예측 실행은 여기까지 (투사체는 서버가 만들고 복제한다)
	end
	local P       = self.Properties
	local Owner   = ctx.Owner
	local Forward = Owner:GetForward()
	Forward       = Aim(Owner, Vector3(Forward.X, Forward.Y, 0):Normalized(), P.Range, P.AimAngle)
	local Position = Owner:GetWorldPosition() + Forward * 70 + Vector3(0, 0, 20)

	self.Ball = Scene.Create("Fireball")
	local Mesh = self.Ball:AddComponent("StaticMeshComponent")
	Mesh.MeshAsset     = "primitive:sphere"
	Mesh.MaterialAsset = "Materials/Orange.emat"
	self.Ball:AddComponent("ReplicatedComponent")
	self.Ball:SetScale(Vector3(0.45, 0.45, 0.45))
	self.Ball:SetPosition(Position)

	local Travelled = 0
	while Travelled < P.Range do
		ctx:Wait(0)
		local Step = P.Speed * Time.DeltaTime
		Position  = Position + Forward * Step
		Travelled = Travelled + Step
		self.Ball:SetPosition(Position)
		local Hits = Abilities.FindInRadius(Position, P.HitRadius, Owner)
		if #Hits > 0 then
			local Target = Hits[1]
			ctx:ApplyEffectToTarget(Target, "FireballHit", { SetByCaller = { Damage = P.Damage } })
			ctx:ApplyEffectToTarget(Target, "Burn")
			Log.Info("화염구 명중:", Target:GetName())
			return
		end
	end
end

function Fireball:OnEnd(ctx, cancelled)
	if self.Ball and self.Ball:IsValid() then
		self.Ball:Destroy()
	end
end

return Fireball