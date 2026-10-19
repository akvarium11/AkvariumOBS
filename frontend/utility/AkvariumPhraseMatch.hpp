/******************************************************************************
    AkvariumOBS: phrase matching for voice commands (shared by the app and the
    offline self-test tool).

    A phrase matches when all of its words appear, in order, in the transcript.
    Short words must match exactly, longer ones tolerate small recognition
    errors. Both English and Russian text is supported (ё is normalised to е).

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

/* Optimal string alignment distance: counts a swapped pair as one edit, which
   matches how people mis-hear short words ("клип" -> "килп"). */
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

inline bool PhraseWordMatches(const QString &heard, const QString &expected)
{
	if (heard == expected)
		return true;
	if (expected.size() <= 3)
		return heard == expected;

	const int tolerance = std::max(1, (int)(expected.size() / 4));
	if (std::abs((int)(heard.size() - expected.size())) > tolerance)
		return false;
	return PhraseEditDistance(heard, expected) <= tolerance;
}

inline bool PhraseMatches(const QString &phrase, const QString &transcript)
{
	const QStringList want = NormalizePhraseText(phrase).split(QChar(' '), Qt::SkipEmptyParts);
	const QStringList got = NormalizePhraseText(transcript).split(QChar(' '), Qt::SkipEmptyParts);
	if (want.isEmpty() || got.isEmpty())
		return false;

	/* Reject when the transcript is far longer than the phrase: avoids firing
	   on normal conversational speech that merely mentions a word. */
	if (got.size() > want.size() * 3 + 4)
		return false;

	int index = 0;
	for (const QString &word : want) {
		bool found = false;
		while (index < got.size()) {
			if (PhraseWordMatches(got.at(index), word)) {
				found = true;
				index++;
				break;
			}
			index++;
		}
		if (!found)
			return false;
	}
	return true;
}

} // namespace akvarium
