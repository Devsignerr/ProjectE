-- HD-2D 데모 슬라임 (Prefabs/Demo/HD2D/Slime.eprefab — 캡슐 이동기 + Visual > Body 스프라이트·Shadow).
--   깡충 뛰며 움직인다: 쉬기(Idle 플립북) → 뛰기(Hop 플립북, HopTime 동안만 이동 입력 + 몸 스프라이트를 포물선으로 띄움) → 착지 먼지.
--   플레이어가 ChaseRadius 안이면 쫓아가고, 아니면 집(처음 자리) 근처를 돌아다닌다. 닿으면 접촉 피해(쿨다운).
--   맞으면 붉게 번쩍 + 넉백(entity:AddKnockback — 경직 동안 이동 입력 무시), 체력 0이면 펑 효과 후 사라지고 관리자가 다른 자리에 부활시킨다.
--   이동기가 루트를 이동 방향으로 돌리므로 Visual 회전을 매 프레임 상쇄해 스프라이트가 항상 카메라(+Y)를 보게 한다.
local HD2DSlime = {
	Properties = {
		MaxHealth     = 3,
		HopInterval   = 1.1,  -- 초 (쉬는 시간 평균, 쫓을 때는 절반)
		HopTime       = 0.42, -- 초 (공중)
		HopHeight     = 46.0, -- cm
		ChaseRadius   = 700.0,
		WanderRadius  = 450.0,
		ContactRadius = 80.0,
		Damage        = 1,
	},
}

local IdleBook = "Sprites/HD2D/Slime_Idle.eflipbook"
local HopBook  = "Sprites/HD2D/Slime_Hop.eflipbook"

function HD2DSlime:OnStart()
	self.GM = Scene.Find("HD2DGame"):GetScript()
	self.Visual = self.entity:FindChild("Visual")
	self.Body = self.Visual:FindChild("Body")
	self.Sprite = self.Body:GetComponent("SpriteComponent")
	self.BaseColor = self.Sprite.Color
	self.BodyBase = self.Body:GetPosition()
	self.Health = self.Properties.MaxHealth
	self.Home = self.entity:GetWorldPosition()
	self.Seed = 7 + self.entity.Id * 977
	self.Wait = 0.3 + self:Random() * self.Properties.HopInterval
	self.HopT = -1.0
	self.HopDir = Vector3(0, 0, 0)
	self.FlashTime = 0.0
	self.ContactCooldown = 0.0
	self.bDead = false
	self.GM:RegisterSlime(self)
	self.GM:SpawnFx("Poof", self.entity:GetWorldPosition() + Vector3(0, 6, -30), { Scale = 0.7 })
end

function HD2DSlime:OnDestroy()
	if self.GM then self.GM:UnregisterSlime(self) end
end

function HD2DSlime:Random()
	self.Seed = (self.Seed * 1103515245 + 12345) % 2147483648
	return (self.Seed % 100000) / 100000.0
end

local function Flat(V)
	return Vector3(V.X, V.Y, 0)
end

function HD2DSlime:OnUpdate(Dt)
	if self.bDead then return end
	local E = self.entity
	local Pos = E:GetWorldPosition()
	local Player = self.GM:GetPlayer()
	local ToPlayer, PlayerDist = Vector3(0, 0, 0), 1.0e9
	if Player and not Player.bDead then
		ToPlayer = Flat(Player.entity:GetWorldPosition() - Pos)
		PlayerDist = ToPlayer:Length()
	end
	local bChase = PlayerDist < self.Properties.ChaseRadius

	-- 피격 번쩍임
	if self.FlashTime > 0 then
		self.FlashTime = self.FlashTime - Dt
		local C = self.BaseColor
		if self.FlashTime > 0 then
			self.Sprite.Color = Vector4(1, 0.35, 0.35, 1)
		else
			self.Sprite.Color = C
		end
	end

	if self.HopT >= 0 then
		-- 공중: 이동 입력 + 포물선
		self.HopT = self.HopT + Dt
		local T = math.min(self.HopT / self.Properties.HopTime, 1.0)
		if not E:IsStunned() then
			E:AddMovementInput(self.HopDir)
		end
		self.Body:SetPosition(self.BodyBase + Vector3(0, 0, math.sin(T * math.pi) * self.Properties.HopHeight))
		if T >= 1.0 then
			self.HopT = -1.0
			self.Body:SetPosition(self.BodyBase)
			self.Body:PlayFlipbook(IdleBook)
			local Interval = self.Properties.HopInterval * (bChase and 0.5 or 1.0)
			self.Wait = Interval * (0.7 + self:Random() * 0.6)
			self.GM:SpawnFx("Dust", Pos + Vector3(0, 4, -40), { Scale = 0.8 })
		end
	else
		self.Wait = self.Wait - Dt
		if self.Wait <= 0 and not E:IsStunned() then
			local Dir
			if bChase then
				Dir = ToPlayer:Normalized()
			else
				local FromHome = Flat(Pos - self.Home)
				if FromHome:Length() > self.Properties.WanderRadius then
					Dir = (FromHome * -1):Normalized()
				else
					local A = self:Random() * math.pi * 2
					Dir = Vector3(math.cos(A), math.sin(A), 0)
				end
			end
			self.HopDir = Dir
			self.HopT = 0.0
			self.Body:PlayFlipbook(HopBook)
			if Dir.X < -0.2 then self.Body:SetSpriteFlip(true, false) elseif Dir.X > 0.2 then self.Body:SetSpriteFlip(false, false) end
		end
	end

	-- 접촉 피해 (뛰는 중이든 쉬는 중이든 닿으면 — 쿨다운)
	self.ContactCooldown = math.max(0.0, self.ContactCooldown - Dt)
	if Player and PlayerDist < self.Properties.ContactRadius and self.ContactCooldown <= 0 then
		if Player:TakeDamage(self.Properties.Damage, Pos) then
			self.ContactCooldown = 0.9
		end
	end
end

function HD2DSlime:OnLateUpdate(Dt)
	-- 이동기가 돌린 루트 회전 상쇄 → 스프라이트는 항상 카메라를 본다
	self.Visual:SetRotation(self.entity:GetRotation():Inverse())
end

-- Dir = 맞은 방향 (수평 단위 벡터)
function HD2DSlime:TakeHit(Damage, Dir)
	if self.bDead then return false end
	self.Health = self.Health - Damage
	self.FlashTime = 0.12
	self.HopT = -1.0
	self.Body:SetPosition(self.BodyBase)
	self.Wait = 0.45
	self.entity:AddKnockback(Dir * 720, 0.22)
	self.Body:PlayFlipbook(IdleBook)
	local P = self.entity:GetWorldPosition()
	self.GM:SpawnFx("Spark", P + Vector3(0, 30, 10), { Blend = 2, Scale = 1.4 })
	if self.Health <= 0 then
		self.bDead = true
		self.GM:SpawnFx("Poof", P + Vector3(0, 10, -30), { Scale = 1.2 })
		self.GM:OnSlimeKilled(self)
		self.entity:Destroy()
	end
	return true
end

return HD2DSlime
