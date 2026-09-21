#include "gCanvas.h"

#include <algorithm>
#include <cmath>

#include "gRenderer.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include "gAppManager.h"
#endif

static const float PI2 = 6.2831853f;

// r, g, b per palette slot
static const int PALETTE[][3] = {
	{ 64, 255, 224},  // 0 cyan (player)
	{255,  64, 168},  // 1 magenta (chaser)
	{255, 168,  48},  // 2 orange (drifter)
	{168,  96, 255},  // 3 purple (heavy)
	{255, 255, 255},  // 4 white
};

static const int ENEMY_COLOR[] = {1, 2, 3};
static const int ENEMY_SIDES[] = {3, 4, 6};
static const int ENEMY_HP[] = {2, 2, 6};
static const int ENEMY_SCORE[] = {20, 30, 100};

// Narrowest the world is allowed to get, in units. The widest thing the HUD
// draws is the touch hint line, at a little over 700.
static const int MIN_UNIT_WIDTH = 900;

static const float BOOT_FADEIN = 0.5f;
static const float BOOT_FADEOUT = 2.4f;
static const float BOOT_END = 3.0f;

// mute toggle in the top right corner, in world units
static const float MUTE_SIZE = 30.0f;
static const float MUTE_MARGIN = 22.0f;
static const float MUTE_TOP = 54.0f;
// fingers are wider than the icon
static const float MUTE_TOUCH_PAD = 14.0f;

// A bullet's speed and how long it lives, so the auto aim can work out whether
// it could reach a target at all before deciding to lock onto it.
static const float BULLET_SPEED = 750.0f;
static const float BULLET_LIFE = 1.2f;

// virtual joystick, in world units
static const float JOY_RADIUS = 100.0f;
static const float JOY_KNOB = 30.0f;
static const float JOY_ACCEL = 7000.0f;

gCanvas::gCanvas(gApp* root) : gBaseCanvas(root) {
	this->root = root;
	state = STATE_BOOT;
	gametime = 0.0f;
	statetime = 0.0f;
	shipangle = 0.0f;
	keyleft = keyright = keyup = keydown = false;
	firemouse = firekey = false;
	touchmode = false;
	followmouse = true;
	movefinger = shootfinger = -1;
	spawntimer = 0.0f;
	firetimer = 0.0f;
	shake = 0.0f;
	nextenemyid = 0;
	targetid = -1;
	score = 0;
	killscore = 0;
	best = 0;
}

gCanvas::~gCanvas() {
}

// Fixed logical height: everyone sees the same 720 world units vertically,
// wider screens see proportionally more world horizontally. Rendering stays
// at native resolution, only the coordinate system is logical.
//
// A phone held upright is the exception. At 720 units tall it is only about 330
// units across, which is narrower than the title, the control hints and the
// boot logo, so below MIN_UNIT_WIDTH the world is sized by its width instead
// and gets taller rather than wider.
void gCanvas::fitUnits() {
	float pw = (float)renderer->getScreenWidth();
	float ph = (float)renderer->getScreenHeight();
	if (pw <= 0.0f || ph <= 0.0f) return;
	int unitw = (int)std::round(720.0f * pw / ph);
	int unith = 720;
	if (unitw < MIN_UNIT_WIDTH) {
		unitw = MIN_UNIT_WIDTH;
		unith = (int)std::round(MIN_UNIT_WIDTH * ph / pw);
	}
	renderer->setUnitScreenSize(unitw, unith);
}

// The browser window is the only display the game has on the web, so the
// drawing surface follows it: on rotation, on a resized window, and on the way
// in, since the engine's own idea of its size is whatever it was started with.
// Without this a phone gets a canvas the size of a desktop window and the game
// runs off the edge of the screen.
void gCanvas::fitToPage() {
#ifdef __EMSCRIPTEN__
	int w = EM_ASM_INT({ return window.innerWidth; });
	int h = EM_ASM_INT({ return window.innerHeight; });
	if (w > 0 && h > 0) appmanager->setWindowSize(w, h);
#endif
	fitUnits();
}

bool gCanvas::isOnScreen(const glm::vec2& p) {
	return p.x > -20.0f && p.x < getWidth() + 20.0f &&
	       p.y > -20.0f && p.y < getHeight() + 20.0f;
}

