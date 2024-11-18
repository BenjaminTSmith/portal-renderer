# portal-renderer
This is a DOOM style renderer with portal based occlusion culling. The renderer supports convex sectors,
and perspective correct, subpixel accurate texture mapping (original DOOM had perspective correct texture mapping).
No slanted walls or floors/ceilings however (same as the original DOOM).

Just compile src/main.c with tigr (see submodule):
```bash
cc -o bin/main src/main.c tigr/tigr.c -Itigr -lm -lGLU -lGL -lX11 -Wall -O0 -g
```
