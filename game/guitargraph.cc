#include "guitargraph.hh"

#include "joystick.hh"

namespace {
	struct Diff { std::string name; int basepitch; } diffv[] = {
		{ "Supaeasy", 0x3C },
		{ "Easy", 0x48 },
		{ "Medium", 0x54 },
		{ "Amazing", 0x60 }
	};
	const size_t diffsz = sizeof(diffv) / sizeof(*diffv);

	const float past = -0.3f;
	const float future = 3.0f;
	const float timescale = 60.0f;
	const float texCoordStep = -0.5f; // Two beat lines per neck texture => 0.5 tex units per beat
	// Note: t is difference from playback time so it must be in range [past, future]
	float time2y(float t) { return -timescale * (t - past) / (future - past); }
	float time2a(float t) { return 1.0f - t / future; } // Note: we want 1.0 alpha already at zero t.
	bool fretPressed[5] = {};
	bool picked = false;
}

GuitarGraph::GuitarGraph(Song const& song): m_song(song), m_button("button.svg"), m_pickValue(0.0, 5.0), m_instrument(), m_level(), m_text(getThemePath("sing_timetxt.svg"), config["graphic/text_lod"].f()), m_score() {
	std::size_t tracks = m_song.tracks.size();
	if (tracks == 0) throw std::runtime_error("No tracks");
	m_necks.push_back(new Texture("guitarneck.svg"));
	if (tracks > 1) m_necks.push_back(new Texture("bassneck.svg"));
	if (tracks > 2) m_necks.push_back(new Texture("bassneck.svg")); // Drums
	difficultyAuto();
}

void GuitarGraph::inputProcess() {
	for (Joysticks::iterator it = joysticks.begin(); it != joysticks.end(); ++it) {
		for (JoystickEvent ev; it->second.tryPollEvent(ev); ) {
			// RockBand pick event
			if (ev.type == JoystickEvent::HAT_MOTION && ev.hat_direction != JoystickEvent::CENTERED) picked = true;
			// GuitarHero pick event
			if (ev.type == JoystickEvent::AXIS_MOTION && ev.axis_id == 5 && ev.axis_value != 0) picked = true;
			if (ev.type != JoystickEvent::BUTTON_DOWN && ev.type != JoystickEvent::BUTTON_UP) continue;
			unsigned b = ev.button_id;
			if (b >= 5) continue;
			static const int gh[] = { 2, 0, 1, 3, 4 };
			static const int rb[] = { 3, 0, 1, 2, 4 };
			fretPressed[(it->second.getType() == Joystick::ROCKBAND ? rb : gh)[b]] = (ev.type == JoystickEvent::BUTTON_DOWN);
		}
	}
}

void GuitarGraph::engine(double time) {
	if (!picked) return; // TODO: hammering etc. later, remove this
	picked = false;
	if (time < -0.5) {
		if (fretPressed[4]) {
			m_instrument = (m_instrument + 1) % m_song.tracks.size();
			if (!difficulty(m_level)) difficultyAuto();
		}
		if (fretPressed[0]) difficulty(DIFFICULTY_SUPAEASY);
		else if (fretPressed[1]) difficulty(DIFFICULTY_EASY);
		else if (fretPressed[2]) difficulty(DIFFICULTY_MEDIUM);
		else if (fretPressed[3]) difficulty(DIFFICULTY_AMAZING);
		return;
	}
	m_pickValue.setValue(1.0);
	for (NoteStatus::iterator it = m_notes.begin(); it != m_notes.end(); ++it) if (it->second == 1) it->second = 2;
	int basepitch = diffv[m_level].basepitch;
	Duration const* dIt[5] = {};
	double begin = getNaN();
	double tolerance = 0.2;
	for (int fret = 0; fret < 5; ++fret) {
		NoteMap const& nm = m_song.tracks[m_instrument].nm;
		NoteMap::const_iterator it = nm.find(basepitch + fret);
		if (it == nm.end()) continue;
		Durations const& dur = it->second;
		Durations::const_iterator it2 = dur.begin(), it2tmp, it2end = dur.end();
		// Find any suitable note within the tolerance
		while (it2 != it2end && (it2->begin < time - tolerance || m_notes[&*it2] != 0)) ++it2;
		// If we are already past the accepted region, skip further processing
		if (it2 == it2end || it2->begin > time + tolerance) continue;
		// Find the note with the smallest error
		it2tmp = it2;
		do {
			it2 = it2tmp;
			if (m_notes[&*it2] != 0) continue; // Notes already played are ignored
			dIt[fret] = &*it2;
			begin = it2->begin;
			tolerance = std::abs(begin - time);
		} while (++it2tmp != it2end && std::abs(it2tmp->begin - time) < tolerance);
	}
	bool need[5] = {};
	int count = 0;
	for (int fret = 0; fret < 5; ++fret) {
		if (!dIt[fret] || dIt[fret]->begin != begin) continue;
		need[fret] = true;
		++count;
	}
	bool shadowed = (count == 1); // If the chord only requires one fret, frets on the left side of that are "shadowed" (ignored)
	for (int fret = 0; fret < 5; ++fret) {
		if (need[fret] && !fretPressed[fret]) return; // Fret missing -> fail
		if (need[fret]) shadowed = false;
		if (!shadowed && fretPressed[fret] && !need[fret]) return; // Pressing non-shadowed fret that is not needed -> fail
	}
	// Okay, got this far, we have a match, let's score it
	for (int fret = 0; fret < 5; ++fret) {
		if (!need[fret]) continue;
		m_notes[dIt[fret]] = 1;
		m_score += 15;
		if (tolerance < 0.1) m_score += 15;
		if (tolerance < 0.05) m_score += 15;
		if (tolerance < 0.03) m_score += 5;
	}
}

