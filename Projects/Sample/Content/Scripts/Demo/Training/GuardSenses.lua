-- 경비병 감각 (비헤이비어 트리 Lua 서비스, AI/Demo/TrainingGuard.ebt 루트): 경비병 스크립트(TrainingGuard.lua)의 판단을 블랙보드 Target으로 옮긴다.
--   값이 바뀔 때만 쓴다 → Blackboard 데코레이터(AbortMode Both)가 순찰 ↔ 따라가기를 바꾼다
local GuardSenses = {
	Properties = {},
}

function GuardSenses:OnTick(dt)
	local Guard = self.entity:GetScript()
	if Guard == nil or Guard.UpdateSenses == nil then return end
	local Target = Guard:UpdateSenses()
	if Guard.TargetDirty then
		Guard.TargetDirty = false
		self.entity:GetBlackboard():Set("Target", Target)
	end
end

return GuardSenses
