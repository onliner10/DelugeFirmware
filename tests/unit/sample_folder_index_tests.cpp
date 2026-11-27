#include "CppUTest/TestHarness.h"
#include "storage/audio/sample_folder_index.h"
#include <cctype>
#include <random>
#include <string>
#include <strings.h>
#include <vector>

// SampleFolderIndex only looks up names it matches just the way FatFs does. Any other name has to be left to FatFs,
// or the index could find a different file than f_open() would, or one f_open() would refuse.

TEST_GROUP(SampleFolderIndexTests){};

TEST(SampleFolderIndexTests, ordinarySampleNamesAreLookedUp) {
	CHECK(SampleFolderIndex::canLookUp("C3.wav"));
	CHECK(SampleFolderIndex::canLookUp("Exit Summer Pad C#3 v127 (rr2).WAV"));
	CHECK(SampleFolderIndex::canLookUp("PIANO~12.WAV")); // A short name
	CHECK(SampleFolderIndex::canLookUp("x"));
	CHECK(SampleFolderIndex::canLookUp("a+b,c;d=e[f]g.aif")); // Not allowed in short names, but in long ones
}

TEST(SampleFolderIndexTests, namesFatFsTrimsAreLeftToIt) {
	// FatFs ignores dots and spaces at the end of a name, so "C3.wav." opens "C3.wav"
	CHECK_FALSE(SampleFolderIndex::canLookUp("C3.wav."));
	CHECK_FALSE(SampleFolderIndex::canLookUp("C3.wav "));
	CHECK_FALSE(SampleFolderIndex::canLookUp("."));
	CHECK_FALSE(SampleFolderIndex::canLookUp(""));
}

TEST(SampleFolderIndexTests, namesNotInAsciiAreLeftToFatFs) {
	// FatFs ignores the case of these letters too, which plain strcasecmp() doesn't
	CHECK_FALSE(SampleFolderIndex::canLookUp("Caf\x82.wav"));            // e acute in code page 437
	CHECK_FALSE(SampleFolderIndex::canLookUp("Fortepian \xC4\x85.wav")); // a with ogonek in UTF-8
	CHECK_FALSE(SampleFolderIndex::canLookUp("tab\there.wav"));
	CHECK_FALSE(SampleFolderIndex::canLookUp("delete\x7F.wav"));
}

TEST(SampleFolderIndexTests, namesFatFsRefusesAreLeftToIt) {
	for (char const* name : {"a\\b.wav", "a*.wav", "a:b.wav", "a<b.wav", "a>b.wav", "a|b.wav", "a\"b.wav", "?"}) {
		CHECK_FALSE_TEXT(SampleFolderIndex::canLookUp(name), name);
	}
}

// A folder's block is as big as a Cluster, which depends on how the card was formatted. Entries fill it from the front
// and names from the back.

TEST(SampleFolderIndexTests, spaceFillsFromBothEndsUntilTheyMeet) {
	SampleFolderIndex::Space space{100, 1000};
	size_t entriesEnd = 100;
	size_t namesStart = 1000;
	for (int32_t file = 0; file < 18; file++) { // 900 bytes free, 50 per file
		auto names = space.take(12, 38);
		CHECK(names.has_value());
		entriesEnd += 12;
		namesStart -= 38;
		CHECK_EQUAL(1000 - namesStart, static_cast<size_t>(*names)); // Distance back from the end of the block
		CHECK(entriesEnd <= namesStart);
	}
	CHECK_FALSE(space.take(12, 38).has_value());
	CHECK_FALSE(space.take(1, 0).has_value()); // 900 bytes used exactly
}

TEST(SampleFolderIndexTests, aLongNameCanStillFitAfterOneThatDidNot) {
	SampleFolderIndex::Space space{0, 100};
	CHECK_FALSE(space.take(12, 89).has_value());
	CHECK(space.take(12, 88).has_value());
}

TEST(SampleFolderIndexTests, blockTooSmallForTheFolderHasNoSpace) {
	SampleFolderIndex::Space space{300, 200};
	CHECK_FALSE(space.take(12, 1).has_value());
}

TEST(SampleFolderIndexTests, namesStayWithin16BitsOfTheEnd) {
	// A 64 KiB Cluster plus its header is more than 16 bits can count back
	SampleFolderIndex::Space space{300, 65536 + 112};
	size_t files = 0;
	while (auto names = space.take(12, 35)) {
		CHECK(*names <= UINT16_MAX);
		files++;
	}
	CHECK(files > 1300); // Barely fewer than the block would hold otherwise
}

TEST(SampleFolderIndexTests, everyClusterSizeHoldsSomeFiles) {
	// Typical sample: 16 byte Entry, "Piano C3 v1.wav" and "PIANOC~1.WAV"
	size_t lastFiles = 0;
	for (size_t clusterSize = 512; clusterSize <= 65536; clusterSize *= 2) {
		SampleFolderIndex::Space space{300, clusterSize + 112};
		size_t files = 0;
		while (space.take(16, 16 + 13)) {
			files++;
		}
		CHECK(files > lastFiles);
		lastFiles = files;
	}
}