// How long a bullet fired now would take to meet an enemy, or -1 when it cannot
// reach it at all before it expires. Solving it rather than aiming at where the
// enemy currently is matters for the drifters: they cross at up to 280 units a
// second, so by the time a bullet arrives they are most of a screen away from
// where they were aimed at.
float gCanvas::interceptTime(const Enemy& e) const {
	glm::vec2 d = e.pos - playerpos;
	float a = glm::dot(e.vel, e.vel) - BULLET_SPEED * BULLET_SPEED;
	float b = 2.0f * glm::dot(d, e.vel);
	float c = glm::dot(d, d);

	float t;
	if (std::abs(a) < 0.001f) {
		// target moving at exactly bullet speed, the quadratic degenerates
		if (std::abs(b) < 0.000001f) return -1.0f;
		t = -c / b;
	} else {
		float disc = b * b - 4.0f * a * c;
		if (disc < 0.0f) return -1.0f;
		float root = std::sqrt(disc);
		float t1 = (-b + root) / (2.0f * a);
		float t2 = (-b - root) / (2.0f * a);
		if (t1 < 0.0f) t1 = BULLET_LIFE * 10.0f;
		if (t2 < 0.0f) t2 = BULLET_LIFE * 10.0f;
		t = std::min(t1, t2);
	}
	if (t <= 0.0f || t > BULLET_LIFE) return -1.0f;
	return t;
}

bool gCanvas::isOnMuteButton(float x, float y) {
	if (state == STATE_BOOT) return false;
	return x >= getWidth() - MUTE_MARGIN - MUTE_SIZE - MUTE_TOUCH_PAD &&
	       x <= getWidth() - MUTE_MARGIN + MUTE_TOUCH_PAD &&
	       y >= MUTE_TOP - MUTE_TOUCH_PAD &&
	       y <= MUTE_TOP + MUTE_SIZE + MUTE_TOUCH_PAD;
}

void gCanvas::setup() {
#ifdef __EMSCRIPTEN__
	emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, EM_FALSE,
	                               [](int type, const EmscriptenUiEvent* event, void* canvas) -> EM_BOOL {
		                               ((gCanvas*)canvas)->fitToPage();
		                               return EM_TRUE;
	                               });
#endif
	fitToPage();
	uifont.loadFont("FreeSansBold.ttf", 18);
	midfont.loadFont("FreeSansBold.ttf", 28);
	titlefont.loadFont("FreeSansBold.ttf", 64);
	logo.loadImage("glistengine_logo.png");
	audio.setup();
	playerpos = glm::vec2(getWidth() / 2.0f, getHeight() / 2.0f);
	playervel = glm::vec2(0.0f);
	mousepos = playerpos;
}

void gCanvas::startGame() {
	audio.play(gAudio::SFX_START);
	state = STATE_PLAYING;
	statetime = 0.0f;
	gametime = 0.0f;
	score = 0;
	killscore = 0;
	spawntimer = 1.2f;
	firetimer = 0.0f;
	shake = 0.0f;
	firemouse = firekey = false;
	movefinger = shootfinger = -1;
	followmouse = true;
	joyoffset = glm::vec2(0.0f);
	targetid = -1;
	enemies.clear();
	bullets.clear();
	trail.clear();
	playerpos = glm::vec2(getWidth() / 2.0f, getHeight() / 2.0f);
	playervel = glm::vec2(0.0f);
	mousepos = playerpos;
	shipangle = -PI2 / 4.0f;
}

void gCanvas::killPlayer() {
	audio.play(gAudio::SFX_DEATH);
	best = std::max(best, score);
	state = STATE_GAMEOVER;
	statetime = 0.0f;
	shake = 22.0f;
	spawnBurst(playerpos, 0, 90, 430.0f, 3.5f);
	spawnBurst(playerpos, 4, 30, 260.0f, 2.4f);
}

void gCanvas::confirmPressed() {
	if (state == STATE_BOOT) {
		state = STATE_TITLE;
		statetime = 0.0f;
		audio.startMusic();
	} else if (state == STATE_TITLE) {
		startGame();
	} else if (state == STATE_GAMEOVER && statetime > 0.6f) {
		startGame();
	}
}

void gCanvas::update() {
	float dt = (float)root->getElapsedTime();
	if (dt > 0.05f) dt = 0.05f;
	if (dt <= 0.0f) return;

	statetime += dt;
	if (state == STATE_BOOT) {
		if (statetime >= BOOT_END) {
			state = STATE_TITLE;
			statetime = 0.0f;
			audio.startMusic();
		}
		return;
	}

	if (state == STATE_PLAYING) {
		gametime += dt;
		score = (int)(gametime * 10.0f) + killscore;
		updatePlayer(dt);
		updateShooting(dt);
	}
	updateWorld(dt);
	shake = std::max(0.0f, shake - dt * 34.0f);
}

