-- Crypt2D 플레이어 (Prefabs/Crypt/Player.eprefab — 2D 이동기 + 수도승 플립북 + 자식 Weapon 스프라이트).
--   이동은 이동기가 한다 (스크립트는 입력만: AddMovementInput/Jump/StopJumping/Dash/DropDown).
--   조준: 실제 마우스 커서(Input.GetMouseUIPosition — HUD UI 좌표, 커서는 숨기고 HUD 조준점을 그 자리에) → Camera.ScreenToWorldRay로 월드 점.
--     커서가 게임 화면 밖이면 마지막 조준 유지. 게임패드 오른쪽 스틱(AimStick)이면 몸 기준 방향. 몸 좌우 = 조준 쪽 (SetSpriteFlip), 무기 = 조준 각으로 회전 (FromAxisAngle(+Y, -각)).
--   대시: 조준 방향, 충전 칸(Balance.DashCharges)을 쓰고 시간으로 다시 채운다 — 이동기 공중 대시 횟수는 넉넉히 두고 칸은 여기서 센다. 대시 중 무적.
--   공격: 근접 = 휘두르기(무기가 위/아래로 번갈아 반원을 쓸고 호 효과) + Physics2D.OverlapCircle("Enemy")로 각 안의 적,
--         원거리 = GameManager 투사체. 무기 2칸 교체 Q/휠. 피격 = 무적 깜빡임 + 넉백(entity:AddKnockback — 짧은 경직 동안 입력 무시).
--   자동 플레이(GameManager.Pilot)면 입력을 AutoPilot이 채운 표에서 읽는다.
local U = Script.Require("Scripts/Crypt/Util.lua")

local Player = {
	Properties = {},
}

local Flipbooks = {
	Idle = "Sprites/Crypt/Player_Idle.eflipbook", Run = "Sprites/Crypt/Player_Run.eflipbook", Jump = "Sprites/Crypt/Player_Jump.eflipbook",
	Fall = "Sprites/Crypt/Player_Fall.eflipbook", Dash = "Sprites/Crypt/Player_Dash.eflipbook", Hurt = "Sprites/Crypt/Player_Hurt.eflipbook",
	Death = "Sprites/Crypt/Player_Death.eflipbook",
}
local SwingTime = 0.11
local SwingSpread = 105 -- 휘두르기 끝 각 (조준 기준 ±)

function Player:OnStart()
	self.GM = Scene.Find("GameManager"):GetScript()
	self.Weapon = self.entity:FindChild("Weapon")
	self.Sprite = self.entity:GetComponent("SpriteComponent")
	self.WeaponSprite = self.Weapon and self.Weapon:GetComponent("SpriteComponent") or nil
	local B = self.GM.Balance
	self.MaxHealth = B.MaxHealth
	self.Health = B.MaxHealth
	self.MaxDashCharges = B.DashCharges
	self.DashCharges = B.DashCharges
	self.DashRecharge = 0.0
	self.DashRechargeFraction = 0.0
	self.Invuln = 0.0
	self.HurtTimer = 0.0
	self.AttackTimer = 0.0
	self.SwingSide = 1
	self.SwingTimer = 0.0
	self.Anim = "Idle"
	self.bFacingRight = true
	self.bDead = false
	self.bGodMode = self.GM.Properties.AutoPlay == "Boss"
	self.CursorX, self.CursorY = 640.0 + 160.0, 360.0
	local P = self.entity:GetWorldPosition()
	self.AimX, self.AimZ = P.X + 300, P.Z
	self.AfterimageTimer = 0.0
	self:OnWeaponsChanged()
	self.GM:RefreshHud()
end

function Player:IsInvulnerable()
	return self.Invuln > 0 or self.entity:IsDashing() or self.bGodMode
end

function Player:OnWeaponsChanged()
	local W = self.GM:CurrentWeapon()
	if self.WeaponSprite and W then
		self.WeaponSprite.Slice = W.Slice
	end
end

