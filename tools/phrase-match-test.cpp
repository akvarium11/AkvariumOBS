#include "utility/AkvariumPhraseMatch.hpp"

#include <cstdio>

static int failures = 0;

static void check(const char *phrase, const char *transcript, bool expected)
{
	const bool got = akvarium::PhraseMatches(QString::fromUtf8(phrase), QString::fromUtf8(transcript));
	printf("%s | phrase='%s' transcript='%s' -> %s\n", got == expected ? "ok" : "FAIL", phrase, transcript,
	       got ? "match" : "no match");
	if (got != expected)
		failures++;
}

int main()
{
	/* Russian */
	check("сделай реплей", "Сделай реплей.", true);
	check("сделай реплей", "сделай реплеи", true);
	check("сделай реплей", "сделай пожалуйста реплей", true);
	check("сделай реплей", "сделай реплей сейчас же", true);
	check("сделай реплей", "сегодня хорошая погода и я хочу гулять", false);
	check("реплей", "реплей", true);
	check("запиши клип", "запиши клип", true);
	check("запиши клип", "запиши клип пожалуйста", true);
	check("запиши клип", "запиши килп", true);
	check("запиши клип", "включи музыку", false);
	check("ёлка", "елка", true);

	/* English */
	check("save replay", "Save replay.", true);
	check("save replay", "please save the replay", true);
	check("save replay", "save a replay", true);
	check("save replay", "the weather is really nice today and I think we should go outside", false);
	check("save replay", "safe replay", true);
	check("save replay", "save", false);

	/* Mixed: command in Russian while transcript has extra words */
	check("сохрани реплей", "ну сохрани же реплей быстрее", true);

	printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
	return failures ? 1 : 0;
}
