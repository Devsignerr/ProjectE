-- FarmBie 소리 (FarmGame에 섞이는 메서드 모음). 효과음 = Content/Audio/FarmBie/<이름>.wav (Tools/FarmBieAudio.py가 만든다).
--   Sfx(이름[, 음량, 피치]) — 설정 효과음 음량을 곱한다. 공간화는 쓰지 않는다 (탑다운 카메라가 멀어 거리 감쇠가 어색함) — 멀리서 나는 소리는 플레이어와 거리로 줄인다(SfxAt).
--   환경음: 씬의 AmbientDay/AmbientNight 반복 소스(시작 시 재생) 음량을 매 프레임 낮·밤(탑은 밤 쪽)에 맞춰 3초로 섞는다.
--   좀비 신음은 밤에 살아 있는 좀비 하나에서 가끔, 처치·폭발·포탑·덫 소리는 C++ 디펜스 카운터 증가분으로 낸다.
local O = Script.Require("Scripts/FarmBie/FarmOptions.lua")

local Sound = {}

local Dir = "Audio/FarmBie/"

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function Sound:InitSound()
	self.Settings = O.LoadSettings()
	self.AmbientDay = Scene.Find("AmbientDay")
	self.AmbientNight = Scene.Find("AmbientNight")
	self.AmbientMix = self:AmbientTarget()
	self.GroanTimer = 3.0
	self.SoundCounters = {}
end

function Sound:Sfx(Name, Volume, Pitch)
	if not self.Settings then return end
	local V = (Volume or 1.0) * self.Settings.Sfx
	self.Report.Sounds = (self.Report.Sounds or 0) + 1
	self.LastSound = Name
	if V <= 0.001 then return end
	Audio.PlayOneShot(Dir .. Name .. ".wav", V, Pitch or 1.0)
end

-- 세계의 한 점에서 나는 소리: 플레이어와 멀수록 작게 (25m에서 0)
function Sound:SfxAt(Name, Pos, Volume, Pitch)
	local P = self:Player()
	local Mul = 1.0
	if P and Pos then
		local Dist = Flat(Pos - P.entity:GetWorldPosition()):Length()
		Mul = math.max(0.0, 1.0 - Dist / 2500.0)
	end
	if Mul > 0.05 then self:Sfx(Name, (Volume or 1.0) * Mul, Pitch) end
end

function Sound:AmbientTarget()
	if self.MapId == "Tower" then return 1.0 end
	return self.Phase == "Night" and 1.0 or 0.0
end

-- 설정을 바꿈 (일시정지 설정 쪽)
function Sound:ApplySettings(S)
	self.Settings = S
end

-- C++ 카운터가 늘었는지 (줄면 기준을 다시 잡는다)
function Sound:CounterRose(Name, Value)
	local Old = self.SoundCounters[Name]
	self.SoundCounters[Name] = Value
	return Old ~= nil and Value > Old
end

function Sound:UpdateSound()
	if not self.Settings then return end
	local UDt = Time.GetUnscaledDelta()
	local Target = self:AmbientTarget()
	self.AmbientMix = self.AmbientMix + math.max(-UDt / 3.0, math.min(UDt / 3.0, Target - self.AmbientMix))
	local A = self.Settings.Ambient
	if self.AmbientDay then self.AmbientDay:GetComponent("AudioSourceComponent").Volume = (1.0 - self.AmbientMix) * A * 0.8 end
	if self.AmbientNight then self.AmbientNight:GetComponent("AudioSourceComponent").Volume = self.AmbientMix * A end
	local Dc = self.Defense
	if not Dc then return end
	if self:CounterRose("Kills", Dc.Kills) then self:Sfx("HitHeavy", 0.55, 0.9 + (self.Report.Sounds % 5) * 0.04) end
	if self:CounterRose("Explosions", Dc.Explosions) then self:Sfx("Boom", 0.9) end
	if self:CounterRose("TurretShots", Dc.TurretShots) then self:Sfx("Shot", 0.25, 1.4) end
	if self:CounterRose("TrapHits", Dc.TrapHits) then self:Sfx("Hit", 0.35, 0.8) end
	-- 좀비 신음
	local Live = self.Zombies and self:LiveZombies() or {}
	if #Live > 0 and Game.GetTimeScale() > 0 then
		self.GroanTimer = self.GroanTimer - UDt
		if self.GroanTimer <= 0 then
			self.Report.Groans = (self.Report.Groans or 0) + 1
			local Z = Live[1 + (self.Report.Groans * 7) % #Live]
			self.GroanTimer = 2.2 + (self.Report.Groans * 13 % 10) * 0.3
			if Z.Entity and Z.Entity:IsValid() then
				local bBig = Z.bBoss or Z.bGuardian
				self:SfxAt(bBig and "Roar" or ("Groan" .. self.Report.Groans % 3), Z.Entity:GetWorldPosition(), bBig and 0.5 or 0.55,
					bBig and 1.0 or (0.85 + (self.Report.Groans % 4) * 0.08))
			end
		end
	end
end

return Sound