void GuitarGraph::difficultyAuto() {
	for (int level = 0; level < DIFFICULTYCOUNT; ++level) if (difficulty(Difficulty(level))) return;
	throw std::runtime_error("No difficulty levels found");
}

bool GuitarGraph::difficulty(Difficulty level) {
	uint8_t basepitch = diffv[level].basepitch;
	NoteMap const& nm = m_song.tracks[m_instrument].nm;
	int fail = 0;
	for (int fret = 0; fret < 5; ++fret) if (nm.find(basepitch + fret) == nm.end()) ++fail;
	if (fail == 5) return false;
	m_level = level;
	return true;
}

void GuitarGraph::draw(double time) {
	if (time < -0.5) {
		std::string txt = "Play a fret to change:\n";
		txt += m_song.tracks[m_instrument].name + "/" + diffv[m_level].name;
		m_text.dimensions.screenBottom(-0.05).middle(0.0);
		m_text.draw(txt);
	} else {
		m_text.dimensions.screenBottom(-0.1).middle(0.2);
		m_text.draw(boost::lexical_cast<std::string>(m_score));
	}
	engine(time);
	Dimensions dimensions(1.0); // FIXME: bogus aspect ratio (is this fixable?)
	dimensions.screenBottom().fixedWidth(0.5f);
	glutil::PushMatrix pmb;
	glTranslatef(0.5 * (dimensions.x1() + dimensions.x2()), dimensions.y2(), 0.0f);
	glRotatef(85.0f, 1.0f, 0.0f, 0.0f);
	{ float s = dimensions.w() / 5.0f; glScalef(s, s, s); }
	// Draw the neck
	{
		UseTexture tex(m_necks[m_instrument]);
		glutil::Begin block(GL_TRIANGLE_STRIP);
		float texCoord = 0.0f;
		float tBeg = 0.0f, tEnd;
		for (Song::Beats::const_iterator it = m_song.beats.begin(); it != m_song.beats.end() && tBeg < future; ++it, texCoord += texCoordStep, tBeg = tEnd) {
			tEnd = *it - time;
			//if (tEnd < past) continue;
			if (tEnd > future) {
				// Crop the end off
				texCoord -= texCoordStep * (tEnd - future) / (tEnd - tBeg);
				tEnd = future;
			}
			glColor4f(1.0f, 1.0f, 1.0f, time2a(tEnd));
			glTexCoord2f(0.0f, texCoord); glVertex2f(-2.5f, time2y(tEnd));
			glTexCoord2f(1.0f, texCoord); glVertex2f(2.5f, time2y(tEnd));
		}
	}
	int basepitch = diffv[m_level].basepitch;

	static glutil::Color fretColors[5] = {
		glutil::Color(0.0f, 1.0f, 0.0f),
		glutil::Color(1.0f, 0.0f, 0.0f),
		glutil::Color(1.0f, 1.0f, 0.0f),
		glutil::Color(0.0f, 0.0f, 1.0f),
		glutil::Color(1.0f, 0.5f, 0.0f)
	};
	// Draw the notes
	NoteMap const& nm = m_song.tracks[m_instrument].nm;
	for (int fret = 0; fret < 5; ++fret) {
		float x = -2.0f + fret;
		float w = 0.5f;
		NoteMap::const_iterator it = nm.find(basepitch + fret);
		if (it == nm.end()) continue;
		Durations const& durs = it->second;
		for (Durations::const_iterator it2 = durs.begin(); it2 != durs.end(); ++it2) {
			float tBeg = it2->begin - time;
			float tEnd = it2->end - time;
			if (tEnd < past) continue;
			if (tBeg > future) break;
			glutil::Color c = fretColors[fret];
			if (m_notes[&*it2] == 1) c.r = c.g = c.b = 1.0f;
			float wLine = 0.5f * w;
			if (tEnd > future) tEnd = future;
			{
				glutil::Begin block(GL_TRIANGLE_STRIP);
				c.a = time2a(tEnd); glColor4fv(c);
				glVertex2f(x - wLine, time2y(tEnd));
				glVertex2f(x + wLine, time2y(tEnd));
				c.a = time2a(tBeg); glColor4fv(c);
				glVertex2f(x - wLine, time2y(tBeg));
				glVertex2f(x + wLine, time2y(tBeg));
			}
			m_button.dimensions.center(time2y(tBeg)).middle(x);
			m_button.draw();
		}
	}
	{
		float pl = m_pickValue.get();
		glColor3f(pl, pl, pl);
	}
	// Draw the cursor
	{	
		glutil::Begin block(GL_TRIANGLE_STRIP);
		glVertex2f(-2.5f, time2y(0.01f));
		glVertex2f(2.5f, time2y(0.01f));
		glVertex2f(-2.5f, time2y(-0.01f));
		glVertex2f(2.5f, time2y(-0.01f));
	}
	// Fret buttons on cursor
	for (int fret = 0; fret < 5; ++fret) {
		float x = -2.0f + fret;
		glutil::Color c = fretColors[fret];
		if (fretPressed[fret]) {
			c.r = 1.0f;
			c.g = 1.0f;
			c.b = 1.0f;
		}
		glColor4fv(c);
		m_button.dimensions.center(time2y(0.0)).middle(x);
		m_button.draw();
	}
	glColor3f(1.0f, 1.0f, 1.0f);
}