void gCanvas::updatePlayer(float dt) {
	glm::vec2 keydir(0.0f);
	if (keyleft) keydir.x -= 1.0f;
	if (keyright) keydir.x += 1.0f;
	if (keyup) keydir.y -= 1.0f;
	if (keydown) keydir.y += 1.0f;

	if (keydir.x != 0.0f || keydir.y != 0.0f) {
		playervel += glm::normalize(keydir) * 5000.0f * dt;
	} else if (touchmode) {
		// analog virtual joystick; no finger means the ship glides to a stop
		if (movefinger != -1) {
			playervel += joyoffset * (JOY_ACCEL / JOY_RADIUS) * dt;
		}
	} else if (followmouse) {
		playervel += (mousepos - playerpos) * 18.0f * dt;
	}
	playervel *= std::pow(0.86f, dt * 60.0f);

	float speed = glm::length(playervel);
	if (speed > 900.0f) playervel *= 900.0f / speed;
	playerpos += playervel * dt;
	playerpos.x = std::clamp(playerpos.x, 12.0f, getWidth() - 12.0f);
	playerpos.y = std::clamp(playerpos.y, 12.0f, getHeight() - 12.0f);

	if (speed > 30.0f) shipangle = std::atan2(playervel.y, playervel.x);

	// engine sparks
	glm::vec2 dir(std::cos(shipangle), std::sin(shipangle));
	glm::vec2 side(-dir.y, dir.x);
	for (int i = 0; i < 2; i++) {
		if (particles.size() > 800) break;
		Particle p;
		p.pos = playerpos - dir * 12.0f + side * gRandomf() * 4.0f;
		p.vel = -dir * (120.0f + gRandom(60.0f)) + side * gRandomf() * 40.0f;
		p.maxlife = p.life = 0.3f + gRandom(0.15f);
		p.size = 2.2f;
		p.color = 0;
		particles.push_back(p);
	}

	// trail
	TrailPoint tp;
	tp.pos = playerpos;
	tp.life = 1.0f;
	trail.push_front(tp);
	if (trail.size() > 48) trail.pop_back();
}

void gCanvas::updateShooting(float dt) {
	firetimer -= dt;
	if (firetimer > 0.0f) return;
	if (!firemouse && !firekey && shootfinger == -1) return;

	glm::vec2 dir(std::cos(shipangle), std::sin(shipangle));
	// space and the touch trigger only say "fire", so the aim picks its own
	// target and stays locked on it until it dies or leaves the screen,
	// otherwise the shots flick between enemies on a crowded field. A mouse
	// click carries a direction of its own and aims at the cursor instead.
	if (touchmode || firekey) {
		// The lock is kept while the bullet can still catch it, and picked by
		// soonest intercept rather than nearest. Nearest used to hand the lock
		// to whatever drifter happened to be crossing close by, which is the one
		// thing on the field a bullet often cannot reach in time.
		const Enemy* target = nullptr;
		float targettime = -1.0f;
		for (const Enemy& e : enemies) {
			if (e.id != targetid || !isOnScreen(e.pos)) continue;
			float t = interceptTime(e);
			if (t > 0.0f) {
				target = &e;
				targettime = t;
			}
			break;
		}
		if (target == nullptr) {
			float besttime = 0.0f;
			for (const Enemy& e : enemies) {
				if (!isOnScreen(e.pos)) continue;
				float t = interceptTime(e);
				if (t < 0.0f) continue;
				if (target == nullptr || t < besttime) {
					besttime = t;
					target = &e;
				}
			}
			targetid = target != nullptr ? target->id : -1;
			targettime = besttime;
		}
		if (target != nullptr) {
			// lead it: aim where it will be when the bullet arrives
			glm::vec2 d = target->pos + target->vel * targettime - playerpos;
			float len = glm::length(d);
			if (len > 1.0f) dir = d / len;
		}
	} else {
		glm::vec2 aim = mousepos - playerpos;
		float len = glm::length(aim);
		// with the cursor on the ship there is no direction to take, keep facing
		if (len >= 25.0f) dir = aim / len;
	}

	Bullet b;
	b.pos = playerpos + dir * 14.0f;
	b.vel = dir * BULLET_SPEED;
	b.life = BULLET_LIFE;
	bullets.push_back(b);
	spawnBurst(b.pos, 0, 2, 60.0f, 1.8f);
	audio.play(gAudio::SFX_SHOOT);
	firetimer = 0.14f;
}

