#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <string.h>

#include "tigr.h"

#define HEIGHT 300
#define WIDTH 400
#define HFOV 90
#define FOCAL_LENGTH 200

#define PI 3.14159265358979323862643383f
#define DEG_TO_RAD(a) ((a) * (PI / 180.f))
#define RAD_TO_DEG(a) ((a) * (180.f / PI))
#define MIN(a, b) ((a) > (b) ? (b) : (a))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(a, l, h) (MAX(MIN((a), (h)), (l)))

#define SECTOR_MAX 256
#define WALL_MAX 1024

typedef struct {
    float x, y;
} vec2;

typedef struct {
    vec2 position;
    float angle; // in degrees, 0 being directed along the positive x axis
    float eyez;
} camera;

typedef struct {
    vec2 p0, p1;
    int portal;
} wall;

typedef struct {
    int id;
    int start_wall, num_walls;
    float floor, ceiling;
} sector;

void vline(Tigr *bmp, int x, int y0, int y1, TPixel color) {
    if (y0 > y1) {
        return;
    }
    tigrLine(bmp, x, y0, x, y1, color);
}

vec2 vec2_rotate(vec2 vec, float a) {
    a = DEG_TO_RAD(a);
    return (vec2) {
        cos(a) * vec.x - sin(a) * vec.y,
        sin(a) * vec.x + cos(a) * vec.y      
    };
}

vec2 world_to_camera(camera camera, vec2 p) {
    vec2 u = { p.x - camera.position.x, p.y - camera.position.y };
    float a = DEG_TO_RAD(camera.angle);
    return (vec2){
        sin(a) * u.x - cos(a) * u.y,
        cos(a) * u.x + sin(a) * u.y      
    };
}

// p0 and p1 belong to line 1, p3 and p4 belong to line 2
vec2 lineseg_intersection(vec2 p0, vec2 p1, vec2 p2, vec2 p3) {
    float denom = (p0.x - p1.x) * (p2.y - p3.y) - (p0.y - p1.y) * (p2.x - p3.x);
    if (fabsf(denom) < 0.0001f) {
        return (vec2) { NAN, NAN };
    }
    float t = ((p0.x - p2.x) * (p2.y - p3.y) - (p0.y - p2.y) * (p2.x - p3.x)) / denom;
    float u = -((p0.x - p1.x) * (p0.y - p2.y) - (p0.y - p1.y) * (p0.x - p2.x)) / denom;
    if (0 <= t && t <= 1 && 0 <= u && u <= 1) {
        return (vec2) {
            p0.x + t * (p1.x - p0.x),
            p0.y + t * (p1.y - p0.y)
        };
    }
    return (vec2) { NAN, NAN };
}

// 0 is left 1 is right side of wall
//
//           p1
//           |
//    left   |     right
//           |
//          p0
int wall_side(vec2 p0, vec2 p1, vec2 position) {
    vec2 point = {
        position.x - p0.x,
        position.y - p0.y
    };
    vec2 wall = {
        p1.x - p0.x,
        p1.y - p0.y
    };
    // point cross wall z component
    float crossz = point.x * wall.y - point.y * wall.x;
    return crossz > 0;
}

static sector sectors[SECTOR_MAX] = {0};
static int sectors_size = 0;
static int last_sector = 0;
static wall walls[WALL_MAX] = {0};
static int walls_size = 0;

typedef enum {
    NONE,
    SECTORS,
    WALLS
} file_read_state;

static void read_level(const char *level) {
    FILE *file = fopen(level, "r");
    file_read_state frs = NONE;
    char line[128];
    while (fgets(line, sizeof(line), file)) {
        if (!strcmp(line, "\n") || line[0] == '#') {
            continue;
        }
        if (!strcmp(line, "[sectors]\n")) {
            frs = SECTORS;
            continue;
        } else if (!strcmp(line, "[walls]\n")) {
            frs = WALLS;
            continue;
        }

        switch(frs) {
            case SECTORS: {
                sector *sector = &sectors[sectors_size];
                sscanf(line,
                       "%d %d %d %f %f",
                       &sector->id,
                       &sector->start_wall,
                       &sector->num_walls,
                       &sector->floor,
                       &sector->ceiling);
                sectors_size++;
                break;
            } case WALLS: {
                wall *wall = &walls[walls_size];
                sscanf(line, "%f %f %f %f %d", &wall->p0.x, &wall->p0.y, &wall->p1.x, &wall->p1.y, &wall->portal);
                walls_size++;
                break;
            } default: {
                break;
            }
        }
    }
}

