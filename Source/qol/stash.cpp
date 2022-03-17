#include "qol/stash.h"

#include "utils/stdcompat/algorithm.hpp"
#include <fmt/format.h>
#include <utility>

#include "DiabloUI/art_draw.h"
#include "control.h"
#include "controls/plrctrls.h"
#include "cursor.h"
#include "engine/points_in_rectangle_range.hpp"
#include "engine/rectangle.hpp"
#include "engine/render/cel_render.hpp"
#include "engine/render/text_render.hpp"
#include "engine/size.hpp"
#include "hwcursor.hpp"
#include "minitext.h"
#include "stores.h"
#include "utils/language.h"
#include "utils/utf8.hpp"

namespace devilution {

bool IsStashOpen;
StashStruct Stash;
bool IsWithdrawGoldOpen;
int WithdrawGoldValue;

namespace {

int InitialWithdrawGoldValue;

/** Contains mappings for the buttons in the stash (2 navigation buttons, withdraw gold buttons, 2 navigation buttons) */
constexpr Rectangle StashButtonRect[] = {
	// clang-format off
	{ {  19, 19 }, { 27, 16 } }, // 10 left
	{ {  56, 19 }, { 27, 16 } }, // 1 left
	{ {  93, 19 }, { 27, 16 } }, // withdraw gold
	{ { 242, 19 }, { 27, 16 } }, // 1 right
	{ { 279, 19 }, { 27, 16 } }  // 10 right
	// clang-format on
};

Art StashPanelArt;
Art StashNavButtonArt;

/**
 * @param stashListIndex The item's StashList index
 * @param itemSize Size of item
 */
void AddItemToStashGrid(Point position, uint16_t stashListIndex, Size itemSize)
{
	for (auto point : PointsInRectangleRange({ { 0, 0 }, itemSize })) {
		Stash.stashGrids[Stash.page][position.x + point.x][position.y + point.y] = stashListIndex + 1;
	}
}

Point FindSlotUnderCursor(Point cursorPosition)
{
	if ((icursSize28.width & 1) == 0)
		cursorPosition.x -= INV_SLOT_HALF_SIZE_PX;
	if ((icursSize28.height & 1) == 0)
		cursorPosition.y -= INV_SLOT_HALF_SIZE_PX;
	cursorPosition.y -= (icursSize28.height - 1) / 2 * INV_SLOT_SIZE_PX;

	for (auto point : PointsInRectangleRange({ { 0, 0 }, { 10, 10 } })) {
		Rectangle cell {
			GetStashSlotCoord(point),
			InventorySlotSizeInPixels
		};

		if (cell.Contains(cursorPosition)) {
			return point;
		}
	}

	return InvalidStashPoint;
}

void CheckStashPaste(Point cursorPosition)
{
	auto &player = Players[MyPlayerId];

	SetICursor(player.HoldItem._iCurs + CURSOR_FIRSTITEM);
	if (!IsHardwareCursor()) {
		cursorPosition += Displacement(icursSize / 2);
	}

	if (player.HoldItem._itype == ItemType::Gold) {
		Stash.gold += player.HoldItem._ivalue;
		Stash.dirty = true;
		player.HoldItem._itype = ItemType::None;
		if (!IsHardwareCursor())
			SetCursorPos(cursorPosition);
		NewCursor(CURSOR_HAND);
		return;
	}

	Point firstSlot = FindSlotUnderCursor(cursorPosition);
	if (firstSlot == InvalidStashPoint)
		return;

	const Size itemSize = icursSize28;

	if (firstSlot.x + itemSize.width > 10 || firstSlot.y + itemSize.height > 10) {
		return; // Item does not fit
	}

	// Check that no more then 1 item is replaced by the move
	uint16_t it = 0;
	for (auto point : PointsInRectangleRange({ { 0, 0 }, itemSize })) {
		uint16_t iv = Stash.stashGrids[Stash.page][firstSlot.x + point.x][firstSlot.y + point.y];
		if (iv == 0 || it == iv)
			continue;
		if (it == 0) {
			it = iv; // Found first item
			continue;
		}
		return; // Found a second item
	}

	PlaySFX(ItemInvSnds[ItemCAnimTbl[player.HoldItem._iCurs]]);

	player.HoldItem.position = firstSlot + Displacement { 0, itemSize.height - 1 };

	int cn = CURSOR_HAND;
	uint16_t stashIndex;
	if (it == 0) {
		Stash.stashList.push_back(player.HoldItem);
		stashIndex = Stash.stashList.size() - 1;
	} else {
		stashIndex = it - 1;
		cn = SwapItem(Stash.stashList[stashIndex], player.HoldItem);
		for (auto &row : Stash.stashGrids[Stash.page]) {
			for (auto &itemId : row) {
				if (itemId == it)
					itemId = 0;
			}
		}
	}

	AddItemToStashGrid(firstSlot, stashIndex, itemSize);

	Stash.dirty = true;

	if (cn == CURSOR_HAND && !IsHardwareCursor())
		SetCursorPos(cursorPosition);
	NewCursor(cn);
}

void CheckStashCut(Point cursorPosition, bool automaticMove, bool dropItem)
{
	auto &player = Players[MyPlayerId];

	if (IsWithdrawGoldOpen) {
		IsWithdrawGoldOpen = false;
		WithdrawGoldValue = 0;
	}

	Point slot = InvalidStashPoint;

	for (auto point : PointsInRectangleRange({ { 0, 0 }, { 10, 10 } })) {
		Rectangle cell {
			GetStashSlotCoord(point),
			{ InventorySlotSizeInPixels.width + 1, InventorySlotSizeInPixels.height + 1 }
		};

		// check which inventory rectangle the mouse is in, if any
		if (cell.Contains(cursorPosition)) {
			slot = point;
			break;
		}
	}

	if (slot == InvalidStashPoint) {
		return;
	}

	Item &holdItem = player.HoldItem;
	holdItem._itype = ItemType::None;

	bool automaticallyMoved = false;
	bool automaticallyEquipped = false;

	uint16_t ii = Stash.stashGrids[Stash.page][slot.x][slot.y];
	if (ii != 0) {
		uint16_t iv = ii - 1;

		holdItem = Stash.stashList[iv];
		if (automaticMove) {
			if (CanBePlacedOnBelt(holdItem)) {
				automaticallyMoved = AutoPlaceItemInBelt(player, holdItem, true);
			} else {
				automaticallyMoved = automaticallyEquipped = AutoEquip(MyPlayerId, holdItem);
			}
		}

		if (!automaticMove || automaticallyMoved) {
			Stash.RemoveStashItem(iv);
		}
	}

	if (!holdItem.isEmpty()) {
		CalcPlrInv(player, true);
		CheckItemStats(player);
		if (automaticallyEquipped) {
			PlaySFX(ItemInvSnds[ItemCAnimTbl[holdItem._iCurs]]);
		} else if (!automaticMove || automaticallyMoved) {
			PlaySFX(IS_IGRAB);
		}

		if (automaticMove) {
			if (!automaticallyMoved) {
				if (CanBePlacedOnBelt(holdItem)) {
					player.SaySpecific(HeroSpeech::IHaveNoRoom);
				} else {
					player.SaySpecific(HeroSpeech::ICantDoThat);
				}
			}

			holdItem._itype = ItemType::None;
		} else {
			NewCursor(holdItem._iCurs + CURSOR_FIRSTITEM);
			if (!IsHardwareCursor() && !dropItem) {
				// For a hardware cursor, we set the "hot point" to the center of the item instead.
				SetCursorPos(cursorPosition - Displacement(cursSize / 2));
			}
		}
	}

	if (dropItem) {
		TryDropItem();
	}
}

void StartGoldWithdraw()
{
	InitialWithdrawGoldValue = Stash.gold;

	if (talkflag)
		control_reset_talk();

	Point start = GetPanelPosition(UiPanels::Stash, { 67, 128 });
	SDL_Rect rect = MakeSdlRect(start.x, start.y, 180, 20);
	SDL_SetTextInputRect(&rect);

	IsWithdrawGoldOpen = true;
	WithdrawGoldValue = 0;
	SDL_StartTextInput();
}

void WithdrawGold(Player &player, int amount)
{
	InitializeItem(player.HoldItem, IDI_GOLD);
	SetGoldSeed(player, player.HoldItem);
	player.HoldItem._ivalue = amount;
	player.HoldItem._iStatFlag = true;
	ControlSetGoldCurs(player);
	Stash.gold -= amount;
	Stash.dirty = true;
}

} // namespace

Point GetStashSlotCoord(Point slot)
{
	constexpr int StashNextCell = INV_SLOT_SIZE_PX + 1; // spacing between each cell

	return GetPanelPosition(UiPanels::Stash, slot * StashNextCell + Displacement { 17, 48 });
}

void FreeStashGFX()
{
	StashPanelArt.Unload();
	StashNavButtonArt.Unload();
}

void InitStash()
{
	InitialWithdrawGoldValue = 0;

	LoadArt("data\\stash.pcx", &StashPanelArt, 1);
	LoadArt("data\\stashnavbtns.pcx", &StashNavButtonArt, 5);
}

int StashButtonPressed = -1;

void CheckStashButtonRelease(Point mousePosition)
{
	if (StashButtonPressed == -1)
		return;

	Rectangle stashButton = StashButtonRect[StashButtonPressed];
	stashButton.position = GetPanelPosition(UiPanels::Stash, stashButton.position);
	if (stashButton.Contains(mousePosition)) {
		switch (StashButtonPressed) {
		case 0:
			Stash.page -= 10;
			break;
		case 1:
			Stash.page -= 1;
			break;
		case 2:
			StartGoldWithdraw();
			break;
		case 3:
			Stash.page += 1;
			break;
		case 4:
			Stash.page += 10;
			break;
		}
	}

	StashButtonPressed = -1;
	Stash.page = clamp(Stash.page, 0, 49);
}

void CheckStashButtonPress(Point mousePosition)
{
	Rectangle stashButton;

	for (int i = 0; i < 5; i++) {
		stashButton = StashButtonRect[i];
		stashButton.position = GetPanelPosition(UiPanels::Stash, stashButton.position);
		if (stashButton.Contains(mousePosition)) {
			StashButtonPressed = i;
			return;
		}
	}

	StashButtonPressed = -1;
}

void DrawStash(const Surface &out)
{
	DrawArt(out, GetPanelPosition(UiPanels::Stash), &StashPanelArt);

	if (StashButtonPressed != -1) {
		Point stashButton = GetPanelPosition(UiPanels::Stash, StashButtonRect[StashButtonPressed].position);
		DrawArt(out, stashButton, &StashNavButtonArt, StashButtonPressed);
	}

	constexpr Displacement offset { 0, INV_SLOT_SIZE_PX - 1 };

	for (auto slot : PointsInRectangleRange({ { 0, 0 }, { 10, 10 } })) {
		if (Stash.stashGrids[Stash.page][slot.x][slot.y] != 0) {
			InvDrawSlotBack(out, GetStashSlotCoord(slot) + offset, InventorySlotSizeInPixels);
		}
	}

	for (auto slot : PointsInRectangleRange({ { 0, 0 }, { 10, 10 } })) {
		if (Stash.stashGrids[Stash.page][slot.x][slot.y] == 0) {
			continue; // No item in the given slot
		}

		uint16_t itemId = Stash.stashGrids[Stash.page][slot.x][slot.y] - 1;
		if (Stash.stashList[itemId].position != slot) {
			continue; // Not the first slot of the item
		}

		Item &item = Stash.stashList[itemId];
		int frame = item._iCurs + CURSOR_FIRSTITEM;

		const Point position = GetStashSlotCoord(item.position) + offset;
		const auto &cel = GetInvItemSprite(frame);
		const int celFrame = GetInvItemFrame(frame);

		if (pcursstashitem == itemId) {
			uint8_t color = GetOutlineColor(Stash.stashList[itemId], true);
			CelBlitOutlineTo(out, color, position, cel, celFrame, false);
		}

		CelDrawItem(Stash.stashList[itemId], out, position, cel, celFrame);
	}

	Point position = GetPanelPosition(UiPanels::Stash);
	UiFlags style = UiFlags::VerticalCenter | UiFlags::ColorWhite;

	DrawString(out, fmt::format("{:d}", Stash.page + 1), { position + Displacement { 132, 0 }, { 57, 11 } }, UiFlags::AlignCenter | style);
	DrawString(out, fmt::format("{:d}", Stash.gold), { position + Displacement { 122, 19 }, { 107, 13 } }, UiFlags::AlignRight | style);
}

void CheckStashItem(Point mousePosition, bool isShiftHeld, bool isCtrlHeld)
{
	if (pcurs >= CURSOR_FIRSTITEM) {
		CheckStashPaste(mousePosition);
	} else {
		CheckStashCut(mousePosition, isShiftHeld, isCtrlHeld);
	}
}

uint16_t CheckStashHLight(Point mousePosition)
{
	Point slot = InvalidStashPoint;
	for (auto point : PointsInRectangleRange({ { 0, 0 }, { 10, 10 } })) {
		Rectangle cell {
			GetStashSlotCoord(point),
			{ InventorySlotSizeInPixels.width + 1, InventorySlotSizeInPixels.height + 1 }
		};

		if (cell.Contains({ mousePosition })) {
			slot = point;
			break;
		}
	}

	if (slot == InvalidStashPoint)
		return -1;

	InfoColor = UiFlags::ColorWhite;

	ClearPanel();

	uint16_t itemId = abs(Stash.stashGrids[Stash.page][slot.x][slot.y]);
	if (itemId == 0)
		return -1;

	uint16_t ii = itemId - 1;
	Item &item = Stash.stashList[ii];

	if (item.isEmpty())
		return -1;

	InfoColor = item.getTextColor();
	if (item._iIdentified) {
		strcpy(infostr, item._iIName);
		PrintItemDetails(item);
	} else {
		strcpy(infostr, item._iName);
		PrintItemDur(item);
	}

	return ii;
}

bool UseStashItem(uint16_t c)
{
	if (MyPlayer->_pInvincible && MyPlayer->_pHitPoints == 0)
		return true;
	if (pcurs != CURSOR_HAND)
		return true;
	if (stextflag != STORE_NONE)
		return true;

	Item *item = &Stash.stashList[c];

	constexpr int SpeechDelay = 10;
	if (item->IDidx == IDI_MUSHROOM) {
		MyPlayer->Say(HeroSpeech::NowThatsOneBigMushroom, SpeechDelay);
		return true;
	}
	if (item->IDidx == IDI_FUNGALTM) {
		PlaySFX(IS_IBOOK);
		MyPlayer->Say(HeroSpeech::ThatDidntDoAnything, SpeechDelay);
		return true;
	}

	if (!AllItemsList[item->IDidx].iUsable)
		return false;

	if (!MyPlayer->CanUseItem(*item)) {
		MyPlayer->Say(HeroSpeech::ICantUseThisYet);
		return true;
	}

	if (IsWithdrawGoldOpen) {
		IsWithdrawGoldOpen = false;
		WithdrawGoldValue = 0;
	}

	if (item->IsScroll() && currlevel == 0 && !spelldata[item->_iSpell].sTownSpell) {
		return true;
	}

	if (item->_iMiscId > IMISC_RUNEFIRST && item->_iMiscId < IMISC_RUNELAST && currlevel == 0) {
		return true;
	}

	if (item->_iMiscId == IMISC_BOOK)
		PlaySFX(IS_RBOOK);
	else
		PlaySFX(ItemInvSnds[ItemCAnimTbl[item->_iCurs]]);

	UseItem(MyPlayerId, item->_iMiscId, item->_iSpell);

	if (Stash.stashList[c]._iMiscId == IMISC_MAPOFDOOM)
		return true;
	if (Stash.stashList[c]._iMiscId == IMISC_NOTE) {
		InitQTextMsg(TEXT_BOOK9);
		CloseInventory();
		return true;
	}
	Stash.RemoveStashItem(c);

	return true;
}

void StashStruct::RemoveStashItem(uint16_t iv)
{
	// Iterate through stashGrid and remove every reference to item
	for (auto &row : Stash.stashGrids[Stash.page]) {
		for (uint16_t &itemId : row) {
			if (itemId - 1 == iv) {
				itemId = 0;
			}
		}
	}

	if (stashList.empty()) {
		return;
	}

	// If the item at the end of stash array isn't the one we removed, we need to swap its position in the array with the removed item
	std::size_t lastItemIndex = stashList.size() - 1;
	if (lastItemIndex != iv) {
		stashList[iv] = stashList[lastItemIndex];

		for (auto &pair : Stash.stashGrids) {
			auto &grid = pair.second;
			for (auto &row : grid) {
				for (uint16_t &itemId : row) {
					if (itemId == lastItemIndex + 1) {
						itemId = iv + 1;
					}
				}
			}
		}
	}
	stashList.pop_back();
	Stash.dirty = true;
}

void WithdrawGoldKeyPress(char vkey)
{
	auto &myPlayer = Players[MyPlayerId];

	if (myPlayer._pHitPoints >> 6 <= 0) {
		CloseGoldWithdraw();
		return;
	}

	if (vkey == DVL_VK_RETURN) {
		if (WithdrawGoldValue > 0) {
			WithdrawGold(myPlayer, WithdrawGoldValue);
		}
		CloseGoldWithdraw();
	} else if (vkey == DVL_VK_ESCAPE) {
		CloseGoldWithdraw();
	} else if (vkey == DVL_VK_BACK) {
		WithdrawGoldValue /= 10;
	}
}

void DrawGoldWithdraw(const Surface &out, int amount)
{
	if (!IsWithdrawGoldOpen) {
		return;
	}

	const int dialogX = 30;

	CelDrawTo(out, GetPanelPosition(UiPanels::Stash, { dialogX, 178 }), *pGBoxBuff, 1);

	constexpr auto BufferSize = sizeof(tempstr) / sizeof(*tempstr);

	CopyUtf8(tempstr, _("How many gold pieces do you want to withdraw? (MAX 5000)"), BufferSize);

	// Pre-wrap the string at spaces, otherwise DrawString would hard wrap in the middle of words
	const std::string wrapped = WordWrapString(tempstr, 200);

	// The split gold dialog is roughly 4 lines high, but we need at least one line for the player to input an amount.
	// Using a clipping region 50 units high (approx 3 lines with a lineheight of 17) to ensure there is enough room left
	//  for the text entered by the player.
	DrawString(out, wrapped, { GetPanelPosition(UiPanels::Stash, { dialogX + 31, 75 }), { 200, 50 } }, UiFlags::ColorWhitegold | UiFlags::AlignCenter, 1, 17);

	tempstr[0] = '\0';
	if (amount > 0) {
		// snprintf ensures that the destination buffer ends in a null character.
		snprintf(tempstr, BufferSize, "%u", amount);
	}
	// Even a ten digit amount of gold only takes up about half a line. There's no need to wrap or clip text here so we
	// use the Point form of DrawString.
	DrawString(out, tempstr, GetPanelPosition(UiPanels::Stash, { dialogX + 37, 128 }), UiFlags::ColorWhite | UiFlags::PentaCursor);
}

void CloseGoldWithdraw()
{
	if (!IsWithdrawGoldOpen)
		return;
	IsWithdrawGoldOpen = false;
	WithdrawGoldValue = 0;
	SDL_StopTextInput();
}

void GoldWithdrawNewText(string_view text)
{
	for (char vkey : text) {
		int digit = vkey - '0';
		if (digit >= 0 && digit <= 9) {
			int newGoldValue = WithdrawGoldValue * 10;
			newGoldValue += digit;
			if (newGoldValue <= GOLD_MAX_LIMIT && newGoldValue <= InitialWithdrawGoldValue) {
				WithdrawGoldValue = newGoldValue;
			}
		}
	}
}

} // namespace devilution