// The blocks world, its picture and the hand: what the program shows on the
// display, and the commands that act on it directly.

#ifndef SCENE_H
#define SCENE_H

// Starts the display on the port given; a client that connects gets the point
// stream. fast makes the hand jump where it would walk.
void sceneInit(int port, int fast);

// The four calls a plan is made of, and what follows them.
void sceneMoveto(int x, int y, int z);
void sceneGrasp(int obj);
void sceneUngrasp(void);
void sceneRefresh(void);
// The world as it starts.
void sceneReset(void);

// The color labels, two letters beside each colored object: up while the program waits
// for the first character of a line, down from that character on. The labels are made
// from the scene as it stands when they go up. The environment variable
// SHRDLU_NOLABELS=1 keeps them down, so the picture stays plain.
void sceneLabelsOn(void);
void sceneLabelsOff(void);

// An object's index from its name, :B1; -1 for none.
int sceneObject(const char *nameP);
// The screen point of a world point.
void sceneProject(int x, int y, int z, int *sxP, int *syP);
// Prints the scene, the CMU manual's way.
void sceneOut(void);
// Prints STATUS's line.
void sceneStatusOut(void);
// All the time the hand has spent waiting on its pace, in microseconds.
long sceneHandMicros(void);
// In a forked child, whose display thread did not come along: from here on the
// scene is not to be touched.
void sceneDetach(void);

// How a scene says something, supplied by the terminal.
void termText(const char *textP);

#endif
