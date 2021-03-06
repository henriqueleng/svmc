#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>	/* usleep, getopt */
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>	/* XkbSetDetectableAutoRepeat() */
#include <X11/extensions/Xdbe.h> /* double buffer */

#ifdef ALSA
#include "alsa.h"
#elif SNDIO
#include "sndio.h"
#endif

/* macros */
#define LENGTH(x)        (sizeof x / sizeof x[0])
#define PRESSED  1
#define RELEASED 0

typedef unsigned int uint;
typedef unsigned long ulong;

/* variables */
uint winwidth, winheight;
const uint keywidth = 50;
uint keyheight;
uint fontheight;
uint nwhitekeys, nblackkeys;

typedef struct {
	KeySym keysym;
	int note;
	int status;
} Key;

#include "midi.h"
#include "config.h"

/* Xlib */
static int screen;
static Display *dpy;
static Window win;
GC gc;
Colormap colormap;
ulong xkeycolor, xkeypressedcolor, xsharpkeycolor, 
	xsharpkeypressedcolor, xkeybordercolor, xfontcolor, xbgcolor;
XFontStruct* font_info;
XWindowAttributes wa;

/* Xdbe */
XdbeBackBuffer buf;
XdbeBackBufferAttributes  *bufattr;
XdbeSwapInfo swapinfo;

void
quit(void)
{
	/* X11 close */
	XdbeDeallocateBackBufferName(dpy, buf);
	XFreeGC(dpy, gc);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);

	/* midi close */
	midiclose();
}

void
changeinstrument(int number)
{
	unsigned char message[] = {PRG_CHANGE | channel, number};
	midisend(message, sizeof(message));
}

void
sendnote(int action, int note, int speed)
{
	int mult = octave;
	mult++;
	unsigned char message[] = {action | channel, note + (OCTAVE * mult), speed};
	midisend(message, sizeof(message));
}

/* idea from svkbd */
ulong
getcolor(const char *colstr)
{
	Colormap cmap = DefaultColormap(dpy, screen);
	XColor color;

	if(!XAllocNamedColor(dpy, cmap, colstr, &color, &color)) {
		fprintf(stderr, "error, cannot allocate color '%s'\n", colstr);
		exit(EXIT_FAILURE);
	}
	return color.pixel;
}

void
startwin(uint initial_width, uint initial_height)
{
	/* open connection with the server */
	dpy = XOpenDisplay(NULL);

	if (dpy == NULL) {
		fprintf(stderr, "Cannot open display\n");
		exit(EXIT_FAILURE);
	}

	screen = DefaultScreen(dpy);

	/* create window */
	win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, initial_width, 
		initial_height, 1, BlackPixel(dpy, screen), WhitePixel(dpy, screen));

	colormap = DefaultColormap(dpy, screen);

	/* colors */
	xkeycolor = getcolor(keycolor);
	xkeypressedcolor = getcolor(keypressedcolor);
	xsharpkeycolor = getcolor(sharpkeycolor);
	xsharpkeypressedcolor = getcolor(sharpkeypressedcolor);
	xkeybordercolor = getcolor(keybordercolor);
	xfontcolor = getcolor(fontcolor);
	xbgcolor = getcolor(bgcolor);

	/* try to load the given font. */
	font_info = XLoadQueryFont(dpy, font);
	if (!font_info) {
		fprintf(stderr, "XLoadQueryFont: failed loading font: %s\n", font);
		exit(EXIT_FAILURE);
	}

	fontheight = font_info->ascent + font_info->descent;

	XGCValues values;
	ulong valuemask = 0;
	gc = XCreateGC(dpy, win, valuemask, &values);
	XSetLineAttributes(dpy, gc, 1, LineSolid, CapRound, JoinMiter);
	XSetFont(dpy, gc, font_info->fid);

	/* Select kind of events we are interested in. */
	XSelectInput(dpy, win, KeyPressMask | KeyReleaseMask |
		StructureNotifyMask | ExposureMask);

	/* Xdbe: back buffer */
	buf = XdbeAllocateBackBufferName(dpy, win, XdbeUndefined);
	bufattr = XdbeGetBackBufferAttributes(dpy, buf);
	swapinfo.swap_window = bufattr->window;
	swapinfo.swap_action = XdbeUndefined;
	XFree(bufattr);

	/* show the window */
	XMapWindow(dpy, win);

	/* fuck autorepeat */
	XAutoRepeatOn(dpy);
	XkbSetDetectableAutoRepeat(dpy, True, NULL);

	/* get initial window size */
	XGetWindowAttributes(dpy, win, &wa);
	winwidth = wa.width;
	winheight = wa.height;
}

