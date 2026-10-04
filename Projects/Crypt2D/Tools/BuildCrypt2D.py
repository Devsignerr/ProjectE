# Crypt2D 예제 게임 생성 스크립트 (Phase 56-6 — 던그리드식 2D 횡스크롤 로그라이트).
#   먼저 원본 에셋을 받는다:  .\Projects\Crypt2D\Scripts\FetchAssets.ps1   (GothicVania Church/Cemetery, CC0 — gitignore)
#   그다음:                   python Projects/Crypt2D/Tools/BuildCrypt2D.py
# 쓰는 것 (모두 커밋 대상 — 씬/프리팹/데이터를 손으로 고치지 말고 이 스크립트를 고쳐 다시 만든다):
#   Config/*.json (프로젝트 설정: 맵·화면·정렬 레이어·충돌 레이어·입력·물리·렌더링·패키징)
#   Content/Sprites/Crypt/*.esprite·*.eflipbook·Crypt.etileset (받은 PNG의 크기·칸을 실제로 읽어 만든다 — 팩 시트는 Aseprite/TexturePacker 격자)
#   Content/Sprites/Crypt/Generated/Generated.png (+ .esprite) — 자체 제작 픽셀 아트 (Crypt2DArt.py)
#   Content/UI/Crypt/*.eui + Icons/*.png, Content/Audio/Crypt/*.wav (Kenney CC0 복사 + 자체 합성), Content/Data/Crypt/* (무기·적·밸런스 표)
#   Content/Scripts/Crypt/Rooms.lua (방 템플릿 — Crypt2DRooms.py), Content/Prefabs/Crypt/*.eprefab
#   Content/Scenes/Title.escene·Crypt.escene + Scenes/Tests/CryptAutoPlay·CryptBoss·CryptRooms.escene
# 좌표: 2D 평면 = 월드 X(오른쪽)·Z(위), 카메라는 +Y에서 -Y를 보는 직교. 1 도트 = 4cm (UnitsPerPixel 4), 타일 16px = 64cm,
#   카메라 OrthoHeight 960cm = 240 도트 → 1280x720에서 도트 하나 = 화면 3px (PixelArtComponent PixelSize 3).
import json
import math
import os
import shutil
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "Tools", "DemoMap"))
import Crypt2DArt as Art  # noqa: E402
import Crypt2DRooms as Rooms  # noqa: E402
from SceneBuilder import FScene, QuatFromEuler  # noqa: E402

Root    = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
Project = os.path.join(Root, "Projects", "Crypt2D")
Content = os.path.join(Project, "Content")
GvDir   = os.path.join(Content, "Asset", "GothicVania")
SpriteDir = os.path.join(Content, "Sprites", "Crypt")

UPP       = 4.0     # cm / 도트
TilePx    = 16
CellCm    = TilePx * UPP  # 64
OrthoHeight = 960.0
PixelSize = 3


def WriteJson(Path, Value):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		json.dump(Value, File, ensure_ascii=False, indent=2)
		File.write("\n")


def WriteText(Path, Text):
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	with open(Path, "w", encoding="utf-8", newline="\n") as File:
		File.write(Text)


def Rel(FromDir, Target):
	return os.path.relpath(Target, FromDir).replace("\\", "/")


def RequireAsset(*Parts):
	Path = os.path.join(GvDir, *Parts)
	if not os.path.exists(Path):
		sys.exit(f"원본 에셋이 없습니다: {Path}\n  먼저 .\\Projects\\Crypt2D\\Scripts\\FetchAssets.ps1 를 실행하세요")
	return Path


# ================================================================ 스프라이트 / 플립북 / 타일셋
def AlphaBox(Img, Rect):
	X, Y, W, H = Rect
	return Img.crop((X, Y, X + W, Y + H)).getchannel("A").getbbox()


def CharacterPivot(Img, Rects, HalfHeightPx):
	# 캐릭터 시트: 기준 프레임들의 불투명 영역 합 → 가로 가운데 = 몸 중심(반전 축), 발 = 아래 끝. 피벗은 캡슐 중심(발 + 반높이)
	MinX, MaxX, MaxY = 10 ** 6, -1, -1
	W = H = 0
	for Rect in Rects:
		Box = AlphaBox(Img, Rect)
		W, H = Rect[2], Rect[3]
		if Box:
			MinX, MaxX, MaxY = min(MinX, Box[0]), max(MaxX, Box[2]), max(MaxY, Box[3])
	# 피벗은 정수 도트에 둔다 (도트 격자에 놓인 위치 + 정수 오프셋 = 텍셀 경계가 픽셀 경계 — 점 필터가 이웃 텍셀을 읽지 않게)
	CenterX = math.floor((MinX + MaxX) / 2.0 + 0.5)
	Below   = H - MaxY
	return [CenterX / W, (Below + math.floor(HalfHeightPx + 0.5)) / H]


def CenterPivot(Img, Rects):
	MinX = MinY = 10 ** 6
	MaxX = MaxY = -1
	W = H = 0
	for Rect in Rects:
		Box = AlphaBox(Img, Rect)
		W, H = Rect[2], Rect[3]
		if Box:
			MinX, MinY, MaxX, MaxY = min(MinX, Box[0]), min(MinY, Box[1]), max(MaxX, Box[2]), max(MaxY, Box[3])
	return [math.floor((MinX + MaxX) / 2.0 + 0.5) / W, (H - math.floor((MinY + MaxY) / 2.0 + 0.5)) / H]


def WriteSprite(Name, TexturePath, Slices, Filter="Point"):
	# Slices: [(이름, x, y, w, h, 피벗 또는 None)]
	Img = Image.open(TexturePath)
	Path = os.path.join(SpriteDir, f"{Name}.esprite")
	Out = []
	for (SliceName, X, Y, W, H, Pivot) in Slices:
		Entry = {"Name": SliceName, "X": X, "Y": Y, "W": W, "H": H}
		if Pivot is not None:
			Entry["Pivot"] = Pivot
		Out.append(Entry)
	WriteJson(Path, {"Version": 1, "Texture": Rel(SpriteDir, TexturePath), "TextureWidth": Img.width, "TextureHeight": Img.height,
	                 "UnitsPerPixel": UPP, "Filter": Filter, "Slices": Out})
	return f"Sprites/Crypt/{Name}.esprite"


def WriteFlipbook(Name, Sprite, Frames, Fps, Loop="Loop", Events=None):
	Value = {"Version": 1, "Sprite": f"{Sprite}.esprite", "Fps": Fps, "Loop": Loop, "Frames": [{"Slice": F} for F in Frames]}
	if Events:
		Value["Events"] = [{"Frame": F, "Name": N} for (F, N) in Events]
	WriteJson(os.path.join(SpriteDir, f"{Name}.eflipbook"), Value)
	return f"Sprites/Crypt/{Name}.eflipbook"