void gCanvas::updateWorld(float dt) {
	// spawning
	spawntimer -= dt;
	if (spawntimer <= 0.0f) {
		if (state == STATE_PLAYING) {
			spawnEnemy();
			spawntimer = std::max(0.32f, 1.0f - gametime * 0.011f) * (0.8f + gRandom(0.4f));
		} else if (enemies.size() < 12) {
			spawnEnemy();
			spawntimer = 0.8f;
		} else {
			spawntimer = 0.5f;
		}
	}

	// enemies
	for (size_t i = 0; i < enemies.size();) {
		Enemy& e = enemies[i];
		if (e.type == ENEMY_CHASER && state == STATE_PLAYING) {
			float chasespeed = std::min(130.0f + gametime * 2.0f, 260.0f);
			glm::vec2 tothis = playerpos - e.pos;
			float len = glm::length(tothis);
			if (len > 1.0f) {
				glm::vec2 desired = tothis * (chasespeed / len);
				e.vel += (desired - e.vel) * std::min(1.0f, 2.2f * dt);
			}
			if (glm::length(e.vel) > 20.0f) e.angle = std::atan2(e.vel.y, e.vel.x);
		} else if (e.type == ENEMY_HEAVY && state == STATE_PLAYING) {
			glm::vec2 tothis = playerpos - e.pos;
			float len = glm::length(tothis);
			if (len > 1.0f) {
				glm::vec2 desired = tothis * (80.0f / len);
				e.vel += (desired - e.vel) * std::min(1.0f, 0.35f * dt);
			}
		}
		e.pos += e.vel * dt;
		e.angle += e.spin * dt;
		e.pulse += dt;

		bool out = e.pos.x < -250.0f || e.pos.x > getWidth() + 250.0f ||
		           e.pos.y < -250.0f || e.pos.y > getHeight() + 250.0f;
		if (out) {
			enemies[i] = enemies.back();
			enemies.pop_back();
			continue;
		}

		if (state == STATE_PLAYING) {
			float hitdist = e.radius * 0.75f + 7.0f;
			glm::vec2 d = e.pos - playerpos;
			if (d.x * d.x + d.y * d.y < hitdist * hitdist) {
				killPlayer();
			}
		}
		i++;
	}

	// push overlapping enemies apart, heavier shapes budge less
	for (size_t i = 0; i < enemies.size(); i++) {
		for (size_t j = i + 1; j < enemies.size(); j++) {
			Enemy& a = enemies[i];
			Enemy& b = enemies[j];
			float mindist = (a.radius + b.radius) * 0.9f;
			glm::vec2 d = b.pos - a.pos;
			float dist2 = d.x * d.x + d.y * d.y;
			if (dist2 >= mindist * mindist) continue;
			float dist = std::sqrt(dist2);
			glm::vec2 dir = dist > 0.001f ? d / dist : glm::vec2(1.0f, 0.0f);
			float overlap = mindist - dist;
			float total = a.radius + b.radius;
			a.pos -= dir * (overlap * b.radius / total);
			b.pos += dir * (overlap * a.radius / total);
		}
	}

	// bullets
	for (size_t i = 0; i < bullets.size();) {
		Bullet& b = bullets[i];
		b.pos += b.vel * dt;
		b.life -= dt;
		bool dead = b.life <= 0.0f ||
		            b.pos.x < -40.0f || b.pos.x > getWidth() + 40.0f ||
		            b.pos.y < -40.0f || b.pos.y > getHeight() + 40.0f;
		if (!dead) {
			for (size_t j = 0; j < enemies.size(); j++) {
				Enemy& e = enemies[j];
				float hitdist = e.radius * 0.9f + 4.0f;
				glm::vec2 d = e.pos - b.pos;
				if (d.x * d.x + d.y * d.y >= hitdist * hitdist) continue;
				dead = true;
				e.hp--;
				if (e.hp <= 0) {
					audio.play(gAudio::SFX_EXPLODE);
					spawnBurst(e.pos, ENEMY_COLOR[e.type], 30, 260.0f, 2.8f);
					killscore += ENEMY_SCORE[e.type];
					shake = std::max(shake, 5.0f);
					enemies[j] = enemies.back();
					enemies.pop_back();
				} else {
					audio.play(gAudio::SFX_HIT);
					spawnBurst(b.pos, ENEMY_COLOR[e.type], 5, 120.0f, 2.0f);
				}
				break;
			}
		}
		if (dead) {
			bullets[i] = bullets.back();
			bullets.pop_back();
			continue;
		}
		i++;
	}

	// particles
	for (size_t i = 0; i < particles.size();) {
		Particle& p = particles[i];
		p.pos += p.vel * dt;
		p.vel *= std::pow(0.92f, dt * 60.0f);
		p.life -= dt;
		if (p.life <= 0.0f) {
			particles[i] = particles.back();
			particles.pop_back();
			continue;
		}
		i++;
	}

	// trail fade
	for (TrailPoint& tp : trail) tp.life -= dt * 2.2f;
	while (!trail.empty() && trail.back().life <= 0.0f) trail.pop_back();
}

