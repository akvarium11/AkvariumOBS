#include "utility/AkvariumPhraseMatch.hpp"
#include "utility/AkvariumVoiceFilter.hpp"

#include <cstdio>

static int failures = 0;

static void check(const char *phrase, const char *transcript, bool expected)
{
	const akvarium::PhraseMatchResult result =
		akvarium::MatchPhrase(QString::fromUtf8(phrase), QString::fromUtf8(transcript));
	const bool got = result.matched;
	printf("%s | phrase='%s' transcript='%s' score=%.2f -> %s\n", got == expected ? "ok" : "FAIL", phrase,
	       transcript, result.score, got ? "match" : "no match");
	if (got != expected)
		failures++;
}

static void checkHallucination(const char *transcript, bool expected)
{
	const bool got = akvarium::IsLikelyHallucination(QString::fromUtf8(transcript));
	printf("%s | hallucination transcript='%s' -> %s\n", got == expected ? "ok" : "FAIL", transcript,
	       got ? "rejected" : "accepted");
	if (got != expected)
		failures++;
}

static void checkSegment(const char *desc, const akvarium::AudioSegmentStats &stats, int sens, bool expected)
{
	const bool got = akvarium::SegmentLooksLikeSpeech(stats, sens);
	printf("%s | segment [sens %d] '%s' (frames=%d, peak=%.3f, rms=%.4f, active=%.0f ms) -> %s\n",
	       got == expected ? "ok" : "FAIL", sens, desc, stats.frames, stats.peak, stats.rms, stats.activeMs,
	       got ? "speech" : "noise/silence");
	if (got != expected)
		failures++;
}

int main()
{
	/* Russian standard */
	check("сделай реплей", "Сделай реплей.", true);
	check("сделай реплей", "сделай реплеи", true);
	check("сделай реплей", "сделай пожалуйста реплей", true);
	check("сделай реплей", "сделай реплей сейчас же", true);
	check("сделай реплей", "сегодня хорошая погода и я хочу гулять", false);
	check("реплей", "реплей", true);
	check("запиши клип", "запиши клип", true);
	check("запиши клип", "запиши клип пожалуйста", true);
	check("запиши клип", "включи музыку", false);
	check("ёлка", "елка", true);

	/* Russian morphology and phonetics */
	check("откат", "откати", true);
	check("откат", "откатить", true);
	check("откат", "откати это", true);
	check("откати это", "откати это", true);
	check("откати это", "откатье это", true);
	check("откати это", "откочи это", true);
	check("откати это", "откачи это", true);
	check("откати это", "откатить это", true);
	check("откати это", "откатиэто", true);
	check("откати это", "откатьеэто", true);
	check("клипни это", "кликниэто", true);
	check("клипни это", "кликни это", true);
	check("клипни это", "клипниэто", true);
	check("клипни это", "клипни то", true);
	check("сохрани клип", "сохранить клип", true);
	check("сохрани клип", "сохраняй клип", true);
	check("клип", "клипни", true);
	check("клип", "заклипай", true);
	check("запиши", "записать", true);

	/* Prevent false positives on short words with typos/different meanings */
	check("клип", "клик", false);
	check("клип", "клоп", false);

	/* Chatter rejection */
	check("откат", "у него откат", false);
	check("откат", "у меня откат скилла был", false);
	check("откат", "мой откат", false);
	check("откат", "был откат", false);
	check("клип", "где этот клип", false);
	check("клип", "дай клип", false);
	check("реплей", "я вчера смотрел реплей матча и там был баг", false);
	check("реплей", "надо будет потом посмотреть этот реплей на ютубе вечером", false);
	check("сделай реплей", "реплей", false);

	/* English */
	check("save replay", "Save replay.", true);
	check("save replay", "please save the replay", true);
	check("save replay", "save a replay", true);
	check("save replay", "savereplay", true);
	check("save replay", "the weather is really nice today and I think we should go outside", false);
	check("save replay", "safe replay", true);
	check("save replay", "save", false);
	check("save replay", "I saved the replay of that match yesterday and it was fine", false);
	check("clip that", "clipthat", true);

	/* Commands surrounded by minimal filler */
	check("реплей", "сделай реплей", true);
	check("реплей", "давай реплей", true);
	check("сохрани реплей", "ну сохрани же реплей быстрее", true);

	/* Caption artefacts */
	checkHallucination("Продолжение следует...", true);
	checkHallucination("Субтитры сделал DimaTorzok", true);
	checkHallucination("Спасибо за просмотр!", true);
	checkHallucination("Thank you for watching.", true);
	checkHallucination("реплей реплей реплей", true);
	checkHallucination("...", true);
	checkHallucination("сделай реплей", false);
	checkHallucination("save replay", false);

	/* Segment VAD / noise rejection tests */
	akvarium::AudioSegmentStats silence;
	silence.frames = 35; /* 700 ms */
	silence.peak = 0.005;
	silence.rms = 0.0008;
	silence.activeMs = 0.0;
	checkSegment("pure room silence", silence, 3, false);
	checkSegment("pure room silence (low sens)", silence, 1, false);

	akvarium::AudioSegmentStats click;
	click.frames = 20; /* 400 ms */
	click.peak = 0.080;
	click.rms = 0.0040;
	click.activeMs = 40.0; /* only 2 frames above threshold */
	checkSegment("single keyboard click spike", click, 3, false);
	checkSegment("single keyboard click spike (low sens)", click, 1, false);

	akvarium::AudioSegmentStats breath;
	breath.frames = 25; /* 500 ms */
	breath.peak = 0.022;
	breath.rms = 0.0030;
	breath.activeMs = 100.0;
	checkSegment("mic breath / sigh", breath, 3, false);
	checkSegment("mic breath / sigh (low sens)", breath, 1, false);

	akvarium::AudioSegmentStats quietBgVoice;
	quietBgVoice.frames = 40; /* 800 ms */
	quietBgVoice.peak = 0.028;
	quietBgVoice.rms = 0.0045;
	quietBgVoice.activeMs = 240.0;
	checkSegment("faint background voice across room", quietBgVoice, 3, false);
	checkSegment("faint background voice across room (low sens)", quietBgVoice, 1, false);

	akvarium::AudioSegmentStats normalSpeech;
	normalSpeech.frames = 45; /* 900 ms */
	normalSpeech.peak = 0.250;
	normalSpeech.rms = 0.0450;
	normalSpeech.activeMs = 500.0;
	checkSegment("normal user voice command", normalSpeech, 3, true);
	checkSegment("normal user voice command (low sens)", normalSpeech, 1, true);

	printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
	return failures ? 1 : 0;
}
