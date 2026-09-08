#include "dock-fonts.hpp"

#include <QFontDatabase>
#include <QSet>
#include <QStringList>

namespace multireplay::fonts {
namespace {

struct Face {
	const char *path;
	const char *family; // the family name the face declares
};

// The eight faces of the .qrc, in the order they are registered. The prefix is
// /multireplay/fonts and the files sit in a fonts/ subdirectory, so the
// resource paths carry "fonts/" twice — that is the qrc's shape, not a typo.
const Face kFaces[] = {
	{":/multireplay/fonts/fonts/BarlowCondensed-SemiBold.ttf",
	 "Barlow Condensed"},
	{":/multireplay/fonts/fonts/BarlowCondensed-Bold.ttf",
	 "Barlow Condensed"},
	{":/multireplay/fonts/fonts/BarlowSemiCondensed-SemiBold.ttf",
	 "Barlow Semi Condensed"},
	{":/multireplay/fonts/fonts/BarlowSemiCondensed-Bold.ttf",
	 "Barlow Semi Condensed"},
	{":/multireplay/fonts/fonts/IBMPlexSans-Regular.ttf", "IBM Plex Sans"},
	{":/multireplay/fonts/fonts/IBMPlexSans-SemiBold.ttf", "IBM Plex Sans"},
	{":/multireplay/fonts/fonts/IBMPlexMono-Regular.ttf", "IBM Plex Mono"},
	{":/multireplay/fonts/fonts/IBMPlexMono-SemiBold.ttf", "IBM Plex Mono"},
};

bool g_ran = false;
int g_registered = 0;
QSet<QString> g_have;

// The family if it registered, otherwise the fallback. Never empty: see the
// header's note on why an empty family is the worst possible answer.
QString familyOr(const char *wanted, const QString &fallback)
{
	const QString w = QString::fromLatin1(wanted);
	return g_have.contains(w) ? w : fallback;
}

} // namespace

int registerEmbedded()
{
	if (g_ran)
		return g_registered;
	g_ran = true;
	for (const Face &f : kFaces) {
		const int id = QFontDatabase::addApplicationFont(
			QString::fromLatin1(f.path));
		if (id < 0)
			continue;
		++g_registered;
		for (const QString &fam :
		     QFontDatabase::applicationFontFamilies(id))
			g_have.insert(fam);
	}
	return g_registered;
}

bool allEmbedded()
{
	registerEmbedded();
	return g_registered == (int)(sizeof(kFaces) / sizeof(kFaces[0]));
}

QString labelFamily()
{
	registerEmbedded();
	// The fallbacks are the condensed faces Windows and macOS actually ship;
	// on a Linux box with neither, Qt walks its own substitution list.
	return familyOr("Barlow Condensed", QStringLiteral("Arial Narrow"));
}

QString displayFamily()
{
	registerEmbedded();
	return familyOr("Barlow Semi Condensed", QStringLiteral("Arial Narrow"));
}

QString bodyFamily()
{
	registerEmbedded();
	return familyOr("IBM Plex Sans",
			QFontDatabase::systemFont(QFontDatabase::GeneralFont)
				.family());
}

QString monoFamily()
{
	registerEmbedded();
	return familyOr("IBM Plex Mono",
			QFontDatabase::systemFont(QFontDatabase::FixedFont)
				.family());
}

} // namespace multireplay::fonts
