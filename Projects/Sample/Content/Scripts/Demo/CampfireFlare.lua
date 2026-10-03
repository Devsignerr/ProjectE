-- 신호 조명탄 (데모 Campfire, ExecutionLocation = Both — 화면 연출이라 각자 돈다).
--   Interval초마다(Timer.Every) 코루틴 하나가 한 발을 쏜다:
--     1) 이 엔티티의 파티클 Asset을 비웠다가 다시 넣어 발사 시스템(발사 연기 → 로켓 → 꼭대기 폭발, 반복 없음)을 처음부터 재생
--     2) RiseTime초 뒤(로켓이 꼭대기) LightName 엔티티를 꼭대기로 옮기고 낙하산 조명탄 파티클(불꽃·빛무리·연기)을 재시작,
--        BurnTime초 동안 바람에 밀리며 천천히 내려오고 붉은 점광원이 일렁이다 꺼진다
--   파티클 재시작 = 경로가 바뀌면 리소스 해석이 런타임을 다시 만든다 (같은 프레임에 비우고 넣으면 바뀐 것으로 보지 않으므로 한 프레임 기다린다)
--   난수 대신 시간 해시 노이즈를 쓴다 (자동 검증 화면이 결정적)
local CampfireFlare = {
	Properties = {
		LaunchAsset = Asset("Particles/Demo/CampfireFlareLaunch.eparticle", ".eparticle"),
		HeadAsset   = Asset("Particles/Demo/CampfireFlareHead.eparticle", ".eparticle"),
		LightName   = "Flare_Light",
		Interval    = 9.0,   -- 초
		FirstDelay  = 2.0,   -- 초: 시작 후 첫 발
		Apex        = 3920.0, -- cm: 발사대 위 꼭대기 높이 (발사 파티클의 폭발 높이와 같아야 한다)
		RiseTime    = 2.8,   -- 초
		BurnTime    = 5.5,   -- 초
		FallSpeed   = 75.0,  -- cm/초
		DriftX      = -60.0, -- cm/초 바람
		DriftY      = 45.0,
		Intensity   = 70.0,
	},
}

local function Hash(N)
	N = (N * 1103515245 + 12345) % 2147483648
	N = (N * 1103515245 + 12345) % 2147483648
	return (N % 100000) / 100000.0
end

local function Noise(T)
	local I = math.floor(T)
	local F = T - I
	F = F * F * (3.0 - 2.0 * F)
	local A, B = Hash(I), Hash(I + 1)
	return A + (B - A) * F
end

function CampfireFlare:OnStart()
	self.Particles = self.entity:GetComponent("ParticleSystemComponent")
	self.Head = Scene.Find(self.Properties.LightName)
	if self.Head then
		self.HeadLight = self.Head:GetComponent("PointLightComponent")
		self.HeadParticles = self.Head:GetComponent("ParticleSystemComponent")
		self.HeadLight.Intensity = 0.0
	end
	self.Home = self.entity:GetPosition()
	Timer.After(self.Properties.FirstDelay, function()
		self:Fire()
		Timer.Every(self.Properties.Interval, function() self:Fire() end)
	end)
end

function CampfireFlare:Fire()
	if self.Running and Coroutine.IsRunning(self.Running) then Coroutine.Stop(self.Running) end
	self.Running = Coroutine.Start(function()
		local P = self.Properties
		self.Particles.Asset = ""
		Wait()
		self.Particles.Asset = P.LaunchAsset.Path
		Wait(P.RiseTime)
		if self.Head == nil then return end
		local Pos = self.Home + Vector3(0.0, 0.0, P.Apex)
		self.Head:SetPosition(Pos)
		self.HeadParticles.Asset = ""
		Wait()
		self.HeadParticles.Asset = P.HeadAsset.Path
		local T = 0.0
		while T < P.BurnTime do
			local Dt = Time.DeltaTime
			T = T + Dt
			Pos = Pos + Vector3(P.DriftX, P.DriftY, -P.FallSpeed) * Dt
			self.Head:SetPosition(Pos)
			local Envelope = math.min(T / 0.15, 1.0) * math.min((P.BurnTime - T) / 0.5, 1.0)
			local Flicker = 0.75 + 0.25 * Noise(T * 13.0) + 0.1 * Noise(T * 31.0 + 7.0)
			self.HeadLight.Intensity = P.Intensity * math.max(Envelope, 0.0) * Flicker
			Wait()
		end
		self.HeadLight.Intensity = 0.0
	end)
end

return CampfireFlare
