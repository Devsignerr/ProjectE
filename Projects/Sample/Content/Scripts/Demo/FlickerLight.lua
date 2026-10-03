-- 깜빡이는 조명 (데모 맵, ExecutionLocation = Both — 화면 연출이라 각자 돈다): 같은 엔티티의 점광원/스포트 강도를 바꾼다.
--   Style: "Fire" = 불꽃 일렁임(부드러운 노이즈 두 겹), "Neon" = 대부분 켜짐 + 가끔 짧게 떨림, "Broken" = 고장 난 등(가끔 꺼졌다 켜짐)
--   난수 대신 시간 해시 노이즈를 쓴다 (Seed가 같으면 같은 깜빡임 — 자동 검증 화면이 결정적)
local FlickerLight = {
	Properties = {
		Style         = "Fire",
		BaseIntensity = 10.0,
		Amount        = 0.35, -- Fire 흔들림 폭 (기본 강도 비율)
		Speed         = 1.0,
		Seed          = 1,
	},
}

local function Hash(N)
	N = (N * 1103515245 + 12345) % 2147483648
	N = (N * 1103515245 + 12345) % 2147483648
	return (N % 100000) / 100000.0
end

-- 1D 값 노이즈 (0~1)
local function Noise(T, Seed)
	local I = math.floor(T)
	local F = T - I
	F = F * F * (3.0 - 2.0 * F)
	local A = Hash(I + Seed * 7919)
	local B = Hash(I + 1 + Seed * 7919)
	return A + (B - A) * F
end

function FlickerLight:OnStart()
	self.Time = 0.0
	self.Light = self.entity:GetComponent("PointLightComponent") or self.entity:GetComponent("SpotLightComponent")
end

function FlickerLight:OnUpdate(dt)
	if self.Light == nil then return end
	local P = self.Properties
	self.Time = self.Time + dt * P.Speed
	local T, Seed = self.Time, P.Seed
	local Scale = 1.0
	if P.Style == "Fire" then
		local N = 0.6 * Noise(T * 7.0, Seed) + 0.4 * Noise(T * 19.0, Seed + 1)
		Scale = 1.0 - P.Amount + 2.0 * P.Amount * N
	elseif P.Style == "Neon" then
		-- 1.5초 칸마다 10% 확률로 0.3초 동안 떨림
		local Slot = math.floor(T / 1.5)
		if Hash(Slot + Seed * 131) < 0.1 and (T - Slot * 1.5) < 0.3 then
			Scale = (Noise(T * 60.0, Seed) > 0.45) and 1.0 or 0.15
		end
	else -- Broken
		local Slot = math.floor(T / 0.9)
		local R = Hash(Slot + Seed * 977)
		if R < 0.18 then
			Scale = 0.04
		elseif R < 0.32 then
			Scale = (Noise(T * 45.0, Seed) > 0.5) and 1.0 or 0.1
		else
			Scale = 0.92 + 0.08 * Noise(T * 11.0, Seed)
		end
	end
	self.Light.Intensity = P.BaseIntensity * Scale
end

return FlickerLight