void gCanvas::spawnEnemy() {
	Enemy e;

	// pick a random edge, a bit outside the screen
	int edge = (int)gRandom(3.999f);
	float w = (float)getWidth(), h = (float)getHeight();
	if (edge == 0) e.pos = glm::vec2(gRandom(w), -60.0f);
	else if (edge == 1) e.pos = glm::vec2(gRandom(w), h + 60.0f);
	else if (edge == 2) e.pos = glm::vec2(-60.0f, gRandom(h));
	else e.pos = glm::vec2(w + 60.0f, gRandom(h));

	if (state == STATE_PLAYING) {
		float r = gRandom(1.0f);
		e.type = r < 0.5f ? ENEMY_CHASER : (r < 0.82f ? ENEMY_DRIFTER : ENEMY_HEAVY);
	} else {
		e.type = ENEMY_DRIFTER;  // ambient background traffic
	}

	glm::vec2 target = state == STATE_PLAYING ? playerpos : glm::vec2(w / 2.0f, h / 2.0f);
	target += glm::vec2(gRandomf() * 160.0f, gRandomf() * 160.0f);
	glm::vec2 dir = target - e.pos;
	float len = glm::length(dir);
	if (len < 1.0f) dir = glm::vec2(1.0f, 0.0f); else dir /= len;

	if (e.type == ENEMY_CHASER) {
		e.radius = 14.0f;
		e.vel = dir * 130.0f;
		e.spin = 0.0f;
	} else if (e.type == ENEMY_DRIFTER) {
		e.radius = 12.0f;
		e.vel = dir * (200.0f + gRandom(80.0f));
		e.spin = gRandomf() * 3.5f;
	} else {
		e.radius = 34.0f;
		e.vel = dir * 80.0f;
		e.spin = 0.6f * (gRandomf() < 0.0f ? -1.0f : 1.0f);
	}
	e.angle = gRandom(PI2);
	e.pulse = gRandom(PI2);
	e.hp = ENEMY_HP[e.type];
	e.id = nextenemyid++;
	enemies.push_back(e);

	// small pop where it appears
	spawnBurst(e.pos, ENEMY_COLOR[e.type], 6, 90.0f, 2.0f);
}

void gCanvas::spawnBurst(const glm::vec2& p, int color, int count, float speed, float size) {
	for (int i = 0; i < count; i++) {
		if (particles.size() > 800) return;
		Particle pt;
		float a = gRandom(PI2);
		float s = speed * (0.25f + gRandom(0.75f));
		pt.pos = p;
		pt.vel = glm::vec2(std::cos(a) * s, std::sin(a) * s);
		pt.maxlife = pt.life = 0.4f + gRandom(0.5f);
		pt.size = size;
		pt.color = color;
		particles.push_back(pt);
	}
}

void gCanvas::draw() {
	clearColor(6, 7, 14);

	if (state == STATE_BOOT) {
		enableAlphaBlending();
		drawBoot();
		return;
	}

	float ox = gRandomf() * shake;
	float oy = gRandomf() * shake;

	enableAlphaBlending();
	drawGrid(ox, oy);

	setBlendMode(gRenderer::BLENDMODE_ADDITIVE);
	drawTrail(ox, oy);
	drawParticles(ox, oy);
	drawBullets(ox, oy);
	drawEnemies(ox, oy);
	if (state == STATE_PLAYING) drawShip(ox, oy);

	// virtual joystick, drawn without shake so it stays under the finger
	if (state == STATE_PLAYING && movefinger != -1) {
		setColor(64, 255, 224, 14);
		gDrawCircle(joyanchor.x, joyanchor.y, JOY_RADIUS, true, 48);
		setColor(64, 255, 224, 60);
		gDrawCircle(joyanchor.x, joyanchor.y, JOY_RADIUS, false, 48);
		gDrawCircle(joyanchor.x, joyanchor.y, JOY_RADIUS - 1.5f, false, 48);
		setColor(64, 255, 224, 110);
		gDrawCircle(joyanchor.x + joyoffset.x, joyanchor.y + joyoffset.y, JOY_KNOB, true, 24);
	}
	setBlendMode(gRenderer::BLENDMODE_ALPHA);

	drawHud();
}

void gCanvas::drawBoot() {
	float a;
	if (statetime < BOOT_FADEIN) {
		a = statetime / BOOT_FADEIN;
	} else if (statetime > BOOT_FADEOUT) {
		a = std::max(0.0f, 1.0f - (statetime - BOOT_FADEOUT) / (BOOT_END - BOOT_FADEOUT));
	} else {
		a = 1.0f;
	}

	float lw = std::min(500.0f, getWidth() * 0.7f);
	float lh = lw * logo.getHeight() / logo.getWidth();
	float x = (getWidth() - lw) / 2.0f;
	float y = (getHeight() - lh) / 2.0f + 10.0f;

	setColor(160, 190, 220, (int)(220.0f * a));
	drawCenteredText(midfont, "made with", y - 40.0f);

	setColor(255, 255, 255, (int)(255.0f * a));
	logo.draw(x, y, lw, lh);
}

