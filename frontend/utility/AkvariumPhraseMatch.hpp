/******************************************************************************
    AkvariumOBS: phrase matching for voice commands (shared by the app and the
    offline self-test tool).

    A phrase matches when all of its words appear, in order, in the transcript.
    Russian and English command morphology is stemmed, short words require exact
    or stem match, and casual conversational speech is rejected.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <QChar>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace akvarium {

/* Minimum score a phrase needs before it fires a command. */
constexpr double kPhraseMatchThreshold = 0.78;

inline QString NormalizePhraseText(QString text)
{
	text = text.toLower();
	text.replace(QChar(0x0451), QChar(0x0435)); /* ё -> е */

	QString out;
	out.reserve(text.size());
	bool space = true;
	for (QChar ch : text) {
		const ushort c = ch.unicode();
		const bool wordChar = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
				      (c >= 0x0400 && c <= 0x04FF) || ch.isLetter();
		if (wordChar) {
			out.append(ch);
			space = false;
		} else if (!space) {
			out.append(QChar(' '));
			space = true;
		}
	}
	while (out.endsWith(QChar(' ')))
		out.chop(1);
	return out;
}

/* Optimal string alignment distance. */
inline int PhraseEditDistance(const QString &a, const QString &b)
{
	const int n = a.size();
	const int m = b.size();
	if (n == 0)
		return m;
	if (m == 0)
		return n;

	std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
	for (int i = 0; i <= n; i++)
		dp[i][0] = i;
	for (int j = 0; j <= m; j++)
		dp[0][j] = j;

	for (int i = 1; i <= n; i++) {
		for (int j = 1; j <= m; j++) {
			const int cost = a.at(i - 1) == b.at(j - 1) ? 0 : 1;
			dp[i][j] = std::min({dp[i - 1][j] + 1, dp[i][j - 1] + 1, dp[i - 1][j - 1] + cost});
			if (i > 1 && j > 1 && a.at(i - 1) == b.at(j - 2) && a.at(i - 2) == b.at(j - 1))
				dp[i][j] = std::min(dp[i][j], dp[i - 2][j - 2] + 1);
		}
	}
	return dp[n][m];
}

inline QStringList PhraseWords(const QString &text)
{
	return NormalizePhraseText(text).split(QChar(' '), Qt::SkipEmptyParts);
}

/* Russian and English stemmer for command verbs and nouns:
   "сохрани" / "сохранить" / "сохраняй" -> "сохран"
   "откат" / "откати" / "откатить" / "откатье" / "откочи" -> "откат"
   "клип" / "клипни" / "заклипай" -> "клип"
   "запиши" / "записать" -> "запис" */
inline QString WordStem(const QString &word)
{
	QString w = word;

	/* Strip English common suffixes */
	if (w.endsWith(QStringLiteral("ing")) && w.size() > 5) {
		w.chop(3);
		return w;
	}
	if (w.endsWith(QStringLiteral("ed")) && w.size() > 4) {
		w.chop(2);
		return w;
	}
	if (w.endsWith(QStringLiteral("es")) && w.size() > 4) {
		w.chop(2);
		return w;
	}
	if (w.endsWith(QStringLiteral("s")) && w.size() > 3) {
		w.chop(1);
		return w;
	}

	/* Russian command prefixes (e.g. "заклипай" -> "клипай") */
	if (w.startsWith(QStringLiteral("заклип")) && w.size() >= 7)
		w = w.mid(2);

	/* Russian verb "клипни" / "клипнуть" -> "клип" */
	if (w.startsWith(QStringLiteral("клипн")) && w.size() >= 6)
		return QStringLiteral("клип");

	/* Russian suffixes sorted by length descending */
	static const QStringList ruSuffixes = {
		QStringLiteral("ите"), QStringLiteral("йте"), QStringLiteral("нуть"),
		QStringLiteral("вать"), QStringLiteral("ать"), QStringLiteral("ять"),
		QStringLiteral("ить"), QStringLiteral("еть"), QStringLiteral("чик"),
		QStringLiteral("ное"), QStringLiteral("ный"), QStringLiteral("ная"),
		QStringLiteral("ные"), QStringLiteral("ну"),  QStringLiteral("ть"),
		QStringLiteral("ла"),  QStringLiteral("ло"),  QStringLiteral("ли"),
		QStringLiteral("ем"),  QStringLiteral("им"),  QStringLiteral("ит"),
		QStringLiteral("ят"),  QStringLiteral("ет"),  QStringLiteral("ут"),
		QStringLiteral("ют"),  QStringLiteral("ай"),  QStringLiteral("яй"),
		QStringLiteral("ей"),  QStringLiteral("ой"),  QStringLiteral("ье"),
		QStringLiteral("ья"),  QStringLiteral("ью"),  QStringLiteral("ы"),
		QStringLiteral("и"),   QStringLiteral("е"),   QStringLiteral("а"),
		QStringLiteral("у"),   QStringLiteral("я"),   QStringLiteral("о"),
		QStringLiteral("й"),
	};

	if (w.size() >= 4) {
		for (const QString &suf : ruSuffixes) {
			if (w.endsWith(suf) && (w.size() - suf.size()) >= 3) {
				w.chop(suf.size());
				break;
			}
		}
	}

	/* Strip trailing soft sign ь */
	if (w.endsWith(QChar(0x044c)) && w.size() >= 4)
		w.chop(1);

	/* Special phonetic/morphological normalizations */
	if (w == QStringLiteral("запиш"))
		w = QStringLiteral("запис");
	else if (w == QStringLiteral("откач") || w == QStringLiteral("откоч") || w == QStringLiteral("откот"))
		w = QStringLiteral("откат");

	return w;
}