-- ---- 입력 모으기 (사람 또는 자동 조종)
function Player:GatherInput(Dt)
	local Pilot = self.GM.Pilot
	if Pilot then
		return Pilot:GetInput(self)
	end
	local In = {}
	local MX, MY = Input.GetAction("Move")
	In.MoveX, In.MoveY = MX or 0, MY or 0
	In.JumpPressed = Input.WasActionPressed("Jump")
	In.JumpReleased = Input.WasActionReleased("Jump")
	In.DashPressed = Input.WasActionPressed("Dash")
	In.AttackHeld = Input.IsActionPressed("Attack")
	In.SwapPressed = Input.WasActionPressed("SwapWeapon") or math.abs(Input.GetAction("WeaponWheel") or 0) > 0.1
	In.InteractPressed = Input.WasActionPressed("Interact")

	-- 조준: 게임패드 스틱 우선, 아니면 가상 마우스 커서
	local Hud = self.GM.Hud
	local AX, AY = Input.GetAction("AimStick")
	local P = self.entity:GetWorldPosition()
	if (AX or 0) * (AX or 0) + (AY or 0) * (AY or 0) > 0.09 then
		local NX, NZ = U.Normalize(AX, AY)
		In.AimX, In.AimZ = P.X + NX * 400, P.Z + NZ * 400
		if Hud then
			local SX, SY = Camera.WorldToScreen(U.V(In.AimX, In.AimZ), Hud.entity)
			self.CursorX, self.CursorY = SX, SY
		end
	else
		if Hud then
			local MX, MY, bInside = Input.GetMouseUIPosition(Hud.entity)
			if bInside then self.CursorX, self.CursorY = MX, MY end
		end
		local Origin = nil
		if Hud then Origin = Camera.ScreenToWorldRay(self.CursorX, self.CursorY, Hud.entity) end
		if Origin then
			In.AimX, In.AimZ = Origin.X, Origin.Z
		else
			In.AimX, In.AimZ = self.AimX, self.AimZ
		end
	end
	return In
end

function Player:OnUpdate(Dt)
	local GM = self.GM
	if GM.bPaused then return end
	local E = self.entity
	local P = E:GetWorldPosition()

	-- 히트스톱(게임 시간 배율 0): 입력·타이머 모두 멈춘 채 (화면이 멈춰 보이게)
	if Time.GetTimeScale() == 0 then return end
	self.Invuln = math.max(0, self.Invuln - Dt)
	self.HurtTimer = math.max(0, self.HurtTimer - Dt)
	self.AttackTimer = math.max(0, self.AttackTimer - Dt)
	self.SwingTimer = math.max(0, self.SwingTimer - Dt)
	local bVisible = self.Invuln <= 0 or (math.floor(self.Invuln / 0.07) % 2 == 0)
	if self.Sprite.Visible ~= bVisible then
		self.Sprite.Visible = bVisible
		if self.WeaponSprite then self.WeaponSprite.Visible = bVisible and not self.bDead end
	end

	-- 대시 충전
	if self.DashCharges < self.MaxDashCharges then
		self.DashRecharge = self.DashRecharge + Dt
		if self.DashRecharge >= GM.Balance.DashRecharge then
			self.DashRecharge = 0
			self.DashCharges = self.DashCharges + 1
		end
		self.DashRechargeFraction = self.DashRecharge / GM.Balance.DashRecharge
	else
		self.DashRechargeFraction = 0
	end

	if self.bDead or GM.bGameOver then
		self:UpdateAnimation()
		if GM.Hud then GM.Hud:SetDash(self.DashCharges, self.MaxDashCharges, self.DashRechargeFraction) end
		return
	end

	local In = self:GatherInput(Dt)
	self.AimX, self.AimZ = In.AimX or self.AimX, In.AimZ or self.AimZ
	if GM.Hud then GM.Hud:SetCrosshair(self.CursorX, self.CursorY, GM.Pilot == nil) end

	-- 이동
	E:AddMovementInput(Vector3(In.MoveX or 0, 0, 0))
	if In.JumpPressed then
		if (In.MoveY or 0) < -0.5 and E:IsGrounded() then
			E:DropDown()
		else
			E:Jump()
		end
	end
	if In.JumpReleased then E:StopJumping() end

	-- 대시 (조준 방향)
	local AimDX, AimDZ = U.Normalize(self.AimX - P.X, self.AimZ - (P.Z + 8))
	if In.DashPressed and self.DashCharges > 0 and not E:IsDashing() then
		self.DashCharges = self.DashCharges - 1
		E:Dash(Vector3(AimDX, 0, AimDZ))
		self.AfterimageTimer = 0.0
		self.DashTime = 0.2
		GM:Sound("Dash")
		GM:SpawnFx("Fx_Dust", P.X, P.Z - 80, { FlipX = AimDX > 0 })
	end
	if self.DashTime and self.DashTime > 0 then
		self.DashTime = self.DashTime - Dt
		self.AfterimageTimer = self.AfterimageTimer - Dt
		if self.AfterimageTimer <= 0 then
			self.AfterimageTimer = 0.035
			GM:SpawnAfterimage("Sprites/Crypt/Player.esprite", "FlyKick0", P.X, P.Z, not self.bFacingRight)
		end
	end

	-- 몸 방향 = 조준 쪽 (시트는 오른쪽을 본다)
	self.bFacingRight = self.AimX >= P.X
	E:SetSpriteFlip(not self.bFacingRight, false)

	-- 무기 교체·상호작용·공격
	if In.SwapPressed then
		GM:SwapWeapon()
		self:OnWeaponsChanged()
	end
	if In.InteractPressed then GM:Interact() end
	local Weapon = GM:CurrentWeapon()
	if In.AttackHeld and self.AttackTimer <= 0 and Weapon then
		self:Attack(Weapon, P, AimDX, AimDZ)
	end
	self:UpdateWeaponPose(Weapon, AimDX, AimDZ)
	self:UpdateAnimation()
	if GM.Hud then GM.Hud:SetDash(self.DashCharges, self.MaxDashCharges, self.DashRechargeFraction) end