void
drawkeyboard(/* winheight */)
{
	char string[100];
	snprintf(string, (size_t)100, "octave: %i   channel: %i    instrument: %i - %s", 
		octave, channel, instrument, instruments[instrument].name);

	/* is there really a need to fill a rectangle bellow text? */
	XSetForeground(dpy, gc, xbgcolor);
	XFillRectangle(dpy, buf, gc, 0, 0, winwidth, winheight);

	XSetForeground(dpy, gc, xfontcolor);
	XDrawString(dpy, buf, gc, 0, fontheight - 2, string, strlen(string));

	uint usedspace = 0, totalkeys = 0;
	uint i = 0;

	keyheight = winheight - 10;

	/* 
	 * White keys must be drawn first so they stay beneath black ones. 
	 */
	for (i = 0; i < nwhitekeys; i++) {
		if (whitekeys[totalkeys].status == PRESSED) {
			XSetForeground(dpy, gc, xkeypressedcolor);
			XFillRectangle(dpy, buf, gc, usedspace, fontheight, keywidth, keyheight);
		} else {
			/* normal color */
			XSetForeground(dpy, gc, xkeycolor);
			XFillRectangle(dpy, buf, gc, usedspace, fontheight, keywidth, keyheight);
		}
		XSetForeground(dpy, gc, xkeybordercolor);
		XDrawRectangle(dpy, buf, gc, usedspace, fontheight, keywidth, keyheight);
		usedspace += keywidth;
		totalkeys++;
	}

	usedspace = 0;
	totalkeys = 0;

	uint subskeys = 0;
	uint alternate = 1;

	/* draw black keys */
	for (i = nblackkeys; i > 1; i--) {
		/* alternate between 3 and 2 subsequent keys */
		if (alternate == 1 && subskeys == 2) {
			usedspace += keywidth;
			subskeys = 0;
			alternate = 0;
		} else if (subskeys == 3) {
			usedspace += keywidth;
			subskeys = 0;
			alternate = 1;
		}

		/* draw */
		if (blackkeys[totalkeys].status == PRESSED) {
			XSetForeground(dpy, gc, xsharpkeypressedcolor);
			XFillRectangle(dpy, buf, gc, usedspace + ((keywidth * 2) / 3), 
				fontheight, (keywidth * 2) / 3 , (keyheight * 2) / 3);
		} else if (blackkeys[totalkeys].status == RELEASED) {
			XSetForeground(dpy, gc, xsharpkeycolor);
			XFillRectangle(dpy, buf, gc, usedspace + ((keywidth * 2) / 3), 
				fontheight, (keywidth * 2) / 3 , (keyheight * 2 / 3)); 
		}
		usedspace += keywidth;
		subskeys++;
		totalkeys++;
	}

	XdbeSwapBuffers(dpy, &swapinfo, 1);
}

void
cleanwindow(void) /* winheight, winwidth */
{
	/* just fill a rectangle with the size of the window */
	XSetForeground(dpy, gc, xbgcolor);
	XFillRectangle(dpy, buf, gc, 0, 0, winwidth, winheight);	
}

