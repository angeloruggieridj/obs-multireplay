// dock-mockup — the panel's control strip, alive, without OBS.
//
// WHY IT EXISTS. Every layout complaint about this dock ("the keys are
// scattered", "it will not resize", "the borders are cut") is about what the
// panel does at a size nobody built it at, and the only way to see that is to
// resize it. Doing that inside OBS costs a four-minute gate run per look, needs
// a rig with cameras on it, and leaves you reading a screenshot of a window
// somebody else's compositor drew.
//
// So this builds the SAME sections, with the SAME layout code (src/dock-layout)
// and the SAME style sheet (src/dock-style), out of dummy keys — and then walks
// itself through a list of sizes, writing a PNG of each. Portrait, landscape,
// docked-narrow, floating-wide: one second, six pictures.
//
// THE ARRANGEMENT IT IS CHECKING is the reference panel's own, two macro-rows:
//
//   MARK        A|B A B  [A 1..8 / B 1..8]  swap                     EXPORT
//   REC         play rev last [Riproduci eventi] step NOW  modes   VELOCITA
//
// Every section is ONE key-row tall except the camera matrix, which is two and
// carries the bay selector beside them. Justification puts the first section of
// a line at the left and the last at the right, which is what places REC at one
// end, the transport in the middle and the speed at the other.
#include "../../src/dock-layout.hpp"
#include "../../src/dock-style.hpp"

#include <QApplication>
#include <QDir>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cstdio>

using namespace multireplay;

namespace {

// The camera keys are square-ish and small, as on the reference panel: a matrix
// is read by position, not by label, and a wide key breaks the grid.
constexpr int kAngleKeyW = 46;

QPushButton *key(const QString &text, const char *role = "")
{
	auto *b = new QPushButton(text);
	if (role && *role)
		b->setObjectName(QString::fromLatin1(role));
	b->setMinimumHeight(kKeyH);
	b->setMaximumHeight(kKeyH);
	b->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	return b;
}

QPushButton *toggle(const QString &text)
{
	auto *b = key(text, "mrToggle");
	b->setCheckable(true);
	return b;
}

class Mock : public QWidget {
public:
	Mock()
	{
		setObjectName(QStringLiteral("MultiReplayDock"));
		setStyleSheet(QString::fromUtf8(kDockStyle));
		setMinimumWidth(420);

		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(4, 4, 4, 4);
		v->setSpacing(3);

		auto *pic = new QLabel(QStringLiteral("preview"), this);
		pic->setAlignment(Qt::AlignCenter);
		pic->setStyleSheet("background:#101010;color:#404040;");
		pic->setMinimumHeight(60);
		pic->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		v->addWidget(pic, 3);

		auto *table = new QTableWidget(6, 6, this);
		table->setObjectName(QStringLiteral("mrEvents"));
		table->verticalHeader()->setVisible(false);
		table->setMinimumHeight(60);
		table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		v->addWidget(table, 2);

		strip_ = new ControlStrip(this);
		// Two macro-rows in the wide arrangement; the numbers are the
		// FOLDED order - what an operator reaches for through a whole
		// match comes first, and the exports, which nobody touches while
		// the ball is in play, come last.
		buildMark(/*rank*/ 1);
		buildAngles(/*rank*/ 2);
		buildExport(/*rank*/ 5);
		buildRec(/*startsLine*/ true, /*rank*/ 0);
		buildPlayback(/*rank*/ 3);
		buildSpeed(/*rank*/ 4);
		addStrip(v, strip_);

		auto *clip = new QLabel(QStringLiteral("  0001  ·  C1  ·  -00:03.20"),
					this);
		clip->setStyleSheet("background:#199847;color:#eaffea;font-weight:700;");
		clip->setFixedHeight(kKeyH);
		v->addWidget(clip, 0);

		auto *seek = new QLabel(this);
		seek->setStyleSheet("background:#16202c;border:1px solid #24313f;");
		seek->setFixedHeight(46);
		v->addWidget(seek, 0);
	}

