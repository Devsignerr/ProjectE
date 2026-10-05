-- HD-2D 데모 플레이어 (Prefabs/Demo/HD2D/Player.eprefab — 캡슐 이동기 + Visual > Body 도트 스프라이트·Shadow).
--   조작 (입력 액션): Move(WASD/왼쪽 스틱) 이동, Attack(J/마우스 왼쪽) 공격, Dodge(Space) 회피 대시, Interact(E) 대화·상자,
--             Inventory(I/Tab) 소지품, Skill2(R) 무기 교체, UsePotion1(1) 회복약, UsePotion2(2) 마나 물약.
--   메뉴(대화·인벤토리·상점)가 열려 있으면 입력을 관리자(HD2DGame:MenuInput)에 넘긴다 — W/S 고르기, E·J 확인, I·Space 닫기.
--   이동은 이동기가 한다 (스크립트는 AddMovementInput만). 방향 = 마지막 입력/조준의 주된 축 → 아래/위/옆 3방향 플립북, 왼쪽은 좌우 반전.
--   무기 (Data/Demo/HD2D/Weapons.etable — 관리자의 장비 무기):
--     Slash  검: 바라보는 쪽 부채꼴, 세 번째 연타는 크게 (피해 ×1.6, 큰 호, 긴 멈춤)
--     Thrust 창: 좁고 긴 직선 — 줄지은 적을 모두 꿰뚫는다 (찌르기 궤적 효과)
--     Arrow  활: 가까운 적을 자동 조준해 화살 (관리자 투사체)
--     Bolt   지팡이: 마나를 써 빛의 탄 — 맞으면 터져 주변도 (꼬리 잔상)
--   대시: entity:AddKnockback(방향 × DashSpeed, DashTime) — 그동안 무적 + 잔상. 피격: 무적 깜빡임 + 넉백 + 붉은 숫자. 쓰러지면 마을 시작 자리에서 부활.
--   레벨: 경험치(Balance.ExpTable)가 차면 레벨 업 — 최대 HP/MP·공격 배율 증가, 완전 회복, 빛기둥 연출 + 팡파르.
--   부스트(옥토패스 BP): BP 최대 Balance.BoostMax, BoostRegenTime초마다·명중 BoostHitsPerPoint번마다 +1. Skill1(Q)로 단계를 올린 뒤(최대 BoostMaxLevel)
--     공격하면 단계 L만큼 BP를 쓰고: 근접 = 1+L번 연타, 활 = 1+L발 부채꼴, 지팡이 = 1+L발 — 피해 ×(1 + BoostDamagePerLevel × L) + 번쩍임·글자·빛.
--   장비: 관리자의 방어구·장신구 합(RecalcStats) — 방어력(받는 피해 × 60/(60+방어×3)), 최대 HP, 이동 속도(이동기 MaxWalkSpeed), 치명타.
--   카메라: OnLateUpdate(물리 뒤)에 지정 카메라를 발 위치 + 시작 오프셋으로 부드럽게 따라 놓는다 (회전은 씬 값 — 고정 시점 디오라마).
--   이동기가 루트를 이동 방향으로 돌리므로 Visual 회전을 매 프레임 상쇄한다. 자동 검증이면 입력을 HD2DAutoPilot.lua가 채운다.
local D = Script.Require("Scripts/Demo/HD2D/HD2DData.lua")
local AutoPilot = Script.Require("Scripts/Demo/HD2D/HD2DAutoPilot.lua")

local HD2DPlayer = {
	Properties = {
		Camera         = "Camera",
		CameraDistance = 2600.0, -- 초점(발 위 FocusHeight)에서 카메라까지 (cm)
		FocusHeight    = 70.0,
		CameraLag      = 7.0,    -- 1/초 (클수록 바짝 따라감)
		MinX = -100000.0, MaxX = 100000.0, MinY = -100000.0, MaxY = 100000.0, -- 카메라 초점 범위 (맵 가장자리가 보이지 않게)
	},
}

local HeroSprite = "Sprites/HD2D/Hero.esprite"
-- 방향 이름 → (월드 방향, 플립북 방향, 옆모습 반전)
local Dirs = {
	Down  = { V = Vector3(0, 1, 0),  Anim = "Down", Flip = false },
	Up    = { V = Vector3(0, -1, 0), Anim = "Up",   Flip = false },
	Right = { V = Vector3(1, 0, 0),  Anim = "Side", Flip = false },
	Left  = { V = Vector3(-1, 0, 0), Anim = "Side", Flip = true },
}

