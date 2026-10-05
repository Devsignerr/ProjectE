-- FarmBie 숲 자원 (FarmGame에 섞이는 메서드 모음). 씬의 "Node_<종류>_<번호>" 엔티티(생성기 AddNode) = 자원 하나.
--   종류는 Data/FarmBie/Resources.etable: 도구(Axe 나무 / Pick 바위 / Hand 풀·약초·버섯), 칠 횟수, 나오는 물건·수량, 다시 자라는 날
--   도끼·곡괭이: 플레이어 도구 동작의 효과 시점에 HitNodeAt(도구, 발 위치, 방향) — 발 앞 ReachDistance+노드 반지름 안의 가장 가까운 노드
--   손으로 줍기: 상호작용(E) — 노드마다 AddInteractable
--   캔 노드는 NodeState[이름] = 캔 날(누적 일수) → 다시 자라는 날이 지나면 아침(또는 씬을 열 때) 되살린다. NodeState는 저장된다 (맵을 오가도 유지)
--   모습: 나무 = Model 숨김(크기 0.001) + Stump 보임 + 충돌 제거, 바위 = Model 숨김 + 충돌 제거, 풀 = 스프라이트 Slice <종류>Picked
local D = Script.Require("Scripts/FarmBie/FarmData.lua")

local Forage = {}

local Tiny = Vector3(0.001, 0.001, 0.001)
local NodeRadius = { Tree = 70, Rock = 60, BigRock = 100, Fiber = 50, Herb = 40, Mushroom = 40 }

local function Flat(V) return Vector3(V.X, V.Y, 0) end

function Forage:InitForage()
	self.NodeState = self.NodeState or {}
	self.Nodes = {}
	for _, Row in ipairs(D.Rows("Resources.etable")) do
		for I = 0, 199 do
			local E = Scene.Find("Node_" .. Row.Name .. "_" .. I)
			if not E then break end
			local Model = E:FindChild("Model")
			local Node = { Name = "Node_" .. Row.Name .. "_" .. I, Type = Row.Name, Row = Row, Entity = E, Model = Model, Stump = E:FindChild("Stump"),
			               Pos = E:GetWorldPosition(), Hits = 0, ModelScale = Model and Model:GetScale() or Vector3(1, 1, 1),
			               Sprite = Model and Model:GetComponent("SpriteComponent") }
			local Col = E:FindChild("Collision")
			if Col then
				Node.Collision = Col
				Node.Half = Col:GetComponent("BoxColliderComponent").HalfExtents
			end
			if Row.Tool == "Hand" then
				self:AddInteractable({ Pos = Node.Pos, Radius = 130, Prompt = function()
					if self:IsNodeReady(Node) then return "E  줍기: " .. Row.DisplayName end
					return nil
				end, Act = function() self:HarvestNode(Node) end })
			end
			self.Nodes[#self.Nodes + 1] = Node
		end
	end
	self:RefreshNodes()
end

function Forage:IsNodeReady(Node)
	return self.NodeState[Node.Name] == nil
end

-- 다시 자람 + 모습 맞추기
function Forage:RefreshNodes()
	local Today = self:TotalDays()
	for _, Node in ipairs(self.Nodes or {}) do
		local Day = self.NodeState[Node.Name]
		if Day and Today >= Day + Node.Row.Regrow then
			self.NodeState[Node.Name] = nil
			Node.Hits = 0
		end
		self:ApplyNodeLook(Node)
	end
end

function Forage:ApplyNodeLook(Node)
	local bReady = self:IsNodeReady(Node)
	if Node.bShownReady == bReady then return end
	Node.bShownReady = bReady
	if Node.Sprite then
		Node.Sprite.Slice = bReady and Node.Type or (Node.Type .. "Picked")
	elseif Node.Model then
		Node.Model:SetScale(bReady and Node.ModelScale or Tiny)
		if Node.Stump then Node.Stump:SetScale(bReady and Tiny or Node.ModelScale) end
	end
	if Node.Collision then
		local bHas = Node.Collision:HasComponent("BoxColliderComponent")
		if bReady and not bHas then
			Node.Collision:AddComponent("BoxColliderComponent")
			Node.Collision:GetComponent("BoxColliderComponent").HalfExtents = Node.Half
		elseif not bReady and bHas then
			Node.Collision:RemoveComponent("BoxColliderComponent")
		end
	end
end

-- 도구로 치기 (도끼·곡괭이) → 맞은 노드 | nil
function Forage:HitNodeAt(Key, Pos, Facing)
	if not self.Nodes then return nil end
	local Tool = Key == "Axe" and "Axe" or (Key == "Pick" and "Pick" or nil)
	if not Tool then return nil end
	local Probe = Pos + Facing * self.Farming.ReachDistance
	local Best, BestD = nil, math.huge
	for _, Node in ipairs(self.Nodes) do
		if Node.Row.Tool == Tool and self:IsNodeReady(Node) then
			local Dist = Flat(Node.Pos - Probe):Length()
			if Dist < (NodeRadius[Node.Type] or 60) + 40 and Dist < BestD then Best, BestD = Node, Dist end
		end
	end
	if not Best then return nil end
	Best.Hits = Best.Hits + 1
	self.Report.NodeHits = (self.Report.NodeHits or 0) + 1
	if Best.Hits >= Best.Row.Hits then self:HarvestNode(Best) end
	return Best
end

function Forage:NodeRandom()
	self.RandState = (self.RandState * 1103515245 + 12345) % 2147483648
	return self.RandState / 2147483648
end

function Forage:HarvestNode(Node)
	if not self:IsNodeReady(Node) then return end
	local Row = Node.Row
	local Count = Row.Min + math.floor(self:NodeRandom() * (Row.Max - Row.Min + 1))
	self:Give(Row.Drop, Count)
	self.NodeState[Node.Name] = self:TotalDays()
	self.Report.Gathered = (self.Report.Gathered or 0) + 1
	self.Report["Gathered" .. Row.Name] = (self.Report["Gathered" .. Row.Name] or 0) + 1
	self:ApplyNodeLook(Node)
end

-- ---- 저장 조각
function Forage:SaveForage(T)
	local S = {}
	for Name, Day in pairs(self.NodeState) do S[#S + 1] = { Name = Name, Day = Day } end
	T.Nodes = S
end

function Forage:LoadForage(T)
	self.NodeState = {}
	for _, E in ipairs(T.Nodes or {}) do self.NodeState[E.Name] = math.floor(E.Day) end
	if self.Nodes then
		for _, Node in ipairs(self.Nodes) do Node.bShownReady = nil end
		self:RefreshNodes()
	end
end

return Forage