	ControlStrip *strip_ = nullptr;

private:
	// MARK — one row, as on the reference panel. The keys keep their natural
	// widths: forcing them all to the widest ("Annulla") puts five short
	// labels in five wide keys, and the row stops reading as a row of marks
	// and starts reading as a form.
	void buildMark(int rank)
	{
		auto *blk = new KeyBlock(QStringLiteral("Mark"), this);
		auto *in = key(QStringLiteral("In"), "mrAccent");
		auto *out = key(QStringLiteral("Out"), "mrAccent");
		auto *m5 = key(QStringLiteral("-5s"));
		auto *m10 = key(QStringLiteral("-10s"));
		auto *m20 = key(QStringLiteral("-20s"));
		auto *tin = key(QStringLiteral("⇤IN"));
		auto *tout = key(QStringLiteral("OUT⇥"));
		auto *cancel = key(QStringLiteral("Annulla"), "mrDanger");
		blk->setShapes(
			// wide: the reference's own row
			{{Cell(in), Cell(out), Cell(m5), Cell(m10), Cell(m20),
			  Cell(tin), Cell(tout), Cell(cancel)}},
			// compact: the three questions this group answers - take
			// a point, take the last N seconds, move a point
			{{Cell(in, 2), Cell(out, 2), Cell(cancel, 2)},
			 {Cell(m5, 2), Cell(m10, 2), Cell(m20, 2)},
			 {Cell(tin, 3), Cell(tout, 3)}});
		strip_->addBlock(blk, false, rank);
	}

	// ANGOLI — the bay selector, then the matrix, then the swap. Two rows,
	// and the two controls that are ABOUT both rows stand beside them
	// instead of under them.
	void buildAngles(int rank)
	{
		auto *blk = new KeyBlock(QStringLiteral("Angoli"), this);
		QVector<Cell> rowA, rowB;
		const char *names[8] = {"C1", "C2", "3", "4", "5", "6", "7", "8"};
		for (int ch = 0; ch < 2; ch++) {
			auto *letter = new QLabel(ch ? "B" : "A", this);
			letter->setObjectName(QStringLiteral("mrSectionLabel"));
			letter->setFixedWidth(10);
			letter->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			(ch ? rowB : rowA) << Cell(letter, 1, false);
			for (int i = 0; i < 8; i++) {
				auto *b = key(QString::fromLatin1(names[i]),
					      "mrAngle");
				b->setCheckable(true);
				b->setFixedWidth(kAngleKeyW);
				// Two cameras configured. The other six keys are
				// NOT DRAWN - a key that does nothing is a key
				// the eye has to rule out every time it reads the
				// row - but their space is kept, so angle 5 is at
				// the same x whatever the rig has in it, and
				// adding a camera in Settings does not slide the
				// keys out from under the operator's fingers.
				if (i >= 2) {
					QSizePolicy sp = b->sizePolicy();
					sp.setRetainSizeWhenHidden(true);
					b->setSizePolicy(sp);
					b->hide();
				}
				(ch ? rowB : rowA) << Cell(b);
			}
		}

		auto *sel = new QWidget(this);
		auto *sl = new QHBoxLayout(sel);
		sl->setContentsMargins(0, 0, 0, 0);
		sl->setSpacing(3);
		for (const char *l : {"A|B", "A", "B"}) {
			auto *b = key(QString::fromLatin1(l), "mrChanSel");
			b->setCheckable(true);
			b->setFixedWidth(38);
			sl->addWidget(b);
		}
		sel->setFixedHeight(kKeyH);

		auto *swap = key(QStringLiteral("⇄"), "mrChanSel");
		swap->setFixedWidth(38);

		QVector<Cell> top;
		top << Cell(sel, 1, false, 2);
		top += rowA;
		top << Cell(swap, 1, false, 2);
		QVector<Cell> bottom;
		bottom << Cell(nullptr, 1); // the selector's column, already taken
		bottom += rowB;
		// compact: the selector drops UNDER the matrix, which is the only
		// way to keep eight slots on a narrow dock.
		QVector<Cell> cSel;
		cSel << Cell(nullptr, 1) << Cell(sel, 3, false)
		     << Cell(nullptr, 1) << Cell(swap, 1, false);
		QVector<Cell> cTop = rowA;
		QVector<Cell> cBottom = rowB;
		blk->setShapes({top, bottom}, {cTop, cBottom, cSel});
		strip_->addBlock(blk, false, rank);
	}