void gCanvas::drawGrid(float ox, float oy) {
	setColor(36, 52, 110, 70);
	float cell = 64.0f;
	for (float x = -cell; x < getWidth() + cell; x += cell) {
		gDrawLine(x + ox, oy - cell, x + ox, getHeight() + oy + cell);
	}
	for (float y = -cell; y < getHeight() + cell; y += cell) {
		gDrawLine(ox - cell, y + oy, getWidth() + ox + cell, y + oy);
	}
}

void gCanvas::drawTrail(float ox, float oy) {
	const int* c = PALETTE[0];
	int n = (int)trail.size();
	for (int i = n - 1; i >= 0; i--) {
		const TrailPoint& tp = trail[i];
		if (tp.life <= 0.0f) continue;
		setColor(c[0], c[1], c[2], (int)(60.0f * tp.life));
		gDrawCircle(tp.pos.x + ox, tp.pos.y + oy, 2.0f + 8.0f * tp.life, true, 12);
	}
}

void gCanvas::drawParticles(float ox, float oy) {
	for (const Particle& p : particles) {
		const int* c = PALETTE[p.color];
		float t = p.life / p.maxlife;
		setColor(c[0], c[1], c[2], (int)(200.0f * t));
		gDrawCircle(p.pos.x + ox, p.pos.y + oy, p.size * (0.5f + t), true, 8);
	}
}

void gCanvas::drawBullets(float ox, float oy) {
	const int* c = PALETTE[0];
	for (const Bullet& b : bullets) {
		float x = b.pos.x + ox, y = b.pos.y + oy;
		setColor(c[0], c[1], c[2], 40);
		gDrawCircle(x, y, 7.0f, true, 10);
		setColor(c[0], c[1], c[2], 130);
		gDrawLine(x, y, x - b.vel.x * 0.02f, y - b.vel.y * 0.02f, 2.0f);
		setColor(255, 255, 255, 230);
		gDrawCircle(x, y, 2.6f, true, 8);
	}
}

void gCanvas::drawEnemies(float ox, float oy) {
	for (const Enemy& e : enemies) {
		float r = e.radius * (1.0f + 0.08f * std::sin(e.pulse * 5.0f));
		drawPolyGlow(e.pos.x + ox, e.pos.y + oy, r, ENEMY_SIDES[e.type], e.angle,
		             ENEMY_COLOR[e.type], 1.0f);
	}
}

void gCanvas::drawPolyGlow(float x, float y, float radius, int sides, float angle, int color, float alphascale) {
	const int* c = PALETTE[color];

	setColor(c[0], c[1], c[2], (int)(26.0f * alphascale));
	gDrawCircle(x, y, radius * 2.1f, true, 24);

	setColor(c[0], c[1], c[2], (int)(60.0f * alphascale));
	for (int i = 0; i < sides; i++) {
		float a1 = angle + PI2 * i / sides;
		float a2 = angle + PI2 * (i + 1) / sides;
		gDrawTriangle(x, y,
		              x + std::cos(a1) * radius, y + std::sin(a1) * radius,
		              x + std::cos(a2) * radius, y + std::sin(a2) * radius);
	}

	setColor(c[0], c[1], c[2], (int)(235.0f * alphascale));
	for (int i = 0; i < sides; i++) {
		float a1 = angle + PI2 * i / sides;
		float a2 = angle + PI2 * (i + 1) / sides;
		gDrawLine(x + std::cos(a1) * radius, y + std::sin(a1) * radius,
		          x + std::cos(a2) * radius, y + std::sin(a2) * radius, 2.5f);
	}
}