local function Flat(V) return Vector3(V.X, V.Y, 0) end

local function FacingFromMove(Move, Current)
	if math.abs(Move.X) < 0.1 and math.abs(Move.Y) < 0.1 then
		return Current
	end
	-- 대각선은 옆모습 (도트 RPG 관례)
	if math.abs(Move.X) >= math.abs(Move.Y) * 0.8 then
		return Move.X > 0 and "Right" or "Left"
	end
	return Move.Y > 0 and "Down" or "Up"
end

function HD2DPlayer:OnStart()
	self.GM = Scene.Find("HD2DGame"):GetScript()
	self.Visual = self.entity:FindChild("Visual")
	self.Body = self.Visual:FindChild("Body")
	self.Sprite = self.Body:GetComponent("SpriteComponent")
	local B = D.Balance()
	self.Name = B.PlayerName
	self.Level, self.Exp = 1, 0
	self.Defense, self.CritBonus = 0, 0
	self.Mover = self.entity:GetComponent("CharacterMovementComponent")
	self.BaseWalkSpeed = self.Mover.MaxWalkSpeed
	self:ApplyLevel()
	self.Health, self.Mana = self.MaxHealth, self.MaxMana
	self.BP, self.BoostLevel, self.BoostRegen, self.HitsForBP = B.BoostStart, 0, 0.0, 0
	self.HitQueue, self.AttackBoost = {}, 0
	self.Facing = "Down"
	self.AimDir = Dirs.Down.V
	self.Anim = ""
	self.AttackTimer, self.AttackCooldown, self.bHitDone, self.SwingSide = 0.0, 0.0, true, 1
	self.Combo, self.ComboWindow, self.bQueued = 0, 0.0, false
	self.DashTimer, self.DashCooldown, self.AfterimageTimer = 0.0, 0.0, 0.0
	self.Invuln, self.HurtFlash, self.NoManaTimer = 0.0, 0.0, 0.0
	self.bDead = false
	self.DamageTakenScale = 1.0
	self.Start = self.entity:GetWorldPosition()
	self.LastPos = self.Start
	self.Stats = { Distance = 0, Attacks = 0, Hits = 0, Dashes = 0, Damaged = 0, Deaths = 0, LevelUps = 0 }
	self.MenuHeldDir, self.MenuRepeat, self.MenuHeldX = 0, 0.0, 0
	if self.GM.Properties.AutoPlay ~= "" then
		self.Pilot = AutoPilot.New(self.GM.Properties.AutoPlay, self, self.GM)
		self.DamageTakenScale = self.Pilot.DamageTakenScale
	end
	self.Camera = Scene.Find(self.Properties.Camera)
	if self.Camera then
		self.CamOffset = self.Camera:GetForward() * -self.Properties.CameraDistance
		self.CamPos = self:CameraTarget()
		self.Camera:SetPosition(self.CamPos)
	end
	self:OnWeaponChanged()
	self:UpdateAnimation(Vector3(0, 0, 0))
	self.bStarted = true
	if self.GM.PendingPlayer then self:ApplySave(self.GM.PendingPlayer) end
end

-- 저장 값 적용 (관리자 ApplySave가 부른다 — 장비는 관리자에 이미 들어 있다)
function HD2DPlayer:ApplySave(Data)
	self.Level = math.max(1, math.floor(Data.Level or 1))
	self.Exp = math.floor(Data.Exp or 0)
	self:ApplyLevel()
	self.Health = math.max(1, math.min(self.MaxHealth, Data.Health or self.MaxHealth))
	self.Mana = math.min(self.MaxMana, Data.Mana or self.MaxMana)
	self.BP = math.floor(Data.BP or 1)
	self.BoostLevel = 0
	self:OnWeaponChanged()
end

-- 장비·레벨에서 능력치 다시 계산
function HD2DPlayer:BaseMaxHealth()
	local B = D.Balance()
	return math.floor(B.MaxHealth + B.HealthPerLevel * (self.Level - 1))
end

