/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "gui/combos/command_search.h"
#include "definitions_cxx.hpp"
#include "extern.h"
#include "gui/combos/tables.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/horizontal_menu.h"
#include "gui/menu_item/menu_item.h"
#include "gui/ui/keyboard/keyboard_screen.h"
#include "gui/ui/menus.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/arranger_view.h"
#include "gui/views/audio_clip_view.h"
#include "gui/views/automation_view.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/session_view.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "model/clip/audio_clip.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound.h"
#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>

using deluge::gui::combos::Gesture;
using deluge::gui::combos::Gestures;
using deluge::gui::combos::Held;
using deluge::gui::combos::kNoButton;
using deluge::gui::combos::Shortcut;
using deluge::hid::display::oled_canvas::Canvas;
namespace button = deluge::hid::button;

PLACE_SDRAM_BSS CommandSearchUI commandSearchUI{};

namespace {

// ---- Matching ----

char lower(char c) {
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool startsWithAt(std::string_view text, size_t pos, std::string_view token) {
	if (pos + token.size() > text.size()) {
		return false;
	}
	for (size_t i = 0; i < token.size(); i++) {
		if (lower(text[pos + i]) != lower(token[i])) {
			return false;
		}
	}
	return true;
}

bool isWordStart(std::string_view text, size_t pos) {
	return pos == 0 || text[pos - 1] == ' ' || text[pos - 1] == ',' || text[pos - 1] == '-';
}

/// How well one typed word matches: 0 best ... 3 worst, -1 no match
int32_t tokenScore(std::string_view name, std::string_view keywords, std::string_view token) {
	if (startsWithAt(name, 0, token)) {
		return 0;
	}
	bool inName = false;
	for (size_t pos = 1; pos < name.size(); pos++) {
		if (startsWithAt(name, pos, token)) {
			if (isWordStart(name, pos)) {
				return 1;
			}
			inName = true;
		}
	}
	for (size_t pos = 0; pos < keywords.size(); pos++) {
		if (isWordStart(keywords, pos) && startsWithAt(keywords, pos, token)) {
			return 2;
		}
	}
	return inName ? 3 : -1;
}

/// Every typed word has to match somewhere; the total says how well. -1: no match.
int32_t matchScore(std::string_view name, std::string_view keywords, std::string_view query) {
	int32_t total = 0;
	size_t start = 0;
	while (start < query.size()) {
		size_t end = query.find(' ', start);
		if (end == std::string_view::npos) {
			end = query.size();
		}
		if (end > start) {
			int32_t score = tokenScore(name, keywords, query.substr(start, end - start));
			if (score < 0) {
				return -1;
			}
			total += score;
		}
		start = end + 1;
	}
	return total;
}

// What the view command search was opened from has: its combos, its gestures, and whether Shift + pad opens params
struct Sources {
	std::array<std::span<const Shortcut>, 3> shortcuts{};
	std::array<Gestures, 6> gestures{};
	bool params = false;
};

Sources sourcesHere() {
	namespace c = deluge::gui::combos;
	RootUI* root = getRootUI();
	Sources sources;
	sources.shortcuts[0] = View::shortcuts();
	sources.gestures[0] = c::globalGestures();
	sources.gestures[1] = c::viewGestures();
	if (root == &instrumentClipView) {
		sources.shortcuts[1] = InstrumentClipView::shortcuts();
		sources.shortcuts[2] = InstrumentClipMinder::shortcuts();
		sources.gestures[2] = c::timelineGestures();
		sources.gestures[3] = c::clipGestures();
		sources.gestures[4] = c::instrumentClipGestures();
		sources.params = true;
	}
	else if (root == &automationView) {
		if (getCurrentClip() != nullptr && getCurrentClip()->type == ClipType::INSTRUMENT) {
			sources.shortcuts[1] = InstrumentClipMinder::shortcuts();
		}
		sources.gestures[2] = c::timelineGestures();
		sources.gestures[3] = c::automationGestures();
	}
	else if (root == &keyboardScreen) {
		sources.shortcuts[1] = InstrumentClipMinder::shortcuts();
		sources.gestures[2] = c::keyboardGestures();
		sources.params = true;
	}
	else if (root == &audioClipView) {
		sources.gestures[2] = c::timelineGestures();
		sources.gestures[3] = c::clipGestures();
		sources.gestures[4] = c::audioClipGestures();
		sources.params = true;
	}
	else if (root == &sessionView) {
		sources.gestures[2] = c::timelineGestures();
		sources.gestures[3] = c::sessionGestures();
	}
	else if (root == &arrangerView) {
		sources.gestures[2] = c::timelineGestures();
		sources.gestures[3] = c::arrangerGestures();
	}
	return sources;
}

/// Case-insensitive, for listing alphabetically
int32_t compareNames(std::string_view a, std::string_view b) {
	for (size_t i = 0; i < a.size() && i < b.size(); i++) {
		if (lower(a[i]) != lower(b[i])) {
			return lower(a[i]) < lower(b[i]) ? -1 : 1;
		}
	}
	return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

using MenuChain = std::span<deluge::gui::menu_item::HorizontalMenu* const>;

/// The horizontal menus params are grouped in for what's being edited
MenuChain paramMenus(bool audio, bool affectEntire) {
	return audio                                ? MenuChain{horizontalMenusChainForAudioClip}
	       : affectEntire                       ? MenuChain{horizontalMenusChainForKit}
	       : SoundEditor::editingCVOrMIDIClip() ? MenuChain{horizontalMenusChainForMidiOrCv}
	                                            : MenuChain{horizontalMenusChainForSound};
}

/// The menu a param shortcut's item sits in ("Reverb" for "Time")
std::string_view parentTitle(MenuItem* item, MenuChain chain) {
	for (deluge::gui::menu_item::HorizontalMenu* menu : chain) {
		if (deluge::gui::menu_item::HorizontalMenu* found = menu->menuWithItem(item)) {
			return found->getTitle();
		}
	}
	return {};
}

/// Some titles are only filled in once their menu has been open ("Samp1 mode"): the plain name does until then
std::string_view paramName(MenuItem* item) {
	std::string_view title = item->getTitle();
	return title.empty() ? item->getName() : title;
}

MenuItem* const kComingSoon = reinterpret_cast<MenuItem*>(0xFFFFFFFF); // menus.cpp's comingSoonMenu

bool isAvailable(Shortcut const& shortcut) {
	return shortcut.available == nullptr || shortcut.available();
}

bool applies(Gesture const& gesture) {
	return gesture.applies == nullptr || gesture.applies();
}

deluge::hid::Button heldButton(Held held) {
	switch (held) {
	case Held::SAVE:
		return button::SAVE;
	case Held::LOAD:
		return button::LOAD;
	case Held::SHIFT:
		return button::SHIFT;
	case Held::LEARN:
	default:
		return button::LEARN;
	}
}

// ---- Drawing ----

constexpr int32_t kTop = OLED_MAIN_TOPMOST_PIXEL;
constexpr int32_t kRight = OLED_MAIN_WIDTH_PIXELS - 1;
constexpr int32_t kNameY = kTop + 13;
constexpr int32_t kComboCentreY = kTop + 32;
constexpr int32_t kArrowX = kRight - 4; // Centre of the ▲▼ next to the name
constexpr int32_t kNameAreaWidth = kArrowX - 5;
// Buttons are shown as key caps with what's printed on the panel, encoders with what to do to them ("PUSH GOLD KNOB").
// Words beat icons here: a drawn knob doesn't say which one. Too wide for the screen: the small font.
struct KeyStyle {
	int32_t textWidth;
	int32_t textHeight;
	int32_t padding;
	int32_t gap; // Either side of a "+"
};
constexpr KeyStyle kNormalKeys{kTextSpacingX, kTextSpacingY, 3, 3};
constexpr KeyStyle kSmallKeys{kTextSmallSpacingX, kTextSmallSizeY, 2, 1};

int32_t textWidth(Canvas& canvas, std::string_view text, KeyStyle const& style) {
	char buffer[24];
	buffer[text.copy(buffer, sizeof(buffer) - 1)] = 0;
	return canvas.getStringWidthInPixels(buffer, style.textHeight);
}

int32_t partWidth(Canvas& canvas, std::string_view label, KeyStyle const& style) {
	return textWidth(canvas, label, style) + 2 * style.padding;
}

void drawTriangle(Canvas& canvas, int32_t tipX, int32_t tipY, int32_t dx, int32_t dy, int32_t size = 3) {
	// A triangle `size` rows (or columns) deep, pointing in (dx, dy), with its tip at (tipX, tipY)
	for (int32_t i = 0; i < size; i++) {
		for (int32_t j = -i; j <= i; j++) {
			if (dx != 0) {
				canvas.drawPixel(tipX - dx * i, tipY + j);
			}
			else {
				canvas.drawPixel(tipX + j, tipY - dy * i);
			}
		}
	}
}

int32_t drawPart(Canvas& canvas, std::string_view label, int32_t x, int32_t centreY, KeyStyle const& style) {
	int32_t width = partWidth(canvas, label, style);
	int32_t halfHeight = style.textHeight / 2 + 2;
	canvas.drawRectangleRounded(x, centreY - halfHeight, x + width - 1, centreY + halfHeight);
	canvas.drawString(label, x + style.padding, centreY - style.textHeight / 2, style.textWidth, style.textHeight);
	return width;
}

// A result's combo as key cap labels: what to hold, then what to press or turn. Params end with their pad.
struct ComboLabels {
	std::array<std::string_view, 4> parts;
	size_t numParts = 0;
	char inputBuffer[24];
	void add(std::string_view label) { parts[numParts++] = label; }
};

// Fills in `labels`, which may point into its own buffer - so it isn't returned by value
// With `drawingPads`, a gesture on one particular pad leaves it out: it's drawn on a grid after the labels
void labelsOf(ComboLabels& labels, Shortcut const* combo, Gesture const* gesture, bool param, bool drawingPads) {
	using deluge::gui::combos::buttonName;
	if (param) {
		labels.add(buttonName(button::SHIFT));
	}
	else if (combo != nullptr) {
		labels.add(buttonName(heldButton(combo->held)));
		if (combo->with != kNoButton) {
			labels.add(buttonName(combo->with));
		}
		labels.add(buttonName(combo->trigger));
	}
	else {
		if (gesture->hold != kNoButton) {
			labels.add(buttonName(gesture->hold));
		}
		if (gesture->hold2 != kNoButton) {
			labels.add(buttonName(gesture->hold2));
		}
		if (gesture->padX < 0 || !drawingPads) {
			labels.add(deluge::gui::combos::inputName(gesture->input, gesture->action, labels.inputBuffer));
		}
	}
}

// The main pads, with the one to press marked: rows go up from the bottom like on the Deluge
constexpr int32_t kPadPitch = 2;
constexpr int32_t kPadGridWidth = kDisplayWidth * kPadPitch + 3;
constexpr int32_t kPadGridHeight = kDisplayHeight * kPadPitch + 3;

void drawPadGrid(Canvas& canvas, int32_t x, int32_t centreY, int32_t padX, int32_t padY) {
	int32_t top = centreY - kPadGridHeight / 2;
	canvas.drawRectangleRounded(x, top, x + kPadGridWidth - 1, top + kPadGridHeight - 1);
	auto padPixel = [&](int32_t px, int32_t py) {
		return std::pair{x + 2 + px * kPadPitch, top + 2 + (kDisplayHeight - 1 - py) * kPadPitch};
	};
	for (int32_t py = 0; py < kDisplayHeight; py++) {
		for (int32_t px = 0; px < kDisplayWidth; px++) {
			if (std::abs(px - padX) > 1 || std::abs(py - padY) > 1) {
				auto [dotX, dotY] = padPixel(px, py);
				canvas.drawPixel(dotX, dotY);
			}
		}
	}
	auto [centreX, centreYPad] = padPixel(padX, padY);
	canvas.drawRectangle(centreX - 1, centreYPad - 1, centreX + 1, centreYPad + 1);
	canvas.drawPixel(centreX, centreYPad);
}

} // namespace

CommandSearchUI::CommandSearchUI() : QwertyUI() {
	title = "Search";
	oledShowsUIUnderneath = false;
}

bool CommandSearchUI::canOpen() {
	// Not the performance view: shift + keyboard is its editing mode
	RootUI* root = getRootUI();
	return display->haveOLED() && getCurrentUI() == root
	       && (root == &instrumentClipView || root == &automationView || root == &keyboardScreen
	           || root == &audioClipView || root == &sessionView || root == &arrangerView);
}

bool CommandSearchUI::opened() {
	if (!QwertyUI::opened()) {
		return false;
	}
	scrollPosHorizontal = 0;
	collectCandidates();
	search();
	drawKeys();
	renderUIsForOled();
	return true;
}

void CommandSearchUI::addCandidate(Result const& result, std::string_view name, std::string_view keywords) {
	if (numCandidates_ == kMaxResults) {
		return;
	}
	for (size_t i = 0; i < numCandidates_; i++) {
		Candidate const& other = candidates_[i];
		bool sameParam = result.param != nullptr && other.result.param == result.param;
		bool sameCombo = result.combo != nullptr && other.result.combo != nullptr && name == other.name;
		bool sameGesture = result.gesture != nullptr && other.result.gesture == result.gesture;
		if (sameParam || sameCombo || sameGesture) {
			return; // The same action from another pad or button (e.g. either gold knob)
		}
	}
	if (name.empty()) {
		return; // Nothing to find it by
	}
	Candidate& candidate = candidates_[numCandidates_++];
	candidate.result = result;
	candidate.name[name.copy(candidate.name, sizeof(candidate.name) - 1)] = 0;
	candidate.keywords = keywords;
}

void CommandSearchUI::collectCandidates() {
	numCandidates_ = 0;
	Sources sources = sourcesHere();

	for (std::span<const Shortcut> table : sources.shortcuts) {
		for (Shortcut const& shortcut : table) {
			if (isAvailable(shortcut)) {
				addCandidate({.combo = &shortcut}, deluge::l10n::getView(shortcut.name),
				             shortcut.keywords ? shortcut.keywords : "");
			}
		}
	}
	for (Gestures table : sources.gestures) {
		for (Gesture const* gesture : table) {
			if (applies(*gesture)) {
				addCandidate({.gesture = gesture}, deluge::l10n::getView(gesture->name),
				             gesture->keywords ? gesture->keywords : "");
			}
		}
	}
	if (sources.params) {
		AudioEngine::routineWithClusterLoading(); // Main loop: let audio render between the lists
		collectParams();
	}
}

void CommandSearchUI::collectParams() {
	// Shift + pad shortcuts, as the Shift overlay shows them for this clip
	size_t firstParam = numCandidates_;
	bool audio = getCurrentClip()->type == ClipType::AUDIO;
	ModControllableAudio* target =
	    audio ? static_cast<ModControllableAudio*>(getCurrentClip()->output->toModControllable())
	          : instrumentClipView.getModControllableAudioOrNone();
	bool affectEntire = !audio && SoundEditor::shouldEditKitAffectEntire();
	bool kit = getCurrentOutputType() == OutputType::KIT;
	MenuChain menus = paramMenus(audio, affectEntire);

	// Param names and relevance read the sound editor's current sound (a filter's "Drive" is "FM" in some modes).
	// That's whatever was edited last - maybe nothing yet, or something deleted since - so point it at this clip's.
	Sound* const previousSound = soundEditor.currentSound;
	ModControllableAudio* const previousModControllable = soundEditor.currentModControllable;
	bool targetIsSound = target != nullptr && (getCurrentOutputType() == OutputType::SYNTH || (kit && !affectEntire));
	Sound* const targetSound = targetIsSound ? static_cast<Sound*>(target) : nullptr;
	auto pointEditorAt = [&](ModControllableAudio* modControllable, Sound* sound) {
		soundEditor.currentModControllable = modControllable;
		soundEditor.currentSound = sound;
	};
	pointEditorAt(target, targetSound);

	for (int8_t x = 0; x < kDisplayWidth; x++) {
		// This all happens in the main loop, so let audio render now and then, with the editor as it was
		if (x > 0 && x % 4 == 0) {
			pointEditorAt(previousModControllable, previousSound);
			AudioEngine::routineWithClusterLoading();
			pointEditorAt(target, targetSound);
		}
		for (int8_t y = 0; y < kDisplayHeight; y++) {
			auto [item, isSound] = SoundEditor::get_basic_shortcut_action(x, y, affectEntire);
			MenuItem* secondLayer = nullptr;
			if (isSound) {
				// The right two columns patch mod sources rather than open a param, apart from the randomizer
				if (!(x < 14 || (x == 14 && y < 5) || (x == 15 && y >= 1 && y <= 3))) {
					item = nullptr;
				}
				if (kit && item == &editNameMenu) {
					item = &drumNameEditMenu;
				}
				secondLayer = paramShortcutsForSoundsSecondLayer[x][y];
			}
			// The menu it's in ("Reverb") finds it too
			if (item != kComingSoon && SoundEditor::shortcutIsRelevant(target, item, x)) {
				addCandidate({.param = item, .x = x, .y = y}, paramName(item), parentTitle(item, menus));
			}
			if (SoundEditor::shortcutIsRelevant(target, secondLayer, x)) {
				addCandidate({.param = secondLayer, .x = x, .y = y, .secondLayer = true}, paramName(secondLayer),
				             parentTitle(secondLayer, menus));
			}
		}
	}

	pointEditorAt(previousModControllable, previousSound);

	// Params called the same (each filter's "Drive") get their menu's name in front
	std::array<bool, kMaxResults> shared{};
	for (size_t i = firstParam; i < numCandidates_; i++) {
		for (size_t j = i + 1; j < numCandidates_; j++) {
			if (strcmp(candidates_[i].name, candidates_[j].name) == 0) {
				shared[i] = shared[j] = true;
			}
		}
	}
	for (size_t i = firstParam; i < numCandidates_; i++) {
		Candidate& candidate = candidates_[i];
		if (shared[i] && !candidate.keywords.empty()) {
			char title[sizeof(candidate.name)];
			strcpy(title, candidate.name);
			size_t n = candidate.keywords.copy(candidate.name, sizeof(candidate.name) - 2);
			candidate.name[n++] = ' ';
			candidate.name[n + std::string_view(title).copy(candidate.name + n, sizeof(candidate.name) - 1 - n)] = 0;
		}
	}
}

void CommandSearchUI::search() {
	std::string_view query = enteredText.get();
	numResults_ = 0;
	selected_ = 0;

	for (size_t i = 0; i < numCandidates_; i++) {
		Candidate const& candidate = candidates_[i];
		int32_t score;
		if (query.empty()) {
			// What was used lately, then everything else alphabetically - so turning select browses it all
			score = static_cast<int32_t>(std::find(recent_.begin(), recent_.end(), candidate.result) - recent_.begin());
		}
		else {
			score = matchScore(candidate.name, candidate.keywords, query);
		}
		if (score >= 0) {
			scores_[i] = score;
			results_[numResults_++] = static_cast<uint16_t>(i);
		}
	}

	// Best match first, then alphabetically
	std::sort(results_.begin(), results_.begin() + numResults_, [this](uint16_t a, uint16_t b) {
		if (scores_[a] != scores_[b]) {
			return scores_[a] < scores_[b];
		}
		return compareNames(candidates_[a].name, candidates_[b].name) < 0;
	});
}

void CommandSearchUI::displayText(bool blinkImmediately) {
	search(); // The text changed
	if (display->haveOLED()) {
		renderUIsForOled();
	}
	else {
		QwertyUI::displayText(blinkImmediately);
	}
}

void CommandSearchUI::renderOLED(Canvas& canvas) {
	canvas.clearAreaExact(0, kTop, kRight, OLED_MAIN_HEIGHT_PIXELS - 1);

	// What's typed, and where we are in the matches
	drawTextForOLEDEditing(3, kRight - 24, kTop, 15, canvas);
	if (enteredText.isEmpty()) {
		canvas.drawString(deluge::l10n::get(deluge::l10n::String::STRING_FOR_COMMAND_SEARCH_HINT),
		                  3 + kTextSpacingX + 3, kTop + 2, kTextSmallSpacingX, kTextSmallSizeY);
	}
	if (numResults_ > 0) {
		char counter[10];
		size_t n = 0;
		auto put = [&](size_t value) {
			if (value >= 100) {
				counter[n++] = static_cast<char>('0' + value / 100);
			}
			if (value >= 10) {
				counter[n++] = static_cast<char>('0' + value / 10 % 10);
			}
			counter[n++] = static_cast<char>('0' + value % 10);
		};
		put(selected_ + 1);
		counter[n++] = '/';
		put(numResults_);
		counter[n] = 0;
		canvas.drawStringAlignRight(counter, kTop + 2, kTextSmallSpacingX, kTextSmallSizeY, kRight - 1);
	}
	for (int32_t x = 0; x <= kRight; x += 2) {
		canvas.drawPixel(x, kTop + 10);
	}

	if (numResults_ == 0) {
		canvas.drawStringCentred(deluge::l10n::get(deluge::l10n::String::STRING_FOR_COMMAND_SEARCH_NO_MATCH),
		                         kNameY + 2, kTextSpacingX, kTextSpacingY);
		return;
	}

	// The action: big, unless it's too long for that
	Result const& result = candidates_[results_[selected_]].result;
	char const* name = candidates_[results_[selected_]].name;
	int32_t width = canvas.getStringWidthInPixels(name, kTextTitleSizeY);
	if (width <= kNameAreaWidth) {
		canvas.drawString(name, (kNameAreaWidth - width) / 2, kNameY, kTextTitleSpacingX, kTextTitleSizeY);
	}
	else if ((width = canvas.getStringWidthInPixels(name, kTextSpacingY)) <= kNameAreaWidth) {
		canvas.drawString(name, (kNameAreaWidth - width) / 2, kNameY + 1, kTextSpacingX, kTextSpacingY);
	}
	else {
		width = canvas.getStringWidthInPixels(name, kTextSmallSizeY);
		canvas.drawString(name, std::max<int32_t>(0, (kNameAreaWidth - width) / 2), kNameY + 3, kTextSmallSpacingX,
		                  kTextSmallSizeY, 0, kNameAreaWidth);
	}

	// More above / below: turn select
	if (selected_ > 0) {
		drawTriangle(canvas, kArrowX, kNameY, 0, -1, 4);
	}
	if (selected_ + 1 < numResults_) {
		drawTriangle(canvas, kArrowX, kNameY + 9, 0, 1, 4);
	}

	drawCombo(canvas, result, kComboCentreY);
}

void CommandSearchUI::drawCombo(Canvas& canvas, Result const& result, int32_t centreY) {
	ComboLabels labels;
	labelsOf(labels, result.combo, result.gesture, result.param != nullptr, true);
	char const* const kTwice = "x2";
	bool gesturePad = result.gesture != nullptr && result.gesture->padX >= 0;
	bool pad = result.param != nullptr || gesturePad;
	int32_t padX = gesturePad ? result.gesture->padX : result.x;
	int32_t padY = gesturePad ? result.gesture->padY : result.y;
	size_t numPluses = labels.numParts == 0 ? 0 : labels.numParts - 1 + (pad ? 1 : 0);

	auto totalWidth = [&](KeyStyle const& style) {
		int32_t total = numPluses * (textWidth(canvas, "+", style) + 2 * style.gap);
		for (size_t i = 0; i < labels.numParts; i++) {
			total += partWidth(canvas, labels.parts[i], style);
		}
		if (pad) {
			total += kPadGridWidth;
			if (result.secondLayer) {
				total += style.gap + textWidth(canvas, kTwice, style);
			}
		}
		return total;
	};
	KeyStyle const& style = totalWidth(kNormalKeys) <= OLED_MAIN_WIDTH_PIXELS ? kNormalKeys : kSmallKeys;
	int32_t plusWidth = textWidth(canvas, "+", style);
	int32_t textY = centreY - style.textHeight / 2;

	int32_t x = std::max<int32_t>(0, (OLED_MAIN_WIDTH_PIXELS - totalWidth(style)) / 2);
	auto plus = [&] {
		canvas.drawString("+", x + style.gap, textY, style.textWidth, style.textHeight);
		x += plusWidth + 2 * style.gap;
	};
	for (size_t i = 0; i < labels.numParts; i++) {
		if (i > 0) {
			plus();
		}
		x += drawPart(canvas, labels.parts[i], x, centreY, style);
	}
	if (pad) {
		if (labels.numParts > 0) {
			plus();
		}
		drawPadGrid(canvas, x, centreY, padX, padY);
		if (result.secondLayer) {
			canvas.drawString(kTwice, x + kPadGridWidth + style.gap, textY, style.textWidth, style.textHeight);
		}
	}
}

size_t CommandSearchUI::comboText(Result const& result, char* text, size_t size) {
	ComboLabels labels;
	labelsOf(labels, result.combo, result.gesture, false, false);
	size_t n = 0;
	for (size_t i = 0; i < labels.numParts; i++) {
		if (i > 0 && n + 3 < size) {
			text[n++] = ' ';
			text[n++] = '+';
			text[n++] = ' ';
		}
		n += labels.parts[i].copy(text + n, size - 1 - n);
	}
	text[n] = 0;
	return n;
}

void CommandSearchUI::selectEncoderAction(int8_t offset) {
	if (numResults_ == 0) {
		return;
	}
	// No wrapping: the end of the list is the end, and the ▼ goes away
	if (offset > 0 && selected_ + 1 < numResults_) {
		selected_++;
	}
	else if (offset < 0 && selected_ > 0) {
		selected_--;
	}
	renderUIsForOled();
}

void CommandSearchUI::enterKeyPress() {
	runSelected();
}

void CommandSearchUI::runSelected() {
	if (numResults_ == 0) {
		return;
	}
	Result result = candidates_[results_[selected_]].result;

	// Remember it, most recent first
	auto it = std::find(recent_.begin(), recent_.end(), result);
	if (it == recent_.end()) {
		it = recent_.end() - 1; // New: the oldest makes room
	}
	std::rotate(recent_.begin(), it, it + 1);
	recent_[0] = result;

	exit(); // Back to the view it acts on

	if (result.param != nullptr) {
		// The sound editor opens on it and blinks its pad, which shows where the shortcut is
		soundEditor.openShortcutFromSearch(result.x, result.y, result.secondLayer);
		return;
	}

	if (result.combo != nullptr) {
		if (!isAvailable(*result.combo)) {
			return;
		}
		result.combo->run(result.combo->trigger);
	}
	else if (result.gesture->run != nullptr && applies(*result.gesture)) {
		result.gesture->run();
	}

	// Show the combo, so next time it's quicker without searching. Gestures that need a turn or a pad only get this.
	char text[64];
	comboText(result, text, sizeof(text));
	display->popupTextTemporary(text);
}

void CommandSearchUI::exit() {
	display->setNextTransitionDirection(-1);
	close();
}

bool CommandSearchUI::getGreyoutColsAndRows(uint32_t* cols, uint32_t* rows) {
	*cols = 0b11;
	return true;
}

ActionResult CommandSearchUI::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	if (b == button::BACK || b == button::SELECT_ENC) {
		if (on && !currentUIMode) {
			if (inCardRoutine) {
				return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
			}
			if (b == button::BACK) {
				exit();
			}
			else {
				runSelected();
			}
		}
		return ActionResult::DEALT_WITH;
	}
	return ActionResult::NOT_DEALT_WITH;
}

ActionResult CommandSearchUI::padAction(int32_t x, int32_t y, int32_t on) {
	if (x < kDisplayWidth) {
		return QwertyUI::padAction(x, y, on);
	}
	// Sidebar: leave, like the other text entry screens
	if (on && !currentUIMode) {
		if (sdRoutineLock) {
			return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
		}
		exit();
	}
	return ActionResult::DEALT_WITH;
}

ActionResult CommandSearchUI::verticalEncoderAction(int32_t offset, bool inCardRoutine) {
	return ActionResult::DEALT_WITH;
}