void gCanvas::drawShip(float ox, float oy) {
	const int* c = PALETTE[0];
	float x = playerpos.x + ox, y = playerpos.y + oy;
	glm::vec2 dir(std::cos(shipangle), std::sin(shipangle));
	glm::vec2 side(-dir.y, dir.x);
	glm::vec2 p(x, y);
	glm::vec2 nose = p + dir * 15.0f;
	glm::vec2 left = p - dir * 9.0f + side * 9.0f;
	glm::vec2 right = p - dir * 9.0f - side * 9.0f;
	glm::vec2 tail = p - dir * 4.0f;

	setColor(c[0], c[1], c[2], 26);
	gDrawCircle(x, y, 30.0f, true, 24);
	setColor(c[0], c[1], c[2], 45);
	gDrawCircle(x, y, 16.0f, true, 16);

	setColor(c[0], c[1], c[2], 90);
	gDrawTriangle(nose.x, nose.y, left.x, left.y, tail.x, tail.y);
	gDrawTriangle(nose.x, nose.y, tail.x, tail.y, right.x, right.y);

	setColor(c[0], c[1], c[2], 255);
	gDrawLine(nose.x, nose.y, left.x, left.y, 2.5f);
	gDrawLine(left.x, left.y, tail.x, tail.y, 2.5f);
	gDrawLine(tail.x, tail.y, right.x, right.y, 2.5f);
	gDrawLine(right.x, right.y, nose.x, nose.y, 2.5f);
}

void gCanvas::drawCenteredText(gFont& font, const std::string& text, float y) {
	font.drawText(text, (getWidth() - font.getStringWidth(text)) / 2.0f, y);
}

void gCanvas::drawHud() {
	float cx = getWidth() / 2.0f;
	float cy = getHeight() / 2.0f;

	drawMuteButton();

	if (state == STATE_PLAYING) {
		setColor(220, 245, 255, 235);
		uifont.drawText("SCORE " + std::to_string(score), 24.0f, 38.0f);
		std::string btext = "BEST " + std::to_string(best);
		uifont.drawText(btext, getWidth() - uifont.getStringWidth(btext) - 24.0f, 38.0f);
		if (gametime < 5.0f) {
			int a = (int)(200.0f * std::min(1.0f, 5.0f - gametime));
			setColor(160, 190, 220, a);
			drawCenteredText(uifont, "mouse or WASD moves, click aims, SPACE auto-aims", getHeight() - 30.0f);
		}
		return;
	}

	if (state == STATE_TITLE) {
		setColor(64, 255, 224, 255);
		drawCenteredText(titlefont, "NEON DODGER", cy - 60.0f);
		int pulse = 150 + (int)(100.0f * std::sin(statetime * 4.0f));
		setColor(255, 255, 255, pulse);
		drawCenteredText(midfont, "click or press SPACE to start", cy + 20.0f);
		setColor(160, 190, 220, 190);
		drawCenteredText(uifont, "mouse or WASD moves, click aims, SPACE auto-aims, M mutes", cy + 70.0f);
		drawCenteredText(uifont, "on touch: first finger steers, second finger fires at the nearest enemy", cy + 96.0f);
		if (best > 0) {
			setColor(220, 245, 255, 220);
			drawCenteredText(uifont, "BEST " + std::to_string(best), cy + 134.0f);
		}
		return;
	}

	// game over
	setColor(255, 64, 168, 255);
	drawCenteredText(titlefont, "WRECKED", cy - 60.0f);
	setColor(220, 245, 255, 235);
	drawCenteredText(midfont, "SCORE " + std::to_string(score) + "   BEST " + std::to_string(best), cy + 20.0f);
	if (statetime > 0.6f) {
		int pulse = 150 + (int)(100.0f * std::sin(statetime * 4.0f));
		setColor(255, 255, 255, pulse);
		drawCenteredText(uifont, "click or press SPACE to fly again", cy + 70.0f);
	}
}

// A speaker in a rounded box, cyan while sound is on and dimmed with a cross
// through it when it is off. Touch players have no M key, so this is the only
// way to silence the game on a phone.
void gCanvas::drawMuteButton() {
	bool on = !audio.isMuted();
	const int* c = PALETTE[on ? 0 : 4];
	int alpha = on ? 170 : 90;
	float x = getWidth() - MUTE_MARGIN - MUTE_SIZE;
	float y = MUTE_TOP;

	setColor(c[0], c[1], c[2], alpha / 4);
	gDrawRoundedRectangle(x, y, MUTE_SIZE, MUTE_SIZE, 5, true);
	setColor(c[0], c[1], c[2], alpha);
	gDrawRoundedRectangle(x, y, MUTE_SIZE, MUTE_SIZE, 5, false);

	// Speaker: a box with the cone flaring out of it, then either two waves or
	// a cross where the waves would be. sx is the middle of the speaker itself,
	// so the glyph runs from sx - 7 to sx + 9 and sits centered in the box.
	float sx = x + MUTE_SIZE / 2.0f - 1.0f;
	float sy = y + MUTE_SIZE / 2.0f;
	gDrawRectangle(sx - 7.0f, sy - 3.0f, 5.0f, 6.0f, true);
	gDrawTriangle(sx - 2.0f, sy - 3.0f, sx - 2.0f, sy + 3.0f, sx + 2.0f, sy + 7.0f, true);
	gDrawTriangle(sx - 2.0f, sy - 3.0f, sx + 2.0f, sy + 7.0f, sx + 2.0f, sy - 7.0f, true);
	if (on) {
		// one wave, not two: at this size a second one merges into the first
		gDrawLine(sx + 5.5f, sy - 4.5f, sx + 8.5f, sy, 2.0f);
		gDrawLine(sx + 8.5f, sy, sx + 5.5f, sy + 4.5f, 2.0f);
	} else {
		gDrawLine(sx + 5.0f, sy - 5.0f, sx + 10.0f, sy + 5.0f, 2.0f);
		gDrawLine(sx + 10.0f, sy - 5.0f, sx + 5.0f, sy + 5.0f, 2.0f);
	}
}