function HD2DPlayer:RecalcStats()
	local Gear = self.GM:GearStats()
	local Old = self.MaxHealth or 0
	self.MaxHealth = math.floor(self:BaseMaxHealth() + Gear.HealthBonus)
	if self.Health then
		self.Health = math.min(self.MaxHealth, self.Health + math.max(0, self.MaxHealth - Old))
	end
	self.Defense = Gear.Defense
	self.CritBonus = Gear.CritBonus
	self.Mover.MaxWalkSpeed = self.BaseWalkSpeed * (1.0 + Gear.SpeedBonus)
end

function HD2DPlayer:SnapCamera()
	if self.Camera then
		self.CamPos = self:CameraTarget()
		self.Camera:SetPosition(self.CamPos)
	end
end

-- ---- 레벨·회복
function HD2DPlayer:ApplyLevel()
	local B = D.Balance()
	self.MaxMana = math.floor(B.MaxMana + B.ManaPerLevel * (self.Level - 1))
	self:RecalcStats()
end

function HD2DPlayer:DamageScale()
	return 1.0 + D.Balance().DamagePerLevel * (self.Level - 1)
end

function HD2DPlayer:ExpFraction()
	local T = D.Balance().ExpTable
	local Cur, Next = T[self.Level] or 0, T[self.Level + 1]
	if not Next then return 1.0 end
	return math.max(0.0, math.min(1.0, (self.Exp - Cur) / (Next - Cur)))
end

function HD2DPlayer:AddExp(N)
	self.Exp = self.Exp + N
	local T = D.Balance().ExpTable
	while T[self.Level + 1] and self.Exp >= T[self.Level + 1] do
		local OldHp, OldMp = self.MaxHealth, self.MaxMana
		self.Level = self.Level + 1
		self:ApplyLevel()
		self.Health, self.Mana = self.MaxHealth, self.MaxMana
		self.Stats.LevelUps = self.Stats.LevelUps + 1
		self.GM:SpawnLevelFx(self.entity:GetWorldPosition())
		self.GM:Hud():Announce("LEVEL UP!", string.format("Lv %d   최대 HP +%d   최대 MP +%d", self.Level, self.MaxHealth - OldHp, self.MaxMana - OldMp), 2.5)
		self.GM:Fanfare()
		Log.Info(string.format("[HD2D] 레벨 업: Lv %d (HP %d, MP %d)", self.Level, self.MaxHealth, self.MaxMana))
	end
end

function HD2DPlayer:Heal(Hp, Mp)
	local Pos = self.entity:GetWorldPosition()
	if Hp > 0 then
		local Before = self.Health
		self.Health = math.min(self.MaxHealth, self.Health + Hp)
		self.GM:DamageNumber(Pos + Vector3(0, 0, 60), "+" .. math.floor(self.Health - Before + 0.5), { 0.5, 1, 0.55, 1 }, 1.0)
		self.GM:SpawnHealFx(Pos, { 0.6, 1, 0.6, 1 })
	end
	if Mp > 0 then
		local Before = self.Mana
		self.Mana = math.min(self.MaxMana, self.Mana + Mp)
		self.GM:DamageNumber(Pos + Vector3(0, 0, 90), "+" .. math.floor(self.Mana - Before + 0.5) .. " MP", { 0.55, 0.75, 1, 1 }, 0.9)
		self.GM:SpawnHealFx(Pos, { 0.6, 0.8, 1, 1 })
	end
end

function HD2DPlayer:OnWeaponChanged()
	self.Combo = 0
	self.Weapon = self.GM:GetWeapon()
	self.GM:Hud():SetWeapon(self.Weapon)
	self.Anim = ""
end