end

function Player:UpdateWeaponPose(Weapon, DX, DZ)
	if self.Weapon == nil or Weapon == nil then return end
	local Aim = U.AngleOf(DX, DZ)
	local Angle = Aim
	local bLeft = DX < 0
	if Weapon.Kind == "Melee" then
		-- 쉬는 자세 = 조준 ± 휘두르기 끝 각, 휘두르는 동안 반대편에서 이쪽으로 쓸어 온다
		local Rest = Aim + self.SwingSide * SwingSpread * (bLeft and -1 or 1)
		if self.SwingTimer > 0 then
			local T = 1 - self.SwingTimer / SwingTime
			local From = Aim - self.SwingSide * SwingSpread * (bLeft and -1 or 1)
			Angle = U.Lerp(From, Rest, T * T * (3 - 2 * T))
		else
			Angle = Rest
		end
	end
	local HX, HZ = U.DirOf(Aim)
	self.Weapon:SetPosition(U.V(HX * 18, 8 + HZ * 10, 2))
	self.Weapon:SetRotation(U.Rot2D(Angle))
	-- 왼쪽을 볼 때 무기가 뒤집혀 보이지 않게 세로 반전
	self.Weapon:SetSpriteFlip(false, bLeft)
end

function Player:Attack(Weapon, P, DX, DZ)
	local GM = self.GM
	self.AttackTimer = Weapon.Cooldown
	local Damage = GM.Rng:Range(Weapon.DamageMin, Weapon.DamageMax)
	local bCrit = GM.Rng:Chance(Weapon.CritChance)
	if bCrit then Damage = Damage * Weapon.CritMultiplier end
	Damage = math.floor(Damage + 0.5)
	Audio.PlayOneShot(Weapon.SwingSound)
	local Aim = U.AngleOf(DX, DZ)
	local CX, CZ = P.X, P.Z + 8
	if Weapon.Kind == "Melee" then
		self.SwingSide = -self.SwingSide
		self.SwingTimer = SwingTime
		-- 호 효과: 조준 방향, 휘두르는 방향에 맞춰 세로 반전
		local FX, FZ = CX + DX * Weapon.Range * 0.55, CZ + DZ * Weapon.Range * 0.55
		GM:SpawnFx("Fx_Slash", FX, FZ, { Rotation = Aim, FlipY = self.SwingSide < 0, Blend = 2, Scale = Weapon.Range / 120,
		                                  Color = bCrit and { 1, 0.85, 0.5, 1 } or { 1, 1, 1, 1 } })
		local Hits = 0
		for _, Enemy in ipairs(Physics2D.OverlapCircle(U.V(CX + DX * Weapon.Range * 0.45, CZ + DZ * Weapon.Range * 0.45), Weapon.Range * 0.75, "Enemy")) do
			local S = Enemy:GetScript()
			if S and S.TakeDamage and not S.bDead then
				local EP = Enemy:GetWorldPosition()
				local TX, TZ, Dist = U.Normalize(EP.X - CX, EP.Z - CZ)
				local Delta = math.abs(U.AngleDelta(Aim, U.AngleOf(TX, TZ)))
				if Dist < (S.Radius or 40) + 40 or Delta <= Weapon.Arc * 0.5 then
					if S:TakeDamage(Damage, bCrit, DX, DZ, Weapon.Knockback) then Hits = Hits + 1 end
				end
			end
		end
		if Hits > 0 then
			GM:SetHitStop(bCrit and 0.085 or 0.055)
			GM:AddShake(bCrit and 7 or 4, 0.15)
		end
	else
		local TipX, TipZ = CX + DX * 70, CZ + DZ * 70
		GM:SpawnProjectile({ Team = "Player", X = TipX, Z = TipZ, VX = DX * Weapon.ProjectileSpeed, VZ = DZ * Weapon.ProjectileSpeed,
		                     Damage = Damage, Crit = bCrit, Kind = Weapon.Projectile, Radius = Weapon.Projectile == "Fireball" and 30 or 18,
		                     Range = Weapon.Range, Pierce = Weapon.Pierce, Explosion = Weapon.Explosion, Knockback = Weapon.Knockback })
		GM:AddShake(2, 0.08)
	end