def GridRects(Columns, Count, W, H, Start=0):
	return [((I % Columns) * W, (I // Columns) * H, W, H) for I in range(Start, Start + Count)]


# 캐릭터 캡슐 (도트) — 프리팹 이동기와 같아야 피벗(캡슐 중심)이 발에 맞는다
PlayerCapsule   = (8, 40)   # (반지름, 전체 높이) 도트
SkeletonCapsule = (7, 38)
GhoulCapsule    = (8, 40)
GatoCapsule     = (11, 30)
WizardCapsule   = (9, 40)


def BuildSprites():
	Church   = lambda N: RequireAsset("Church", N)
	Cemetery = lambda N: RequireAsset("Cemetery", N)

	# ---- 플레이어 (교회 팩 수도승, 82x60 13열 — player.json 순서: 점프2 낙하2 날아차기2 웅크림2 웅크려차기5 차기5 피격2 대기4 주먹6 걷기6)
	Img = Image.open(Church("player.png"))
	Names = (["Jump0", "Jump1", "Fall0", "Fall1", "FlyKick0", "FlyKick1", "Crouch0", "Crouch1"] + [f"CrouchKick{I}" for I in range(5)]
	         + [f"Kick{I}" for I in range(5)] + ["Hurt0", "Hurt1"] + [f"Idle{I}" for I in range(4)] + [f"Punch{I}" for I in range(6)]
	         + [f"Walk{I}" for I in range(6)])
	Rects = GridRects(13, 36, 82, 60)
	Pivot = CharacterPivot(Img, Rects[20:24], PlayerCapsule[1] / 2)
	WriteSprite("Player", Church("player.png"), [(N,) + R + (Pivot,) for N, R in zip(Names, Rects)])
	print(f"플레이어 피벗 {Pivot}")
	WriteFlipbook("Player_Idle", "Player", [f"Idle{I}" for I in range(4)], 7)
	WriteFlipbook("Player_Run", "Player", [f"Walk{I}" for I in range(6)], 12)
	WriteFlipbook("Player_Jump", "Player", ["Jump0", "Jump1"], 10, "Once")
	WriteFlipbook("Player_Fall", "Player", ["Fall0", "Fall1"], 8)
	WriteFlipbook("Player_Dash", "Player", ["FlyKick0", "FlyKick1"], 14)
	WriteFlipbook("Player_Hurt", "Player", ["Hurt0", "Hurt1"], 10, "Once")
	WriteFlipbook("Player_Death", "Player", ["Hurt0", "Hurt1", "Crouch0", "Crouch1"], 6, "Once")

	# ---- 마법사 (81x66 15칸: 대기 0~4, 시전 5~14 — 12번째 칸에서 불덩이를 던진다)
	Img = Image.open(Church("wizard.png"))
	Rects = GridRects(15, 15, 81, 66)
	Pivot = CharacterPivot(Img, Rects[0:5], WizardCapsule[1] / 2)
	WriteSprite("Wizard", Church("wizard.png"), [(f"Wizard{I}",) + R + (Pivot,) for I, R in enumerate(Rects)])
	WriteFlipbook("Wizard_Idle", "Wizard", [f"Wizard{I}" for I in range(5)], 8)
	WriteFlipbook("Wizard_Cast", "Wizard", [f"Wizard{I}" for I in range(5, 15)], 12, "Once", [(7, "Shoot")])

	# ---- 천사 보스 (122x117 9열 11칸: 공격 0~2, 대기 3~10)
	Img = Image.open(Church("angel.png"))
	Rects = GridRects(9, 11, 122, 117)
	Pivot = CenterPivot(Img, Rects[3:11])
	WriteSprite("Angel", Church("angel.png"), [(f"Angel{I}",) + R + (Pivot,) for I, R in enumerate(Rects)])
	WriteFlipbook("Angel_Idle", "Angel", [f"Angel{I}" for I in range(3, 11)], 10)
	WriteFlipbook("Angel_Attack", "Angel", [f"Angel{I}" for I in range(3)], 8)

	# ---- 불타는 구울 (57x60 16칸 달리기)
	Img = Image.open(Church("burning-ghoul.png"))
	Rects = GridRects(16, 16, 57, 60)
	Pivot = CharacterPivot(Img, Rects, GhoulCapsule[1] / 2)
	WriteSprite("Ghoul", Church("burning-ghoul.png"), [(f"Ghoul{I}",) + R + (Pivot,) for I, R in enumerate(Rects)])
	WriteFlipbook("Ghoul_Run", "Ghoul", [f"Ghoul{I}" for I in range(16)], 16)

	# ---- 효과: 적 사망 폭발 (81x66 9칸), 불덩이 (26x26 3칸)
	Img = Image.open(Church("enemy-death.png"))
	Rects = GridRects(9, 9, 81, 66)
	WriteSprite("DeathFx", Church("enemy-death.png"), [(f"Death{I}",) + R + ([0.5, 0.5],) for I, R in enumerate(Rects)])
	WriteFlipbook("Fx_Death", "DeathFx", [f"Death{I}" for I in range(9)], 20, "Once")
	Rects = GridRects(3, 3, 26, 26)
	WriteSprite("Fireball", Church("fireball.png"), [(f"Fireball{I}",) + R + ([0.5, 0.5],) for I, R in enumerate(Rects)])
	WriteFlipbook("Fx_Fireball", "Fireball", [f"Fireball{I}" for I in range(3)], 12)

	# ---- 묘지 팩 아틀라스 (atlas.json 칸 위치 — TexturePacker, 잘라내기 없음)
	Img = Image.open(Cemetery("atlas.png"))
	Atlas = {
		"enemy-death-1": (830, 2, 44, 52), "enemy-death-2": (784, 2, 44, 52), "enemy-death-3": (692, 2, 44, 52),
		"enemy-death-4": (738, 2, 44, 52), "enemy-death-5": (646, 2, 44, 52),
		"ghost-halo-1": (716, 178, 37, 65), "ghost-halo-2": (794, 178, 37, 65), "ghost-halo-3": (833, 178, 37, 65), "ghost-halo-4": (755, 178, 37, 65),
		"hell-gato-1": (296, 56, 96, 53), "hell-gato-2": (198, 56, 96, 53), "hell-gato-3": (100, 56, 96, 53), "hell-gato-4": (2, 56, 96, 53),
		"skeleton-clothed-1": (48, 2, 44, 52), "skeleton-clothed-2": (186, 2, 44, 52), "skeleton-clothed-3": (140, 2, 44, 52),
		"skeleton-clothed-4": (232, 2, 44, 52), "skeleton-clothed-5": (94, 2, 44, 52), "skeleton-clothed-6": (2, 2, 44, 52),
		"skeleton-clothed-7": (278, 2, 44, 52), "skeleton-clothed-8": (370, 2, 44, 52),
		"skeleton-rise-clothed-1": (508, 2, 44, 52), "skeleton-rise-clothed-2": (462, 2, 44, 52), "skeleton-rise-clothed-3": (554, 2, 44, 52),
		"skeleton-rise-clothed-4": (416, 2, 44, 52), "skeleton-rise-clothed-5": (324, 2, 44, 52), "skeleton-rise-clothed-6": (600, 2, 44, 52),
	}
	SkelRects = [Atlas[f"skeleton-clothed-{I}"] for I in range(1, 9)]
	SkelPivot = CharacterPivot(Img, SkelRects, SkeletonCapsule[1] / 2)
	GatoRects = [Atlas[f"hell-gato-{I}"] for I in range(1, 5)]
	GatoPivot = CharacterPivot(Img, GatoRects, GatoCapsule[1] / 2)
	GhostRects = [Atlas[f"ghost-halo-{I}"] for I in range(1, 5)]
	GhostPivot = CenterPivot(Img, GhostRects)
	Slices = []
	Slices += [(f"Skeleton{I}",) + R + (SkelPivot,) for I, R in enumerate(SkelRects)]
	Slices += [(f"SkeletonRise{I}",) + Atlas[f"skeleton-rise-clothed-{I + 1}"] + (SkelPivot,) for I in range(6)]
	Slices += [(f"Gato{I}",) + R + (GatoPivot,) for I, R in enumerate(GatoRects)]
	Slices += [(f"Ghost{I}",) + R + (GhostPivot,) for I, R in enumerate(GhostRects)]
	Slices += [(f"Flame{I}",) + Atlas[f"enemy-death-{I + 1}"] + ([0.5, 0.3],) for I in range(5)]
	WriteSprite("Cemetery", Cemetery("atlas.png"), Slices)
	WriteFlipbook("Skeleton_Walk", "Cemetery", [f"Skeleton{I}" for I in range(8)], 10)
	WriteFlipbook("Skeleton_Rise", "Cemetery", [f"SkeletonRise{I}" for I in range(6)], 9, "Once")
	WriteFlipbook("Gato_Run", "Cemetery", [f"Gato{I}" for I in range(4)], 10)
	WriteFlipbook("Ghost_Float", "Cemetery", [f"Ghost{I}" for I in range(4)], 8)
	WriteFlipbook("Fx_Flame", "Cemetery", [f"Flame{I}" for I in range(5)], 14, "Once")

	# ---- 묘지 소품 (atlas-props.json)
	Props = {"BushLarge": (146, 2, 76, 65), "BushSmall": (2, 2, 34, 29), "Statue": (2, 69, 63, 75), "Stone1": (88, 2, 27, 39),
	         "Stone2": (117, 2, 27, 40), "Stone3": (38, 2, 27, 33), "Stone4": (67, 2, 19, 38), "Tree1": (2, 188, 166, 117),
	         "Tree2": (67, 69, 166, 117), "Tree3": (2, 307, 176, 171)}
	WriteSprite("CemeteryProps", Cemetery("atlas-props.png"), [(N,) + R + ([0.5, 0.0],) for N, R in Props.items()])

	# ---- 배경: 교회 장식 판 (backgrounds.png — 완전히 투명한 세로 줄로 나눈다), 기둥, 묘지 하늘/산/묘지
	# 장식 판: FetchAssets.ps1이 단색 바탕(39,38,56)을 투명으로 바꾼 backgrounds_keyed.png. 원본에서 완전히 투명한 세로 줄로 판을 나누고
	# 판마다 키 처리본의 불투명 영역으로 자른다 (피벗 = 아래 가운데)
	Img = Image.open(Church("backgrounds.png")).convert("RGBA")
	Keyed = Image.open(Church("backgrounds_keyed.png")).convert("RGBA")
	Alpha = Img.getchannel("A")
	Columns = [any(Alpha.getpixel((X, Y)) > 0 for Y in range(Img.height)) for X in range(Img.width)]
	Panels = []
	X = 0
	while X < Img.width:
		if Columns[X]:
			Start = X
			while X < Img.width and Columns[X]:
				X += 1
			Box = Keyed.crop((Start, 0, X, Img.height)).getchannel("A").getbbox()
			Panels.append((Start + Box[0], Box[1], Box[2] - Box[0], Box[3] - Box[1]))
		X += 1
	PanelNames = ["Window", "Pillar", "Altar", "Gargoyle", "Lantern"]
	if len(Panels) != len(PanelNames):
		print(f"경고: 배경 판 {len(Panels)}개 (예상 {len(PanelNames)}) — 이름을 번호로")
		PanelNames = [f"Panel{I}" for I in range(len(Panels))]
	WriteSprite("Backdrop", Church("backgrounds_keyed.png"), [(N,) + P + ([0.5, 0.0],) for N, P in zip(PanelNames, Panels)])
	print(f"배경 판: {list(zip(PanelNames, Panels))}")
	Img = Image.open(Church("column.png"))
	WriteSprite("Column", Church("column.png"), [("Column", 0, 0, Img.width, Img.height, [0.5, 0.0])])
	for Name in ("background", "mountains", "graveyard"):
		Img = Image.open(Cemetery(f"{Name}.png"))
		WriteSprite(f"Cemetery_{Name.capitalize()}", Cemetery(f"{Name}.png"), [(Name.capitalize(), 0, 0, Img.width, Img.height, [0.5, 0.0])])

	# ---- 타일셋 (교회 tileset.png, 16px 21x14). 충돌: 윗면 = 아래 절반 다각형, 몸통·단색 = Full, 안쪽 모서리 = 오각형, 발판 = 원웨이
	Img = Image.open(Church("tileset.png"))
	Tiles = []
	LowerHalf = [[0.0, 8.0], [16.0, 8.0], [16.0, 16.0], [0.0, 16.0]]
	CornerPoly = [[0.0, 0.0], [8.0, 0.0], [16.0, 8.0], [16.0, 16.0], [0.0, 16.0]]
	Defs = {}
	for Group in Rooms.TopGroups:
		for Id in Group:
			Defs[Id] = {"Id": Id, "Collision": "Polygon", "Points": LowerHalf, "Tags": ["Surface"]}
			Defs[Id + 21] = {"Id": Id + 21, "Collision": "Full", "Tags": ["Ground"]}
	Defs[Rooms.FillTile] = {"Id": Rooms.FillTile, "Collision": "Full", "Tags": ["Ground"]}
	Defs[Rooms.CornerTile] = {"Id": Rooms.CornerTile, "Collision": "Polygon", "Points": CornerPoly, "Tags": ["Ground"]}
	for Id in Rooms.PlatformTiles:
		Defs[Id] = {"Id": Id, "Collision": "Full", "OneWay": True, "Tags": ["Platform"]}
	for Row in Rooms.GateTiles:
		for Id in Row:
			Defs[Id] = {"Id": Id, "Collision": "Full", "Tags": ["Gate"]}
	Tiles = [Defs[Id] for Id in sorted(Defs)]
	WriteJson(os.path.join(SpriteDir, "Crypt.etileset"), {
		"Version": 1, "Texture": Rel(SpriteDir, Church("tileset.png")), "TextureWidth": Img.width, "TextureHeight": Img.height,
		"TileWidth": TilePx, "TileHeight": TilePx, "Margin": 0, "Spacing": 0, "Columns": Img.width // TilePx, "Rows": Img.height // TilePx,
		"UnitsPerPixel": UPP, "Filter": "Point", "Tiles": Tiles})

	# ---- 자체 제작 아틀라스
	Atlas, Slices, Weapons = Art.BuildGeneratedAtlas()
	GenDir = os.path.join(SpriteDir, "Generated")
	os.makedirs(GenDir, exist_ok=True)
	GenPng = os.path.join(GenDir, "Generated.png")
	Atlas.save(GenPng)
	WriteSprite("Generated", GenPng, Slices)
	WriteFlipbook("Fx_Slash", "Generated", ["Slash0", "Slash1", "Slash2"], 24, "Once")
	WriteFlipbook("Fx_Spark", "Generated", ["Spark0", "Spark1", "Spark2"], 24, "Once")
	WriteFlipbook("Fx_Dust", "Generated", [f"Dust{I}" for I in range(4)], 16, "Once")
	WriteFlipbook("Fx_Coin", "Generated", [f"Coin{I}" for I in range(4)], 10)
	WriteFlipbook("Fx_Portal", "Generated", [f"Portal{I}" for I in range(4)], 8)
	WriteFlipbook("Fx_Orb", "Generated", ["Orb0", "Orb1"], 10)
	Art.WriteUiIcons(os.path.join(Content, "UI", "Crypt", "Icons"), Weapons)
	return SkelPivot


# ================================================================ 오디오
def BuildAudio():
	Out = os.path.join(Content, "Audio", "Crypt")
	os.makedirs(Out, exist_ok=True)
	SampleContent = os.path.join(Root, "Projects", "Sample", "Content")
	Copies = {
		"Swing1.wav": "Audio/RPG/Swing1.wav", "Swing2.wav": "Audio/RPG/Swing2.wav", "Swing3.wav": "Audio/RPG/Swing3.wav",
		"Hit.wav": "Audio/RPG/Hit.wav", "HitHeavy.wav": "Audio/RPG/HitHeavy.wav", "Hurt.wav": "Audio/RPG/Hurt.wav",
		"Dash.wav": "Audio/RPG/Dash.wav", "Block.wav": "Audio/RPG/Block.wav",
		"Coin.wav": "Asset/Kenney_RPGAudio/coins_pickup.wav", "ChestOpen.wav": "Asset/Kenney_RPGAudio/item_drop.wav",
		"Equip.wav": "Asset/Kenney_RPGAudio/equip.wav", "Heal.wav": "Asset/Kenney_InterfaceSounds/potion.wav",
		"UIConfirm.wav": "Asset/Kenney_InterfaceSounds/confirm.wav", "UIClick.wav": "Asset/Kenney_InterfaceSounds/click.wav",
		"DoorOpen.wav": "Asset/Kenney_InterfaceSounds/open.wav", "DoorClose.wav": "Asset/Kenney_InterfaceSounds/close.wav",
	}
	for Name, Source in Copies.items():
		shutil.copyfile(os.path.join(SampleContent, Source), os.path.join(Out, Name))
	shutil.copyfile(os.path.join(SampleContent, "Asset/Kenney_RPGAudio/License.txt"), os.path.join(Out, "Kenney_RPGAudio_License.txt"))
	shutil.copyfile(os.path.join(SampleContent, "Asset/Kenney_InterfaceSounds/License.txt"), os.path.join(Out, "Kenney_InterfaceSounds_License.txt"))
	Art.WriteSynthSounds(Out)
	WriteText(os.path.join(Out, "SOURCE.txt"),
	          "Crypt2D 효과음 (BuildCrypt2D.py가 만든다)\n"
	          "- Swing1~3/Hit/HitHeavy/Hurt/Dash/Block: Projects/Sample/Content/Audio/RPG (프로젝트 자체 절차 생성, 라이선스 제약 없음)\n"
	          "- Coin/ChestOpen/Equip: Kenney RPG Audio (CC0), Heal/UIConfirm/UIClick/DoorOpen/DoorClose: Kenney Interface Sounds (CC0)\n"
	          "- Jump/Shoot/Fireball/Explode/Portal/BossRoar/Door: Tools/Crypt2DArt.py가 합성 (자체 제작)\n"
	          "형식: WAV (miniaudio 빌드에 Vorbis 디코더가 없어 .ogg는 재생되지 않는다)\n")


# ================================================================ 데이터 표
def BuildData():
	Dir = os.path.join(Content, "Data", "Crypt")
	WriteJson(os.path.join(Dir, "Weapon.estruct"), {
		"Version": 1, "Name": "Weapon", "Description": "무기 (Weapons.etable, 행 이름 = 무기 id)",
		"Fields": [
			{"Name": "DisplayName", "Type": "String", "Default": "무기", "Description": "표시 이름"},
			{"Name": "Kind", "Type": "Enum", "Values": ["Melee", "Ranged"], "Default": "Melee", "Description": "근접(휘두르기 호 판정) / 원거리(투사체)"},
			{"Name": "Slice", "Type": "String", "Default": "Sword", "Description": "Generated.esprite 슬라이스 (손에 든 모습)"},
			{"Name": "Icon", "Type": "String", "Default": "UI/Crypt/Icons/Sword.png", "Description": "HUD 슬롯 아이콘"},
			{"Name": "DamageMin", "Type": "Float", "Default": 8, "Description": "한 대 최소"},
			{"Name": "DamageMax", "Type": "Float", "Default": 12, "Description": "한 대 최대"},
			{"Name": "CritChance", "Type": "Float", "Default": 0.1, "Description": "치명타 확률 0~1"},
			{"Name": "CritMultiplier", "Type": "Float", "Default": 1.8, "Description": "치명타 배율"},
			{"Name": "Cooldown", "Type": "Float", "Default": 0.35, "Description": "공격 간격 (초)"},
			{"Name": "Range", "Type": "Float", "Default": 110, "Description": "근접 = 판정 반경 (cm, 손에서), 원거리 = 사거리"},
			{"Name": "Arc", "Type": "Float", "Default": 130, "Description": "근접 판정 각 (도, 조준 방향 중심)"},
			{"Name": "Knockback", "Type": "Float", "Default": 600, "Description": "넉백 속도 (cm/s)"},
			{"Name": "ProjectileSpeed", "Type": "Float", "Default": 2200, "Description": "원거리 투사체 속도 (cm/s)"},
			{"Name": "Projectile", "Type": "Enum", "Values": ["Bolt", "Fireball"], "Default": "Bolt", "Description": "원거리 투사체 모양"},
			{"Name": "Pierce", "Type": "Int", "Default": 0, "Description": "투사체가 더 꿰뚫는 적 수"},
			{"Name": "Explosion", "Type": "Float", "Default": 0, "Description": "> 0이면 명중 시 폭발 반경 (cm)"},
			{"Name": "SwingSound", "Type": "String", "Default": "Audio/Crypt/Swing1.wav", "Description": "공격 소리"},
			{"Name": "MinFloor", "Type": "Int", "Default": 1, "Description": "상자에서 나오기 시작하는 층"},
		]})
	Weapons = [
		("ShortSword", {"DisplayName": "녹슨 검", "Kind": "Melee", "Slice": "Sword", "Icon": "UI/Crypt/Icons/Sword.png", "DamageMin": 8, "DamageMax": 12,
		                "CritChance": 0.12, "CritMultiplier": 1.8, "Cooldown": 0.32, "Range": 120, "Arc": 140, "Knockback": 520,
		                "ProjectileSpeed": 0, "Projectile": "Bolt", "Pierce": 0, "Explosion": 0, "SwingSound": "Audio/Crypt/Swing1.wav", "MinFloor": 1}),
		("Crossbow", {"DisplayName": "사냥꾼의 석궁", "Kind": "Ranged", "Slice": "Crossbow", "Icon": "UI/Crypt/Icons/Crossbow.png", "DamageMin": 7, "DamageMax": 10,
		              "CritChance": 0.15, "CritMultiplier": 2.0, "Cooldown": 0.42, "Range": 1400, "Arc": 0, "Knockback": 260,
		              "ProjectileSpeed": 2400, "Projectile": "Bolt", "Pierce": 0, "Explosion": 0, "SwingSound": "Audio/Crypt/Shoot.wav", "MinFloor": 1}),
		("GreatAxe", {"DisplayName": "처형인의 도끼", "Kind": "Melee", "Slice": "Axe", "Icon": "UI/Crypt/Icons/Axe.png", "DamageMin": 20, "DamageMax": 28,
		              "CritChance": 0.1, "CritMultiplier": 2.0, "Cooldown": 0.7, "Range": 150, "Arc": 170, "Knockback": 950,
		              "ProjectileSpeed": 0, "Projectile": "Bolt", "Pierce": 0, "Explosion": 0, "SwingSound": "Audio/Crypt/Swing3.wav", "MinFloor": 1}),
		("Spear", {"DisplayName": "성기사의 창", "Kind": "Melee", "Slice": "Spear", "Icon": "UI/Crypt/Icons/Spear.png", "DamageMin": 12, "DamageMax": 16,
		           "CritChance": 0.2, "CritMultiplier": 1.7, "Cooldown": 0.45, "Range": 190, "Arc": 50, "Knockback": 700,
		           "ProjectileSpeed": 0, "Projectile": "Bolt", "Pierce": 0, "Explosion": 0, "SwingSound": "Audio/Crypt/Swing2.wav", "MinFloor": 1}),
		("FireStaff", {"DisplayName": "화염 지팡이", "Kind": "Ranged", "Slice": "Staff", "Icon": "UI/Crypt/Icons/Staff.png", "DamageMin": 14, "DamageMax": 18,
		               "CritChance": 0.08, "CritMultiplier": 1.6, "Cooldown": 0.65, "Range": 1100, "Arc": 0, "Knockback": 400,
		               "ProjectileSpeed": 1300, "Projectile": "Fireball", "Pierce": 0, "Explosion": 150, "SwingSound": "Audio/Crypt/Fireball.wav", "MinFloor": 2}),
	]
	WriteJson(os.path.join(Dir, "Weapons.etable"), {"Version": 1, "Struct": "Data/Crypt/Weapon.estruct",
	                                                "Rows": [{"Name": N, "Values": V} for N, V in Weapons]})

	WriteJson(os.path.join(Dir, "Enemy.estruct"), {
		"Version": 1, "Name": "Enemy", "Description": "적 (Enemies.etable, 행 이름 = Enemy.lua Kind = 프리팹 이름)",
		"Fields": [
			{"Name": "DisplayName", "Type": "String", "Default": "적", "Description": "표시 이름"},
			{"Name": "Behavior", "Type": "Enum", "Values": ["Flyer", "Melee", "Charger", "Leaper", "Caster", "Boss"], "Default": "Melee",
			 "Description": "행동: 비행 추적 / 순찰·추적·돌진 베기 / 질주 / 도약 / 거리 유지 시전 / 보스"},
			{"Name": "MaxHealth", "Type": "Float", "Default": 30, "Description": "체력"},
			{"Name": "ContactDamage", "Type": "Float", "Default": 6, "Description": "몸에 닿으면 (0 = 없음)"},
			{"Name": "AttackDamage", "Type": "Float", "Default": 10, "Description": "공격·투사체 한 대"},
			{"Name": "MoveSpeed", "Type": "Float", "Default": 250, "Description": "이동 속도 (cm/s)"},
			{"Name": "AggroRange", "Type": "Float", "Default": 900, "Description": "발견 거리 (cm)"},
			{"Name": "AttackRange", "Type": "Float", "Default": 140, "Description": "공격 시작 거리 (cm)"},
			{"Name": "AttackCooldown", "Type": "Float", "Default": 1.6, "Description": "공격 간격 (초)"},
			{"Name": "ProjectileSpeed", "Type": "Float", "Default": 700, "Description": "투사체 속도 (cm/s)"},
			{"Name": "GoldMin", "Type": "Int", "Default": 1, "Description": "떨어뜨리는 코인 최소"},
			{"Name": "GoldMax", "Type": "Int", "Default": 3, "Description": "최대"},
			{"Name": "FacesRight", "Type": "Bool", "Default": False, "Description": "시트 원본이 오른쪽을 보는가 (반전 기준)"},
			{"Name": "Radius", "Type": "Float", "Default": 40, "Description": "피격·접촉 판정 반지름 (cm)"},
			{"Name": "MinFloor", "Type": "Int", "Default": 1, "Description": "나오기 시작하는 층"},
			{"Name": "Weight", "Type": "Float", "Default": 1, "Description": "같은 표식 안 뽑기 가중치"},
		]})
	Enemies = [
		("Ghost", {"DisplayName": "떠도는 망령", "Behavior": "Flyer", "MaxHealth": 18, "ContactDamage": 7, "AttackDamage": 7, "MoveSpeed": 190,
		           "AggroRange": 1100, "AttackRange": 0, "AttackCooldown": 1.0, "ProjectileSpeed": 0, "GoldMin": 1, "GoldMax": 2, "FacesRight": False,
		           "Radius": 44, "MinFloor": 1, "Weight": 1}),
		("Skeleton", {"DisplayName": "해골 전사", "Behavior": "Melee", "MaxHealth": 30, "ContactDamage": 5, "AttackDamage": 11, "MoveSpeed": 210,
		              "AggroRange": 900, "AttackRange": 150, "AttackCooldown": 1.5, "ProjectileSpeed": 0, "GoldMin": 2, "GoldMax": 4, "FacesRight": False,
		              "Radius": 40, "MinFloor": 1, "Weight": 1.4}),
		("Ghoul", {"DisplayName": "불타는 구울", "Behavior": "Charger", "MaxHealth": 34, "ContactDamage": 10, "AttackDamage": 10, "MoveSpeed": 430,
		           "AggroRange": 800, "AttackRange": 0, "AttackCooldown": 1.0, "ProjectileSpeed": 0, "GoldMin": 2, "GoldMax": 5, "FacesRight": False,
		           "Radius": 42, "MinFloor": 1, "Weight": 0.8}),
		("Gato", {"DisplayName": "지옥 고양이", "Behavior": "Leaper", "MaxHealth": 26, "ContactDamage": 9, "AttackDamage": 9, "MoveSpeed": 360,
		          "AggroRange": 900, "AttackRange": 600, "AttackCooldown": 1.8, "ProjectileSpeed": 0, "GoldMin": 2, "GoldMax": 4, "FacesRight": False,
		          "Radius": 46, "MinFloor": 2, "Weight": 0.9}),
		("Wizard", {"DisplayName": "타락한 사제", "Behavior": "Caster", "MaxHealth": 24, "ContactDamage": 4, "AttackDamage": 9, "MoveSpeed": 170,
		            "AggroRange": 1300, "AttackRange": 1100, "AttackCooldown": 2.4, "ProjectileSpeed": 650, "GoldMin": 3, "GoldMax": 5, "FacesRight": False,
		            "Radius": 40, "MinFloor": 1, "Weight": 1}),
		("Angel", {"DisplayName": "타락 천사 세라핌", "Behavior": "Boss", "MaxHealth": 650, "ContactDamage": 12, "AttackDamage": 10, "MoveSpeed": 320,
		           "AggroRange": 4000, "AttackRange": 0, "AttackCooldown": 1.2, "ProjectileSpeed": 520, "GoldMin": 30, "GoldMax": 40, "FacesRight": True,
		           "Radius": 100, "MinFloor": 3, "Weight": 1}),
	]
	WriteJson(os.path.join(Dir, "Enemies.etable"), {"Version": 1, "Struct": "Data/Crypt/Enemy.estruct",
	                                                "Rows": [{"Name": N, "Values": V} for N, V in Enemies]})

	WriteJson(os.path.join(Dir, "Balance.estruct"), {
		"Version": 1, "Name": "Balance", "Description": "플레이어·던전 밸런스 (Balance.edata 하나)",
		"Fields": [
			{"Name": "MaxHealth", "Type": "Float", "Default": 80, "Description": "플레이어 최대 체력"},
			{"Name": "InvulnTime", "Type": "Float", "Default": 0.9, "Description": "피격 후 무적 (초)"},
			{"Name": "DashCharges", "Type": "Int", "Default": 2, "Description": "대시 충전 칸"},
			{"Name": "DashRecharge", "Type": "Float", "Default": 1.4, "Description": "대시 한 칸 재충전 (초)"},
			{"Name": "StartWeapons", "Type": "Array", "Element": "String", "Default": ["ShortSword", "Crossbow"], "Description": "시작 무기 2칸"},
			{"Name": "FloorRooms", "Type": "Array", "Element": "Int", "Default": [7, 8], "Description": "층별 방 수 (마지막 층 다음은 보스 층)"},
			{"Name": "SpawnRatio", "Type": "Array", "Element": "Float", "Default": [0.65, 0.85], "Description": "층별 템플릿 적 표식 사용 비율"},
			{"Name": "HealthMultiplier", "Type": "Array", "Element": "Float", "Default": [1.0, 1.35, 1.0], "Description": "층별 적 체력 배율"},
			{"Name": "ClearGold", "Type": "Int", "Default": 5, "Description": "방 정리 보상 코인"},
			{"Name": "HeartHeal", "Type": "Float", "Default": 20, "Description": "하트 회복량"},
			{"Name": "HeartChance", "Type": "Float", "Default": 0.35, "Description": "방 정리 시 하트가 나올 확률"},
		]})
	WriteJson(os.path.join(Dir, "Balance.edata"), {"Version": 1, "Struct": "Data/Crypt/Balance.estruct", "Values": {
		"MaxHealth": 80, "InvulnTime": 0.9, "DashCharges": 2, "DashRecharge": 1.4, "StartWeapons": ["ShortSword", "Crossbow"],
		"FloorRooms": [7, 8], "SpawnRatio": [0.65, 0.85], "HealthMultiplier": [1.0, 1.35, 1.0], "ClearGold": 5, "HeartHeal": 20, "HeartChance": 0.35}})


# ================================================================ 방 템플릿 → Lua
def BuildRoomsLua(Templates):
	Lines = ["-- Crypt2D 방 템플릿 (생성물 — Projects/Crypt2D/Tools/Crypt2DRooms.py를 고치고 BuildCrypt2D.py로 다시 만든다).",
	         "-- 행은 위 → 아래, 문자 규칙은 Crypt2DRooms.py 머리 주석. Dungeon.lua가 읽어 층 타일맵을 조립한다.",
	         "return {",
	         f"\tWidth = {Rooms.RoomWidth},",
	         f"\tHeight = {Rooms.RoomHeight},",
	         "\tTemplates = {"]
	for T in Templates:
		Lines.append(f"\t\t{{ Name = \"{T.Name}\", Kind = \"{T.Kind}\", Rows = {{")
		for Row in T.Rows():
			Lines.append(f"\t\t\t\"{Row}\",")
		Lines.append("\t\t} },")
	Lines += ["\t},", "}", ""]
	WriteText(os.path.join(Content, "Scripts", "Crypt", "Rooms.lua"), "\n".join(Lines))


# ================================================================ 프로젝트 설정
def BuildConfig():
	Config = os.path.join(Project, "Config")
	WriteJson(os.path.join(Project, "Crypt2D.eproject"), {"Name": "Crypt2D", "EngineVersion": "0.1.0"})
	WriteJson(os.path.join(Config, "Project.json"), {"DisplayName": "Crypt2D", "Version": "0.1.0", "Company": "ProjectE", "Icon": "",
	                                                 "ExecutableName": "Crypt2D", "SteamAppId": 0, "SteamDepotId": 0})
	WriteJson(os.path.join(Config, "Maps.json"), {"EditorStartupMap": "Scenes/Crypt.escene", "GameDefaultMap": "Scenes/Title.escene",
	                                              "ServerDefaultMap": "", "PlayerPrefab": ""})
	WriteJson(os.path.join(Config, "Display.json"), {"WindowMode": "Windowed", "WindowWidth": 1280, "WindowHeight": 720, "VSync": True,
	                                                 "ResolutionQuality": "Native", "DynamicResolution": False, "DynamicResolutionTargetMs": 16.6,
	                                                 "HdrOutput": "Off", "HdrPaperWhiteNits": 200.0, "HdrMaxNits": 0.0})
	# 2D: 레이 트레이싱 효과는 스프라이트(TLAS 밖)에 쓸모가 없다 — 끈다
	WriteJson(os.path.join(Config, "Rendering.json"), {"RayTracing": False, "RayTracedShadows": False, "RayTracedReflections": False})
	WriteJson(os.path.join(Config, "Physics.json"), {"Gravity": [0.0, 0.0, -980.665], "Gravity2D": [0.0, -980.665], "FixedStepHz": 60.0, "MaxSubSteps": 4})
	WriteJson(os.path.join(Config, "SortingLayers.json"), {"Layers": ["Default", "Background", "BackDecor", "Tiles", "Props", "Pickups",
	                                                                  "Characters", "Weapons", "Projectiles", "FX", "Foreground"]})
	WriteJson(os.path.join(Config, "Collision.json"), {
		"Layers": ["Default", "Terrain", "Player", "Enemy", "PlayerAttack", "EnemyAttack", "Pickup"],
		"DisabledPairs": [["Player", "Enemy"], ["Enemy", "Enemy"], ["Player", "Player"], ["PlayerAttack", "Player"], ["PlayerAttack", "PlayerAttack"],
		                  ["PlayerAttack", "EnemyAttack"], ["EnemyAttack", "Enemy"], ["EnemyAttack", "EnemyAttack"], ["Pickup", "Player"],
		                  ["Pickup", "Enemy"], ["Pickup", "PlayerAttack"], ["Pickup", "EnemyAttack"], ["Pickup", "Pickup"]]})
	WriteJson(os.path.join(Config, "Console.json"), {"EnableInPackagedGame": False})
	WriteJson(os.path.join(Config, "Packaging.json"), {"Configuration": "Release", "UsePak": True, "IncludeSourceAssets": False,
	                                                   "AdditionalDirectories": "", "AdditionalAssets": "Scripts/Crypt;Data/Crypt;Audio/Crypt;Sprites/Crypt"})

	def Button(Name, Source, Mods=None):
		Binding = {"Source": Source}
		if Mods:
			Binding["Modifiers"] = Mods
		return Binding

	Negate = [{"Type": "Negate", "X": True, "Y": True}]
	Swizzle = [{"Type": "Swizzle"}]
	Dead = [{"Type": "DeadZone", "Lower": 0.25, "Upper": 1.0, "Radial": True}]
	Actions = [
		{"Name": "Move", "Type": "Axis2D", "Description": "이동 (X = 오른쪽, Y = 위 — 아래 + 점프 = 원웨이 발판 내려가기)", "ActuationThreshold": 0.3,
		 "Bindings": [Button("", "D"), Button("", "A", Negate), Button("", "W", Swizzle), Button("", "S", Swizzle + Negate),
		              Button("", "Right"), Button("", "Left", Negate), Button("", "Up", Swizzle), Button("", "Down", Swizzle + Negate),
		              Button("", "Gamepad_LeftStick", Dead)]},
		{"Name": "Jump", "Type": "Button", "Description": "점프 (공중에서 한 번 더 = 2단 점프, 짧게 누르면 낮게)", "ActuationThreshold": 0.5,
		 "Bindings": [Button("", "Space"), Button("", "Gamepad_A")]},
		{"Name": "Dash", "Type": "Button", "Description": "대시 (조준 방향, 충전 칸 소모, 대시 중 무적)", "ActuationThreshold": 0.5,
		 "Bindings": [Button("", "MouseRight"), Button("", "LeftShift"), Button("", "Gamepad_RightShoulder")]},
		{"Name": "Attack", "Type": "Button", "Description": "공격 (누르고 있으면 계속)", "ActuationThreshold": 0.5,
		 "Bindings": [Button("", "MouseLeft"), Button("", "J"), Button("", "Gamepad_RightTrigger")]},
		{"Name": "SwapWeapon", "Type": "Button", "Description": "무기 교체", "ActuationThreshold": 0.5,
		 "Bindings": [Button("", "Q"), Button("", "Gamepad_Y")]},
		{"Name": "WeaponWheel", "Type": "Axis1D", "Description": "무기 교체 (휠)", "ActuationThreshold": 0.1,
		 "Bindings": [Button("", "MouseWheel")]},
		{"Name": "Interact", "Type": "Button", "Description": "상호작용 (상자·무기·계단)", "ActuationThreshold": 0.5,
		 "Bindings": [Button("", "F"), Button("", "E"), Button("", "Gamepad_X")]},
		{"Name": "Pause", "Type": "Button", "Description": "일시정지", "ActuationThreshold": 0.5,
		 "Bindings": [Button("", "Escape"), Button("", "P"), Button("", "Gamepad_Start")]},
		{"Name": "AimStick", "Type": "Axis2D", "Description": "게임패드 조준 (오른쪽 스틱)", "ActuationThreshold": 0.3,
		 "Bindings": [Button("", "Gamepad_RightStick", Dead)]},
		{"Name": "Confirm", "Type": "Button", "Description": "메뉴 확인 (타이틀 시작·사망 화면 재시작)", "ActuationThreshold": 0.5,
		 "Bindings": [Button("", "Enter"), Button("", "R"), Button("", "Gamepad_A")]},
	]
	with open(os.path.join(Config, "Input.json"), "w", encoding="utf-8", newline="\n") as File:
		json.dump({"Version": 1, "Actions": Actions}, File, ensure_ascii=False, indent="\t")
		File.write("\n")


# ================================================================ UI (.eui)
def Brush(Color, Corner=0, BorderWidth=0, BorderColor=(0, 0, 0, 1), Texture=None, Margin=None):
	B = {"Color": list(Color), "CornerRadius": Corner, "BorderWidth": BorderWidth, "BorderColor": list(BorderColor)}
	if Texture:
		B["Texture"] = Texture
	if Margin:
		B["DrawAs"] = "NineSlice"
		B["Margin"] = Margin
	return B


def CanvasSlot(Anchor, X, Y, W, H, Align=(0, 0), AutoSize=False, Z=0):
	return {"AnchorMin": list(Anchor), "AnchorMax": list(Anchor), "Offsets": [X, Y, W, H], "Alignment": list(Align), "AutoSize": AutoSize, "ZOrder": Z}


def StretchSlot(Z=0):
	return {"AnchorMin": [0, 0], "AnchorMax": [1, 1], "Offsets": [0, 0, 0, 0], "Alignment": [0, 0], "AutoSize": False, "ZOrder": Z}


def BoxSlot(Pad=(0, 0, 0, 0), HAlign="Fill", VAlign="Fill", Size="Auto"):
	return {"Padding": list(Pad), "HAlign": HAlign, "VAlign": VAlign, "SizeRule": Size, "FillWeight": 1}


def Widget(Type, Name, Slot=None, Visibility="HitTestInvisible", Children=None, **Fields):
	W = {"Type": Type, "Name": Name, "Enabled": True, "Opacity": 1, "Visibility": Visibility}
	if Slot is not None:
		W["Slot"] = Slot
	W.update(Fields)
	if Children:
		W["Children"] = Children
	return W


TextLight = (0.95, 0.9, 0.84, 1)
TextGold  = (1.0, 0.84, 0.36, 1)
OutlineCol = (0.04, 0.02, 0.06, 1)


def Text(Name, Value, Size, Slot=None, Color=TextLight, Justify="Left", Visibility="HitTestInvisible"):
	return Widget("Text", Name, Slot, Visibility, Text=Value, FontSize=Size, TextColor=list(Color), Justify=Justify, Wrap=False,
	              OutlineWidth=2, OutlineColor=list(OutlineCol), ShadowOffset=[0, 2], ShadowColor=[0, 0, 0, 0.6])


def MenuButton(Name, Label, Slot):
	return Widget("Button", Name, Slot, "Visible",
	              [Text(Name + "Text", Label, 28, BoxSlot(HAlign="Center", VAlign="Center"), Justify="Center")],
	              Brush=Brush((0.2, 0.12, 0.28, 0.95), 2, 2, (0.05, 0.02, 0.08, 1)),
	              HoveredBrush=Brush((0.36, 0.2, 0.46, 1), 2, 2, (0.95, 0.78, 0.4, 1)),
	              PressedBrush=Brush((0.14, 0.08, 0.2, 1), 2, 2, (0.95, 0.78, 0.4, 1)),
	              DisabledBrush=Brush((0.12, 0.1, 0.14, 0.8), 2, 2, (0.05, 0.02, 0.08, 1)),
	              ContentPadding=[24, 8, 24, 8], MinSize=[320, 56])


def Overlay(Name, Title, TitleColor, BodyName, Buttons, Z):
	Box = [Text(Name + "Title", Title, 64, BoxSlot((0, 0, 0, 18), HAlign="Center"), TitleColor, "Center")]
	if BodyName:
		Box.append(Text(BodyName, "", 26, BoxSlot((0, 0, 0, 26), HAlign="Center"), TextLight, "Center"))
	for (ButtonName, Label) in Buttons:
		Box.append(MenuButton(ButtonName, Label, BoxSlot((0, 6, 0, 6), HAlign="Center")))
	return Widget("Overlay", Name, StretchSlot(Z), "Collapsed", [
		Widget("Border", Name + "Shade", BoxSlot(), "Visible", Brush=Brush((0.02, 0.0, 0.04, 0.72)), ContentPadding=[0, 0, 0, 0]),
		Widget("VerticalBox", Name + "Box", BoxSlot(HAlign="Center", VAlign="Center"), "SelfHitTestInvisible", Box),
	])


MapCols, MapRows = 5, 4
MapCellW, MapCellH, MapGap = 30, 18, 8


def BuildHud():
	Children = []
	# ---- 왼쪽 위: 체력 / 대시 / 코인
	Children.append(Widget("Border", "StatusPanel", CanvasSlot((0, 0), 16, 16, 330, 104), "HitTestInvisible", [
		Widget("VerticalBox", "StatusBox", BoxSlot(), "SelfHitTestInvisible", [
			Widget("Overlay", "HpRow", BoxSlot((0, 0, 0, 8)), "SelfHitTestInvisible", [
				Widget("ProgressBar", "HpBar", BoxSlot(), MinSize=[298, 28], Brush=Brush((0.1, 0.03, 0.05, 1), 2, 2, (0.02, 0, 0.02, 1)),
				       FillBrush=Brush((0.84, 0.16, 0.22, 1), 1), Percent=1.0, FillDirection="LeftToRight"),
				Text("HpText", "80 / 80", 20, BoxSlot(HAlign="Center", VAlign="Center"), Justify="Center"),
			], MinSize=[298, 28]),
			Widget("HorizontalBox", "DashRow", BoxSlot((0, 0, 0, 8)), "SelfHitTestInvisible",
			       [Widget("ProgressBar", f"Dash{I}", BoxSlot((0, 0, 6, 0)), MinSize=[56, 12],
			               Brush=Brush((0.06, 0.06, 0.1, 1), 1, 1, (0, 0, 0, 1)), FillBrush=Brush((0.45, 0.85, 1.0, 1), 1),
			               Percent=1.0, FillDirection="LeftToRight", Visibility="HitTestInvisible") for I in range(3)]),
			Widget("HorizontalBox", "GoldRow", BoxSlot(), "SelfHitTestInvisible", [
				Widget("Image", "GoldIcon", BoxSlot((0, 0, 8, 0), VAlign="Center"), Brush=Brush((1, 1, 1, 1), Texture="UI/Crypt/Icons/Coin.png"), ImageSize=[24, 24]),
				Text("GoldText", "0", 22, BoxSlot((0, 0, 24, 0), VAlign="Center"), TextGold),
				Text("KillText", "처치 0", 20, BoxSlot(VAlign="Center"), (0.8, 0.76, 0.86, 1)),
			]),
		]),
	], Brush=Brush((0.03, 0.01, 0.05, 0.6), 4, 2, (0.2, 0.12, 0.26, 0.9)), ContentPadding=[16, 12, 16, 10]))

	# ---- 오른쪽 위: 층 + 미니맵 (방 격자 5x4 — 칸/연결 위젯 Map_x_y, MapH_x_y(x와 x+1 사이), MapV_x_y(y와 y+1 사이). y 0 = 아래)
	MapW = MapCols * MapCellW + (MapCols - 1) * MapGap
	MapH = MapRows * MapCellH + (MapRows - 1) * MapGap
	PanelX = -(16 + MapW + 28)
	Children.append(Widget("Border", "MapPanel", CanvasSlot((1, 0), PanelX, 16, MapW + 28, MapH + 62), "HitTestInvisible",
	                       [Text("FloorText", "지하 1층", 22, BoxSlot(HAlign="Center", VAlign="Top"), TextGold, "Center")],
	                       Brush=Brush((0.03, 0.01, 0.05, 0.6), 4, 2, (0.2, 0.12, 0.26, 0.9)), ContentPadding=[14, 8, 14, 8]))
	Left = PanelX + 14
	Top = 16 + 46
	for Y in range(MapRows):
		for X in range(MapCols):
			CX = Left + X * (MapCellW + MapGap)
			CY = Top + (MapRows - 1 - Y) * (MapCellH + MapGap)
			Children.append(Widget("Border", f"Map_{X}_{Y}", CanvasSlot((1, 0), CX, CY, MapCellW, MapCellH, Z=2), "Collapsed",
			                       Brush=Brush((0.4, 0.4, 0.45, 1), 2, 1, (0, 0, 0, 1)), ContentPadding=[0, 0, 0, 0]))
			if X < MapCols - 1:
				Children.append(Widget("Border", f"MapH_{X}_{Y}", CanvasSlot((1, 0), CX + MapCellW, CY + MapCellH // 2 - 2, MapGap, 4, Z=1), "Collapsed",
				                       Brush=Brush((0.7, 0.66, 0.6, 1)), ContentPadding=[0, 0, 0, 0]))
			if Y < MapRows - 1:
				Children.append(Widget("Border", f"MapV_{X}_{Y}", CanvasSlot((1, 0), CX + MapCellW // 2 - 2, CY - MapGap, 4, MapGap, Z=1), "Collapsed",
				                       Brush=Brush((0.7, 0.66, 0.6, 1)), ContentPadding=[0, 0, 0, 0]))

	# ---- 왼쪽 아래: 무기 2칸
	def Slot(I, X):
		return Widget("Overlay", f"WeaponSlot{I}", CanvasSlot((0, 1), X, -16, 96, 96, (0, 1)), "HitTestInvisible", [
			Widget("Border", f"WeaponSlot{I}Bg", BoxSlot(), Brush=Brush((0.05, 0.03, 0.08, 0.8), 4, 3, (0.3, 0.2, 0.36, 1)), ContentPadding=[0, 0, 0, 0]),
			Widget("Image", f"WeaponSlot{I}Icon", BoxSlot((8, 8, 8, 8), "Center", "Center"), Brush=Brush((1, 1, 1, 1), Texture="UI/Crypt/Icons/Sword.png"), ImageSize=[76, 76]),
			Text(f"WeaponSlot{I}Key", str(I + 1), 18, BoxSlot((6, 2, 0, 0), "Left", "Top"), TextGold),
		])
	Children.append(Slot(0, 16))
	Children.append(Slot(1, 120))
	Children.append(Text("WeaponName", "녹슨 검", 22, CanvasSlot((0, 1), 16, -122, 400, 30, (0, 1)), TextLight))
	Children.append(Text("HintText", "A/D 이동  Space 점프(2단)  우클릭/Shift 대시  좌클릭 공격  Q·휠 무기 교체  F 상호작용  S+Space 내려가기  Esc 일시정지",
	                     15, CanvasSlot((0.5, 1), 0, -10, 1100, 22, (0.5, 1)), (0.7, 0.66, 0.74, 1), "Center"))

	# ---- 보스 체력 (아래 가운데)
	Children.append(Widget("VerticalBox", "BossPanel", CanvasSlot((0.5, 1), 0, -46, 620, 64, (0.5, 1)), "Collapsed", [
		Text("BossName", "타락 천사 세라핌", 24, BoxSlot((0, 0, 0, 4), HAlign="Center"), (1.0, 0.7, 0.7, 1), "Center"),
		Widget("ProgressBar", "BossBar", BoxSlot(), MinSize=[620, 20], Brush=Brush((0.08, 0.02, 0.04, 0.9), 2, 2, (0, 0, 0, 1)),
		       FillBrush=Brush((0.9, 0.2, 0.3, 1), 1), Percent=1.0, FillDirection="LeftToRight"),
	]))

	# ---- 가운데: 알림 / 상호작용 안내
	Children.append(Text("Toast", "", 44, CanvasSlot((0.5, 0), 0, 150, 0, 0, (0.5, 0), True), TextGold, "Center", "Collapsed"))
	Children.append(Widget("Border", "Prompt", CanvasSlot((0.5, 1), 0, -150, 0, 0, (0.5, 1), True), "Collapsed",
	                       [Text("PromptText", "F  상자 열기", 24, BoxSlot(HAlign="Center", VAlign="Center"), TextLight, "Center")],
	                       Brush=Brush((0.04, 0.02, 0.06, 0.8), 4, 2, (0.95, 0.78, 0.4, 0.9)), ContentPadding=[16, 6, 16, 6]))

	# ---- 데미지 숫자 템플릿 (복제해 쓴다), 조준점
	Children.append(Text("DmgTemplate", "0", 28, CanvasSlot((0, 0), 0, 0, 0, 0, (0.5, 1), True, -1), (1, 1, 1, 1), "Center", "Collapsed"))
	Children.append(Widget("Image", "Crosshair", CanvasSlot((0, 0), 640, 360, 60, 60, (0.5, 0.5), Z=20), "HitTestInvisible",
	                       Brush=Brush((1, 1, 1, 1), Texture="UI/Crypt/Icons/Crosshair.png"), ImageSize=[60, 60]))

	# ---- 메뉴 화면
	Children.append(Overlay("PauseScreen", "일시정지", TextGold, None,
	                        [("ResumeButton", "계속하기"), ("RestartButton", "다시 시작"), ("TitleButton", "타이틀로"), ("QuitButton", "게임 종료")], 30))
	Children.append(Overlay("DeathScreen", "사망", (0.95, 0.3, 0.3, 1), "DeathResult",
	                        [("DeathRestartButton", "다시 도전 (R)"), ("DeathTitleButton", "타이틀로")], 31))
	Children.append(Overlay("VictoryScreen", "승리!", TextGold, "VictoryResult",
	                        [("VictoryRestartButton", "새 도전 (R)"), ("VictoryTitleButton", "타이틀로")], 32))

	Root = Widget("Canvas", "Root", None, "SelfHitTestInvisible", Children)
	WriteJson(os.path.join(Content, "UI", "Crypt", "HUD.eui"), {"Version": 2, "DesignSize": [1280, 720], "ScaleMode": "MatchHeight",
	                                                            "Root": Root, "Animations": []})


def BuildTitleUi():
	Children = [
		Text("TitleText", "CRYPT 2D", 96, CanvasSlot((0.5, 0), 0, 70, 0, 0, (0.5, 0), True), TextGold, "Center"),
		Text("SubtitleText", "지하 묘지의 수도승", 30, CanvasSlot((0.5, 0), 0, 186, 0, 0, (0.5, 0), True), (0.85, 0.78, 0.95, 1), "Center"),
		Widget("VerticalBox", "MenuBox", CanvasSlot((0.5, 0.5), 0, 60, 0, 0, (0.5, 0), True), "SelfHitTestInvisible", [
			MenuButton("StartButton", "모험 시작 (Enter)", BoxSlot((0, 6, 0, 6), HAlign="Center")),
			MenuButton("QuitButton", "종료", BoxSlot((0, 6, 0, 6), HAlign="Center")),
		]),
		Text("RecordText", "", 22, CanvasSlot((0.5, 1), 0, -64, 0, 0, (0.5, 1), True), (0.85, 0.82, 0.7, 1), "Center"),
		Text("CreditText", "Art: ansimuz — GothicVania Church & Cemetery (CC0)  ·  Sound: Kenney (CC0)", 15,
		     CanvasSlot((0.5, 1), 0, -16, 0, 0, (0.5, 1), True), (0.6, 0.56, 0.66, 1), "Center"),
	]
	Root = Widget("Canvas", "Root", None, "SelfHitTestInvisible", Children)
	WriteJson(os.path.join(Content, "UI", "Crypt", "Title.eui"), {"Version": 2, "DesignSize": [1280, 720], "ScaleMode": "MatchHeight",
	                                                              "Root": Root, "Animations": []})


# ================================================================ 프리팹
def PrefabEntity(Name, Id, Parent, Components, Position=(0, 0, 0), Rotation=None, Scale=(1, 1, 1)):
	Comps = {"PrefabLinkComponent": {"Id": str(Id), "Root": -1},
	         "TransformComponent": {"Position": [float(V) for V in Position], "Rotation": Rotation or [0.0, 0.0, 0.0, 1.0],
	                                "Scale": [float(V) for V in Scale]}}
	Comps.update(Components)
	return {"Name": Name, "Parent": Parent, "Components": Comps}


def Script(Asset, Overrides=None, Location=0):
	return {"ScriptAsset": Asset, "PropertyOverrides": json.dumps(Overrides, ensure_ascii=False) if Overrides else "", "ExecutionLocation": Location}


def Mover(Capsule, Layer, **Fields):
	Value = {"Mode": 0, "CapsuleRadius": Capsule[0] * UPP, "CapsuleHeight": Capsule[1] * UPP, "Layer": Layer, "ClientPrediction": False}
	Value.update(Fields)
	return Value


def SpriteComp(Sprite, Slice, Layer, Order=0, Lit=False, Visible=True, Blend=0, Color=(1, 1, 1, 1)):
	return {"Sprite": Sprite, "Slice": Slice, "Color": list(Color), "FlipX": False, "FlipY": False, "SortingLayer": Layer, "OrderInLayer": Order,
	        "Lit": Lit, "CastShadows": False, "Size": [0.0, 0.0], "Visible": Visible, "Blend": Blend, "AlphaCutoff": 0.5, "SliceMode": 0}


def WritePrefab(Name, Entities):
	WriteJson(os.path.join(Content, "Prefabs", "Crypt", f"{Name}.eprefab"), {"Entities": Entities})
	return f"Prefabs/Crypt/{Name}.eprefab"


def BuildPrefabs():
	Gen = "Sprites/Crypt/Generated.esprite"
	# 플레이어: 이동기(점프 3.5칸 = v²/2g, g = 980 × 3) + 수도승 스프라이트 + 손에 든 무기(자식 — 조준 방향으로 돈다)
	WritePrefab("Player", [
		PrefabEntity("Player", 1, -1, {
			"SpriteComponent": SpriteComp("Sprites/Crypt/Player.esprite", "Idle0", "Characters", 10),
			"FlipbookComponent": {"Flipbook": "Sprites/Crypt/Player_Idle.eflipbook", "Speed": 1.0, "Playing": True, "StartTime": 0.0},
			"CharacterMovement2DComponent": Mover(PlayerCapsule, "Player", MaxSpeed=520.0, GroundAcceleration=7000.0, GroundDeceleration=9000.0,
			                                      AirAcceleration=5200.0, AirDeceleration=3200.0, JumpVelocity=1260.0, MaxJumps=2, GravityScale=3.0,
			                                      MaxFallSpeed=1800.0, CoyoteTime=0.1, JumpBufferTime=0.12, JumpCutFactor=0.45, MaxSlopeAngle=50.0,
			                                      GroundSnapDistance=12.0, DashSpeed=1900.0, DashTime=0.16, DashCooldown=0.12, MaxAirDashes=99,
			                                      DashIgnoresGravity=True, DropThroughTime=0.25),
			"ScriptComponent": Script("Scripts/Crypt/Player.lua"),
		}, (0, 4, 0)),
		PrefabEntity("Weapon", 2, 0, {"SpriteComponent": SpriteComp(Gen, "Sword", "Weapons", 0)}, (0, 2, 8)),
	])

	def Walker(Name, Sprite, Slice, Flipbook, Capsule, **Move):
		Fields = dict(MaxSpeed=300.0, GroundAcceleration=3000.0, GroundDeceleration=4000.0, AirAcceleration=1500.0, AirDeceleration=800.0,
		              JumpVelocity=950.0, MaxJumps=1, GravityScale=3.0, MaxFallSpeed=1800.0, DashSpeed=650.0, DashTime=0.12, DashCooldown=0.05,
		              MaxAirDashes=99, DashIgnoresGravity=False)
		Fields.update(Move)
		WritePrefab(Name, [PrefabEntity(Name, 1, -1, {
			"SpriteComponent": SpriteComp(Sprite, Slice, "Characters", 0),
			"FlipbookComponent": {"Flipbook": Flipbook, "Speed": 1.0, "Playing": True, "StartTime": 0.0},
			"CharacterMovement2DComponent": Mover(Capsule, "Enemy", **Fields),
			"ScriptComponent": Script("Scripts/Crypt/Enemy.lua", {"Kind": Name}),
		})])

	Walker("Skeleton", "Sprites/Crypt/Cemetery.esprite", "SkeletonRise0", "Sprites/Crypt/Skeleton_Rise.eflipbook", SkeletonCapsule,
	       DashSpeed=900.0, DashTime=0.22)
	Walker("Ghoul", "Sprites/Crypt/Ghoul.esprite", "Ghoul0", "Sprites/Crypt/Ghoul_Run.eflipbook", GhoulCapsule,
	       GroundAcceleration=2600.0, GroundDeceleration=1800.0, JumpVelocity=1050.0)
	Walker("Gato", "Sprites/Crypt/Cemetery.esprite", "Gato0", "Sprites/Crypt/Gato_Run.eflipbook", GatoCapsule, JumpVelocity=1150.0,
	       AirAcceleration=3000.0, AirDeceleration=200.0)
	Walker("Wizard", "Sprites/Crypt/Wizard.esprite", "Wizard0", "Sprites/Crypt/Wizard_Idle.eflipbook", WizardCapsule)

	def Flyer(Name, Sprite, Slice, Flipbook, Radius):
		WritePrefab(Name, [PrefabEntity(Name, 1, -1, {
			"SpriteComponent": SpriteComp(Sprite, Slice, "Characters", 2 if Name == "Angel" else 1),
			"FlipbookComponent": {"Flipbook": Flipbook, "Speed": 1.0, "Playing": True, "StartTime": 0.0},
			"RigidBody2DComponent": {"BodyType": 1, "Mass": 0.0, "GravityScale": 0.0, "LinearDamping": 0.0, "AngularDamping": 0.0,
			                         "FixedRotation": True, "Bullet": False, "ReportContacts": False, "Enabled": True},
			"CircleCollider2DComponent": {"Radius": Radius, "Offset": [0.0, 0.0], "Friction": 0.0, "Restitution": 0.0, "Density": 1.0,
			                              "IsTrigger": False, "OneWay": False, "Layer": "Enemy"},
			"ScriptComponent": Script("Scripts/Crypt/Enemy.lua", {"Kind": Name}),
		})])

	Flyer("Ghost", "Sprites/Crypt/Cemetery.esprite", "Ghost0", "Sprites/Crypt/Ghost_Float.eflipbook", 44.0)
	Flyer("Angel", "Sprites/Crypt/Angel.esprite", "Angel3", "Sprites/Crypt/Angel_Idle.eflipbook", 100.0)

	# 범용 2D 조각 (효과·투사체·코인·상자·포털·배경 장식 — GameManager가 만든 직후 콜백에서 모양을 정한다, 스크립트 없음)
	WritePrefab("Sprite2D", [PrefabEntity("Sprite2D", 1, -1, {
		"SpriteComponent": SpriteComp(Gen, "Pixel", "FX", 0, Visible=False),
		"FlipbookComponent": {"Flipbook": "", "Speed": 1.0, "Playing": True, "StartTime": 0.0},
	})])
	# 횃불: 점광원(그림자 없음) + 가산 빛무리 — 뒷벽 타일(받침)은 GameManager가 칠한다
	WritePrefab("Torch", [
		PrefabEntity("Torch", 1, -1, {"PointLightComponent": {"Color": [1.0, 0.62, 0.32], "Intensity": 4.0, "Radius": 520.0, "CastShadows": False}}, (0, 60, 0)),
		PrefabEntity("Glow", 2, 0, {"SpriteComponent": SpriteComp(Gen, "Glow", "BackDecor", 5, Blend=2, Color=(1.0, 0.75, 0.5, 0.55))}, (0, -50, 0)),
	])


# ================================================================ 씬
def AddCamera(Scene, Position=(0, 2000, 0)):
	Scene.Add("Camera2D", {"CameraComponent": {"FovYDegrees": 60.0, "NearZ": 10.0, "FarZ": 20000.0, "Primary": True, "Priority": 0,
	                                          "Orthographic": True, "OrthoHeight": OrthoHeight}}, Position, QuatFromEuler(Yaw=-90.0))
	# 픽셀 아트 모드: 427x240 내부 해상도 + 정수 3배 확대 — 회전하는 무기·호 효과도 같은 도트 크기, 카메라 도트 스냅(서브픽셀 보정)
	Scene.Add("PixelArt", {"PixelArtComponent": {"Enabled": True, "PixelSize": PixelSize, "SnapCamera": True, "OutlineStrength": 0.0,
	                                            "HighlightStrength": 0.0, "DepthThreshold": 25.0, "ColorLevels": 0, "DitherStrength": 0.0,
	                                            "SnapMovingObjects": True}})


def Tilemap(Name, Layer, Order, Lit, Collision, Color, TileData="", Depth=0.0, Position=(0, 0, 0)):
	return (Name, {"TilemapComponent": {"Tileset": "Sprites/Crypt/Crypt.etileset", "CellSize": [CellCm, CellCm], "SortingLayer": Layer,
	                                    "OrderInLayer": Order, "Color": list(Color), "Lit": Lit, "Collision": Collision,
	                                    "CollisionLayer": "Terrain" if Collision else "", "Friction": 0.0, "Restitution": 0.0,
	                                    "TileData": TileData, "Blend": 0, "AlphaCutoff": 0.5, "CastShadows": False}},
	        (Position[0], Depth, Position[2]))


def BuildGameScene(Path, Overrides):
	Scene = FScene()
	AddCamera(Scene)
	Scene.Add("SkyLight", {"SkyLightComponent": {"Intensity": 1.0}})
	Scene.Add("GameManager", {"ScriptComponent": Script("Scripts/Crypt/GameManager.lua", Overrides)})
	Scene.Add(*Tilemap("Terrain", "Tiles", 0, True, True, (1, 1, 1, 1)))
	# 뒷벽: 벽돌(언릿 — 팩 그대로의 어두운 남색) + 장식(창문·아치·횃불 받침, 벽돌 위 — 투명한 부분 뒤에도 벽돌이 보이게 다른 타일맵)
	Scene.Add(*Tilemap("BackWall", "Background", 0, False, False, (0.85, 0.85, 1.0, 1), Depth=-60.0))
	Scene.Add(*Tilemap("BackDecor", "Background", 2, False, False, (0.9, 0.88, 1.0, 1), Depth=-55.0))
	Scene.Add("HUD", {"UIComponent": {"Asset": "UI/Crypt/HUD.eui", "ZOrder": 0, "Visible": True, "ReceiveInput": True, "KeyboardFocus": False},
	                  "ScriptComponent": Script("Scripts/Crypt/Hud.lua")})
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	Scene.Save(Path)


def BuildTitleScene(Path):
	Scene = FScene()
	AddCamera(Scene)
	Scene.Add("SkyLight", {"SkyLightComponent": {"Intensity": 0.5}})
	# 하늘(묘지 팩 달 — 2배 확대) → 산(3장) → 묘지(2장). 스크립트(Title.lua)가 층마다 다른 폭으로 좌우로 흔들어 패럴랙스.
	#   교회 팩 title-screen.png는 "GothicVania Church" 글자가 박혀 있어 쓰지 않는다 (제목은 UI "CRYPT 2D")
	Scene.Add("Sky", {"SpriteComponent": SpriteComp("Sprites/Crypt/Cemetery_Background.esprite", "Background", "Background", 0)}, (0, -30, -1222), None, (2, 1, 2))
	for Index, X in enumerate((-768, 0, 768)):
		Scene.Add(f"Mountains{Index}", {"SpriteComponent": SpriteComp("Sprites/Crypt/Cemetery_Mountains.esprite", "Mountains", "BackDecor", 0)}, (X, -20, -480))
	for Index, X in enumerate((-768, 768)):
		Scene.Add(f"Graveyard{Index}", {"SpriteComponent": SpriteComp("Sprites/Crypt/Cemetery_Graveyard.esprite", "Graveyard", "Props", 0)}, (X, -10, -480))
	Scene.Add("TitleUI", {"UIComponent": {"Asset": "UI/Crypt/Title.eui", "ZOrder": 0, "Visible": True, "ReceiveInput": True, "KeyboardFocus": False},
	                      "ScriptComponent": Script("Scripts/Crypt/Title.lua")})
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	Scene.Save(Path)


def EncodeTilemap(Cells):
	import base64
	import struct
	Chunks = {}
	for (X, Y), Value in Cells.items():
		if Value == 0:
			continue
		Key = (Y >> 5, X >> 5)
		Chunk = Chunks.setdefault(Key, [0] * 1024)
		Chunk[((Y & 31) << 5) | (X & 31)] = Value
	if not Chunks:
		return ""
	Data = bytearray(struct.pack("<BI", 1, len(Chunks)))
	for (ChunkY, ChunkX) in sorted(Chunks.keys()):
		Chunk = Chunks[(ChunkY, ChunkX)]
		Data += struct.pack("<ii", ChunkX, ChunkY)
		Index = 0
		while Index < 1024:
			Run = 1
			while Index + Run < 1024 and Chunk[Index + Run] == Chunk[Index]:
				Run += 1
			Data += struct.pack("<HI", Run, Chunk[Index])
			Index += Run
	return base64.b64encode(bytes(Data)).decode("ascii")


def MakeCell(TileId, FlipX=False, FlipY=False, Rotate90=False):
	return (TileId + 1) | (1 << 29 if FlipX else 0) | (1 << 30 if FlipY else 0) | (1 << 31 if Rotate90 else 0)


def BuildRoomsScene(Path, Templates):
	# 에디터 확인용: 템플릿마다 문 네 개를 뚫은 채 자동 타일한 타일맵 (런타임 Dungeon.lua와 같은 규칙), 4열 격자. 스크립트 없음
	Scene = FScene()
	Scene.Add("Camera2D", {"CameraComponent": {"FovYDegrees": 60.0, "NearZ": 10.0, "FarZ": 50000.0, "Primary": True, "Priority": 0,
	                                          "Orthographic": True, "OrthoHeight": 9000.0}}, (5500, 5000, -3800), QuatFromEuler(Yaw=-90.0))
	Scene.Add("SkyLight", {"SkyLightComponent": {"Intensity": 1.0}})
	for Index, T in enumerate(Templates):
		Tiles = Rooms.TemplateTiles(T)
		Cells = {Key: MakeCell(Id, FX, FY, R) for Key, (Id, FX, FY, R) in Tiles.items()}
		GX, GY = Index % 4, Index // 4
		Origin = (GX * (Rooms.RoomWidth + 4) * CellCm, 0, -GY * (Rooms.RoomHeight + 4) * CellCm)
		Scene.Add(*Tilemap(f"Room_{T.Name}", "Tiles", 0, False, True, (1, 1, 1, 1), EncodeTilemap(Cells), 0.0, Origin))
	os.makedirs(os.path.dirname(Path), exist_ok=True)
	Scene.Save(Path)


def Main():
	Templates = Rooms.BuildTemplates()
	BuildSprites()
	BuildAudio()
	BuildData()
	BuildRoomsLua(Templates)
	BuildConfig()
	BuildHud()
	BuildTitleUi()
	BuildPrefabs()
	Scenes = os.path.join(Content, "Scenes")
	BuildTitleScene(os.path.join(Scenes, "Title.escene"))
	BuildGameScene(os.path.join(Scenes, "Crypt.escene"), None)
	BuildGameScene(os.path.join(Scenes, "Tests", "CryptAutoPlay.escene"), {"AutoPlay": "Test", "Seed": 7})
	BuildGameScene(os.path.join(Scenes, "Tests", "CryptBoss.escene"), {"AutoPlay": "Boss", "Seed": 3, "StartFloor": 3})
	BuildGameScene(os.path.join(Scenes, "Tests", "CryptFloor.escene"), {"AutoPlay": "Explore", "Seed": 11})
	BuildRoomsScene(os.path.join(Scenes, "Tests", "CryptRooms.escene"), Templates)
	print(f"완료: 템플릿 {len(Templates)}개 → {Content}")


if __name__ == "__main__":
	Main()