-- ---- 입력 (사람 또는 자동 조종)
function HD2DPlayer:GatherInput()
	if self.Pilot then
		return self.Pilot:Step(Time.GetUnscaledDelta())
	end
	local MX, MY = Input.GetAction("Move")
	local In = { Move = Vector3(MX, -MY, 0), Attack = Input.WasActionPressed("Attack"), Dash = Input.WasActionPressed("Dodge"),
	             Interact = Input.WasActionPressed("Interact"), Inventory = Input.WasActionPressed("Inventory"), Switch = Input.WasActionPressed("Skill2"),
	             Use1 = Input.WasActionPressed("UsePotion1"), Use2 = Input.WasActionPressed("UsePotion2"), Boost = Input.WasActionPressed("Skill1") }
	local DirX = MX > 0.5 and 1 or (MX < -0.5 and -1 or 0)
	if DirX ~= 0 and DirX ~= self.MenuHeldX then
		In.MenuRight, In.MenuLeft = DirX == 1, DirX == -1
	end
	self.MenuHeldX = DirX
	In.Confirm = In.Interact or In.Attack
	In.Cancel = In.Dash
	-- 메뉴 위/아래: 누른 순간 + 누르고 있으면 반복
	local Dir = MY > 0.5 and 1 or (MY < -0.5 and -1 or 0)
	local UDt = Time.GetUnscaledDelta()
	if Dir ~= 0 and Dir ~= self.MenuHeldDir then
		self.MenuRepeat = 0.35
		In.MenuUp, In.MenuDown = Dir == 1, Dir == -1
	elseif Dir ~= 0 then
		self.MenuRepeat = self.MenuRepeat - UDt
		if self.MenuRepeat <= 0 then
			self.MenuRepeat = 0.11
			In.MenuUp, In.MenuDown = Dir == 1, Dir == -1
		end
	end
	self.MenuHeldDir = Dir
	return In
end

function HD2DPlayer:OnUpdate(Dt)
	local E = self.entity
	local Pos = E:GetWorldPosition()
	local Step = Flat(Pos - self.LastPos)
	if Step:Length() < 500 then self.Stats.Distance = self.Stats.Distance + Step:Length() end
	self.LastPos = Pos

	local In = self:GatherInput()
	if self.GM:IsMenuOpen() then
		self.GM:MenuInput(In)
		self:UpdateHud()
		return
	end
	self.GM:UpdateInteract(Pos)

	self.AttackCooldown = math.max(0.0, self.AttackCooldown - Dt)
	self.DashCooldown = math.max(0.0, self.DashCooldown - Dt)
	self.Invuln = math.max(0.0, self.Invuln - Dt)
	self.ComboWindow = math.max(0.0, self.ComboWindow - Dt)
	self.NoManaTimer = math.max(0.0, self.NoManaTimer - Dt)
	self.Mana = math.min(self.MaxMana, self.Mana + D.Balance().ManaRegen * Dt)

	if In.Inventory then
		self.GM:OpenInventory()
		self:UpdateHud()
		return
	end
	if In.Interact and self.GM.Target and self.AttackTimer <= 0 and self.DashTimer <= 0 then
		self.GM:Interact()
		self:UpdateHud()
		return
	end
	if In.Switch then self.GM:CycleWeapon() end
	if In.Boost then self:RaiseBoost() end
	self:UpdateBoostRegen(Dt)
	if In.Use1 then self.GM:QuickUse({ "Potion", "HiPotion", "Elixir" }) end
	if In.Use2 then self.GM:QuickUse({ "Ether", "Elixir" }) end

	local Move = In.Move
	if Move:Length() > 1 then Move = Move:Normalized() end

	if self.DashTimer > 0 then
		-- 대시 중: 이동기가 넉백 경직으로 미끄러뜨린다. 잔상만 남긴다
		self.DashTimer = self.DashTimer - Dt
		self.AfterimageTimer = self.AfterimageTimer - Dt
		if self.AfterimageTimer <= 0 then
			self.AfterimageTimer = 0.035
			local Dd = Dirs[self.Facing]
			self.GM:SpawnAfterimage(HeroSprite, "Dash" .. Dd.Anim .. "0", self.Body:GetWorldPosition() + Vector3(0, -2, 0), Dd.Flip)
		end
	elseif self.AttackTimer > 0 then
		-- 공격 중: 이동 입력 무시, HitDelay에 판정. 공격 끝 무렵 누르면 다음 공격 예약 (검 연타)
		self.AttackTimer = self.AttackTimer - Dt
		if In.Attack then self.bQueued = true end
		while self.HitQueue[1] and self.AttackElapsed + Dt >= self.HitQueue[1] do
			table.remove(self.HitQueue, 1)
			self.HitIndex = (self.HitIndex or 0) + 1
			self:DoAttackHit(self.HitIndex)
		end
		self.AttackElapsed = self.AttackElapsed + Dt
		if self.AttackTimer <= 0 then
			self.ComboWindow = 0.32
		end
	else
		if In.Dash and self.DashCooldown <= 0 then
			self:StartDash(Move)
		elseif (In.Attack or self.bQueued) and self.AttackCooldown <= 0 then
			self:StartAttack(Move)
		elseif Move:Length() > 0.05 then
			self.Facing = FacingFromMove(Move, self.Facing)
			self.AimDir = Dirs[self.Facing].V
			E:AddMovementInput(Move)
		end
	end

	-- 무적 깜빡임 + 피격 붉은빛
	if self.HurtFlash > 0 then self.HurtFlash = self.HurtFlash - Dt end
	self.Sprite.Color = self.HurtFlash > 0 and Vector4(1, 0.4, 0.4, 1) or Vector4(1, 1, 1, 1)
	self.Sprite.Visible = not (self.Invuln > 0 and self.DashTimer <= 0 and math.floor(self.Invuln * 16) % 2 == 1)

	self:UpdateAnimation(E:GetMovementVelocity())
	self:UpdateHud()
