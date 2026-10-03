-- 능력 데모 규칙 (Tests/Abilities, 서버에서만): 플레이어 폰에 능력 시스템 컴포넌트를 붙인다.
--   멀티플레이: 서버가 입장한 플레이어의 폰(프로젝트 설정 PlayerPrefab)에 붙인다 (컴포넌트 값은 복제되어 클라이언트도 같은 표를 읽는다)
--   Standalone(런타임/에디터 플레이): 직접 플레이어 프리팹을 PlayerStart에 만들고 로컬 플레이어(0) 소유로 정한다
-- 정의는 데이터 테이블 Data/Abilities/Demo*.etable, 능력 로직은 Abilities/*.lua
local Rules = {
	Properties = {
		PlayerPrefab = Prefab("Prefabs/Player.eprefab"),
	},
}

local function SetupPawn(Pawn)
	local Component = Pawn:GetComponent("AbilitySystemComponent") or Pawn:AddComponent("AbilitySystemComponent")
	Component.AttributeTable = "Data/Abilities/DemoAttributes.etable"
	Component.EffectTable    = "Data/Abilities/DemoEffects.etable"
	Component.AbilityTable   = "Data/Abilities/DemoAbilities.etable"
	Component.StartupEffects = "Regen"
	Component.AcceptInput    = true
	Log.Info("능력 시스템 연결:", Pawn:GetName())
end

function Rules:OnStart()
	if Net.GetMode() ~= "Standalone" then
		return
	end
	local Start = Scene.Find("PlayerStart")
	Scene.SpawnPrefab(self.Properties.PlayerPrefab, Start and Start:GetPosition() or nil, function(Root)
		if Start then
			Root:SetRotation(Start:GetRotation())
		end
		local Replicated = Root:GetComponent("ReplicatedComponent")
		if Replicated then
			Replicated.OwnerPlayerId = 0 -- Standalone 로컬 플레이어
		end
		SetupPawn(Root)
	end)
end

function Rules:OnPlayerJoined(playerId, pawn)
	if pawn then
		SetupPawn(pawn)
	end
end

return Rules