void
drawinstruments(uint textwidth, uint len)
{
	int spacey = 0, spacex = 0, i;

	for (i = 0; i < LENGTH(instruments); i++) {
		char string[len];
		snprintf(string, len, "%i: %s", instruments[i].number, instruments[i].name);

		if (spacey >= winheight - (fontheight * 2)) {
			spacey = fontheight;
			spacex += textwidth;
		} else {
			spacey += fontheight;
		}
		XSetForeground(dpy, gc, xfontcolor);
		XDrawString(dpy, buf, gc, spacex, spacey, string, strlen(string));
	}
}

void
run(void)
{
	KeySym keysym;
	XEvent e;

	/*
	 * calculate the biggest array in instument list and propper 
	 * text width.
	 */
	uint i;
	uint biggest = 0, instlen = 0;
	for (i = 0; i < LENGTH(instruments); i++) {
		if ((strlen(instruments[i].name)) > instlen) {
			instlen = strlen(instruments[i].name);
			biggest = i;
		}
	}

	instlen += 5;
	uint textwidth = XTextWidth(font_info, instruments[biggest].name, instlen) + 10;

	while (1) {
		uint i = 0;
		XNextEvent(dpy, &e);
		switch (e.type) {

		case KeyPress:
				keysym = XLookupKeysym (&e.xkey, 0);

				/* enter instrument select loop if Ctrl + i */
				if (keysym == XK_i && e.xkey.state & ControlMask) {
					keysym = NoSymbol;
					cleanwindow();
					drawinstruments(textwidth, instlen);

					/* vars */
					XEvent e2;
					KeySym tmpkeysym = NoSymbol;

					char string[10] = {0};

					char prompt[] = "> ";
					int promptwidth = XTextWidth(font_info, prompt, strlen(prompt));

					XDrawString(dpy, buf, gc, 0, winheight - 5,
						prompt , strlen(prompt));
					XdbeSwapBuffers(dpy, &swapinfo, 1);

					int len = 0;

					while (len < 10 - 1 &&
					       tmpkeysym != XK_Return &&
					       tmpkeysym != XK_KP_Enter) {
						XNextEvent(dpy, &e2);
						char input[1] = {0};

						switch (e2.type) {
						case KeyPress:
							len = strlen(string);
							XLookupString(&e2.xkey, input, 25, &tmpkeysym, NULL);
							if (isdigit(input[0])) {
								string[len] = input[0];
								string[len + 1] = '\0';
								i++;
							} else if (tmpkeysym == XK_BackSpace) {
								string[len - 1] = '\0';
							}

							cleanwindow();
							drawinstruments(textwidth, instlen);
							XDrawString(dpy, buf, gc,
								0, winheight - 5,
								prompt , strlen(prompt));
							XDrawString(dpy, buf, gc,
								promptwidth, winheight - 5,
								string, strlen(string));
							XdbeSwapBuffers(dpy, &swapinfo, 1);
							break;

						case Expose:
							cleanwindow();
							drawinstruments(textwidth, instlen);
							XDrawString(dpy, buf, gc, 0, winheight - 5,
								prompt , strlen(prompt));
							XdbeSwapBuffers(dpy, &swapinfo, 1);
							break;

						case ConfigureNotify:
							winheight = e2.xconfigure.height;
							winwidth = e2.xconfigure.width;
							cleanwindow();
							drawinstruments(textwidth, instlen);
							XDrawString(dpy, buf, gc, 0, winheight - 5,
								prompt , strlen(prompt));
							XdbeSwapBuffers(dpy, &swapinfo, 1);
							break;
						}
					}

					instrument = atoi(string);
					if (instrument > 127 || instrument < 0) {
						instrument = 0;
						cleanwindow();
						drawinstruments(textwidth, instlen);
						char tmpstring[] = "ERROR: number out of range";
						XSetForeground(dpy, gc, xfontcolor);
						XDrawString(dpy, buf, gc,
							0, winheight - 5, 
							tmpstring, strlen(tmpstring));
						XdbeSwapBuffers(dpy, &swapinfo, 1);
						/* wait for keypress */
						e2.type = NoSymbol;
						while (e2.type != KeyPress) {
							XNextEvent(dpy, &e2);
						}
					} else {
						changeinstrument(instrument);
					}
					break;
				}

				if (keysym == XK_k && e.xkey.state & ControlMask && octave < 9) {
					octave++;
					break;
				}

				if (keysym == XK_j && e.xkey.state & ControlMask && octave > -1) {
					octave--;
					break;
				}

				if (keysym == XK_l && e.xkey.state & ControlMask) {
					channel++;
					break;
				}

				if (keysym == XK_h && e.xkey.state & ControlMask && channel > 0) {
					channel--;
					break;
				}

				if (keysym == XK_q && e.xkey.state & ControlMask) {
					quit();
					exit(EXIT_SUCCESS);
				}

				/* match key with a member of whitekeys[] or blackkeys[] */
				for (i = 0; i < nwhitekeys; i++) {
					if (whitekeys[i].keysym == keysym &&
					    whitekeys[i].status == RELEASED) {
						sendnote(NOTE_ON, whitekeys[i].note, 100);
						whitekeys[i].status = PRESSED;
						break;
					}
				}

				for (i = 0; i < nblackkeys; i++) {
					if (blackkeys[i].keysym == keysym &&
					    blackkeys[i].status == RELEASED) {
						sendnote(NOTE_ON, blackkeys[i].note, 100);
						blackkeys[i].status = PRESSED;
						break;
					}
				}

				drawkeyboard();
			break;

		case KeyRelease:
			keysym = XLookupKeysym(&e.xkey, 0);

			for (i = 0; i < nwhitekeys; i++) {
				if (whitekeys[i].keysym == keysym &&
				    whitekeys[i].status == PRESSED) {
					sendnote(NOTE_OFF, whitekeys[i].note, 100);
					whitekeys[i].status = RELEASED;
					break;
				}
			}

			for (i = 0; i < nblackkeys; i++) {
				if (blackkeys[i].keysym == keysym &&
				    blackkeys[i].status == PRESSED) {
					blackkeys[i].status = RELEASED;
					sendnote(NOTE_OFF, blackkeys[i].note, 100);
					break;
				}
			}
			drawkeyboard();
			break;

		case ConfigureNotify:
			winheight = e.xconfigure.height;
			winwidth = e.xconfigure.width;
			drawkeyboard();
			break;

		case Expose:
			drawkeyboard();
			break;

		default:
			break;
		}
	}
}