end

function Player:UpdateAnimation()
	local E = self.entity
	local Want = "Idle"
	local V = E:GetMovementVelocity()
	if self.bDead then
		Want = "Death"
	elseif self.HurtTimer > 0 then
		Want = "Hurt"
	elseif E:IsDashing() then
		Want = "Dash"
	elseif not E:IsGrounded() then
		Want = V.Z > 50 and "Jump" or "Fall"
	elseif math.abs(V.X) > 40 then
		Want = "Run"
	end
	if Want ~= self.Anim then
		self.Anim = Want
		E:PlayFlipbook(Flipbooks[Want])
	end
end

function Player:TakeDamage(Amount, SX, SZ)
	if self.bDead or self:IsInvulnerable() or self.GM.bGameOver then
		return false
	end
	local GM = self.GM
	self.Health = math.max(0, self.Health - Amount)
	self.Invuln = GM.Balance.InvulnTime
	self.HurtTimer = 0.28
	local P = self.entity:GetWorldPosition()
	GM:Sound("Hurt")
	GM:AddShake(9, 0.25)
	GM:SetHitStop(0.06)
	-- 넉백: 맞은 쪽 반대로 튕기며 살짝 뜬다 (경직 동안 입력 무시 — 이동기가 무브 안에서 처리)
	local Away = (P.X >= (SX or P.X)) and 1 or -1
	if not self.entity:IsDashing() then
		self.entity:AddKnockback(Vector3(Away * 520, 0, 420), 0.16)
	end
	GM:ShowNumber(P.X, P.Z + 110, tostring(math.floor(Amount)), { 1, 0.3, 0.3, 1 }, 1.1)
	GM:SpawnFx("Fx_Spark", P.X, P.Z + 20, { Color = { 1, 0.4, 0.4, 1 } })
	if self.Health <= 0 then
		self:Die()
	end
	GM:RefreshHud()
	return true
end

function Player:Heal(Amount)
	self.Health = math.min(self.MaxHealth, self.Health + Amount)
	self.GM:RefreshHud()
end

function Player:Die()
	self.bDead = true
	if self.WeaponSprite then self.WeaponSprite.Visible = false end
	self.GM:SpawnFx("Fx_Death", self.entity:GetWorldPosition().X, self.entity:GetWorldPosition().Z, {})
	self.GM:OnPlayerDied()
end

function Player:OnJumped(N)
	local P = self.entity:GetWorldPosition()
	self.GM:Sound("Jump")
	self.GM:SpawnFx("Fx_Dust", P.X, P.Z - 80, { Scale = N >= 2 and 0.8 or 1.0 })
	if self.GM.Pilot then self.GM.Pilot:OnPlayerEvent("Jumped", N) end
end

function Player:OnLanded()
	local P = self.entity:GetWorldPosition()
	self.GM:SpawnFx("Fx_Dust", P.X, P.Z - 80, {})
	if self.GM.Pilot then self.GM.Pilot:OnPlayerEvent("Landed", 0) end
end

function Player:OnDashStarted()
	if self.GM.Pilot then self.GM.Pilot:OnPlayerEvent("Dash", 0) end
end

return Player
