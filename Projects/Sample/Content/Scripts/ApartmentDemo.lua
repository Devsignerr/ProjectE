-- 절차적 아파트 데모 (Tests/Apartment): 런타임 생성 경로 — 에디터 "생성"과 같은 코어(FBuildingSceneBuilder)를 Lua에서 부른다.
--   G = 무작위 시드로 다시 생성, H = 시드 + 1로 다시 생성 (씬에 굳혀 둔 생성물은 교체되고 BuildingPartComponent(유지) 엔티티만 남는다)
-- 스크립트는 건물과 다른 엔티티에 둔다 (내비메시 굽기는 스크립트 엔티티 하위를 움직이는 물체로 보고 뺀다)
local ApartmentDemo = {
	Properties = {
		Building  = "Apartment", -- ProceduralBuildingComponent 엔티티 이름
		StartSeed = 0,           -- 0이 아니면 시작할 때 이 시드로 다시 생성 (0 = 씬에 굳힌 결과 그대로)
	},
}

function ApartmentDemo:OnStart()
	self.Target = Scene.Find(self.Properties.Building)
	if not self.Target then
		Log.Warn("절차적 아파트 데모: 건물 엔티티가 없습니다", self.Properties.Building)
		return
	end
	if self.Properties.StartSeed ~= 0 then
		self:Generate(self.Properties.StartSeed)
	end
end

function ApartmentDemo:Generate(Seed)
	local Count = self.Target:GenerateBuilding(Seed)
	Log.Info("절차적 아파트 다시 생성: 시드", Seed, "프리팹 인스턴스", Count)
end

function ApartmentDemo:OnUpdate(dt)
	if not self.Target then
		return
	end
	if Input.IsKeyPressed("G") then
		self:Generate(math.random(1, 99999))
	elseif Input.IsKeyPressed("H") then
		self:Generate(self.Target:GetBuildingSeed() + 1)
	end
end

return ApartmentDemo