static int current_sector(vec2 position) {
    // TODO(Ben): slow, so omptimize if need to
    for (int i = 0; i < sectors_size; i++) {
        sector *sector = &sectors[i];
        int inside = 1;
        for (int j = 0; j < sector->num_walls; j++) {
            if (!wall_side(walls[sector->start_wall + j].p0,
                           walls[sector->start_wall + j].p1,
                           position)) {
                inside = 0;
                break;
            }
        }
        if (inside) {
            return i;
        }
    }
    // NOTE(Ben): maybe put this back to return 0/-1;
    return -1;
}

typedef struct {
    int secnum;
    int x0, x1;
} qentry;

// y is the distance from the screen, x is where on the screen; - is left + is right
// requires wall endpoints to be ordered clockwise to the player
inline static void render(Tigr *screen, camera camera) {
    qentry queue[SECTOR_MAX];
    int size = 0;
    int front = 0;
    queue[size++] = (qentry) {
        last_sector, 0, WIDTH - 1
    };

    int high[WIDTH];
    for (int i = 0; i < WIDTH; i++) {
        high[i] = 0;
    }
    int low[WIDTH];
    for (int i = 0; i < WIDTH; i++) {
        low[i] = HEIGHT - 1;
    }

    while (front < size) {
        assert(size < SECTOR_MAX);
        qentry entry = queue[front++];
        sector *cursector = &sectors[entry.secnum];
        for (int i = 0; i < cursector->num_walls; i++) {
            wall *curwall = &walls[cursector->start_wall + i];

            vec2 p0 = world_to_camera(camera, curwall->p0);
            vec2 p1 = world_to_camera(camera, curwall->p1);
            if (p0.y <= 0 && p1.y <= 0) {
                continue;
            }
            if (!wall_side(curwall->p0, curwall->p1, camera.position)) {
                continue;
            }
            
            vec2 origin = { 0, 0 };
            vec2 far_left = { -1000, 1000 };
            vec2 far_right = { 1000, 1000 };
            vec2 left_intersect = lineseg_intersection(p0, p1, origin, far_left);
            vec2 right_intersect = lineseg_intersection(p0, p1, origin, far_right);
            if (!isnan(left_intersect.x)) {
                p0 = left_intersect;
            }
            if (!isnan(right_intersect.x)) {
                p1 = right_intersect;
            }

            if (p0.y < 0.00001f) {
                p0.y = 0.00001f;
            }
            if (p1.y < 0.00001f) {
                p1.y = 0.00001f;
            }

            int x0 = (p0.x / p0.y) * FOCAL_LENGTH + WIDTH / 2.f;
            int x1 = (p1.x / p1.y) * FOCAL_LENGTH + WIDTH / 2.f;
            if (x1 < x0 || x0 > entry.x1 || x1 < entry.x0) {
                continue;
            }

            // NOTE(Ben): Maybe fix up this z calculation code. something more elegant than
            // negating the values *shrug*.
            float topl = -(cursector->ceiling - camera.eyez) / p0.y * FOCAL_LENGTH + HEIGHT / 2.f;
            float topr = -(cursector->ceiling - camera.eyez) / p1.y * FOCAL_LENGTH + HEIGHT / 2.f;
            float bottoml = -(cursector->floor - camera.eyez) / p0.y * FOCAL_LENGTH + HEIGHT / 2.f;
            float bottomr = -(cursector->floor - camera.eyez) / p1.y * FOCAL_LENGTH + HEIGHT / 2.f;
            for (int x = MAX(x0, entry.x0); x < MIN(x1, entry.x1); x++) {
                assert(x >= 0);
                assert(x < WIDTH);
                float t = (x - x0) / (float)(x1 - x0);
                int top = topl * (1 - t) + topr * t;
                int bottom = bottoml * (1 - t) + bottomr * t;
                sector *neighbor = &sectors[curwall->portal];
                float ceil_borderl = -(neighbor->ceiling - camera.eyez) / p0.y * FOCAL_LENGTH + HEIGHT / 2.f;
                float ceil_borderr = -(neighbor->ceiling - camera.eyez) / p1.y * FOCAL_LENGTH + HEIGHT / 2.f;
                float floor_borderl = -(neighbor->floor - camera.eyez) / p0.y * FOCAL_LENGTH + HEIGHT / 2.f;
                float floor_borderr = -(neighbor->floor - camera.eyez) / p1.y * FOCAL_LENGTH + HEIGHT / 2.f;
                int ceil = ceil_borderl * (1 - t) + ceil_borderr * t;
                int floor = floor_borderl * (1 - t) + floor_borderr * t;
                if (curwall->portal != -1) {
                    vline(screen, x, MAX(0, high[x]), top, tigrRGB(0x0, 0x0, 30 * cursector->ceiling));
                    if (neighbor->ceiling < cursector->ceiling) {
                        vline(screen,
                              x,
                              MAX(top, high[x]),
                              MIN(ceil, low[x]),
                              tigrRGB(0xaa, 0x0, 0x0));
                        high[x] = MAX(ceil, high[x]);
                    } else {
                        high[x] = MAX(top, high[x]);
                    }
                    if (neighbor->floor > cursector->floor) {
                        vline(screen,
                              x,
                              MAX(floor, high[x]),
                              MIN(bottom, low[x]),
                              tigrRGB(0xaa, 0x0, 0x0));
                        low[x] = MIN(floor, low[x]);
                    } else {
                        low[x] = MIN(bottom, low[x]);
                    }
                } else {
                    vline(screen, x, MAX(top, high[x]), MIN(bottom, low[x]), tigrRGB(0xaf, 0xaf, 0xaf));
                    vline(screen, x, MAX(0, high[x]), top, tigrRGB(0x0, 0x0, 30 * cursector->ceiling));
                    high[x] = HEIGHT - 1;
                    low[x] = 0;
                }
            }

            if (curwall->portal != -1) {
                queue[size++] = (qentry) {
                    curwall->portal, MAX(x0, entry.x0), MIN(x1, entry.x1)
                };
            } 
        }
    }
}

