#ifndef GCANVAS_H_
#define GCANVAS_H_

#include <deque>
#include <string>
#include <vector>

#include "gBaseCanvas.h"
#include "gApp.h"
#include "gAudio.h"
#include "gFont.h"
#include "gImage.h"

class gCanvas : public gBaseCanvas {
public:
	gCanvas(gApp* root);
	virtual ~gCanvas();

	void setup();
	void update();
	void draw();

	void keyPressed(int key);
	void keyReleased(int key);
	void charPressed(unsigned int codepoint);
	void mouseMoved(int x, int y);
	void mouseDragged(int x, int y, int button);
	void mousePressed(int x, int y, int button);
	void mouseReleased(int x, int y, int button);
	void mouseScrolled(int x, int y);
	void mouseEntered();
	void mouseExited();
	void touchMoved(int x, int y, int fingerId);
	void touchPressed(int x, int y, int fingerId);
	void touchReleased(int x, int y, int fingerId);
	void windowResized(int w, int h);

	void showNotify();
	void hideNotify();

private:
	enum GameState { STATE_BOOT, STATE_TITLE, STATE_PLAYING, STATE_GAMEOVER };
	enum EnemyType { ENEMY_CHASER, ENEMY_DRIFTER, ENEMY_HEAVY };

	struct Enemy {
		glm::vec2 pos, vel;
		float radius;
		float angle, spin;
		float pulse;
		int type;
		int hp;
		int id;
	};

	struct Bullet {
		glm::vec2 pos, vel;
		float life;
	};

	struct Particle {
		glm::vec2 pos, vel;
		float life, maxlife, size;
		int color;
	};

	struct TrailPoint {
		glm::vec2 pos;
		float life;
	};

	void fitUnits();
	void fitToPage();
	bool isOnScreen(const glm::vec2& p);
	bool isOnMuteButton(float x, float y);
	float interceptTime(const Enemy& e) const;
	void startGame();
	void killPlayer();
	void confirmPressed();
	void updateWorld(float dt);
	void updatePlayer(float dt);
	void updateShooting(float dt);
	void spawnEnemy();
	void spawnBurst(const glm::vec2& p, int color, int count, float speed, float size);
	void drawBoot();
	void drawGrid(float ox, float oy);
	void drawTrail(float ox, float oy);
	void drawParticles(float ox, float oy);
	void drawBullets(float ox, float oy);
	void drawEnemies(float ox, float oy);
	void drawShip(float ox, float oy);
	void drawHud();
	void drawMuteButton();
	void drawPolyGlow(float x, float y, float radius, int sides, float angle, int color, float alphascale);
	void drawCenteredText(gFont& font, const std::string& text, float y);

	gApp* root;
	gFont uifont, midfont, titlefont;
	gImage logo;
	gAudio audio;

	int state;
	float gametime, statetime;

	glm::vec2 playerpos, playervel, mousepos;
	glm::vec2 joyanchor, joyoffset;
	float shipangle;
	bool keyleft, keyright, keyup, keydown;
	bool firemouse, firekey;
	bool touchmode;
	bool followmouse;
	int movefinger, shootfinger;

	std::vector<Enemy> enemies;
	std::vector<Bullet> bullets;
	std::vector<Particle> particles;
	std::deque<TrailPoint> trail;

	float spawntimer, firetimer;
	float shake;
	int nextenemyid, targetid;
	int score, killscore, best;
};

#endif /* GCANVAS_H_ */