/* Russian phonetic reduction for spoken command recognition:
   Handles unstressed vowel reduction (о/а, е/и) and palatalized consonant confusion (т/ч, с/ш). */
inline QString RussianPhoneticStem(QString s)
{
	s.replace(QChar(0x043e), QChar(0x0430)); /* о -> а */
	s.replace(QChar(0x0435), QChar(0x0438)); /* е -> и */
	s.replace(QChar(0x0447), QChar(0x0442)); /* ч -> т (откачи -> откати) */
	s.replace(QChar(0x0448), QChar(0x0441)); /* ш -> с */
	s.remove(QChar(0x044c));                 /* ь */
	s.remove(QChar(0x044a));                 /* ъ */
	return s;
}

/* 1.0 for exact or stem match, 0.7..1.0 for tolerated misspellings in longer words,
   0.0 for different words. Words of 4 letters or fewer require exact or stem match. */
inline double PhraseWordSimilarity(const QString &heard, const QString &expected)
{
	if (heard == expected)
		return 1.0;

	/* English common whisper acoustic confusion */
	if ((heard == QStringLiteral("safe") && expected == QStringLiteral("save")) ||
	    (heard == QStringLiteral("save") && expected == QStringLiteral("safe")))
		return 0.90;

	/* Russian / English stem equality */
	const QString stemH = WordStem(heard);
	const QString stemE = WordStem(expected);
	if (stemH == stemE && stemH.size() >= 3)
		return 1.0;

	/* Russian phonetic stem equality */
	const QString phonH = RussianPhoneticStem(stemH);
	const QString phonE = RussianPhoneticStem(stemE);
	if (phonH == phonE && phonH.size() >= 3)
		return 1.0;

	/* Short words (<= 4 chars, like "клип", "клик", "save") must match exactly or by stem */
	if (expected.size() <= 4 || heard.size() <= 4)
		return 0.0;

	/* For words of 5+ letters: allow tolerance up to length / 3 */
	const int tolerance = std::max(1, (int)(expected.size() / 3));
	if (std::abs((int)(heard.size() - expected.size())) > tolerance)
		return 0.0;

	const int distance = PhraseEditDistance(heard, expected);
	if (distance > tolerance)
		return 0.0;

	const int length = std::max(heard.size(), expected.size());
	return std::max(0.68, 1.0 - (double)distance / (double)length);
}

inline bool PhraseWordMatches(const QString &heard, const QString &expected)
{
	return PhraseWordSimilarity(heard, expected) > 0.0;
}

struct PhraseMatchResult {
	bool matched = false;
	double score = 0.0;
	int extraWords = 0;
};

/* Allowed command filler words that don't indicate conversational chatter */
inline bool IsCommandFillerWord(const QString &word)
{
	static const QStringList fillers = {
		QStringLiteral("ну"), QStringLiteral("давай"), QStringLiteral("сделай"),
		QStringLiteral("пожалуйста"), QStringLiteral("быстро"), QStringLiteral("сейчас"),
		QStringLiteral("же"), QStringLiteral("ка"), QStringLiteral("обска"),
		QStringLiteral("обс"), QStringLiteral("это"), QStringLiteral("то"),
		QStringLiteral("э"),  QStringLiteral("please"), QStringLiteral("now"),
		QStringLiteral("the"), QStringLiteral("a"), QStringLiteral("an"),
		QStringLiteral("just"), QStringLiteral("that"), QStringLiteral("this"),
	};
	return fillers.contains(word);
}

/* Aligns the phrase against the transcript, keeping word order.
   Rejects transcripts where the command is buried inside conversational speech. */