end

function HD2DPlayer:UpdateHud()
	local H = self.GM:Hud()
	if not H then return end
	H:SetStatus(self.Name, self.Level, self.Health, self.MaxHealth, self.Mana, self.MaxMana, self:ExpFraction())
	H:SetGold(self.GM.Gold)
	H:SetBoost(self.BP, self.BoostLevel, D.Balance().BoostMax)
end

-- ---- 부스트 (BP)
function HD2DPlayer:RaiseBoost()
	local B = D.Balance()
	if self.BoostLevel < math.min(B.BoostMaxLevel, self.BP) then
		self.BoostLevel = self.BoostLevel + 1
		self.GM:SpawnFx("Sparkle", self.entity:GetWorldPosition() + Vector3(0, 30, 40 + self.BoostLevel * 20),
			{ Blend = 2, Scale = 0.8 + self.BoostLevel * 0.25, Color = { 0.6, 0.9, 1, 1 } })
		Audio.PlayOneShot(self.GM.Sounds.Move)
		self.GM:Hud():BoostBurst("BOOST " .. string.rep("▶", self.BoostLevel), 0.6)
	else
		Audio.PlayOneShot(self.GM.Sounds.Error)
	end
end

function HD2DPlayer:UpdateBoostRegen(Dt)
	local B = D.Balance()
	if self.BP >= B.BoostMax then
		self.BoostRegen = 0
		return
	end
	self.BoostRegen = self.BoostRegen + Dt
	if self.BoostRegen >= B.BoostRegenTime then
		self.BoostRegen = 0
		self:GainBP(1)
	end
end

function HD2DPlayer:GainBP(N)
	local Before = self.BP
	self.BP = math.min(D.Balance().BoostMax, self.BP + N)
	if self.BP > Before then
		self.GM:Hud():PulseOrb(self.BP - 1)
	end
end

-- 관리자 HitEnemy가 플레이어 공격 명중마다 부른다
function HD2DPlayer:OnHitLanded()
	self.HitsForBP = self.HitsForBP + 1
	if self.HitsForBP >= D.Balance().BoostHitsPerPoint then
		self.HitsForBP = 0
		self:GainBP(1)
	end
end

function HD2DPlayer:StartDash(Move)
	local Dir = Move:Length() > 0.1 and Move:Normalized() or Dirs[self.Facing].V
	local B = D.Balance()
	self.Facing = FacingFromMove(Dir, self.Facing)
	self.DashTimer = B.DashTime
	self.DashCooldown = B.DashCooldown
	self.AfterimageTimer = 0.0
	self.bQueued = false
	self.entity:AddKnockback(Dir * B.DashSpeed, B.DashTime)
	self.GM:SpawnFx("Dust", self.entity:GetWorldPosition() + Vector3(-Dir.X * 40, -Dir.Y * 40 + 4, -82), { FlipX = Dir.X < 0 })
	Audio.PlayOneShot("Audio/RPG/Dash.wav")
	self.Stats.Dashes = self.Stats.Dashes + 1
end

-- 조준 방향: 원거리는 Arc 안 가장 가까운 적 (없으면 사거리 안 아무 적 중 가장 가까운 것이 앞쪽 반원에 있으면), 근접은 가까이 있는 적 쪽으로 돌아선다
function HD2DPlayer:ChooseAim(Move, W)
	local Base = Move:Length() > 0.1 and Move:Normalized() or Dirs[self.Facing].V
	local Pos = self.entity:GetWorldPosition()
	local bRanged = W.Kind == "Arrow" or W.Kind == "Bolt"
	local Reach = bRanged and W.Range * 0.9 or W.Range + 80
	local CosHalf = math.cos(math.rad((bRanged and W.Arc or 120) * 0.5))
	local Target = self.GM:NearestEnemy(Pos, Reach, function(S)
		local To = Flat(S.entity:GetWorldPosition() - Pos)
		local L = To:Length()
		return L < 1 or (To * (1.0 / L)):Dot(Base) >= CosHalf or Move:Length() <= 0.1
	end)
	if Target then
		local To = Flat(Target.entity:GetWorldPosition() - Pos)
		if To:Length() > 1 then return To:Normalized() end
	end
	return Base