	// EXPORT — the reference keeps it in the far corner of the first row.
	void buildExport(int rank)
	{
		auto *blk = new KeyBlock(QStringLiteral("Export"), this);
		auto *up = key(QStringLiteral("▲"), "mrTransport");
		auto *dn = key(QStringLiteral("▼"), "mrTransport");
		auto *more = key(QStringLiteral("⋯"), "mrTransport");
		auto *exp = key(QStringLiteral("Esporta clip"));
		auto *reel = key(QStringLiteral("Sequenza"));
		blk->setShapes({{Cell(up), Cell(dn), Cell(more), Cell(exp),
				 Cell(reel)}},
			       // compact: the three small keys share the width of
			       // the two wide ones, and nothing is a hole
			       {{Cell(up), Cell(dn), Cell(more)},
				{Cell(exp, 3)},
				{Cell(reel, 3)}});
		strip_->addBlock(blk, false, rank);
	}

	// REC — the take. Gear, the record key, and the two lines of clock beside
	// them (one row tall, like every other section of this macro-row).
	void buildRec(bool startsLine, int rank)
	{
		auto *blk = new KeyBlock(QStringLiteral("REC"), this);
		auto *gear = key(QStringLiteral("⚙"), "mrGear");
		gear->setFixedWidth(34);
		auto *rec = key(QStringLiteral("●  REC"), "mrRec");
		rec->setMinimumWidth(84);

		auto *clockBox = new QWidget(this);
		auto *cv = new QVBoxLayout(clockBox);
		cv->setContentsMargins(2, 0, 0, 0);
		cv->setSpacing(0);
		auto *clock = new QLabel(QStringLiteral("17:22:06"), clockBox);
		clock->setObjectName(QStringLiteral("mrClock"));
		auto *status = new QLabel(QStringLiteral("01:10:24 rimanenti"),
					  clockBox);
		status->setObjectName(QStringLiteral("mrMuted"));
		cv->addWidget(clock);
		cv->addWidget(status);
		clockBox->setFixedHeight(kKeyH);

		blk->setShapes({{Cell(gear), Cell(rec), Cell(clockBox, 1, false)}},
			       {{Cell(gear), Cell(rec, 2)},
				{Cell(clockBox, 3, false)}});
		strip_->addBlock(blk, startsLine, rank);
	}

	// RIPRODUZIONE — the reference's centre group, in its order.
	void buildPlayback(int rank)
	{
		auto *blk = new KeyBlock(QStringLiteral("Riproduzione"), this);
		auto *pp = key(QStringLiteral("▶"), "mrPlay");
		auto *rev = key(QStringLiteral("◀"), "mrTransport");
		auto *last = key(QStringLiteral("↺"), "mrTransport");
		auto *play = key(QStringLiteral("Riproduci eventi"), "mrAccent");
		auto *more = key(QStringLiteral("▾"), "mrGear");
		more->setFixedWidth(24);
		auto *sb = key(QStringLiteral("⏮"), "mrTransport");
		auto *sf = key(QStringLiteral("⏭"), "mrTransport");
		// NOW IS A DESTINATION, not a modifier: it drops the replay and
		// puts the operator back on the live edge. It gets a width of its
		// own and the red of the thing it does, at rest as well as live.
		auto *now = key(QStringLiteral("NOW"), "mrNow");
		now->setMinimumWidth(56);
		auto *loop = toggle(QStringLiteral("Loop"));
		auto *music = toggle(QStringLiteral("♫"));
		auto *toOut = toggle(QStringLiteral("In output"));
		blk->setShapes(
			// wide: the reference's centre group, one row
			{{Cell(pp), Cell(rev), Cell(last), Cell(play),
			  Cell(more), Cell(sb), Cell(sf), Cell(now), Cell(loop),
			  Cell(music), Cell(toOut)}},
			// compact: what you press first on top, what drives the
			// clip under it, the modes last
			{{Cell(play, 3), Cell(more), Cell(now, 2)},
			 {Cell(pp), Cell(rev), Cell(last), Cell(sb), Cell(sf),
			  Cell(nullptr, 1)},
			 {Cell(loop, 2), Cell(music), Cell(toOut, 3)}});
		strip_->addBlock(blk, false, rank);
	}

