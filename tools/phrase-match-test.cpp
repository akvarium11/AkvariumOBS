#include "utility/AkvariumPhraseMatch.hpp"

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
	check("клип", "где этот клип", false);
	check("реплей", "я вчера смотрел реплей матча и там был баг", false);
	check("реплей", "надо будет потом посмотреть этот реплей на ютубе вечером", false);
	check("сделай реплей", "реплей", false);

	/* English */
	check("save replay", "Save replay.", true);
	check("save replay", "please save the replay", true);
	check("save replay", "save a replay", true);
	check("save replay", "the weather is really nice today and I think we should go outside", false);
	check("save replay", "safe replay", true);
	check("save replay", "save", false);
	check("save replay", "I saved the replay of that match yesterday and it was fine", false);

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

	printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
	return failures ? 1 : 0;
}