void gCanvas::keyPressed(int key) {
	// the keyboard takes over steering until the cursor is moved again
	if (key == G_KEY_A || key == G_KEY_W || key == G_KEY_S || key == G_KEY_D ||
	    key == G_KEY_LEFT || key == G_KEY_RIGHT || key == G_KEY_UP || key == G_KEY_DOWN) {
		followmouse = false;
	}
	if (key == G_KEY_A || key == G_KEY_LEFT) keyleft = true;
	else if (key == G_KEY_D || key == G_KEY_RIGHT) keyright = true;
	else if (key == G_KEY_W || key == G_KEY_UP) keyup = true;
	else if (key == G_KEY_S || key == G_KEY_DOWN) keydown = true;
	else if (key == G_KEY_M) audio.setMuted(!audio.isMuted());
	else if (key == G_KEY_SPACE) {
		if (state == STATE_PLAYING) firekey = true;
		else confirmPressed();
	}
}

void gCanvas::keyReleased(int key) {
	if (key == G_KEY_A || key == G_KEY_LEFT) keyleft = false;
	else if (key == G_KEY_D || key == G_KEY_RIGHT) keyright = false;
	else if (key == G_KEY_W || key == G_KEY_UP) keyup = false;
	else if (key == G_KEY_S || key == G_KEY_DOWN) keydown = false;
	else if (key == G_KEY_SPACE) firekey = false;
}

void gCanvas::charPressed(unsigned int codepoint) {
}

// On the web the GLFW layer also reports the primary touch as mouse events,
// so once real touch input shows up the mouse callbacks go quiet for good.
void gCanvas::mouseMoved(int x, int y) {
	if (touchmode) return;
	glm::vec2 p(x, y);
	if (p != mousepos) followmouse = true;
	mousepos = p;
}

void gCanvas::mouseDragged(int x, int y, int button) {
	if (touchmode) return;
	glm::vec2 p(x, y);
	if (p != mousepos) followmouse = true;
	mousepos = p;
}

void gCanvas::mousePressed(int x, int y, int button) {
	if (touchmode) return;
	mousepos = glm::vec2(x, y);
	if (isOnMuteButton(x, y)) {
		audio.setMuted(!audio.isMuted());
		return;
	}
	if (state == STATE_PLAYING) firemouse = true;
	else confirmPressed();
}

void gCanvas::mouseReleased(int x, int y, int button) {
	if (touchmode) return;
	firemouse = false;
}

void gCanvas::mouseScrolled(int x, int y) {
}

void gCanvas::mouseEntered() {
}

void gCanvas::mouseExited() {
}

void gCanvas::touchMoved(int x, int y, int fingerId) {
	if (fingerId == movefinger) {
		joyoffset = glm::vec2(x, y) - joyanchor;
		float len = glm::length(joyoffset);
		if (len > JOY_RADIUS) joyoffset *= JOY_RADIUS / len;
	}
}

void gCanvas::touchPressed(int x, int y, int fingerId) {
	touchmode = true;
	firemouse = firekey = false;
	if (isOnMuteButton(x, y)) {
		audio.setMuted(!audio.isMuted());
		return;
	}
	if (state != STATE_PLAYING) {
		confirmPressed();
		return;
	}
	if (movefinger == -1 || movefinger == fingerId) {
		// a floating joystick appears wherever the first finger lands
		movefinger = fingerId;
		joyanchor = glm::vec2(x, y);
		joyoffset = glm::vec2(0.0f);
	} else if (shootfinger == -1) {
		shootfinger = fingerId;
	}
}

void gCanvas::touchReleased(int x, int y, int fingerId) {
	if (fingerId == movefinger) {
		movefinger = -1;
		joyoffset = glm::vec2(0.0f);
	} else if (fingerId == shootfinger) {
		shootfinger = -1;
	}
}

void gCanvas::windowResized(int w, int h) {
	fitUnits();
}

void gCanvas::showNotify() {
}

void gCanvas::hideNotify() {
}