void
usage(void)
{
	fprintf(stderr, "usage: svmidi [-i instrument] [-o octave] [-v]\n");
	exit(EXIT_FAILURE);
}

int 
main(int argc, char *argv[])
{
	int ch;

	while ((ch = getopt(argc, argv, "i:o:hv")) != -1) {
		switch (ch) {
		case 'i':
			instrument = atoi(optarg);
			break;
		case 'o':
			octave = atoi(optarg);
			break;
		case 'h':
			usage();
			break;
		case 'v':
			printf("svmidi-"VERSION"\n");
			return 0;
		default:
			break;
		}
	}
	argc -= optind;
	argv += optind;

	if (instrument > INST_MAX || instrument < INST_MIN) {
		fprintf(stderr, "intrument number out of range\n");
		exit(EXIT_FAILURE);
	}

	if (octave > OCTAVE_MAX || octave < OCTAVE_MIN) {
		fprintf(stderr, "octave number out of range\n");
		exit(EXIT_FAILURE);
	}

	/* start midi */
	if (midiopen()) {
		fprintf(stderr, "failed to open MIDI device\n");
		exit(EXIT_FAILURE);
	}
	changeinstrument(instrument);

	/* assign some variables */
	channel = 1;
	nwhitekeys = (int)LENGTH(whitekeys);
	nblackkeys = (int)LENGTH(blackkeys);

	startwin(1000, 400);
	run();
	quit();
	return 0;
}
