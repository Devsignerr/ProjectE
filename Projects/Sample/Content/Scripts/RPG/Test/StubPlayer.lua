-- 시험용 대역 플레이어 (RPG_Test_Items 전용 — 실제 플레이어는 트랙 B의 Scripts/RPG/PlayerController.lua).
--   공통 계약의 플레이어 메서드만 흉내 낸다: GetStats/GetSkillCooldowns/EquipWeapon/EquipShield/RestoreHealth/RestoreMana/IsDead/
--   GetAttackPower/Respawn. 이동/카메라는 IsoPlayer.lua와 같은 방식(화면 기준 WASD), Q/R/Space는 재사용 대기만 돈다.
local StubPlayer = {
	IsPlayer = true, -- 공통 계약: 플레이어 표식 (OnStart 전에도 보이게 클래스에)
	Properties = {
		Camera      = "Camera",
		StartHealth = 64.0,  -- 물약 사용을 볼 수 있게 덜 찬 상태로 시작
		StartMana   = 20.0,
	},
}

function StubPlayer:OnStart()
	self.IsPlayer   = true
	self.MaxHealth  = 100
	self.MaxMana    = 50
	self.MaxStamina = 100
	self.Health     = self.Properties.StartHealth
	self.Mana       = self.Properties.StartMana
	self.Stamina    = self.MaxStamina
	self.Cooldowns  = {
		Skill1 = { Remaining = 0, Duration = 4, Name = "강타" },
		Skill2 = { Remaining = 0, Duration = 8, Name = "화염" },
		Dodge  = { Remaining = 0, Duration = 1.5, Name = "구르기" },
	}
	self.Weapon, self.Shield = nil, nil
	self.Age = 0
	self.SpawnPosition = self.entity:GetPosition()

	self.Camera = Scene.Find(self.Properties.Camera)
	if self.Camera ~= nil then
		local Forward = self.Camera:GetForward()
		local Height  = self.Camera:GetWorldPosition().Z - self.entity:GetWorldPosition().Z
		self.CameraOffset = Forward * (Height / Forward.Z)
		self.ScreenUp     = Vector3(Forward.X, Forward.Y, 0):Normalized()
		self.ScreenRight  = Vector3(-self.ScreenUp.Y, self.ScreenUp.X, 0)
		self.Camera:SetPosition(self.entity:GetWorldPosition() + self.CameraOffset)
	end
end

function StubPlayer:OnUpdate(dt)
	self.Age = self.Age + dt
	for _, Entry in pairs(self.Cooldowns) do
		Entry.Remaining = math.max(0, Entry.Remaining - dt)
	end
	if self:IsDead() or self.Camera == nil then
		return
	end
	self.Stamina = math.min(self.MaxStamina, self.Stamina + 15 * dt)

	local MoveX, MoveY = Input.GetAction("Move")
	local Move = self.ScreenUp * MoveY + self.ScreenRight * MoveX
	if Move:LengthSquared() > 0 then
		self.entity:AddMovementInput(Move)
	end
	if Input.IsKeyPressed("Q") then self:UseSkill("Skill1", 0) end
	if Input.IsKeyPressed("R") then self:UseSkill("Skill2", 15) end
	if Input.IsKeyPressed("Space") then self:UseSkill("Dodge", 0, 25) end
end

function StubPlayer:UseSkill(Name, ManaCost, StaminaCost)
	local Entry = self.Cooldowns[Name]
	if Entry.Remaining > 0 or self.Mana < ManaCost or self.Stamina < (StaminaCost or 0) then
		return
	end
	self.Mana    = self.Mana - ManaCost
	self.Stamina = self.Stamina - (StaminaCost or 0)
	Entry.Remaining = Entry.Duration
end

function StubPlayer:OnLateUpdate(dt)
	if self.Camera ~= nil then
		self.Camera:SetPosition(self.entity:GetWorldPosition() + self.CameraOffset)
	end
end

-- ---------------------------------------------------------------- 공통 계약
function StubPlayer:GetStats()
	return { Health = self.Health, MaxHealth = self.MaxHealth, Mana = self.Mana, MaxMana = self.MaxMana,
		Stamina = self.Stamina, MaxStamina = self.MaxStamina }
end

function StubPlayer:GetSkillCooldowns()
	return self.Cooldowns
end

function StubPlayer:EquipWeapon(Def)
	self.Weapon = Def
	Log.Info("[StubPlayer] 무기 장착:", Def and Def.Name or "(없음)", Def and Def.Model or "")
end

function StubPlayer:EquipShield(Def)
	self.Shield = Def
	Log.Info("[StubPlayer] 방패 장착:", Def and Def.Name or "(없음)", Def and Def.Model or "")
end

function StubPlayer:RestoreHealth(Amount)
	self.Health = math.min(self.MaxHealth, self.Health + Amount)
end

function StubPlayer:RestoreMana(Amount)
	self.Mana = math.min(self.MaxMana, self.Mana + Amount)
end

function StubPlayer:IsDead()
	return self.Health <= 0
end

function StubPlayer:GetAttackPower()
	return 10 + (self.Weapon and self.Weapon.Damage or 0)
end

-- 시험용 (ItemsTestDirector)
function StubPlayer:DebugKill()
	self.Health = 0
	Log.Info("[StubPlayer] 시험용 사망")
end

function StubPlayer:Respawn()
	self.Health  = self.MaxHealth
	self.Mana    = self.MaxMana
	self.entity:SetPosition(self.SpawnPosition)
	Log.Info("[StubPlayer] 부활")
end

return StubPlayer