end

function HD2DPlayer:StartAttack(Move)
	local W = self.Weapon
	self.bQueued = false
	if W.ManaCost > 0 and self.Mana < W.ManaCost then
		if self.NoManaTimer <= 0 then
			self.NoManaTimer = 1.0
			self.GM:Hud():Toast(D.Item("Ether").Icon, "마나가 부족하다")
			Audio.PlayOneShot(self.GM.Sounds.Error)
		end
		return
	end
	self.Mana = self.Mana - W.ManaCost
	-- 부스트: 올린 단계만큼 BP를 쓴다
	local L = self.BoostLevel
	self.AttackBoost = L
	self.BoostLevel = 0
	if L > 0 then
		self.BP = self.BP - L
		local R_ = self.GM.Report
		R_.Boosts = R_.Boosts + 1
		R_.BoostMax = math.max(R_.BoostMax, L)
		local Pos = self.entity:GetWorldPosition()
		self.GM:Hud():BoostBurst(string.format("BOOST ×%d!", L + 1), 1.0)
		self.GM:Hud():ScreenFlash(0.35 + 0.1 * L, 0.3)
		self.GM:SpawnSprite({ Sprite = "Sprites/HD2D/Fx.esprite", Slice = "Pillar", Position = Pos + Vector3(0, 20, -85), Blend = 2, Life = 0.5, Fade = true,
		                      Color = { 0.55, 0.85, 1.0, 0.9 }, Scale = { 1.0 + L * 0.2, 1.2 } })
		self.GM:SpawnFx("Ring", Pos + Vector3(0, 0, -82), { Flat = true, Blend = 2, Scale = 0.4, Grow = 3.5, Life = 0.35, Fade = true, Color = { 0.6, 0.9, 1, 1 } })
		self.GM:AddShake(4 + L * 2, 0.2)
		Audio.PlayOneShot("Audio/RPG/Spin.wav", 1.0, 1.0 + 0.18 * L) -- 단계가 높을수록 높은 소리
		Log.Info(string.format("[HD2D] 부스트 공격: %d단계 (남은 BP %d)", L, self.BP))
	end
	self.AimDir = self:ChooseAim(Move, W)
	self.Facing = FacingFromMove(self.AimDir, self.Facing)
	local bMelee = W.Kind == "Slash" or W.Kind == "Thrust"
	local Extra = bMelee and L * 0.1 or 0
	self.AttackTimer = W.AttackTime + Extra
	self.AttackElapsed = 0.0
	self.AttackCooldown = W.AttackTime + Extra + W.Cooldown * 0.25
	self.HitQueue, self.HitIndex = {}, 0
	for I = 0, (bMelee and L or 0) do self.HitQueue[#self.HitQueue + 1] = W.HitDelay + I * 0.1 end
	self.SwingSide = -self.SwingSide
	self.Combo = (W.Kind == "Slash" and self.ComboWindow > 0) and (self.Combo % 3 + 1) or 1
	self.Anim = "" -- 같은 방향 연타도 처음부터
	self.Stats.Attacks = self.Stats.Attacks + 1
	Audio.PlayOneShot((W.Kind == "Slash" and self.Combo == 3) and "Audio/RPG/Swing3.wav" or W.Sound)
	if W.Kind == "Slash" or W.Kind == "Thrust" then
		self.entity:AddKnockback(self.AimDir * (W.Kind == "Thrust" and 380 or 260), 0.08) -- 반 걸음 내딛기
	end
end

-- 피해 굴림 (레벨 배율 · ±10% · 치명타)
function HD2DPlayer:RollDamage(Base)
	local B = D.Balance()
	local Value = Base * self:DamageScale() * (0.9 + 0.2 * self.GM:Random())
	local bCrit = self.GM:Random() < B.CritChance + (self.CritBonus or 0)
	if bCrit then Value = Value * B.CritMultiplier end
	return math.max(1, math.floor(Value + 0.5)), bCrit
end

function HD2DPlayer:DoAttackHit(HitIndex)
	local W = self.Weapon
	local Aim = self.AimDir
	local Pos = self.entity:GetWorldPosition()
	local L = self.AttackBoost or 0
	local Boost = 1.0 + D.Balance().BoostDamagePerLevel * L
	local bFinisher = W.Kind == "Slash" and (self.Combo == 3 or (L > 0 and HitIndex == L + 1))
	if L > 0 and (HitIndex or 1) > 1 then
		self.SwingSide = -self.SwingSide
		Audio.PlayOneShot(HitIndex % 2 == 0 and "Audio/RPG/Swing2.wav" or "Audio/RPG/Swing1.wav")
	end
	local Angle = self.GM.ScreenAngle(Aim)
	local Targets = {}
	if W.Kind == "Slash" then
		local Center = Pos + Aim * (W.Range * 0.45)
		self.GM:SpawnFx("Slash", Center + Vector3(0, 12, -5), { Rotation = Angle, FlipY = self.SwingSide < 0, Blend = 2, Scale = bFinisher and 1.6 or 1.1,
		                                                        Color = bFinisher and { 1, 0.9, 0.6, 1 } or nil })
		Targets = self.GM:FindEnemiesInCone(Pos, Aim, W.Range * (bFinisher and 1.2 or 1.0), bFinisher and 220 or W.Arc)
	elseif W.Kind == "Thrust" then
		self.GM:SpawnFx("Thrust", Pos + Aim * 30 + Vector3(0, 12, -8), { Rotation = Angle, Blend = 2, Scale = { W.Range / 280.0, 1.3 } })
		Targets = self.GM:FindEnemiesInLine(Pos, Aim, W.Range, 40)
	else
		-- 투사체: 관리자가 움직이고 맞힌다
		for K = 0, L do
			-- 부스트: 부채꼴로 1+L발 (가운데부터 좌우 번갈아)
			local Off = (K == 0) and 0 or ((K % 2 == 1) and 1 or -1) * math.ceil(K / 2) * (W.Kind == "Bolt" and 11 or 7)
			local A = math.rad(Off)
			local Dir = Vector3(Aim.X * math.cos(A) - Aim.Y * math.sin(A), Aim.X * math.sin(A) + Aim.Y * math.cos(A), 0)
			local Damage, bCrit = self:RollDamage(W.Damage * Boost)
			self.GM:SpawnProjectile({ Kind = W.Kind, Pos = Pos + Dir * 50 + Vector3(0, 10, 8), Dir = Dir, Speed = W.ProjectileSpeed, Range = W.Range,
			                          Damage = Damage, Knockback = W.Knockback, Splash = W.Splash, Team = "Player", Crit = bCrit, Weapon = self.GM.Equipped,
			                          Scale = L > 0 and 1.3 or nil })
		end
		if W.Kind == "Bolt" then
			self.GM:SpawnFx("Sparkle", Pos + Aim * 50 + Vector3(0, 14, 20), { Blend = 2, Scale = 0.9, Color = { 0.6, 0.8, 1, 1 } })
		end
		return
	end
	local Hit = 0
	for _, S in ipairs(Targets) do
		local Away = Flat(S.entity:GetWorldPosition() - Pos)
		Away = Away:Length() > 1 and Away:Normalized() or Aim
		local Damage, bCrit = self:RollDamage(W.Damage * (bFinisher and 1.6 or 1.0) * Boost)
		if self.GM:HitEnemy(S, Damage, Away, W.Knockback * (bFinisher and 1.4 or 1.0), bCrit, self.GM.Equipped) then
			Hit = Hit + 1
		end
	end
	if Hit > 0 then
		self.Stats.Hits = self.Stats.Hits + Hit
		Game.HitStop(W.HitStop * (bFinisher and 1.8 or 1.0))
		self.GM:AddShake(bFinisher and 11 or 6, 0.15)
		Audio.PlayOneShot((bFinisher or W.Kind == "Thrust") and "Audio/RPG/HitHeavy.wav" or "Audio/RPG/Hit.wav")
	end
end

-- 적·투사체·독이 부른다. 피해를 받았으면 true. Opt: bNoKnockback, bNoInvuln(독 — 무적 시간을 주지 않음), Color
function HD2DPlayer:TakeDamage(Amount, From, Opt)
	Opt = Opt or {}
	if self.bDead or self.DashTimer > 0 or (self.Invuln > 0 and not Opt.bNoInvuln) then
		return false
	end
	local Damage = math.max(1, math.floor(Amount * self.DamageTakenScale * 60.0 / (60.0 + (self.Defense or 0) * 3.0) + 0.5))
	self.Health = self.Health - Damage
	self.Stats.Damaged = self.Stats.Damaged + 1
	local Pos = self.entity:GetWorldPosition()
	self.GM:DamageNumber(Pos + Vector3(0, 0, 70), tostring(Damage), Opt.Color or { 1, 0.4, 0.4, 1 }, 1.1)
	self.HurtFlash = 0.15
	if not Opt.bNoInvuln then
		self.Invuln = D.Balance().InvulnTime
		self.AttackTimer = 0.0
		self.GM:AddShake(9, 0.2)
		self.GM:SpawnFx("Spark", Pos + Vector3(0, 30, 0), { Blend = 2, Color = { 1, 0.45, 0.45, 1 } })
		Audio.PlayOneShot("Audio/RPG/Hurt.wav")
	end
	if not Opt.bNoKnockback then
		local Away = Flat(Pos - From)
		Away = Away:Length() > 1 and Away:Normalized() or Vector3(0, 1, 0)
		self.entity:AddKnockback(Away * 650, 0.18)
	end
	if self.Health <= 0 then
		self:Respawn()
	end
	return true
end

function HD2DPlayer:Respawn()
	self.GM:SpawnFx("Poof", self.entity:GetWorldPosition() + Vector3(0, 10, -60), { Scale = 1.3 })
	self.entity:SetPosition(self.Start)
	self.Health = self.MaxHealth
	self.Mana = self.MaxMana
	self.Invuln = 2.0
	self.Stats.Deaths = self.Stats.Deaths + 1
	self.GM:Hud():Announce("쓰러졌다…", self.GM.Properties.Map == "Village" and "마을에서 다시 일어섰다" or "입구에서 다시 일어섰다", 2.5)
	Log.Info("[HD2D] 플레이어 쓰러짐 → 시작 자리에서 부활")
end

function HD2DPlayer:UpdateAnimation(Velocity)
	local Dd = Dirs[self.Facing]
	local Want
	if self.DashTimer > 0 then
		Want = "Hero_Dash" .. Dd.Anim
	elseif self.AttackTimer > 0 then
		Want = self.Weapon.Flipbook .. Dd.Anim
	elseif Velocity.X * Velocity.X + Velocity.Y * Velocity.Y > 40 * 40 then
		Want = "Hero_Walk" .. Dd.Anim
	else
		Want = "Hero_Idle" .. Dd.Anim
	end
	if Want ~= self.Anim then
		self.Anim = Want
		self.Body:PlayFlipbook("Sprites/HD2D/" .. Want .. ".eflipbook")
	end
	self.Body:SetSpriteFlip(Dd.Flip, false)
end

function HD2DPlayer:CameraTarget()
	local P = self.entity:GetWorldPosition()
	local Focus = Vector3(math.max(self.Properties.MinX, math.min(self.Properties.MaxX, P.X)),
	                      math.max(self.Properties.MinY, math.min(self.Properties.MaxY, P.Y)), P.Z - 85 + self.Properties.FocusHeight)
	return Focus + self.CamOffset
end

-- 물리 뒤: 스프라이트 정면 유지 + 카메라 (OnUpdate에서 놓으면 한 프레임 늦게 따라가 떨린다)
function HD2DPlayer:OnLateUpdate(Dt)
	self.Visual:SetRotation(self.entity:GetRotation():Inverse())
	if self.Camera then
		local Target = self:CameraTarget()
		self.CamPos = Vector3.Lerp(self.CamPos, Target, 1.0 - math.exp(-self.Properties.CameraLag * Dt))
		self.Camera:SetPosition(self.CamPos + self.GM:GetShakeOffset())
	end
end

-- 자동 조종용: 즉시 이동 (막혀 오래 못 가면)
function HD2DPlayer:Teleport(Pos)
	self.entity:SetPosition(Pos)
	self.LastPos = Pos
end

return HD2DPlayer