inline PhraseMatchResult MatchPhrase(const QString &phrase, const QString &transcript)
{
	PhraseMatchResult result;

	const QStringList want = PhraseWords(phrase);
	const QStringList got = PhraseWords(transcript);
	if (want.isEmpty() || got.isEmpty())
		return result;

	const int n = want.size();
	const int m = got.size();

	/* A command cannot have excessive extra words.
	   Single-word command ("откат"): at most 1 extra non-filler or 2 filler words.
	   Multi-word command: at most 3 extra words. */
	const int maxExtraWords = (n <= 1) ? 2 : 3;
	if (m > n + maxExtraWords)
		return result;

	/* For single-word commands, reject if any non-filler extra words are present
	   (e.g. "мой откат", "был откат", "у него откат", "где клип"). */
	if (n == 1 && m > 1) {
		int nonFillers = 0;
		for (const QString &w : got) {
			if (PhraseWordSimilarity(w, want.first()) == 0.0 && !IsCommandFillerWord(w))
				nonFillers++;
		}
		if (nonFillers > 0)
			return result;
	}

	const double kUnreachable = -1e9;
	const double kSkipPenalty = (n <= 1) ? 0.16 : 0.08;

	std::vector<std::vector<double>> dp(n + 1, std::vector<double>(m + 1, kUnreachable));
	dp[0][0] = 0.0;
	for (int j = 1; j <= m; j++)
		dp[0][j] = dp[0][j - 1] - kSkipPenalty;

	for (int i = 1; i <= n; i++) {
		for (int j = i; j <= m; j++) {
			double best = dp[i][j - 1] - kSkipPenalty;
			if (dp[i - 1][j - 1] > kUnreachable / 2) {
				const double similarity = PhraseWordSimilarity(got.at(j - 1), want.at(i - 1));
				if (similarity > 0.0)
					best = std::max(best, dp[i - 1][j - 1] + similarity);
			}
			dp[i][j] = best;
		}
	}

	const double value = dp[n][m];
	if (value <= kUnreachable / 2)
		return result;

	result.score = std::max(0.0, std::min(1.0, value / (double)n));
	result.extraWords = std::max(0, m - n);
	result.matched = result.score >= kPhraseMatchThreshold;
	return result;
}

inline bool PhraseMatches(const QString &phrase, const QString &transcript)
{
	return MatchPhrase(phrase, transcript).matched;
}

/* Subtitle and video caption artifacts that whisper.cpp emits on non-speech audio. */
inline const QStringList &HallucinationMarkers()
{
	static const QStringList markers = {
		QStringLiteral("субтитры"),
		QStringLiteral("продолжение следует"),
		QStringLiteral("спасибо за просмотр"),
		QStringLiteral("подписывайтесь"),
		QStringLiteral("подпишитесь"),
		QStringLiteral("ставьте лайк"),
		QStringLiteral("редактор субтитров"),
		QStringLiteral("корректор"),
		QStringLiteral("перевод"),
		QStringLiteral("семкин"),
		QStringLiteral("егорова"),
		QStringLiteral("dimatorzok"),
		QStringLiteral("amara org"),
		QStringLiteral("thank you for watching"),
		QStringLiteral("thanks for watching"),
		QStringLiteral("please subscribe"),
		QStringLiteral("like and subscribe"),
		QStringLiteral("subscribe to my channel"),
		QStringLiteral("transcribed by"),
		QStringLiteral("subtitles by"),
		QStringLiteral("subtitle"),
	};
	return markers;
}

/* Rejects caption artifacts, degenerate repeats, and transcripts without letters. */
inline bool IsLikelyHallucination(const QString &transcript)
{
	const QString normalized = NormalizePhraseText(transcript);
	if (normalized.isEmpty())
		return true;

	for (const QString &marker : HallucinationMarkers()) {
		if (normalized.contains(marker))
			return true;
	}

	const QStringList words = normalized.split(QChar(' '), Qt::SkipEmptyParts);
	if (words.isEmpty())
		return true;

	bool hasLetter = false;
	for (const QString &w : words) {
		for (QChar ch : w) {
			if (ch.isLetter()) {
				hasLetter = true;
				break;
			}
		}
		if (hasLetter)
			break;
	}
	if (!hasLetter)
		return true;

	/* Word repetition loop ("реплей реплей реплей") */
	if (words.size() >= 3) {
		bool same = true;
		for (int i = 1; i < words.size() && same; i++)
			same = words.at(i) == words.at(0);
		if (same)
			return true;
	}

	/* Bigram repetition loop */
	const int wordCount = words.size();
	if (wordCount >= 4) {
		for (int i = 0; i + 2 < wordCount; i++) {
			const QString a = words.at(i);
			const QString b = words.at(i + 1);
			int repeats = 1;
			for (int j = i + 2; j + 1 < wordCount; j += 2) {
				if (words.at(j) == a && words.at(j + 1) == b)
					repeats++;
				else
					break;
			}
			if (repeats >= 2)
				return true;
		}
	}

	return false;
}

} // namespace akvarium