	// VELOCITA — presets then dial, one row, as on the reference panel.
	void buildSpeed(int rank)
	{
		auto *blk = new KeyBlock(QStringLiteral("Velocità"), this);
		QVector<Cell> row;
		QList<QPushButton *> chips;
		for (const char *l : {"25%", "33%", "50%", "75%", "100%",
				      "2×"}) {
			auto *b = key(QString::fromUtf8(l), "mrSpeedChip");
			chips << b;
			row << Cell(b);
		}
		equaliseKeyWidths(chips);
		auto *dial = new QSlider(Qt::Horizontal, this);
		dial->setObjectName(QStringLiteral("mrSpeed"));
		dial->setRange(5, 200);
		dial->setValue(100);
		dial->setMinimumWidth(110);
		dial->setFixedHeight(kKeyH);
		auto *read = new QLabel(QStringLiteral("1.00×"), this);
		read->setObjectName(QStringLiteral("mrTimecode"));
		read->setMinimumWidth(42);
		read->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		blk->setShapes({row,
				{Cell(dial, 5), Cell(read, 1, false)}},
			       // compact: two rows of presets over the dial
			       {{Cell(chips[0]), Cell(chips[1]), Cell(chips[2])},
				{Cell(chips[3]), Cell(chips[4]), Cell(chips[5])},
				{Cell(dial, 2), Cell(read, 1, false)}});
		strip_->addBlock(blk, false, rank);
	}
};

} // namespace

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QStringList args = app.arguments();

	auto *w = new Mock();

	if (args.contains(QStringLiteral("--show"))) {
		w->resize(1100, 800);
		w->show();
		return app.exec();
	}

	const QString outDir = args.size() > 1 ? args[1] : QStringLiteral(".");
	QDir().mkpath(outDir);

	struct Size {
		const char *name;
		int w, h;
	};
	QVector<Size> sizes = {{"portrait-narrow", 520, 980},
			       {"portrait-wide", 760, 1000},
			       {"landscape-short", 1500, 560},
			       {"landscape-tall", 1500, 900},
			       {"tiny", 460, 420},
			       {"floating", 1000, 760}};
	if (args.size() > 2) {
		const QStringList wh = args[2].split(QLatin1Char('x'));
		if (wh.size() == 2)
			sizes = {{"custom", wh[0].toInt(), wh[1].toInt()}};
	}

	for (const Size &s : sizes) {
		w->resize(s.w, s.h);
		w->show();
		for (int i = 0; i < 3; i++) {
			QApplication::processEvents();
			QApplication::sendPostedEvents();
		}
		const QString path =
			QDir(outDir).filePath(QString("mock-%1.png").arg(s.name));
		w->grab().save(path);
		std::printf("%-16s asked %4dx%4d  got %4dx%4d  min %4dx%4d  %s\n",
			    s.name, s.w, s.h, w->width(), w->height(),
			    w->minimumSizeHint().width(),
			    w->minimumSizeHint().height(),
			    w->strip_->isFlat() ? "FLAT" : "TALL");
	}
	return 0;
}