/*static void render(Tigr *screen, camera camera, vec2 p0, vec2 p1) {
    // TODO(Ben): I think I want to rewrite most of the previous render function. Just thought of a lifehack
    // for clipping. We can skip if we are on the wrong side of the wall with wall_side :P !!! :D
}*/

int main() {
    read_level("level2.txt");

    Tigr *screen = tigrWindow(WIDTH, HEIGHT, "Hello", TIGR_FIXED);

    camera camera = {0};
    camera.position = (vec2) { 2, 3 };
    camera.angle = 0;
    camera.eyez = 1.5f;

    int update_fps = 0;
    tigrTime();
    while (!tigrClosed(screen)) {
        float dt = tigrTime();
        float fps = 1 / dt;

        if (tigrKeyDown(screen, TK_ESCAPE)) {
            break;
        } if (tigrKeyHeld(screen, TK_LEFT)) {
            camera.angle += 100 * dt;
        } if (tigrKeyHeld(screen, TK_RIGHT)) {
            camera.angle -= 100 * dt;
        } if (tigrKeyHeld(screen, 'W')) {
            camera.position.x += cos(DEG_TO_RAD(camera.angle)) * 5 * dt;
            camera.position.y += sin(DEG_TO_RAD(camera.angle)) * 5 * dt;
        } if (tigrKeyHeld(screen, 'S')) {
            camera.position.x -= cos(DEG_TO_RAD(camera.angle)) * 5 * dt;
            camera.position.y -= sin(DEG_TO_RAD(camera.angle)) * 5 * dt;
        } if (tigrKeyHeld(screen, 'A')) {
            camera.position.x += cos(DEG_TO_RAD(camera.angle + 90)) * 5 * dt;
            camera.position.y += sin(DEG_TO_RAD(camera.angle + 90)) * 5 * dt;
        } if (tigrKeyHeld(screen, 'D')) {
            camera.position.x += cos(DEG_TO_RAD(camera.angle - 90)) * 5 * dt;
            camera.position.y += sin(DEG_TO_RAD(camera.angle - 90)) * 5 * dt;
        }
        tigrClear(screen, tigrRGB(0, 0, 0));
        
        int secnum = current_sector(camera.position);
        if (secnum == -1) {
            secnum = last_sector;
        }
        last_sector = secnum;
        camera.eyez = sectors[last_sector].floor + 1.5f;

        render(screen, camera);

        char fps_str[100];
        if (update_fps % 10 == 0) {
            sprintf(fps_str, "%d", (int)fps);
        }
        update_fps++;
        tigrPrint(screen, tfont, 3, 3, tigrRGB(0xff, 0xff, 0xff), fps_str);

        tigrUpdate(screen);
    }
    tigrFree(screen);
    return 0;
}