// find() compares a name only with the files whose names hash the same. That must never miss a file strcasecmp() would
// have matched, or the index would fall back to f_open() - slower, and unnoticed.

TEST(SampleFolderIndexTests, hashIgnoresTheCaseOfAsciiLetters) {
	CHECK_EQUAL(SampleFolderIndex::nameHash("Grand Piano C#3 v025 rr1.wav"),
	            SampleFolderIndex::nameHash("GRAND PIANO c#3 V025 RR1.WAV"));
	CHECK_EQUAL(SampleFolderIndex::nameHash("PIANOC~1.WAV"), SampleFolderIndex::nameHash("pianoc~1.wav"));
	CHECK(SampleFolderIndex::nameHash("C3.wav") != SampleFolderIndex::nameHash("C4.wav"));
	CHECK(SampleFolderIndex::nameHash("[3].wav") != SampleFolderIndex::nameHash("{3}.wav")); // Not letters
}

TEST(SampleFolderIndexTests, everyPairOfBytesStrcasecmpCallsEqualHashesTheSame) {
	// Any byte, as a name read from the card can hold anything - at the start and further in
	for (int a = 1; a < 256; a++) {
		for (int b = 1; b < 256; b++) {
			char nameA[] = {static_cast<char>(a), 'x', static_cast<char>(a), 0};
			char nameB[] = {static_cast<char>(b), 'X', static_cast<char>(b), 0};
			bool equal = !strcasecmp(nameA, nameB);
			bool sameHash = SampleFolderIndex::nameHash(nameA) == SampleFolderIndex::nameHash(nameB);
			if (equal && !sameHash) {
				FAIL(("bytes " + std::to_string(a) + " and " + std::to_string(b)).c_str());
			}
		}
	}
}

namespace {
struct TestFile {
	std::string longName;
	std::string shortName;
};

std::string randomCase(std::string name, std::mt19937& rnd) {
	for (char& c : name) {
		if (rnd() & 1) {
			auto ch = static_cast<unsigned char>(c);
			c = static_cast<char>((rnd() & 1) ? toupper(ch) : tolower(ch));
		}
	}
	return name;
}

std::string multisampleName(int i) {
	char const* notes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	return "Grand Piano " + std::string(notes[i % 12]) + std::to_string(i / 12 % 8) + " v" + std::to_string(i / 96 * 10)
	       + " rr" + std::to_string(i % 3) + ".wav";
}

// The first file whose long or short name matches: find() before names were hashed
int32_t findByComparing(std::vector<TestFile> const& files, char const* name) {
	for (size_t i = 0; i < files.size(); i++) {
		if (!strcasecmp(name, files[i].longName.c_str()) || !strcasecmp(name, files[i].shortName.c_str())) {
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

// find() now
int32_t findByHash(std::vector<TestFile> const& files, char const* name) {
	uint16_t hash = SampleFolderIndex::nameHash(name);
	for (size_t i = 0; i < files.size(); i++) {
		char const* longName = files[i].longName.c_str();
		char const* shortName = files[i].shortName.c_str();
		if ((SampleFolderIndex::nameHash(longName) == hash && !strcasecmp(name, longName))
		    || (SampleFolderIndex::nameHash(shortName) == hash && !strcasecmp(name, shortName))) {
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}
} // namespace

TEST(SampleFolderIndexTests, hashedLookupFindsWhatComparingEveryNameFinds) {
	std::mt19937 rnd(1234);
	std::vector<TestFile> files;
	for (int i = 0; i < 1200; i++) {
		std::string longName = multisampleName(i);
		if (i % 7 == 0) {
			longName.insert(6, "\xC4\x85\xE9"); // Not ASCII: never looked up, but in the folder all the same
		}
		if (i % 50 == 0 && !files.empty()) {
			longName = randomCase(files[rnd() % files.size()].longName, rnd); // Differs only in case: the first wins
		}
		files.push_back({longName, "GRANDP~" + std::to_string(i % 9) + ".WAV"}); // Short names repeat: the first wins
	}

	int32_t found = 0;
	for (int i = 0; i < 20000; i++) {
		TestFile const& file = files[rnd() % files.size()];
		std::string name = randomCase((rnd() % 4) ? file.longName : file.shortName, rnd);
		if (rnd() % 5 == 0) {
			name[rnd() % name.size()] ^= 1; // Most likely no such file
		}
		int32_t expected = findByComparing(files, name.c_str());
		CHECK_EQUAL_TEXT(expected, findByHash(files, name.c_str()), name.c_str());
		found += (expected >= 0);
	}
	CHECK(found > 15000);
}

TEST(SampleFolderIndexTests, aMultisamplesNamesRarelyShareAHash) {
	// Otherwise the hash saves little: names that share one get compared anyway
	std::vector<uint16_t> hashes;
	for (int i = 0; i < 1000; i++) {
		hashes.push_back(SampleFolderIndex::nameHash(multisampleName(i).c_str()));
	}
	int32_t sharing = 0;
	for (size_t i = 0; i < hashes.size(); i++) {
		for (size_t j = 0; j < i; j++) {
			sharing += (hashes[i] == hashes[j]);
		}
	}
	CHECK(sharing < 30); // About 8 expected of 1000 random 16-bit hashes
}
