//  checkerboard.c
//
// 	version 1.0 was written by Martin Fierz	on															
// 	15th february 2000	
//  (c) 2000-2011 by Martin Fierz - all rights reserved.
//  contributions by Ed Gilbert are gratefully acknowledged
// 																			
// 	checkerboard is a graphical front-end for checkers engines. it checks	
// 	user moves for correctness. you can save and load games. you can change
// 	the board and piece colors. you can change the window size. 			
// 																						
// 	interfacing to checkers engines: 													
// 	if you want your checkers engine to use checkerboard as a front-end, 	
// 	you must compile your engine as a dll, and provide the following 2
// 	functions:		
//  int WINAPI getmove(int board[8][8], int color, double maxtime, char str[1024], int *playnow, int info, int moreinfo, struct CBmove *move);
//  int WINAPI enginecommand(char command[256], char reply[1024]);
// TODO: bug report: if you hit takeback while CB is animating a move, you get an undefined state

/******************************************************************************/

// CB uses multithreading, up to 4 threads:
//	-> main thread for the window
//	-> checkers engine runs in 'Thread'
//	-> animation runs in 'AniThread'
//	-> game analysis & engine match are driven by 'AutoThread'
#define STRICT

// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//
#define WIN32_LEAN_AND_MEAN		// Exclude rarely-used stuff from Windows headers

// C RunTime Header Files
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>

// TODO: reference additional headers your program requires here
#include <windows.h>
#include <windowsx.h>
#include <wininet.h>
#include <commctrl.h>
#include <stdio.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <time.h>
#include <io.h>
#include <intrin.h>
#include <vector>
#include <algorithm>

#include "standardheader.h"
#include "cb_interface.h"
#include "min_movegen.h"
#include "CBstructs.h"
#include "CBconsts.h"
#include "PDNparser.h"
#include "dialogs.h"
#include "pdnfind.h"
#include "checkerboard.h"
#include "bmp.h"
#include "coordinates.h"
#include "bitboard.h"
#include "utility.h"
#include "fen.h"
#include "resource.h"
#include "saveashtml.h"
#include "graphics.h"
#include "registry.h"
#include "app_instance.h"

#ifdef _WIN64
#pragma message("_WIN64 is defined.")
#endif

//---------------------------------------------------------------------
// globals - should be identified in code by g_varname but aren't all...
PDNgame cbgame;

// all checkerboard options are collected in CBoptions; like this, they can be saved
// as one struct in the registry, instead of using lots of commands.
CBoptions cboptions;

int g_app_instance;				/* 0, 1, 2, ... */
char g_app_instance_suffix[10]; /* "", "[1]", "[2]", ... */
DWORD g_ThreadId, g_AniThreadId, AutoThreadId;
HANDLE hThread, hAniThread, hAutoThread;
int enginethreadpriority = THREAD_PRIORITY_NORMAL;	/* default priority setting*/
int usersetpriority = THREAD_PRIORITY_NORMAL;		/* default priority setting*/
HICON hIcon;						/* CB icon for the window */
TBBUTTON tbButtons[NUMBUTTONS];		/* for the toolbar */

/* these globals are used to synchronize threads */
int abortcalculation = 0;			// used to tell the threadfunc that the calculation has been aborted
static BOOL enginebusy = FALSE;		/* true while engine thread is busy */
static BOOL animationbusy = FALSE;	/* true while animation thread is busy */
static BOOL enginestarting = FALSE; // true when a play command is issued to the engine but engine has

// not started yet
BOOL gameover = FALSE;				/* true when autoplay or engine match game is finished */
BOOL startmatch = TRUE;				/* startmatch is only true before engine match was started */
BOOL newposition = TRUE;			/* is true when position has changed. used in analysis mode to
								restart search and then reset */
BOOL startengine = FALSE;			/* is true if engine is expected to start */
int result;
clock_t starttime;

int toolbarheight = 30;				//30;
int statusbarheight = 20;			//20;
int menuheight = 16;				//16;
int titlebarheight = 12;			//12;
int offset = 40;					//40;
int upperoffset = 20;				//20;
char szWinName[] = "CheckerBoard";	/* name of window class */
int cbboard8[8][8];					/* the board being displayed in the GUI*/
int cbcolor = CB_BLACK;				/* color is the side to move next in the GUI */
int setup = 0;						/* 1 if in setup mode */
static int addcomment = 0;
int handicap = 0;
int testset_number = 0;
int playnow = 0;					/* playnow is passed to the checkers engines, it is set to nonzero if the user chooses 'play' */
bool reset_move_history;			/* send option to engine to reset its list of game moves. */
int gameindex = 0;					/* game to load/replace from/in a database*/

/* dll globals */

/* CB uses function pointers to access the dll.
enginename, engineabout, engineoptions, enginehelp, getmove point to the currently used functions
...1 and ...2 are the pointers to dll1 and dll2 as read from engines.ini. */

/* library instances for primary, secondary and analysis engines */
HINSTANCE hinstLib = 0, hinstLib1 = 0, hinstLib2 = 0;

/* function pointers for the engine functions */
CB_GETMOVE getmove = 0, getmove1 = 0, getmove2 = 0;
CB_ENGINECOMMAND enginecommandtmp = 0, enginecommand1 = 0, enginecommand2 = 0;

// multi-version support
CB_ISLEGAL islegal = 0, islegal1 = 0, islegal2 = 0;
CB_GETSTRING enginename1 = 0, enginename2 = 0;
CB_GETGAMETYPE CBgametype = 0;		// built in gametype and islegal functions
CB_ISLEGAL CBislegal = 0;

int enginename(char Lstr[256]);
BOOL fFreeResult;

// instance and window handles
HINSTANCE g_hInst;					//instance of checkerboard
HWND hwnd;					// main window
HWND hStatusWnd;			// status window
static HWND tbwnd;			// toolbar window
HWND hHeadWnd;				// window of header control for game load
HWND hDlgSelectgame;

std::vector<gamepreview> game_previews;

// statusbar_txt holds the output string shown in the status bar - it is updated by WM_TIMER messages
char statusbar_txt[1024] = "";
char playername[256];		// name of the player we are searching games of
char eventname[256];		// event we're searching for
char datename[256];			// date we're searching for
char commentname[256];		// comment we're searching for
int searchwithposition = 0; // search with position?
char string[256];
HMENU hmenu;				// menu handle
double o, xmetric, ymetric; //gives the size of the board8: one square is xmetric*ymetric
int dummy, x1 = -1, x2 = -1, y1_ = -1, y2 = -1;

/* When cboptions.use_incremental_time is true, these are game clocks for black and white. */
double black_time_remaining;
double white_time_remaining;

char reply[ENGINECOMMAND_REPLY_SIZE];	// holds reply of engine to command requests
char CBdirectory[256] = "";				// holds the directory from where CB is started:
char CBdocuments[MAX_PATH];				// CheckerBoard directory under My Documents
char database[256] = "";				// current PDN database
char userbookname[256];					// current userbook
CBmove cbmove;
char filename[255] = "";
char engine1[255] = "";
char engine2[255] = "";
int currentengine = 1;					// 1=primary, 2=secondary
int op = 0;
int togglemode = 0;						// 1-2-player toggle state
int togglebook = 0;						// engine book state (0/1/2/3)
int toggleengine = 1;					// primary/secondary engine (1/2)
struct pos currentposition;

// keep a small user book
struct userbookentry userbook[MAXUSERBOOK];
size_t userbooknum = 0;
size_t userbookcur = 0;
static CHOOSECOLOR ccs;

// reindex tells whether we have to reindex a database when searching.
// reindex is set to 1 if a game is saved, a game is replaced, or the
// database changed. and initialized to 1.
int reindex = 1;
int re_search_ok = 0;
char piecesetname[MAXPIECESET][256];
int maxpieceset = 0;
CRITICAL_SECTION ani_criticalsection, engine_criticalsection;
int handletooltiprequest(LPTOOLTIPTEXT TTtext);
void reset_game(PDNgame &game);
void forward_to_game_end(void);

// checkerboard goes finite-state: it can be in one of the modes above.
//	normal:	after the user enters a move, checkerboard starts to calculate
//				with engine.
//	autoplay: checkerboard plays engine-engine
//	enginematch: checkerboard plays engine-engine2
//	analyzegame: checkerboard moves through a game and comments on every move
//	entergame: checkerboard does nothing while the user enters a game
//	observegame: checkerboard calculates while the user enters a game
enum state
{
	NORMAL,
	AUTOPLAY,
	ENGINEMATCH,
	ENGINEGAME,
	ANALYZEGAME,
	OBSERVEGAME,
	ENTERGAME,
	BOOKVIEW,
	BOOKADD,
	RUNTESTSET,
	ANALYZEPDN
} CBstate = NORMAL;

int WINAPI WinMain(HINSTANCE hThisInst, HINSTANCE hPrevInst, LPSTR lpszArgs, int nWinMode)
{
	// the main function which runs all the time, processing messages and sending them on
	// to windowfunc() which is the heart of CB
	MSG msg;
	WNDCLASS wcl;
	HACCEL hAccel;
	INITCOMMONCONTROLSEX iccex;
	RECT rect;

	// Define a window class.
	wcl.lpszMenuName = NULL;
	wcl.hInstance = hThisInst;					// handle to this instance
	wcl.lpszClassName = szWinName;				// window class name
	wcl.lpfnWndProc = WindowFunc;				// window function
	wcl.style = 0;								// default style
	wcl.hIcon = LoadIcon(hThisInst, "icon1");	// load CB icon
	wcl.hCursor = LoadCursor(NULL, IDC_ARROW);	// cursor style
	wcl.cbClsExtra = 0;						// no extra
	wcl.cbWndExtra = 0;						// information needed
	wcl.hbrBackground = (HBRUSH) GetSysColorBrush(GetSysColor(COLOR_MENU));

	// register the window class
	if (!RegisterClass(&wcl))
		return 0;

	// create the window
	hwnd = CreateWindow(szWinName,			// name of window class
						"CheckerBoard:",	// title
						WS_OVERLAPPEDWINDOW ^ WS_MAXIMIZEBOX,	// window style - normal
						CW_USEDEFAULT,	// x coordinate - let windows decide
						CW_USEDEFAULT,	// y coordinate - let windows decide
						480,			// width
						560,			// height
						HWND_DESKTOP,	// no parent window
						NULL,			// no menu
						hThisInst,		// handle of this instance of the program
						NULL			// no additional arguments
						);

	// load settings from the registry
	createcheckerboard(hwnd);

	// display the window
	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	// set database filename in case of shell-doubleclick on a *.pdn file
	sprintf(filename, lpszArgs);

	// initialize common controls - toolbar and status bar need this
	iccex.dwSize = sizeof(INITCOMMONCONTROLSEX);
	iccex.dwICC = ICC_COOL_CLASSES | ICC_BAR_CLASSES;
	InitCommonControlsEx(&iccex);

	// save the instance in a global variable - the dialog boxes in dialogs.c need this
	g_hInst = hThisInst;

	// load the keyboard accelerator table
	hAccel = LoadAccelerators(hThisInst, "MENUENGLISH");

	// Initialize the Toolbar
	tbwnd = CreateAToolBar(hwnd);

	// get toolbar height
	GetWindowRect(tbwnd, &rect);
	toolbarheight = rect.bottom - rect.top;
	if (cboptions.use_incremental_time)
		toolbarheight += CLOCKHEIGHT;

	// initialize status bar
	InitStatus(hwnd);

	// get status bar height
	GetWindowRect(hStatusWnd, &rect);
	statusbarheight = rect.bottom - rect.top;

	// get menu and title bar height:
	menuheight = GetSystemMetrics(SM_CYMENU);
	titlebarheight = GetSystemMetrics(SM_CXSIZE);

	// get offsets before the board is printed for the first time
	offset = toolbarheight + statusbarheight - 1;
	upperoffset = toolbarheight - 1;
	setoffsets(offset, upperoffset);

	// start a timer @ 10Hz: every time this timer goes off, handletimer() is called
	// this updates the status bar and the toolbar
	SetTimer(hwnd, 1, 100, NULL);

	// create the message loop
	while (GetMessage(&msg, NULL, 0, 0)) {
		if (!TranslateAccelerator(hwnd, hAccel, &msg)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	return (int)msg.wParam;
}

void reset_game_clocks()
{
	if (cboptions.use_incremental_time) {
		black_time_remaining = cboptions.initial_time;
		white_time_remaining = cboptions.initial_time;
		starttime = clock();
	}
}

/*
 * Get the instantaneous values of time left of black and white clocks.
 * Add the value spent on thinking for the current move to
 * the clock value that was last updated at the start of its turn.
 */
void get_game_clocks(double *black_clock, double *white_clock)
{
	double newtime;

	newtime = (clock() - starttime) / (double)CLOCKS_PER_SEC;
	if (cbcolor == CB_BLACK) {
		*black_clock = black_time_remaining - newtime;
		*white_clock = white_time_remaining;
	}
	else {
		*black_clock = black_time_remaining;
		*white_clock = white_time_remaining - newtime;
	}
}

/*
 * Decided if the move described by moveindex is a first player or second player move.
 * If the game has a normal start position, even moves are first player, odd moves are second player.
 * If the game has a FEN setup, see if the start color is the same as the gametype's start color.
 * If the same, then even moves are first player, odd moves are second player.
 * If not the same, then odd moves are first player, even moves are second player.
 */
bool is_second_player(PDNgame &game, int moveindex)
{
	int startcolor;

	if (game.FEN[0] == 0) {
		if (moveindex & 1)
			return(true);
		else
			return(false);
	}

	startcolor = get_startcolor(game.gametype);
	if (game.FEN[0] == 'B' && startcolor == CB_BLACK || game.FEN[0] == 'W' && startcolor == CB_WHITE) {
		if (moveindex & 1)
			return(true);
		else
			return(false);
	}
	else {
		if (moveindex & 1)
			return(false);
		else
			return(true);
	}
}

int moveindex2movenum(PDNgame &game, int moveindex)
{
	if (game.FEN[0] == 0)
		return(1 + moveindex / 2);

	int startcolor = get_startcolor(game.gametype);
	if (game.FEN[0] == 'B' && startcolor == CB_BLACK || game.FEN[0] == 'W' && startcolor == CB_WHITE)
		return(1 + moveindex / 2);
	else
		return(1 + (moveindex + 1) / 2);
}

LRESULT CALLBACK WindowFunc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	// this is the main function of checkerboard. it receives messages from winmain(), and
	// then acts appropriately
	FILE *fp;
	LPRECT lprec;
	int x, y;
	char str2[256], Lstr[256];
	char str1024[1024];
	char *gamestring;
	static enum state laststate;
	static int oldengine;
	RECT windowrect;
	RECT WinDim;
	static int cxClient, cyClient;
	MENUBARINFO mbi;
	HINSTANCE hinst;

	switch (message) {
	case WM_CREATE:
		InitializeCriticalSection(&ani_criticalsection);
		InitializeCriticalSection(&engine_criticalsection);
		PostMessage(hwnd, WM_COMMAND, LOADENGINES, 0);
		break;

	case WM_NOTIFY:
		// respond to tooltip request //
		// lParam contains (LPTOOLTIPTEXT) - send it on to handletooltiprequest function
		handletooltiprequest((LPTOOLTIPTEXT) lParam);
		break;

	case WM_DROPFILES:
		DragQueryFile((HDROP) wParam, 0, database, sizeof(database));
		DragFinish((HDROP) wParam);
		PostMessage(hwnd, WM_COMMAND, GAMELOAD, 0);
		break;

	case WM_TIMER:
		// timer goes off, telling us to update status bar, toolbar
		// icons. handletimer does this, only if it's necessary.
		handletimer();
		break;

	case WM_RBUTTONDOWN:
		x = (int)(LOWORD(lParam) / xmetric);
		y = (int)(8 - (HIWORD(lParam) - toolbarheight) / ymetric);
		handle_rbuttondown(x, y);
		break;

	case WM_LBUTTONDOWN:
		x = (int)(LOWORD(lParam) / xmetric);
		y = (int)(8 - (HIWORD(lParam) - toolbarheight) / ymetric);
		handle_lbuttondown(x, y);
		break;

	case WM_PAINT:	// repaint window
		updategraphics(hwnd);
		break;

	case WM_SIZING: // keep window quadratic
		lprec = (LPRECT) lParam;
		mbi.cbSize = sizeof(MENUBARINFO);
		GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
		menuheight = mbi.rcBar.bottom - mbi.rcBar.top;
		offset = toolbarheight + statusbarheight - 1;
		upperoffset = toolbarheight - 1;
		setoffsets(offset, upperoffset);
		cxClient = lprec->right - lprec->left;
		cxClient -= cxClient % 8;
		cxClient += 2 * (GetSystemMetrics(SM_CXSIZEFRAME) - 4);
		cyClient = cxClient;
		lprec->right = lprec->left + cxClient;
		lprec->bottom = lprec->top + cyClient + offset + menuheight + titlebarheight + 2;	//+ cboptions.addoffset;
		break;

	case WM_SIZE:
		// window size has changed
		cxClient = LOWORD(lParam);
		cyClient = HIWORD(lParam);

		// check menu height
		mbi.cbSize = sizeof(MENUBARINFO);
		GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
		menuheight = mbi.rcBar.bottom - mbi.rcBar.top;
		offset = toolbarheight + statusbarheight - 1;
		upperoffset = toolbarheight - 1;
		setoffsets(offset, upperoffset);

		// get window size, set xmetric and ymetric which CB needs to know where user clicks
		GetClientRect(hwnd, &WinDim);
		xmetric = WinDim.right / 8.0;
		ymetric = (WinDim.bottom - offset) / 8.0;

		// get error:
		GetWindowRect(hwnd, &WinDim);

		// make window quadratic
		if ((xmetric - ymetric) * 8 != 0) {
			MoveWindow(hwnd,
					   WinDim.left,
					   WinDim.top,
					   WinDim.right - WinDim.left,
					   WinDim.bottom - WinDim.top + (int)((xmetric - ymetric) * 8),
					   1);
		}

		// update stretched stones etc
		resizegraphics(hwnd);
		updateboardgraphics(hwnd);
		SendMessage(hStatusWnd, WM_SIZE, wParam, lParam);
		SendMessage(tbwnd, WM_SIZE, wParam, lParam);
		break;

	case WM_COMMAND:
		// the following case structure handles user command (and also internal commands
		// that CB may generate itself
		switch (LOWORD(wParam)) {
		case LOADENGINES:
			hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) initengines, (HWND) 0, 0, &g_ThreadId);
			break;

		case GAMENEW:
			PostMessage(hwnd, WM_COMMAND, ABORTENGINE, 0);
			newgame();
			break;

		case GAMEANALYZE:
			if (CBstate == BOOKVIEW || CBstate == BOOKADD)
				break;
			changeCBstate(CBstate, ANALYZEGAME);
			startmatch = TRUE;

			// the rest is taken care of in the AutoThreadFunc section
			break;

		case GAMEANALYZEPDN:
			if (CBstate == BOOKVIEW || CBstate == BOOKADD)
				break;
			changeCBstate(CBstate, ANALYZEPDN);
			startmatch = TRUE;

			// the rest is taken care of in the AutoThreadFunc section
			break;

		case GAME3MOVE:
			PostMessage(hwnd, WM_COMMAND, ABORTENGINE, 0);
			if (gametype() == GT_ENGLISH) {
				if (cboptions.op_crossboard || cboptions.op_barred || cboptions.op_mailplay) {
					op = getopening(&cboptions);
					PostMessage(hwnd, WM_COMMAND, START3MOVE, 0);
				}
				else
					MessageBox(hwnd, "nothing selected in the 3-move deck!", "Error", MB_OK);
			}
			else {
				MessageBox(hwnd,
						   "This option is only for engines\nwhich play the english/american\nversion of checkers.",
						   "Error",
						   MB_OK);
			}
			break;

		case START3MOVE:
			start3move();
			break;

		case GAMEREPLACE:
			// replace a game in the pdn database
			// assumption: you have loaded a game, so now "database" holds the db filename
			// and gameindex is the index of the game in the file
			handlegamereplace(gameindex, database);
			break;

		case GAMESAVE:
			// show save game dialog. if OK, call 'dosave' to do the work
			SetCurrentDirectory(cboptions.userdirectory);
			if (DialogBox(g_hInst, "IDD_SAVEGAME", hwnd, (DLGPROC) DialogFuncSavegame)) {
				if (getfilename(filename, OF_SAVEGAME)) {
					SendMessage(hwnd, WM_COMMAND, DOSAVE, 0);
				}
			}

			SetCurrentDirectory(CBdirectory);
			break;

		case GAMESAVEASHTML:
			// show save game dialog. if OK is selected, call 'savehtml' to do the work
			if (DialogBox(g_hInst, "IDD_SAVEGAME", hwnd, (DLGPROC) DialogFuncSavegame)) {
				if (getfilename(filename, OF_SAVEASHTML)) {
					saveashtml(filename, &cbgame);
					sprintf(statusbar_txt, "game saved as HTML!");
				}
			}
			break;

		case DOSAVE:
			// saves the game stored in cbgame
			fp = fopen(filename, "at+");

			// file with filename opened	- we append to that file
			// filename was set by save game
			if (fp != NULL) {
				gamestring = (char *)malloc(GAMEBUFSIZE);
				if (gamestring != NULL) {
					PDNgametoPDNstring(cbgame, gamestring, "\n");
					fprintf(fp, "%s", gamestring);
					free(gamestring);
				}

				fclose(fp);
			}

			// set reindex flag
			reindex = 1;
			break;

		case GAMEDATABASE:
			// set working database
			sprintf(database, "%s", cboptions.userdirectory);
			getfilename(database, OF_LOADGAME);

			// set reindex flag
			reindex = 1;
			break;

		case SELECTUSERBOOK:
			// set user book.
			sprintf(userbookname, "%s", CBdocuments);
			if (getfilename(userbookname, OF_USERBOOK)) {

				// load user book
				fp = fopen(userbookname, "rb");
				if (fp != NULL) {
					userbooknum = fread(userbook, sizeof(struct userbookentry), MAXUSERBOOK, fp);
					fclose(fp);
				}

				sprintf(statusbar_txt, "found %zi positions in user book", userbooknum);
			}
			break;

		case GAMELOAD:
			// call selectgame with GAMELOAD to let the user select from all games
			cblog("pdn load game\n");
			selectgame(GAMELOAD);
			break;

		case GAMEINFO:
			// display a box with information on the game
			//cpuid(str);
			sprintf(str1024,
					"Black: %s\nWhite: %s\nEvent: %s\nResult: %s",
					cbgame.black,
					cbgame.white,
					cbgame.event,
					cbgame.resultstring);
			MessageBox(hwnd, str1024, "Game information", MB_OK);
			sprintf(statusbar_txt, "");
			break;

		case SEARCHMASK:
			// call selectgame with SEARCHMASK to let the user
			// select from games of a certain player/event/date
			cblog("pdn search with player, event, or date.\n");
			selectgame(SEARCHMASK);
			break;

		case RE_SEARCH:
			cblog("pdn research\n");
			selectgame(RE_SEARCH);
			break;

		case GAMEFIND:
			// find a game with the current position in the current database
			// index the database
			cblog("find current pos\n");
			selectgame(GAMEFIND);
			break;

		case GAMEFINDCR:
			// find games with current position color-reversed
			selectgame(GAMEFINDCR);
			cblog("find colors reversed pos\n");
			break;

		case GAMEFINDTHEME:
			// find a game with the current position in the current database
			// index the database
			selectgame(GAMEFINDTHEME);
			cblog("find theme\n");
			break;

		case LOADNEXT:
			sprintf(statusbar_txt, "load next game");
			cblog("load next game\n");
			loadnextgame();
			break;

		case LOADPREVIOUS:
			sprintf(statusbar_txt, "load previous game");
			cblog("load previous game\n");
			loadpreviousgame();
			break;

		case GAMEEXIT:
			PostMessage(hwnd, WM_DESTROY, 0, 0);
			break;

		case DIAGRAM:
			diagramtoclipboard(hwnd);
			break;

		case SAMPLEDIAGRAM:
			samplediagramtoclipboard(hwnd);
			break;

		case GAME_FENTOCLIPBOARD:
			if (setup) {
				MessageBox(hwnd,
						   "Cannot copy position in setup mode.\nLeave the setup mode first if you\nwant to copy this position.",
					   "Error",
						   MB_OK);
			}
			else
				FENtoclipboard(hwnd, cbboard8, cbcolor, cbgame.gametype);
			break;

		case GAME_FENFROMCLIPBOARD:
			// first, get the stuff that is in the clipboard
			gamestring = textfromclipboard(hwnd, statusbar_txt);

			// now if we have something, do something with it
			if (gamestring != NULL) {
				PostMessage(hwnd, WM_COMMAND, ABORTENGINE, 0);

				if (FENtoboard8(cbboard8, gamestring, &cbcolor, cbgame.gametype)) {
					updateboardgraphics(hwnd);
					reset_move_history = true;
					newposition = TRUE;
					sprintf(statusbar_txt, "position copied");
					PostMessage(hwnd, WM_COMMAND, GAMEINFO, 0);
					sprintf(cbgame.setup, "1");
					sprintf(cbgame.FEN, gamestring);
				}
				else
					sprintf(statusbar_txt, "no valid FEN position in clipboard!");
				free(gamestring);
			}
			break;

		case GAMECOPY:
			if (setup) {
				MessageBox(hwnd,
						   "Cannot copy game in setup mode.\nLeave the setup mode first if you\nwant to copy this game.",
						   "Error",
						   MB_OK);
			}
			else
				PDNtoclipboard(hwnd, cbgame);
			break;

		case GAMEPASTE:
			// copy game or fen string from the clipboard...
			gamestring = textfromclipboard(hwnd, statusbar_txt);

			// now that the game is in gamestring doload() on it
			if (gamestring != NULL) {
				PostMessage(hwnd, WM_COMMAND, ABORTENGINE, 0);

				/* Detect fen or game, load it in either case. */
				if (is_fen(gamestring)) {
					if (!FENtoboard8(cbboard8, gamestring, &cbcolor, cbgame.gametype)) {
						doload(&cbgame, gamestring, &cbcolor, cbboard8);
						sprintf(statusbar_txt, "game copied");
					}
					else {
						reset_game(cbgame);
						sprintf(statusbar_txt, "position copied");
					}
				}
				else {
					doload(&cbgame, gamestring, &cbcolor, cbboard8);
					sprintf(statusbar_txt, "position copied");
				}

				free(gamestring);

				// game is fully loaded, clean up
				updateboardgraphics(hwnd);
				reset_move_history = true;
				newposition = TRUE;
				PostMessage(hwnd, WM_COMMAND, GAMEINFO, 0);
			}
			else
				sprintf(statusbar_txt, "clipboard open failed");
			break;

		case MOVESPLAY:
			// force the engine to either play now, or to start calculating
			// this is the only place where the engine is started
			if (!getenginebusy() && !getanimationbusy()) {

				// TODO think about synchronization issues here!
				setenginebusy(TRUE);
				setenginestarting(FALSE);
				CloseHandle(hThread);
				hThread = CreateThread(NULL, 100000, (LPTHREAD_START_ROUTINE) ThreadFunc, (LPVOID) 0, 0, &g_ThreadId);
			}
			else
				SendMessage(hwnd, WM_COMMAND, INTERRUPTENGINE, 0);
			x1 = -1;
			break;

		case INTERRUPTENGINE:
			// tell engine to stop thinking and play a move
			if (getenginebusy())
				playnow = 1;
			break;

		case ABORTENGINE:
			// tell engine to stop thinking and not play a move
			if (getenginebusy()) {
				abortcalculation = 1;
				playnow = 1;
			}
			break;

		case MOVESBACK:
			// take back a move
			abortengine();
			if (CBstate == BOOKVIEW && userbooknum != 0) {
				if (userbookcur > 0)
					userbookcur--;
				userbookcur %= userbooknum;
				sprintf(statusbar_txt,
						"position %zi of %zi: %i-%i",
						userbookcur + 1,
						userbooknum,
						coortonumber(userbook[userbookcur].move.from, cbgame.gametype),
						coortonumber(userbook[userbookcur].move.to, cbgame.gametype));

				// set up position
				if (userbookcur < userbooknum) {

					// only if there are any positions
					bitboardtoboard8(&(userbook[userbookcur].position), cbboard8);
					updateboardgraphics(hwnd);
				}
				break;
			}

			if (cbgame.movesindex == 0 && (CBstate == ANALYZEGAME || CBstate == ANALYZEPDN))
				gameover = TRUE;

			if (cbgame.movesindex > 0) {
				--cbgame.movesindex;

				gamebody_entry *tbmove = &cbgame.moves[cbgame.movesindex];
				undomove(tbmove->move, cbboard8);
				updateboardgraphics(hwnd);

				// shouldnt this color thing be handled in undomove?
				cbcolor = CB_CHANGECOLOR(cbcolor);
				sprintf(statusbar_txt, "takeback: ");

				// and print move number and move into the status bar
				// get move number:
				if (is_second_player(cbgame, cbgame.movesindex))
					sprintf(Lstr, "%i... %s", moveindex2movenum(cbgame, cbgame.movesindex), tbmove->PDN);
				else
					sprintf(Lstr, "%i. %s", moveindex2movenum(cbgame, cbgame.movesindex), tbmove->PDN);
				strcat(statusbar_txt, Lstr);

				if (strcmp(tbmove->comment, "") != 0) {
					sprintf(Lstr, " %s", tbmove->comment);
					strcat(statusbar_txt, Lstr);
				}

				if (CBstate == OBSERVEGAME)
					PostMessage(hwnd, WM_COMMAND, INTERRUPTENGINE, 0);
				else
					abortengine();
			}
			else
				sprintf(statusbar_txt, "Takeback not possible: you are at the start of the game!");

			newposition = TRUE;
			reset_move_history = true;
			break;

		case MOVESFORWARD:
			// go forward one move
			// stop the engine if it is still running
			abortengine();

			// if in user book mode, move to the next position in user book
			if (CBstate == BOOKVIEW && userbooknum != 0) {
				if (userbookcur < userbooknum - 1)
					userbookcur++;
				userbookcur %= userbooknum;
				sprintf(statusbar_txt,
						"position %zi of %zi: %i-%i",
						userbookcur + 1,
						userbooknum,
						coortonumber(userbook[userbookcur].move.from, cbgame.gametype),
						coortonumber(userbook[userbookcur].move.to, cbgame.gametype));

				// set up position
				if (userbookcur < userbooknum) {

					// only if there are any positions
					bitboardtoboard8(&(userbook[userbookcur].position), cbboard8);
					updateboardgraphics(hwnd);
				}
				break;
			}

			// normal case - move forward one move
			if (cbgame.movesindex < (int)cbgame.moves.size()) {
				gamebody_entry *pmove = &cbgame.moves[cbgame.movesindex];
				domove(pmove->move, cbboard8);
				updateboardgraphics(hwnd);
				cbcolor = CB_CHANGECOLOR(cbcolor);

				// get move number:
				// and print move number and move into the status bar
				if (is_second_player(cbgame, cbgame.movesindex))
					sprintf(Lstr, "%i... %s", moveindex2movenum(cbgame, cbgame.movesindex), pmove->PDN);
				else
					sprintf(Lstr, "%i. %s", moveindex2movenum(cbgame, cbgame.movesindex), pmove->PDN);
				sprintf(statusbar_txt, "%s ", Lstr);

				if (strcmp(pmove->comment, "") != 0) {
					sprintf(Lstr, "%s", pmove->comment);
					strcat(statusbar_txt, Lstr);
				}

				++cbgame.movesindex;

				if (CBstate == OBSERVEGAME)
					PostMessage(hwnd, WM_COMMAND, INTERRUPTENGINE, 0);
				newposition = TRUE;
				reset_move_history = true;
			}
			else
				sprintf(statusbar_txt, "Forward not possible: End of game");
			break;

		case MOVESBACKALL:
			// take back all moves
			if (CBstate == BOOKVIEW || CBstate == BOOKADD)
				break;
			PostMessage(hwnd, WM_COMMAND, ABORTENGINE, 0);
			while (cbgame.movesindex > 0) {
				--cbgame.movesindex;
				undomove(cbgame.moves[cbgame.movesindex].move, cbboard8);
				cbcolor = CB_CHANGECOLOR(cbcolor);
			}

			if (CBstate == OBSERVEGAME)
				PostMessage(hwnd, WM_COMMAND, INTERRUPTENGINE, 0);
			updateboardgraphics(hwnd);
			sprintf(statusbar_txt, "you are now at the start of the game");
			newposition = TRUE;
			reset_move_history = true;
			break;

		case MOVESFORWARDALL:
			// go forward all moves
			if (CBstate == BOOKVIEW || CBstate == BOOKADD)
				break;
			PostMessage(hwnd, WM_COMMAND, ABORTENGINE, 0);
			forward_to_game_end();
			if (CBstate == OBSERVEGAME)
				PostMessage(hwnd, WM_COMMAND, INTERRUPTENGINE, 0);
			updateboardgraphics(hwnd);
			sprintf(statusbar_txt, "you are now at the end of the game");
			newposition = TRUE;
			reset_move_history = true;
			break;

		case MOVESCOMMENT:
			// add a comment to the last move
			DialogBox(g_hInst, "IDD_COMMENT", hwnd, (DLGPROC) DialogFuncAddcomment);
			break;

		case LEVELEXACT:
			if (cboptions.exact_time) {
				cboptions.exact_time = false;
				CheckMenuItem(hmenu, LEVELEXACT, MF_UNCHECKED);
			}
			else {
				cboptions.exact_time = true;
				CheckMenuItem(hmenu, LEVELEXACT, MF_CHECKED);
			}
			break;

		case LEVELINSTANT:
		case LEVEL01S:
		case LEVEL02S:
		case LEVEL05S:
		case LEVEL1S:
		case LEVEL2S:
		case LEVEL5S:
		case LEVEL10S:
		case LEVEL15S:
		case LEVEL30S:
		case LEVEL1M:
		case LEVEL2M:
		case LEVEL5M:
		case LEVEL15M:
		case LEVEL30M:
		case LEVELINFINITE:
			cboptions.use_incremental_time = false;

			RECT rect;
			GetWindowRect(tbwnd, &rect);
			toolbarheight = rect.bottom - rect.top;
			PostMessage(hwnd, (UINT) WM_SIZE, (WPARAM) 0, (LPARAM) 0);

			cboptions.level = timetoken_to_level(LOWORD(wParam));
			checklevelmenu(&cboptions, hmenu, LOWORD(wParam));
			if (LOWORD(wParam) == LEVELINFINITE)
				sprintf(statusbar_txt, "search time set to infinite");
			else
				sprintf(statusbar_txt, "search time set to %.1f sec/move", timelevel_to_time(cboptions.level));
			break;

		case LEVELINCREMENT:
			DialogBox(g_hInst, MAKEINTRESOURCE(IDD_INCREMENTAL_TIMES), hwnd, (DLGPROC) DialogIncrementalTimesFunc);
			if (cboptions.use_incremental_time) {
				reset_game_clocks();

				RECT rect;

				GetWindowRect(tbwnd, &rect);
				toolbarheight = CLOCKHEIGHT + rect.bottom - rect.top;
				checklevelmenu(&cboptions, hmenu, timelevel_to_token(cboptions.level));
				sprintf(statusbar_txt,
						"incremental time set: initial time %.0f sec, increment %.3f sec",
						cboptions.initial_time,
						cboptions.time_increment);
				PostMessage(hwnd, (UINT) WM_SIZE, (WPARAM) 0, (LPARAM) 0);
			}
			break;

		case LEVELADDTIME:
			// add 1 second when '+' is pressed
			if (cboptions.use_incremental_time) {
				if (cbcolor == CB_BLACK) {
					black_time_remaining += 1.0;
					sprintf(statusbar_txt, "black remaining time: %.1f", black_time_remaining);
				}
				else {
					white_time_remaining += 1.0;
					sprintf(statusbar_txt, "white remaining time: %.1f", white_time_remaining);
				}
			}
			else
				sprintf(statusbar_txt, "not in increment mode!");
			break;

		case LEVELSUBTRACTTIME:
			// subtract 1 second when '-' is pressed
			if (cboptions.use_incremental_time) {
				if (cbcolor == CB_BLACK) {
					black_time_remaining -= 1.0;
					sprintf(statusbar_txt, "black remaining time: %.1f", black_time_remaining);
				}
				else {
					white_time_remaining -= 1.0;
					sprintf(statusbar_txt, "white remaining time: %.1f", white_time_remaining);
				}
			}
			else
				sprintf(statusbar_txt, "not in increment mode!");
			break;

		// piece sets
		case PIECESET:
		case PIECESET + 1:
		case PIECESET + 2:
		case PIECESET + 3:
		case PIECESET + 4:
		case PIECESET + 5:
		case PIECESET + 6:
		case PIECESET + 7:
		case PIECESET + 8:
		case PIECESET + 9:
		case PIECESET + 10:
		case PIECESET + 11:
		case PIECESET + 12:
		case PIECESET + 13:
		case PIECESET + 14:
		case PIECESET + 15:
			cboptions.piecesetindex = LOWORD(wParam) - PIECESET;
			sprintf(statusbar_txt, "piece set %i: %s", cboptions.piecesetindex, piecesetname[cboptions.piecesetindex]);
			SetCurrentDirectory(CBdirectory);
			SetCurrentDirectory("bmp");
			initbmp(hwnd, piecesetname[cboptions.piecesetindex]);
			resizegraphics(hwnd);
			updateboardgraphics(hwnd);
			InvalidateRect(hwnd, NULL, 1);
			SetCurrentDirectory(CBdirectory);
			break;

		//  set highlighting color to draw frame around selected stone square
		case COLORHIGHLIGHT:
			initcolorstruct(hwnd, &ccs, 0);
			if (ChooseColor(&ccs)) {
				cboptions.colors[0] = (COLORREF) ccs.rgbResult;
				sprintf(statusbar_txt, "new highlighting color");
			}
			else
				sprintf(statusbar_txt, "no new colors! error %i", CommDlgExtendedError());
			updateboardgraphics(hwnd);
			break;

		//  set color for board numbers
		case COLORBOARDNUMBERS:
			initcolorstruct(hwnd, &ccs, 1);
			if (ChooseColor(&ccs)) {
				cboptions.colors[1] = (COLORREF) ccs.rgbResult;
				sprintf(statusbar_txt, "new board number color");
			}
			else
				sprintf(statusbar_txt, "no new colors! error %i", CommDlgExtendedError());
			updateboardgraphics(hwnd);
			break;

		case OPTIONS3MOVE:
			DialogBox(g_hInst, "IDD_3MOVE", hwnd, (DLGPROC) ThreeMoveDialogFunc);
			break;

		case OPTIONSDIRECTORIES:
			DialogBox(g_hInst, "IDD_DIRECTORIES", hwnd, (DLGPROC) DirectoryDialogFunc);
			break;

		case OPTIONSUSERBOOK:
			toggle(&(cboptions.userbook));
			setmenuchecks(&cboptions, hmenu);
			break;

		case OPTIONSLANGUAGEENGLISH:
			SetMenuLanguage(OPTIONSLANGUAGEENGLISH);
			break;

		case OPTIONSLANGUAGEDEUTSCH:
			SetMenuLanguage(OPTIONSLANGUAGEDEUTSCH);
			break;

		case OPTIONSLANGUAGEESPANOL:
			SetMenuLanguage(OPTIONSLANGUAGEESPANOL);
			break;

		case OPTIONSLANGUAGEITALIANO:
			SetMenuLanguage(OPTIONSLANGUAGEITALIANO);
			break;

		case OPTIONSLANGUAGEFRANCAIS:
			SetMenuLanguage(OPTIONSLANGUAGEFRANCAIS);
			break;

		case OPTIONSPRIORITY:
			toggle(&(cboptions.priority));
			if (cboptions.priority) // low priority mode
				usersetpriority = THREAD_PRIORITY_BELOW_NORMAL;
			else
				usersetpriority = THREAD_PRIORITY_NORMAL;
			setmenuchecks(&cboptions, hmenu);
			break;

		case OPTIONSHIGHLIGHT:
			if (cboptions.highlight == TRUE)
				cboptions.highlight = FALSE;
			else
				cboptions.highlight = TRUE;
			if (cboptions.highlight == TRUE)
				CheckMenuItem(hmenu, OPTIONSHIGHLIGHT, MF_CHECKED);
			else
				CheckMenuItem(hmenu, OPTIONSHIGHLIGHT, MF_UNCHECKED);
			break;

		case OPTIONSSOUND:
			if (cboptions.sound == TRUE)
				cboptions.sound = FALSE;
			else
				cboptions.sound = TRUE;
			if (cboptions.sound == TRUE)
				CheckMenuItem(hmenu, OPTIONSSOUND, MF_CHECKED);
			else
				CheckMenuItem(hmenu, OPTIONSSOUND, MF_UNCHECKED);
			break;

		case BOOKMODE_VIEW:
			// go in view book mode
			if (userbooknum == 0) {
				sprintf(statusbar_txt, "no moves in user book");
				break;
			}

			if (CBstate == BOOKVIEW)
				changeCBstate(CBstate, NORMAL);
			else {
				changeCBstate(CBstate, BOOKVIEW);

				// now display the first user book position
				userbookcur = 0;
				sprintf(statusbar_txt,
						"position %zi of %zi: %i-%i",
						userbookcur + 1,
						userbooknum,
						coortonumber(userbook[userbookcur].move.from, cbgame.gametype),
						coortonumber(userbook[userbookcur].move.to, cbgame.gametype));

				// set up position
				if (userbookcur < userbooknum) {

					// only if there are any positions
					bitboardtoboard8(&(userbook[userbookcur].position), cbboard8);
					updateboardgraphics(hwnd);
				}
			}
			break;

		case BOOKMODE_ADD:
			// go in add/edit book mode
			if (CBstate == BOOKADD)
				changeCBstate(CBstate, NORMAL);
			else
				changeCBstate(CBstate, BOOKADD);
			break;

		case BOOKMODE_DELETE:
			// remove current user book position from book
			if (CBstate == BOOKVIEW && userbooknum != 0) {

				// want to delete book move here:
				for (size_t i = userbookcur; i < userbooknum - 1; i++)
					userbook[i] = userbook[i + 1];
				userbooknum--;

				// if we deleted last position, move to new last position.
				if (userbookcur == userbooknum)
					userbookcur--;

				// display what position we have:
				sprintf(statusbar_txt,
						"position %zi of %zi: %i-%i",
						userbookcur + 1,
						userbooknum,
						coortonumber(userbook[userbookcur].move.from, cbgame.gametype),
						coortonumber(userbook[userbookcur].move.to, cbgame.gametype));
				if (userbooknum == 0)
					sprintf(statusbar_txt, "no moves in user book");

				// set up position
				if (userbookcur < userbooknum && userbooknum != 0) {

					// only if there are any positions
					bitboardtoboard8(&(userbook[userbookcur].position), cbboard8);
					updateboardgraphics(hwnd);
				}

				// save user book
				fp = fopen(userbookname, "wb");
				if (fp != NULL) {
					fwrite(userbook, sizeof(struct userbookentry), userbooknum, fp);
					fclose(fp);
				}
				else
					sprintf(statusbar_txt, "unable to write to user book");
			}
			else {
				if (CBstate == BOOKVIEW)
					sprintf(statusbar_txt, "no moves in user book");
				else
					sprintf(statusbar_txt, "You must be in 'view user book' mode to delete moves!");
			}
			break;

		case CM_NORMAL:
			// go to normal play mode
			if (getenginebusy())
				playnow = 1;

			// stop engine
			PostMessage(hwnd, WM_COMMAND, GOTONORMAL, 0);
			break;

		case CM_AUTOPLAY:
			if (CBstate == BOOKVIEW || CBstate == BOOKADD)
				break;
			changeCBstate(CBstate, AUTOPLAY);

			//setenginebusy(FALSE); // what is this here for?
			break;

		case CM_ENGINEMATCH:
			if (CBstate == BOOKVIEW || CBstate == BOOKADD)
				break;
			changeCBstate(CBstate, ENGINEMATCH);
			break;

		case ENGINEVSENGINE:
			startmatch = TRUE;
			changeCBstate(CBstate, ENGINEGAME);
			break;

		case CM_ANALYSIS:
			// go to analysis mode
			if (CBstate == BOOKVIEW || CBstate == BOOKADD)
				break;
			changeCBstate(CBstate, OBSERVEGAME);
			break;

		case CM_2PLAYER:
			SendMessage(hwnd, WM_COMMAND, TOGGLEMODE, 0);
			break;

		case GOTONORMAL:
			// the following is weird, posts the same message again, why?
			if (getenginebusy()) {
				PostMessage(hwnd, WM_COMMAND, GOTONORMAL, 0);
				Sleep(10);
			}
			else {
				changeCBstate(CBstate, NORMAL);

				//setenginebusy(FALSE);
				//setanimationbusy(FALSE);
			}
			break;

		case ENGINESELECT:
			// select engines
			DialogBox(g_hInst, "IDD_DIALOGENGINES", hwnd, (DLGPROC) EngineDialogFunc);
			break;

		case ENGINEOPTIONS:
			// select engine options
			oldengine = currentengine;
			DialogBox(g_hInst, "IDD_ENGINEOPTIONS", hwnd, (DLGPROC) EngineOptionsFunc);
			setcurrentengine(oldengine);
			enginecommand("get book", Lstr);
			togglebook = atoi(Lstr);
			break;

		case ENGINEEVAL:
			// static eval of the current positions
			board8toFEN(cbboard8, str2, cbcolor, cbgame.gametype);
			sprintf(Lstr, "staticevaluation %s", str2);
			if (enginecommand(Lstr, reply))
				MessageBox(hwnd, reply, "Static Evaluation", MB_OK);
			else
				MessageBox(hwnd, "This engine does not support\nstatic evaluation", "About Engine", MB_OK);
			break;

		case ENGINEABOUT:
			// tell engine to display information about itself
			if (enginecommand("about", reply))
				MessageBox(hwnd, reply, "About Engine", MB_OK);
			break;

		case ENGINEHELP:
			// get a help-filename from engine and display file with default viewer
			if (enginecommand("help", str2)) {
				if (strcmp(str2, "")) {
					SetCurrentDirectory(CBdirectory);
					SetCurrentDirectory("engines");
					showfile(str2);
					SetCurrentDirectory(CBdirectory);
				}
				else
					MessageBox(hwnd, "This engine has no help file", "CheckerBoard says:", MB_OK);
			}
			else
				MessageBox(hwnd, "This engine has no help file", "CheckerBoard says:", MB_OK);
			break;

		case DISPLAYINVERT:
			// toggle: invert the board yes/no
			toggle(&(cboptions.invert));
			if (cboptions.invert)
				CheckMenuItem(hmenu, DISPLAYINVERT, MF_CHECKED);
			else
				CheckMenuItem(hmenu, DISPLAYINVERT, MF_UNCHECKED);
			updateboardgraphics(hwnd);
			break;

		case DISPLAYMIRROR:
			// toggle: mirror the board yes/no
			// this is a trick to make checkers variants like italian display properly.
			toggle(&(cboptions.mirror));
			if (cboptions.mirror)
				CheckMenuItem(hmenu, DISPLAYMIRROR, MF_CHECKED);
			else
				CheckMenuItem(hmenu, DISPLAYMIRROR, MF_UNCHECKED);
			updateboardgraphics(hwnd);
			break;

		case DISPLAYNUMBERS:
			// toggle display numbers on / off
			toggle(&(cboptions.numbers));
			if (cboptions.numbers)
				CheckMenuItem(hmenu, DISPLAYNUMBERS, MF_CHECKED);
			else
				CheckMenuItem(hmenu, DISPLAYNUMBERS, MF_UNCHECKED);
			updateboardgraphics(hwnd);
			break;

		case SETUPMODE:
			// toggle from play to setup mode
			PostMessage(hwnd, WM_COMMAND, ABORTENGINE, 0);
			toggle(&setup);
			if (setup) {

				// entering setup mode
				CheckMenuItem(hmenu, SETUPMODE, MF_CHECKED);
			}
			else {

				// leaving setup mode;
				CheckMenuItem(hmenu, SETUPMODE, MF_UNCHECKED);
				reset_move_history = true;
				x1 = -1;
			}

			if (setup)
				sprintf(statusbar_txt, "Setup mode...");
			if (!setup) {
				sprintf(statusbar_txt, "Setup done");

				// get FEN string
				reset_game(cbgame);
				board8toFEN(cbboard8, cbgame.FEN, cbcolor, cbgame.gametype);
				sprintf(cbgame.setup, "1");
			}
			break;

		case SETUPCLEAR:
			// clear board
			memset(cbboard8, 0, sizeof(cbboard8));
			updateboardgraphics(hwnd);
			break;

		case TOGGLEMODE:
			if (getenginebusy() || getanimationbusy())
				break;
			toggle(&togglemode);
			if (togglemode)
				changeCBstate(CBstate, ENTERGAME);
			else
				changeCBstate(CBstate, NORMAL);
			break;

		case TOGGLEBOOK:
			if (getenginebusy() || getanimationbusy())
				break;
			togglebook++;
			togglebook %= 4;

			// set opening book on/off
			sprintf(Lstr, "set book %i", togglebook);
			enginecommand(Lstr, statusbar_txt);
			break;

		case TOGGLEENGINE:
			if (getenginebusy() || getanimationbusy())
				break;
			toggleengine++;
			if (toggleengine > 2)
				toggleengine = 1;

			setcurrentengine(toggleengine);

			// reset game if an engine of different game type was selected!
			if (gametype() != cbgame.gametype) {
				PostMessage(hwnd, (UINT) WM_COMMAND, (WPARAM) GAMENEW, (LPARAM) 0);
				PostMessage(hwnd, (UINT) WM_SIZE, (WPARAM) 0, (LPARAM) 0);
			}
			break;

		case SETUPCC:
			handlesetupcc(&cbcolor);
			break;

		case HELPHELP:
			// open the help.htm file with default .htm viewer
			SetCurrentDirectory(CBdirectory);
			switch (cboptions.language) {
			case ESPANOL:
				showfile("helpspanish.htm");
				break;

			case DEUTSCH:
				if (fileispresent("helpdeutsch.htm"))
					showfile("helpdeutsch.htm");
				else
					showfile("help.htm");
				break;

			case ITALIANO:
				if (fileispresent("helpitaliano.htm"))
					showfile("helpitaliano.htm");
				else
					showfile("help.htm");
				break;

			case FRANCAIS:
				if (fileispresent("helpfrancais.htm"))
					showfile("helpfrancais.htm");
				else
					showfile("help.htm");
				break;

			case ENGLISH:
				showfile("help.htm");
				break;
			}
			break;

		case HELPCHECKERSINANUTSHELL:
			// open the richard pask's tutorial with default .htm viewer
			showfile("nutshell.htm");
			break;

		case HELPABOUT:
			// display a an about box
			DialogBox(g_hInst, "IDD_CBABOUT", hwnd, (DLGPROC) AboutDialogFunc);
			break;

		case HELPHOMEPAGE:
			// open the checkerboard homepage with default htm viewer
			hinst = ShellExecute(NULL, "open", "www.fierz.ch/checkers.htm", NULL, NULL, SW_SHOW);
			break;

		case CM_ENGINECOMMAND:
			DialogBox(g_hInst, "IDD_ENGINECOMMAND", hwnd, (DLGPROC) DialogFuncEnginecommand);
			break;

		case CM_ADDCOMMENT:
			toggle(&addcomment);
			if (addcomment)
				CheckMenuItem(hmenu, CM_ADDCOMMENT, MF_CHECKED);
			else
				CheckMenuItem(hmenu, CM_ADDCOMMENT, MF_UNCHECKED);
			break;

		case CM_HANDICAP:
			toggle(&handicap);
			if (handicap == TRUE)
				CheckMenuItem(hmenu, CM_HANDICAP, MF_CHECKED);
			else
				CheckMenuItem(hmenu, CM_HANDICAP, MF_UNCHECKED);
			break;

		case CM_RUNTESTSET:
			// let CB run over a set of test positions in the current pdn database
			testset_number = 0;
			changeCBstate(CBstate, RUNTESTSET);
			break;
		}
		break;

	case WM_DESTROY:
		// terminate the program
		// save window size:
		GetWindowRect(hwnd, &windowrect);
		cboptions.window_x = windowrect.left;
		cboptions.window_y = windowrect.top;
		cboptions.window_width = windowrect.right - windowrect.left;
		cboptions.window_height = windowrect.bottom - windowrect.top;

		// save settings
		savesettings(&cboptions);

		//Shell_NotifyIcon(NIM_DELETE,&pnid); // remove tray icon
		// unload engines
		fFreeResult = FreeLibrary(hinstLib1);
		fFreeResult = FreeLibrary(hinstLib2);

		PostQuitMessage(0);
		break;

	default:
		// Let Windows process any messages not specified
		//	in the preceding switch statement
		return DefWindowProc(hwnd, message, wParam, lParam);
	}

	return 0;
}

void reset_game(PDNgame &game)
{
	sprintf(game.black, "");
	sprintf(game.white, "");
	sprintf(game.resultstring, "*");
	sprintf(game.event, "");
	sprintf(game.date, "");
	sprintf(game.FEN, "");
	sprintf(game.round, "");
	sprintf(game.setup, "");
	sprintf(game.site, "");
	game.result = CB_UNKNOWN;
	game.moves.clear();
	game.movesindex = 0;
	game.gametype = gametype();
}

int SetMenuLanguage(int language)
{
	// load the proper menu
	DestroyMenu(hmenu);
	switch (language) {
	case ENGLISH:
	case OPTIONSLANGUAGEENGLISH:
		hmenu = LoadMenu(g_hInst, "MENUENGLISH");
		cboptions.language = ENGLISH;
		SetMenu(hwnd, hmenu);
		break;

	case DEUTSCH:
	case OPTIONSLANGUAGEDEUTSCH:
		hmenu = LoadMenu(g_hInst, "MENUDEUTSCH");
		cboptions.language = DEUTSCH;
		SetMenu(hwnd, hmenu);
		break;

	case ESPANOL:
	case OPTIONSLANGUAGEESPANOL:
		hmenu = LoadMenu(g_hInst, "MENUESPANOL");
		cboptions.language = ESPANOL;
		SetMenu(hwnd, hmenu);
		break;

	case ITALIANO:
	case OPTIONSLANGUAGEITALIANO:
		hmenu = LoadMenu(g_hInst, "MENUITALIANO");
		cboptions.language = ITALIANO;
		SetMenu(hwnd, hmenu);
		break;

	case FRANCAIS:
	case OPTIONSLANGUAGEFRANCAIS:
		hmenu = LoadMenu(g_hInst, "MENUFRANCAIS");
		cboptions.language = FRANCAIS;
		SetMenu(hwnd, hmenu);
		break;
	}

	// delete stuff we don't need
	SetCurrentDirectory(CBdirectory);
	if (fileispresent("db\\db6.cpr"))
		DeleteMenu(hmenu, 8, MF_BYPOSITION);

	// now insert stuff we do need: piece set choice depending on what is installed
	add_piecesets_to_menu(hmenu);

	DrawMenuBar(hwnd);
	return 1;
}

int get_startcolor(int gametype)
{
	int color = CB_BLACK;

	if (gametype == GT_ENGLISH)
		color = CB_BLACK;
	else if (gametype == GT_ITALIAN)
		color = CB_WHITE;
	else if (gametype == GT_SPANISH)
		color = CB_WHITE;
	else if (gametype == GT_RUSSIAN)
		color = CB_WHITE;
	else if (gametype == GT_CZECH)
		color = CB_WHITE;

	return(color);
}

int is_mirror_gametype(int gametype)
{
	if (gametype == GT_ITALIAN)
		return(1);
	if (gametype == GT_SPANISH)
		return(1);

	return(0);
}

int handlesetupcc(int *color)
// handle change color request
{
	char str2[256];

	*color = CB_CHANGECOLOR(*color);

	reset_game(cbgame);
	cboptions.mirror = is_mirror_gametype(cbgame.gametype);

	// and the setup codes
	sprintf(cbgame.setup, "1");
	board8toFEN(cbboard8, str2, *color, cbgame.gametype);
	sprintf(cbgame.FEN, str2);
	return 1;
}

int handle_rbuttondown(int x, int y)
{
	if (setup) {
		coorstocoors(&x, &y, cboptions.invert, cboptions.mirror);
		if ((x + y + 1) % 2) {
			switch (cbboard8[x][y]) {
			case CB_WHITE | CB_MAN:
				cbboard8[x][y] = CB_WHITE | CB_KING;
				break;

			case CB_WHITE | CB_KING:
				cbboard8[x][y] = 0;
				break;

			default:
				cbboard8[x][y] = CB_WHITE | CB_MAN;
				break;
			}
		}

		updateboardgraphics(hwnd);
	}

	return 1;
}

int handle_lbuttondown(int x, int y)
{
	int i, legal, legalmovenumber;
	int from, to;
	CBmove localmove;

	// if we are in setup mode, add a black piece.
	if (setup) {
		coorstocoors(&x, &y, cboptions.invert, cboptions.mirror);
		if ((x + y + 1) % 2) {
			switch (cbboard8[x][y]) {
			case CB_BLACK | CB_MAN:
				cbboard8[x][y] = CB_BLACK | CB_KING;
				break;

			case CB_BLACK | CB_KING:
				cbboard8[x][y] = 0;
				break;

			default:
				cbboard8[x][y] = CB_BLACK | CB_MAN;
				break;
			}
		}

		updateboardgraphics(hwnd);
		return 1;
	}

	// if the engine is calculating we don't accept any input - except if
	//	we are in "enter game" mode
	if ((getenginebusy() || getanimationbusy()) && (CBstate != OBSERVEGAME))
		return 0;

	if (x1 == -1) {

		//then its the first click
		x1 = x;
		y1_ = y;
		coorstocoors(&x1, &y1_, cboptions.invert, cboptions.mirror);

		// if there is only one move with this piece, then do it!
		if (islegal != NULL) {
			legal = 0;
			legalmovenumber = 0;
			for (i = 1; i <= 32; i++) {
				from = coorstonumber(x1, y1_, cbgame.gametype);
				if (islegal(cbboard8, cbcolor, from, i, &localmove) != 0) {
					legal++;
					legalmovenumber = i;
					to = i;
				}
			}

			// look for a single move possible to an empty square
			if (legal == 0) {
				for (i = 1; i <= 32; i++) {
					if (islegal(cbboard8, cbcolor, i, coorstonumber(x1, y1_, cbgame.gametype), &localmove) != 0) {
						legal++;
						legalmovenumber = i;
						from = i;
						to = coorstonumber(x1, y1_, cbgame.gametype);
					}
				}

				if (legal != 1)
					legal = 0;
			}

			// remove the output that islegal generated, it's disturbing ("1-32 illegal move")
			sprintf(statusbar_txt, "");
			if (legal == 1) {

				// is it the only legal move?
				// if yes, do it!
				// if we are in user book mode, add it to user book!
				//if(islegal((int *)board8,color,coorstonumber(x1,y1,cbgame.gametype),legalmovenumber,&localmove)!=0)
				if (islegal(cbboard8, cbcolor, from, to, &localmove) != 0) {

				// a legal move!
					// insert move in the linked list
					appendmovetolist(localmove);

					// animate the move:
					cbmove = localmove;

					// if we are in userbook mode, we save the move
					if (CBstate == BOOKADD)
						addmovetouserbook(cbboard8, &localmove);

					// call animation function which will also execute the move
					CloseHandle(hAniThread);
					setanimationbusy(TRUE);
					hAniThread = CreateThread(NULL,
											  0,
											  (LPTHREAD_START_ROUTINE) AnimationThreadFunc,
											  hwnd,
											  0,
											  &g_AniThreadId);
					x1 = -1;

					// if we are in enter game mode: tell engine to stop
					if (CBstate == OBSERVEGAME)
						SendMessage(hwnd, WM_COMMAND, INTERRUPTENGINE, 0);
					newposition = TRUE;
					if (CBstate == NORMAL)
						startengine = TRUE;

					// startengine = TRUE tells the autothread to start the engine
					// if we are in add moves to book mode, add this move to the book
				}
			}
		}

		// if the stone is the color of the side to move, allow it to be selected
		if
		(
			(cbcolor == CB_BLACK && cbboard8[x1][y1_] & CB_BLACK) ||
			(cbcolor == CB_WHITE && cbboard8[x1][y1_] & CB_WHITE)
		) {

			// re-print board to overwrite last selection if there was one
			updateboardgraphics(hwnd);

			// and then select stone
			selectstone(x1, y1_, hwnd, cbboard8);
		}

		// else, reset the click count to 0.
		else
			x1 = -1;
	}
	else {

	//then its the second click
		x2 = x;
		y2 = y;
		coorstocoors(&x2, &y2, cboptions.invert, cboptions.mirror);
		if (!((x2 + y2 + 1) % 2))
			return 0;

		// now, perhaps the user selected another stone; i.e. the second
		// click is ALSO on a stone of the user. then we assume he has changed
		// his mind and now wants to move this stone.
		// if the stone is the color of the side to move, allow it to be selected
		// !! there is one exception to this: a round-trip move such as
		// here [FEN "W:WK14:B19,18,11,10."]
		// however, with the new one-click-move input, this will work fine now!
		if
		(
			(cbcolor == CB_BLACK && cbboard8[x2][y2] & CB_BLACK) ||
			(cbcolor == CB_WHITE && cbboard8[x2][y2] & CB_WHITE)
		) {

			// re-print board to overwrite last selection if there was one
			updateboardgraphics(hwnd);

			// and then select stone
			selectstone(x2, y2, hwnd, cbboard8);

			// set second click to first click
			x1 = x2;
			y1_ = y2;

			// check whether this is an only move
			legal = 0;
			legalmovenumber = 0;
			if (islegal != NULL) {
				legalmovenumber = 0;
				for (i = 1; i <= 32; i++) {
					if (islegal(cbboard8, cbcolor, coorstonumber(x1, y1_, cbgame.gametype), i, &localmove) != 0) {
						legal++;
						legalmovenumber = i;
					}
				}
			}

			sprintf(statusbar_txt, "");
			if (legal == 1) {

				// only one legal move
				if
				(
					islegal(cbboard8,
							cbcolor,
							coorstonumber(x1, y1_, cbgame.gametype),
							legalmovenumber,
							&localmove) != 0
				) {

				// a legal move!
					// insert move in the linked list
					appendmovetolist(localmove);

					// animate the move:
					cbmove = localmove;
					CloseHandle(hAniThread);

					// if we are in userbook mode, we save the move
					if (CBstate == BOOKADD)
						addmovetouserbook(cbboard8, &localmove);

					// call animation function which will also execute the move
					setanimationbusy(TRUE);
					hAniThread = CreateThread(NULL,
											  0,
											  (LPTHREAD_START_ROUTINE) AnimationThreadFunc,
											  (HWND) hwnd,
											  0,
											  &g_AniThreadId);

					// if we are in enter game mode: tell engine to stop
					if (CBstate == OBSERVEGAME)
						SendMessage(hwnd, WM_COMMAND, INTERRUPTENGINE, 0);
					newposition = TRUE;
					if (CBstate == NORMAL)
						startengine = TRUE;

					// startengine = TRUE tells the autothread to start the engine
					// if we are in add moves to book mode, add this move to the book
				}
			}
			else
				// and break so as not to execute the rest of this clause, because
				// that is for actually making a move.
				return 0;
		}

		// check move and if ok
		if (islegal != NULL) {
			if
			(
				islegal(cbboard8,
						cbcolor,
						coorstonumber(x1, y1_, cbgame.gametype),
						coorstonumber(x2, y2, cbgame.gametype),
						&localmove) != 0
			) {

			// a legal move!
				// insert move in the linked list
				appendmovetolist(localmove);

				// animate the move:
				cbmove = localmove;
				CloseHandle(hAniThread);

				// if we are in userbook mode, we save the move
				if (CBstate == BOOKADD)
					addmovetouserbook(cbboard8, &localmove);

				// call animation function which will also execute the move
				setanimationbusy(TRUE);
				hAniThread = CreateThread(NULL,
										  0,
										  (LPTHREAD_START_ROUTINE) AnimationThreadFunc,
										  (HWND) hwnd,
										  0,
										  &g_AniThreadId);

				// if we are in enter game mode: tell engine to stop
				if (CBstate == OBSERVEGAME)
					SendMessage(hwnd, WM_COMMAND, INTERRUPTENGINE, 0);
				newposition = TRUE;
				if (CBstate == NORMAL)
					startengine = TRUE;

				// startengine = TRUE tells the autothread to start the engine
				// if we are in add moves to book mode, add this move to the book
			}
		}

		x1 = -1;
	}

	//updateboardgraphics(hwnd);
	return 1;
}

int handletimer(void)
{
	// timer goes off all 1/10th of a second. this function polls some things and updates
	// them if necessary:
	// icons in the toolbar (color, two-player, engine, book mode).
	// generates pseudo-logfile for engines that don't do this themselves.
	static char oldstr[1024];
	char filename[MAX_PATH];
	static int oldcolor;
	static int oldtogglemode;
	static int oldtogglebook;
	static int oldtoggleengine;
	static int engineIcon;
	FILE *Lfp;
	int ch = '=';

	if (strcmp(oldstr, statusbar_txt) != 0) {
		SendMessage(hStatusWnd, SB_SETTEXT, (WPARAM) 0, (LPARAM) statusbar_txt);
		sprintf(oldstr, "%s", statusbar_txt);

		// if we're running a test set, create a pseudolog-file
		if (CBstate == RUNTESTSET) {
			if (strchr(statusbar_txt, ch) != NULL) {
				strcpy(filename, CBdocuments);
				PathAppend(filename, "testlog.txt");
				Lfp = fopen(filename, "a");
				if (Lfp != NULL) {
					fprintf(Lfp, "%s\n", statusbar_txt);
					fclose(Lfp);
				}
			}
		}
	}

	// update toolbar to display whose turn it is
	if (oldcolor != cbcolor) {
		if (cbcolor == CB_BLACK)
			SendMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) SETUPCC, MAKELPARAM(10, 0));
		else
			SendMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) SETUPCC, MAKELPARAM(9, 0));
		oldcolor = cbcolor;
		InvalidateRect(hwnd, NULL, 0);
	}

	// update toolbar to display what mode (normal/2player) we're in
	if (oldtogglemode != togglemode) {
		if (togglemode == 0)
			SendMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) TOGGLEMODE, MAKELPARAM(17, 0));
		else
			SendMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) TOGGLEMODE, MAKELPARAM(18, 0));
		oldtogglemode = togglemode;
		InvalidateRect(hwnd, NULL, 0);
	}

	// update toolbar to display book mode (on/off) we're in
	if (oldtogglebook != togglebook) {
		switch (togglebook) {
		case 0:
			SendMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) TOGGLEBOOK, MAKELPARAM(3, 0));
			break;

		case 1:
			SendMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) TOGGLEBOOK, MAKELPARAM(6, 0));
			break;

		case 2:
			SendMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) TOGGLEBOOK, MAKELPARAM(5, 0));
			break;

		case 3:
			SendMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) TOGGLEBOOK, MAKELPARAM(4, 0));
			break;
		}

		oldtogglebook = togglebook;
		InvalidateRect(hwnd, NULL, 0);
	}

	// update toolbar to display active engine (primary/secondary)
	if (oldtoggleengine != toggleengine) {
		if (toggleengine == 1)
			SendMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) TOGGLEENGINE, MAKELPARAM(0, 0));
		else
			SendMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) TOGGLEENGINE, MAKELPARAM(1, 0));
		oldtoggleengine = toggleengine;
		InvalidateRect(hwnd, NULL, 0);
	}

	updateboardgraphics(hwnd);
	return 1;
}

int addmovetouserbook(int b[8][8], struct CBmove *move)
{
	size_t i, n;
	FILE *fp;
	struct pos userbookpos;

	// if we have too many moves, stop!
	// userbooknum is a global, as it is also used in removing stuff from book
	if (userbooknum >= MAXUSERBOOK) {
		sprintf(statusbar_txt, "user book size limit reached!");
		return 0;
	}

	boardtobitboard(b, &userbookpos);

	// check if we already have this position:
	n = userbooknum;
	for (i = 0; i < userbooknum; i++) {
		if
		(
			userbook[i].position.bm == userbookpos.bm &&
			userbook[i].position.bk == userbookpos.bk &&
			userbook[i].position.wm == userbookpos.wm &&
			userbook[i].position.wk == userbookpos.wk
		) {

			// we already have this position!
			// in this case, we overwrite the old entry!
			n = i;
			break;
		}
	}

	userbook[n].position = userbookpos;
	userbook[n].move = *move;
	if (n == userbooknum) {
		(userbooknum)++;
		sprintf(statusbar_txt, "added move to userbook (%zi moves)", userbooknum);
	}
	else
		sprintf(statusbar_txt, "replaced move in userbook (%zi moves)", userbooknum);

	// save user book
	fp = fopen(userbookname, "wb");
	if (fp != NULL) {
		fwrite(userbook, sizeof(struct userbookentry), userbooknum, fp);
		fclose(fp);
	}
	else
		sprintf(statusbar_txt, "unable to write to user book");

	return 1;
}

int handlegamereplace(int replaceindex, char *databasename)
{
	FILE *fp;
	char *gamestring, *dbstring, *p;
	size_t bytesread;
	int i;
	size_t filesize = getfilesize(databasename);

	// give the user a chance to save new results / names
	if (DialogBox(g_hInst, "IDD_SAVEGAME", hwnd, (DLGPROC) DialogFuncSavegame)) {

		// if the user gives his ok, replace:
		// set reindex flag
		reindex = 1;

		// read database into memory */
		dbstring = (char *)malloc(filesize);
		if (dbstring == NULL) {
			sprintf(statusbar_txt, "malloc error");
			return 0;
		}

		fp = fopen(databasename, "r");

		if (fp == NULL) {
			sprintf(statusbar_txt, "invalid filename");
			free(dbstring);
			return 0;
		}

		bytesread = fread(dbstring, 1, filesize, fp);
		dbstring[bytesread] = 0;
		fclose(fp);

		// allocate gamestring for pdnstring
		gamestring = (char *)malloc(GAMEBUFSIZE);
		if (gamestring == NULL) {
			sprintf(statusbar_txt, "malloc error");
			free(dbstring);
			return 0;
		}

		// rewrite file
		fp = fopen(databasename, "w");

		// get all games up to gameindex and write them into file
		p = dbstring;
		for (i = 0; i < replaceindex; i++) {
			PDNparseGetnextgame(&p, gamestring);
			fprintf(fp, "%s", gamestring);
		}

		// skip current game
		PDNparseGetnextgame(&p, gamestring);

		// write replaced game
		PDNgametoPDNstring(cbgame, gamestring, "\n");
		if (gameindex != 0)
			fprintf(fp, "\n\n\n\n%s", gamestring);
		else
			fprintf(fp, "%s", gamestring);

		// and read the rest of the file
		while (PDNparseGetnextgame(&p, gamestring))
			fprintf(fp, "%s", gamestring);

		fclose(fp);
		if (gamestring != NULL)
			free(gamestring);
		if (dbstring != NULL)
			free(dbstring);
		return 1;
	}

	return 0;
}

int loadnextgame(void)
{
	// load the next game of the last search.
	char *dbstring;
	int i;

	if (game_previews.size() == 0) {
		sprintf(statusbar_txt, "no game list to move through");
		return(0);
	}

	for (i = 0; i < (int)game_previews.size(); i++)
		if (gameindex == game_previews[i].game_index)
			break;

	if (i >= (int)game_previews.size())
		return(0);

	if (gameindex != game_previews[i].game_index) {
		sprintf(statusbar_txt, "error while looking for next game...");
		return 0;
	}

	if (i >= (int)game_previews.size() - 1) {
		sprintf(statusbar_txt, "at last game in list");
		return 0;
	}

	gameindex = game_previews[i + 1].game_index;

	// ok, if we arrive here, we have a valid game index for the game to load.
	sprintf(statusbar_txt, "should load game %i", gameindex);

	// load the database into memory
	dbstring = loadPDNdbstring(database);

	// extract game from database
	loadgamefromPDNstring(gameindex, dbstring);

	// free up database memory
	free(dbstring);
	sprintf(statusbar_txt, "loaded game %i of %i", i + 2, (int)game_previews.size());

	// return the number of the game we loaded
	return(i + 2);
}

int loadpreviousgame(void)
{
	// load the previous game of the last search.
	char *dbstring;
	int i;

	if (game_previews.size() == 0) {
		sprintf(statusbar_txt, "no game list to move through");
		return(0);
	}

	for (i = 0; i < (int)game_previews.size(); i++)
		if (gameindex == game_previews[i].game_index)
			break;

	if (i >= (int)game_previews.size())
		return(0);

	if (gameindex != game_previews[i].game_index) {
		sprintf(statusbar_txt, "error while looking for next game...");
		return 0;
	}

	if (i == 0) {
		sprintf(statusbar_txt, "at first game in list");
		return 0;
	}

	gameindex = game_previews[i - 1].game_index;

	sprintf(statusbar_txt, "should load game %i", gameindex);
	dbstring = loadPDNdbstring(database);
	loadgamefromPDNstring(gameindex, dbstring);
	free(dbstring);
	sprintf(statusbar_txt, "loaded game %i of %i", i, (int)game_previews.size());

	return 0;
}

char *loadPDNdbstring(char *dbname)
{
	// attempts to load the file <dbname> into the
	// string dbstring - checks for existence of that
	// file, allocates enough memory for the file, and loads it.
	FILE *fp;
	size_t filesize;
	size_t bytesread;
	char *dbstring;

	filesize = getfilesize(dbname);

	dbstring = (char *)malloc(filesize);
	if (dbstring == NULL) {
		MessageBox(hwnd, "not enough memory for this operation", "Error", MB_OK);
		SetCurrentDirectory(CBdirectory);
		return 0;
	}

	fp = fopen(dbname, "r");
	if (fp == NULL) {
		MessageBox(hwnd,
				   "not a valid database!\nuse game->select database\nto select a valid database",
				   "Error",
				   MB_OK);
		SetCurrentDirectory(CBdirectory);
		return 0;
	}

	bytesread = fread(dbstring, 1, filesize, fp);
	dbstring[bytesread] = 0;
	fclose(fp);

	return dbstring;
}

/*
 * Get headers and moves for this game
 */
void assign_headers(gamepreview &preview, char *pdn)
{
	char *tag;
	char header[256];
	char headername[256], headervalue[256];
	char token[1024];

	preview.white[0] = 0;
	preview.black[0] = 0;
	preview.event[0] = 0;
	preview.result[0] = 0;
	preview.date[0] = 0;

	// parse headers
	while (PDNparseGetnextheader(&pdn, header)) {
		tag = header;
		PDNparseGetnexttoken(&tag, headername);
		PDNparseGetnexttag(&tag, headervalue);
		if (strcmp(headername, "Event") == 0)
			sprintf(preview.event, "%s", headervalue);
		else if (strcmp(headername, "White") == 0)
			sprintf(preview.white, "%s", headervalue);
		else if (strcmp(headername, "Black") == 0)
			sprintf(preview.black, "%s", headervalue);
		else if (strcmp(headername, "Result") == 0)
			sprintf(preview.result, "%s", headervalue);
		else if (strcmp(headername, "Date") == 0)
			sprintf(preview.date, "%s", headervalue);
	}

	// headers parsed
	// add the first few moves to the preview structure to display them
	// when the user selects a game.
	sprintf(preview.PDN, "");
	for (int i = 0; i < 48; ++i) {
		if (!PDNparseGetnextPDNtoken(&pdn, token))
			break;
		if (strlen(preview.PDN) + strlen(token) < sizeof(preview.PDN) - 1) {
			strcat(preview.PDN, token);
			strcat(preview.PDN, " ");
		}
		else
			break;
	}
}

void get_pdnsearch_stats(std::vector<gamepreview> &previews, RESULT &res)
{
	memset(&res, 0, sizeof(res));
	for (int i = 0; i < (int)previews.size(); ++i) {
		if (strcmp(game_previews[i].result, "1-0") == 0)
			res.win++;
		else if (strcmp(game_previews[i].result, "1/2-1/2") == 0)
			res.draw++;
		else if (strcmp(game_previews[i].result, "0-1") == 0)
			res.loss++;
	}
}

int selectgame(int how)
// lets the user select a game from a PDN database in a dialog box.
// how describes which games are displayed:
//		GAMELOAD:		all games
//		SEARCHMASK:		only games of a specific player/event/date/with comment
//						new: also with optional "searchwithposition" enabled!
//		GAMEFIND:		only games with the current board position
//		GAMEFINDCR:		only games with current board position color-reversed
//		FINDTHEME:		only games with the current board position as "theme"
//		LASTSEARCH:		re-display results of the last search
{
	int i;
	static int oldgameindex;
	int entry;
	char *dbstring = NULL;
	char *gamestring = NULL;
	char *p;
	int searchhit;
	gamepreview preview;
	std::vector<int> pos_match_games;	/* Index of games matching the position part of search criteria */

	sprintf(statusbar_txt, "wait ...");
	SendMessage(hStatusWnd, SB_SETTEXT, (WPARAM) 0, (LPARAM) statusbar_txt);

	// stop engine
	PostMessage(hwnd, WM_COMMAND, ABORTENGINE, 0);

	if (how == RE_SEARCH) {

		// the easiest: re-display the result of the last search.
		// only possible if there is a last search!
		// re-uses game_previews array.
		if (re_search_ok == 0) {
			sprintf(statusbar_txt, "no old search to re-search!");
			return 0;
		}

		sprintf(statusbar_txt, "re-searching! game_previews.size() is %zd", game_previews.size());

		// load database into dbstring:
		dbstring = loadPDNdbstring(database);

		gamestring = (char *)malloc(GAMEBUFSIZE);
		if (gamestring == NULL) {
			MessageBox(hwnd, "not enough memory for this operation", "Error", MB_OK);
			SetCurrentDirectory(CBdirectory);
			return 0;
		}
	}
	else {

		// if we're looking for a player name, get it
		if (how == SEARCHMASK) {

			// this dialog box sets the variables
			// <playername>, <eventname> and <datename>
			if (DialogBox(g_hInst, "IDD_SEARCHMASK", hwnd, (DLGPROC) DialogSearchMask) == 0)
				return 0;
		}

		// set directory to games directory
		SetCurrentDirectory(cboptions.userdirectory);

		// get a valid database filename. if we already have one, we reuse it,
		// else we prompt the user to select a PDN database
		if (strcmp(database, "") == 0) {

			// no valid database name
			// display a dialog box with the available databases
			sprintf(database, "%s", cboptions.userdirectory);
			result = getfilename(database, OF_LOADGAME);	// 1 on ok, 0 on cancel
			if (!result)
				sprintf(database, "");
		}
		else {
			sprintf(statusbar_txt, "database is '%s'", database);
			result = 1;
		}

		if (strcmp(database, "") != 0 && result) {
			sprintf(statusbar_txt, "loading...");

			// get number of games
			i = PDNparseGetnumberofgames(database);
			sprintf(statusbar_txt, "%i games in database", i);

			if
			(
				how == GAMEFIND ||
				how == GAMEFINDTHEME ||
				how == GAMEFINDCR ||
				(how == SEARCHMASK && searchwithposition == 1)
			) {

				// search for a position: this is done by calling pdnopen to index
				// the pdn file, pdnfind to return a list of games with the current position
				if (reindex) {
					sprintf(statusbar_txt, "indexing database...");
					SendMessage(hStatusWnd, SB_SETTEXT, (WPARAM) 0, (LPARAM) statusbar_txt);

					// index database with pdnopen; fills pdn_positions[].
					pdnopen(database, cbgame.gametype);
					reindex = 0;
				}

				// search for games with current position
				// transform the current position into a bitboard:
				boardtobitboard(cbboard8, &currentposition);
				if (how == GAMEFINDCR)
					boardtocrbitboard(cbboard8, &currentposition);

				sprintf(statusbar_txt, "searching database...");
				SendMessage(hStatusWnd, SB_SETTEXT, (WPARAM) 0, (LPARAM) statusbar_txt);
				if (how == GAMEFIND)
					pdnfind(&currentposition, cbcolor, pos_match_games);
				if (how == SEARCHMASK)
					pdnfind(&currentposition, cbcolor, pos_match_games);
				if (how == GAMEFINDCR)
					pdnfind(&currentposition, CB_CHANGECOLOR(cbcolor), pos_match_games);
				if (how == GAMEFINDTHEME)
					pdnfindtheme(&currentposition, pos_match_games);

				// pos_match_games now contains a list of games matching the position part of search criteria
				if (pos_match_games.size() == 0) {
					sprintf(statusbar_txt, "no games matching position criteria found");
					re_search_ok = 0;
					game_previews.clear();
					SendMessage(hStatusWnd, SB_SETTEXT, (WPARAM) 0, (LPARAM) statusbar_txt);
					return 0;
				}
				else {
					sprintf(statusbar_txt, "%zd games matching position criteria found", pos_match_games.size());
					re_search_ok = 1;
				}
			}

			SendMessage(hStatusWnd, SB_SETTEXT, (WPARAM) 0, (LPARAM) statusbar_txt);

			// read database file into buffer 'dbstring'
			dbstring = loadPDNdbstring(database);

			// fill the struct 'data' with the PDN headers
			gamestring = (char *)malloc(GAMEBUFSIZE);
			if (gamestring == NULL) {
				MessageBox(hwnd, "not enough memory for this operation", "Error", MB_OK);
				SetCurrentDirectory(CBdirectory);
				return 0;
			}

			p = dbstring;
			entry = 0;
			game_previews.clear();
			for (i = 0; PDNparseGetnextgame(&p, gamestring); ++i) {
				if (how == GAMEFIND || how == GAMEFINDTHEME || how == GAMEFINDCR) {

					// we already know what should go in the list, no point parsing
					if (entry >= (int)pos_match_games.size())
						break;	/* done*/

					if (i != pos_match_games[entry])
						continue;
				}

				/* Parse the game text and fill in the headers of the preview struct. */
				assign_headers(preview, gamestring);
				preview.game_index = i;

				// now, depending on what we are doing, we add this game to the list of
				// games to display
				// remember: entry is our running variable, from 0...numberofgames in db
				// i is index of game in the pdn database file.
				try
				{
					switch (how) {
					case GAMEFIND:
					case GAMEFINDCR:
					case GAMEFINDTHEME:
						game_previews.push_back(preview);
						entry++;
						break;

					case GAMELOAD:
						//	remember what game number this has
						game_previews.push_back(preview);
						entry++;
						break;

					case SEARCHMASK:
						// add the entry to the list
						// only if the name matches one of the players
						searchhit = 1;
						if (searchwithposition) {
							if (std::find(pos_match_games.begin(), pos_match_games.end(), i) == pos_match_games.end())
								searchhit = 0;
							else
								searchhit = 1;
						}

						// if a player name to search is set, search for that name
						if (strcmp(playername, "") != 0) {
							if (strstr(preview.black, playername) || strstr(preview.white, playername))
								searchhit &= 1;
							else
								searchhit = 0;
						}

						// if an event name to search is set, search for that event
						if (strcmp(eventname, "") != 0) {
							if (strstr(preview.event, eventname))
								searchhit &= 1;
							else
								searchhit = 0;
						}

						// if a date to search is set, search for that date
						if (strcmp(datename, "") != 0) {
							if (strstr(preview.date, datename))
								searchhit &= 1;
							else
								searchhit = 0;
						}

						// if a comment is defined, search for that comment
						if (strcmp(commentname, "") != 0) {
							if (strstr(gamestring, commentname))
								searchhit &= 1;
							else
								searchhit = 0;
						}

						if (searchhit == 1) {
							game_previews.push_back(preview);
							entry++;
						}
						break;
					}
				}

				/* Catch memory allocation errors from push_back(). */
				catch(...) {
					MessageBox(hwnd, "not enough memory for this operation", "Error", MB_OK);
					SetCurrentDirectory(CBdirectory);
					return(0);
				}
			}

			assert(entry == game_previews.size());
			sprintf(statusbar_txt, "%i games found matching search criteria", entry);

			// save old game index
			oldgameindex = gameindex;

			// default game index
			gameindex = 0;
		}
	}

	// headers loaded into 'game_previews', display load game dialog
	if (game_previews.size()) {
		if (DialogBox(g_hInst, "IDD_SELECTGAME", hwnd, (DLGPROC) DialogFuncSelectgame)) {
			if (selected_game < 0 || selected_game >= (int)game_previews.size())
				// dialog box didn't select a proper preview index
				gameindex = oldgameindex;
			else {

				// a game was selected; with index <selected_game> in the dialog box
				sprintf(statusbar_txt, "game previews index is %i", selected_game);

				// transform dialog box index to game index in database
				gameindex = game_previews[selected_game].game_index;

				// load game with index 'gameindex'
				loadgamefromPDNstring(gameindex, dbstring);
			}
		}
		else {

			// dialog box was cancelled
			gameindex = oldgameindex;
		}
	}

	// free up memory
	if (gamestring != NULL)
		free(gamestring);
	if (dbstring != NULL)
		free(dbstring);

	SendMessage(hStatusWnd, SB_SETTEXT, (WPARAM) 0, (LPARAM) statusbar_txt);
	return 1;
}

int loadgamefromPDNstring(int gameindex, char *dbstring)
{
	char *p;
	int i;
	char *gamestring = (char *)malloc(GAMEBUFSIZE);

	p = dbstring;
	i = gameindex + 1;
	while (i) {
		if (!PDNparseGetnextgame(&p, gamestring))
			break;
		i--;
	}

	// now the game is in gamestring. use pdnparser routines to convert
	//	it into a cbgame
	doload(&cbgame, gamestring, &cbcolor, cbboard8);
	free(gamestring);

	// game is fully loaded, clean up
	updateboardgraphics(hwnd);
	reset_move_history = true;
	newposition = TRUE;
	sprintf(statusbar_txt, "game loaded");
	SetCurrentDirectory(CBdirectory);

	//  only display info if not in analyzepdnmode
	if (CBstate != ANALYZEPDN)
		PostMessage(hwnd, WM_COMMAND, GAMEINFO, 0);
	return 1;
}

int handletooltiprequest(LPTOOLTIPTEXT TTtext)
{
	if (TTtext->hdr.code != TTN_NEEDTEXT)
		return 0;

	switch (TTtext->hdr.idFrom) {
	// set tooltips
	case TOGGLEBOOK:
		TTtext->lpszText = "Change engine book setting";
		break;

	case TOGGLEMODE:
		TTtext->lpszText = "Switch from normal to 2-player mode and back";
		break;

	case TOGGLEENGINE:
		TTtext->lpszText = "Switch from primary to secondary engine and back";
		break;

	case GAMEFIND:
		TTtext->lpszText = "Find Game";
		break;

	case GAMENEW:
		TTtext->lpszText = "New Game";
		break;

	case MOVESBACK:
		TTtext->lpszText = "Take Back";
		break;

	case MOVESFORWARD:
		TTtext->lpszText = "Forward";
		break;

	case MOVESPLAY:
		TTtext->lpszText = "Play";
		break;

	case HELPHELP:
		TTtext->lpszText = "Help";
		break;

	case GAMELOAD:
		TTtext->lpszText = "Load Game";
		break;

	case GAMESAVE:
		TTtext->lpszText = "Save Game";
		break;

	case MOVESBACKALL:
		TTtext->lpszText = "Go to the start of the game";
		break;

	case MOVESFORWARDALL:
		TTtext->lpszText = "Go to the end of the game";
		break;

	case DISPLAYINVERT:
		TTtext->lpszText = "Invert Board";
		break;

	case SETUPCC:
		if (cbcolor == CB_BLACK)
			TTtext->lpszText = "Red to move";
		else
			TTtext->lpszText = "White to move";
		break;

	case BOOKMODE_VIEW:
		TTtext->lpszText = "View User Book";
		break;

	case BOOKMODE_ADD:
		TTtext->lpszText = "Add Moves to User Book";
		break;

	case BOOKMODE_DELETE:
		TTtext->lpszText = "Delete Position from User Book";
		break;

	case HELPHOMEPAGE:
		TTtext->lpszText = "CheckerBoard Homepage";
		break;
	}

	return 1;
}

void add_piecesets_to_menu(HMENU hmenu)
{
	// this is a bit ugly; hard-coded that piece sets appear at
	// a certain position in the menu.
	HMENU hpop, hsubpop;
	HANDLE hfind;
	WIN32_FIND_DATA FileData;
	int i = 0;

	hpop = GetSubMenu(hmenu, 4);
	hsubpop = GetSubMenu(hpop, 3);

	// first, we need to find the subdirectories in the bmp dir
	SetCurrentDirectory(CBdirectory);
	SetCurrentDirectory("bmp");

	FileData.dwFileAttributes = 0;

	hfind = FindFirstFile("*", &FileData);

	DeleteMenu(hsubpop, 0, MF_BYPOSITION);
	while (i < MAXPIECESET) {
		if (hfind == INVALID_HANDLE_VALUE)
			return;

		if (FileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (strlen(FileData.cFileName) > 3 && FileData.cFileName[0] != '.') {

				/* Exclude ".svn" directory. */
				AppendMenu(hsubpop, MF_STRING, PIECESET + i, FileData.cFileName);
				sprintf(piecesetname[i], "%s", FileData.cFileName);
				i++;
				maxpieceset = 1;
			}
		}

		if (FindNextFile(hfind, &FileData) == 0)
			return;
	}
}

int createcheckerboard(HWND hwnd)
{
	FILE *fp;
	COLORREF dCustomColors[16];
	char windowtitle[256], str2[256];
	RECT rect;

	/* To support running multiple instances of CB, use suffixes in logfilenames. */
	get_app_instance(szWinName, &g_app_instance);
	if (g_app_instance > 0)
		sprintf(g_app_instance_suffix, "[%d]", g_app_instance);

	// load settings from registry: &cboptions is one key, CBdirectory another.
	loadsettings(&cboptions, CBdirectory);
	SetMenuLanguage(cboptions.language);

	// set appropriate pieceset - load bitmaps for the board
	SendMessage(hwnd, WM_COMMAND, PIECESET + cboptions.piecesetindex, 0);

	// resize window to last window size:
	if (cboptions.window_x != 0) {

		// check if we have stored something
		MoveWindow(hwnd, cboptions.window_x, cboptions.window_y, cboptions.window_width, cboptions.window_height, 1);
	}

	GetClientRect(hwnd, &rect);
	PostMessage(hwnd, WM_SIZE, 0, MAKELPARAM(rect.right - rect.left, rect.bottom - rect.top));

	initgraphics(hwnd);

	//initialize global that stores the game
	reset_game(cbgame);

	// initialize colors
	ccs.lStructSize = (DWORD) sizeof(CHOOSECOLOR);
	ccs.hwndOwner = (HWND) hwnd;
	ccs.hInstance = (HWND) NULL;
	ccs.rgbResult = RGB(255, 0, 0);
	ccs.lpCustColors = dCustomColors;
	ccs.Flags = CC_RGBINIT | CC_FULLOPEN;
	ccs.lCustData = 0L;
	ccs.lpfnHook = NULL;
	ccs.lpTemplateName = (LPSTR) NULL;

	// load user book
	strcpy(userbookname, CBdocuments);
	PathAppend(userbookname, "userbook.bin");
	fp = fopen(userbookname, "rb");
	if (fp != 0) {
		userbooknum = fread(userbook, sizeof(struct userbookentry), MAXUSERBOOK, fp);
		fclose(fp);
	}

	setmenuchecks(&cboptions, hmenu);
	checklevelmenu(&cboptions, hmenu, timelevel_to_token(cboptions.level));

	// in case of shell double click
	if (strcmp(filename, "") != 0) {
		sprintf(database, "%s", filename);
		PostMessage(hwnd, WM_COMMAND, GAMELOAD, 0);
	}

	// support drag and drop
	DragAcceptFiles(hwnd, 1);

	// start autothread
	hAutoThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) AutoThreadFunc, (HWND) 0, 0, &AutoThreadId);
	if (hAutoThread == 0)
		CBlog("failed to start auto thread");

	// and issue a newgame command
	PostMessage(hwnd, WM_COMMAND, GAMENEW, 0);

	// display the engine name in the window title
	sprintf(windowtitle, "loading engine - please wait...");
	sprintf(str2, "CheckerBoard%s: %s", windowtitle, g_app_instance_suffix);
	SetWindowText(hwnd, str2);

	// initialize the board which is stored in board8
	InitCheckerBoard(cbboard8);

	// print version number in status bar
	sprintf(statusbar_txt, "This is CheckerBoard %s, %s", VERSION, PLACE);
	return 1;
}

int showfile(char *filename)
{
	// opens a file with the default viewer, e.g. a html help file
	int error;
	HINSTANCE hinst;

	hinst = ShellExecute(NULL, "open", filename, NULL, NULL, SW_SHOW);
	error = PtrToLong(hinst);

	sprintf(statusbar_txt, "CheckerBoard could not open\nthe file %s\nError code %i", filename, error);
	if (error < 32) {
		if (error == ERROR_FILE_NOT_FOUND)
			strcat(statusbar_txt, ": File not found");
		if (error == SE_ERR_NOASSOC)
			strcat(statusbar_txt, ": no .htm viewer configured");
		MessageBox(hwnd, statusbar_txt, "Error !", MB_OK);
	}
	else
		sprintf(statusbar_txt, "opened %s", filename);
	return 1;
}

int start11man(int number)
{
	// start a new 11-move game:
	// read FEN for this 11 man from file
	// returns 1 if a game with gamenumber could be started, 0 if there is no FEN left in the file.
	int i = 0;
	FILE *fp;
	char str[256];

	// set directory to CB directory
	SetCurrentDirectory(CBdirectory);

	// don't play entire match as it takes way too much time!
	//if(number >= 500)
	//	return 0;
	fp = fopen("11man_FEN.txt", "r");
	if (fp == NULL)
		return 0;

	while (!feof(fp) && i <= number) {

		// read a line
		fgets(str, 255, fp);
		i++;
	}

	if (feof(fp)) {
		fclose(fp);
		return 0;
	}

	fclose(fp);

	// now we have the right FEN in str
	FENtoboard8(cbboard8, str, &cbcolor, GT_ENGLISH);

	cbgame.moves.clear();
	board8toFEN(cbboard8, str, cbcolor, GT_ENGLISH);
	sprintf(cbgame.FEN, "%s", str);
	sprintf(cbgame.event, "11-man #%i", number + 1);
	sprintf(cbgame.setup, "1");

	updateboardgraphics(hwnd);
	InvalidateRect(hwnd, NULL, 0);
	sprintf(str, "11 man opening number %i", number + 1);
	newposition = TRUE;
	reset_move_history = true;

	return 1;
}

/*
 * Translate the current game into pdn of its colors-reversed mirror.
 */
void game_to_colors_reversed_pdn(char *pdn)
{
	int gindex;
	int from, to;

	pdn[0] = 0;
	for (gindex = 0; gindex < cbgame.movesindex; ++gindex) {
		PDNparseTokentonumbers(cbgame.moves[gindex].PDN, &from, &to);
		sprintf(pdn + strlen(pdn), "%d-%d ", 33 - from, 33 - to);
	}
}

/*
 * Move to the end of the current game.
 */
void forward_to_game_end(void)
{
	while (cbgame.movesindex < (int)cbgame.moves.size()) {
		domove(cbgame.moves[cbgame.movesindex].move, cbboard8);
		cbcolor = CB_CHANGECOLOR(cbcolor);
		++cbgame.movesindex;
	}
}

int start3move(void)
{
	// start a new 3-move game:
	// this function executes the 3 first moves of 3moveopening #(op), op
	// is a global which is set by random if the user chooses
	// 3-move, or it can be set controlled by engine match
	CBmove movelist[28];

	extern int three[174][4];			// describes 3-move-openings
	InitCheckerBoard(cbboard8);
	InvalidateRect(hwnd, NULL, 0);
	cbcolor = CB_BLACK;
	cbgame.moves.clear();

	getmovelist(1, movelist, cbboard8, &dummy);
	domove(movelist[three[op][0]], cbboard8);
	appendmovetolist(movelist[three[op][0]]);

	cbcolor = CB_CHANGECOLOR(cbcolor);
	getmovelist(-1, movelist, cbboard8, &dummy);
	domove(movelist[three[op][1]], cbboard8);
	appendmovetolist(movelist[three[op][1]]);

	cbcolor = CB_CHANGECOLOR(cbcolor);
	getmovelist(1, movelist, cbboard8, &dummy);
	domove(movelist[three[op][2]], cbboard8);
	appendmovetolist(movelist[three[op][2]]);

	cbcolor = CB_CHANGECOLOR(cbcolor);

	if (gametype() == GT_ITALIAN) {
		char pdn[80];

		game_to_colors_reversed_pdn(pdn);
		doload(&cbgame, pdn, &cbcolor, cbboard8);
		forward_to_game_end();
	}

	updateboardgraphics(hwnd);
	sprintf(statusbar_txt, "ACF opening number %i", op + 1);
	newposition = TRUE;

	// new march 2005, jon kreuzer told me this was missing.
	reset_move_history = true;
	reset_game_clocks();

	return 1;
}

void format_time_args(double increment, double remaining, uint32_t *info, uint32_t *moreinfo)
{
	int i;
	const double limit = 65535 * 0.1;
	uint16_t increment16, remaining16;
	double mult[] = { 0.001, 0.01, 0.1 };

	remaining = max(remaining, 0.001);	/* Dont allow negative remaining time. */
	double largest = max(increment, remaining);
	if (largest > limit) {
		largest = min(largest, limit);
		increment = min(increment, limit);
		remaining = min(remaining, limit);
	}

	for (i = 0; i < ARRAY_SIZE(mult); ++i) {
		if (largest <= 65535 * mult[i]) {

			/* Pack the 2 times into a 32-bit int. */
			increment16 = (uint16_t) (increment / mult[i]);
			remaining16 = (uint16_t) (remaining / mult[i]);
			*moreinfo = ((remaining16 << 16) & 0xffff0000) | (increment16 & 0xffff);

			/* Write the multiplier into *info. */
			*info |= (i + 1) << 2;
			return;
		}
	}

	assert(0);
}

DWORD ThreadFunc(LPVOID param)
// Threadfunc calls the checkers engine to find a move
// it also logs the return string of the checkers engine
// to a file if CB is in either ANALYZEGAME or ENGINEMATCH mode
{
	size_t i, nmoves;
	int original8board[8][8], b8copy[8][8], originalcopy[8][8];
	CBmove m[MAXMOVES];
	CBmove localmove;
	char PDN[40];
	int found = 0;
	int c;
	int dummy;
	FILE *Lfp;
	char Lstr[1024];
	struct pos userbookpos;
	int founduserbookmove = 0;
	double maxtime;

	if (cboptions.use_incremental_time && CBstate != ENGINEMATCH) {

		/* Player must have just made a move.
		 * Subtract his accumulated clock time, add his increment.
		 */
		if (cbcolor == CB_BLACK)
			/* Player was white. */
			white_time_remaining += cboptions.time_increment - (clock() - starttime) / (double)CLK_TCK;
		else
			black_time_remaining += cboptions.time_increment - (clock() - starttime) / (double)CLK_TCK;
	}

	abortcalculation = 0;				// if this remains 0, we will execute the move - else not

	// test if there is a move at all: if not, return and set state to NORMAL
	if (cbcolor == CB_BLACK)
		c = 1;
	else
		c = -1;
	nmoves = getmovelist(c, m, cbboard8, &dummy);
	if (nmoves == 0) {
		sprintf(statusbar_txt, "there is no move in this position");

		// if this happens in autoplay or in an enginematch, set mode back to normal
		if (CBstate == AUTOPLAY) {
			gameover = TRUE;
			sprintf(statusbar_txt, "game over");
		}

		if (CBstate == ENGINEMATCH || CBstate == ENGINEGAME) {
			gameover = TRUE;
			sprintf(statusbar_txt, "game over");
		}

		setenginebusy(FALSE);
		return 1;
	}

	// check if this position is in the userbook
	if (cboptions.userbook) {
		boardtobitboard(cbboard8, &userbookpos);
		for (i = 0; i < userbooknum; i++) {
			if
			(
				userbookpos.bm == userbook[i].position.bm &&
				userbookpos.bk == userbook[i].position.bk &&
				userbookpos.wm == userbook[i].position.wm &&
				userbookpos.wk == userbook[i].position.wk
			) {

				// we have this position in the userbook!
				cbmove = userbook[i].move;
				founduserbookmove = 1;
				found = 1;
				sprintf(statusbar_txt, "found move in user book");
			}
		}
	}

	if (!founduserbookmove) {

		// we did not find a move in our user book, so continue
		//board8 is a global [8][8] int which holds the board
		//get 3 copies of the global board8
		memcpy(b8copy, cbboard8, sizeof(cbboard8));
		memcpy(original8board, cbboard8, sizeof(cbboard8));
		memcpy(originalcopy, cbboard8, sizeof(cbboard8));

		// set thread priority
		// next lower ist '_LOWEST', higher '_NORMAL'
		enginethreadpriority = usersetpriority;
		SetThreadPriority(hThread, enginethreadpriority);

		// set directory to CB directory
		SetCurrentDirectory(CBdirectory);

		//--------------------------------------------------------------//
		//						do a search								//
		//--------------------------------------------------------------//
		if (getmove != NULL) {
			uint32_t info, moreinfo;	/* arguments sent to engine. */

			/* Display the Play! bitmap with red foreground when the engine is searching. */
			PostMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) MOVESPLAY, MAKELPARAM(19, 0));

			info = 0;
			moreinfo = 0;
			if (reset_move_history)
				info |= CB_RESET_MOVES;
			if (cboptions.use_incremental_time) {
				if (cbcolor == CB_BLACK) {
					format_time_args(cboptions.time_increment, black_time_remaining, &info, &moreinfo);
					maxtime = black_time_remaining / 4;
				}
				else {
					format_time_args(cboptions.time_increment, white_time_remaining, &info, &moreinfo);
					maxtime = white_time_remaining / 4;
				}
			}
			else {
				if (cboptions.exact_time)
					info |= CB_EXACT_TIME;

				maxtime = timelevel_to_time(cboptions.level);

				// if in engine match handicap mode, give primary engine half the time of secondary engine.
				if (CBstate == ENGINEMATCH && handicap && currentengine == 1)
					maxtime /= 2;
			}

			starttime = clock();
			result = (getmove) (originalcopy, cbcolor, maxtime, statusbar_txt, &playnow, info, moreinfo, &localmove);

			/* Display the Play! bitmap with black foreground when the engine is not searching. */
			PostMessage(tbwnd, TB_CHANGEBITMAP, (WPARAM) MOVESPLAY, MAKELPARAM(2, 0));

			if (cboptions.use_incremental_time) {
				if (cbcolor == CB_BLACK)
					black_time_remaining += cboptions.time_increment - (clock() - starttime) / (double)CLK_TCK;
				else
					white_time_remaining += cboptions.time_increment - (clock() - starttime) / (double)CLK_TCK;

				/* If not engine match, then human player's clock starts now. For engine matches the
				 * starttime will be set just before calling getmove().
				 */
				starttime = clock();
			}
		}
		else
			sprintf(statusbar_txt, "error: no engine defined!");

		// reset playnow immediately
		playnow = 0;

		// save engine string as comment if it's an engine match
		// actually, always save if add comment is on
		if (addcomment) {
			if (cbgame.movesindex > 0) {
				gamebody_entry *pgame = &cbgame.moves[cbgame.movesindex - 1];
				if (strlen(statusbar_txt) < COMMENTLENGTH)
					sprintf(pgame->comment, "%s", statusbar_txt);
				else
					strncpy(pgame->comment, statusbar_txt, COMMENTLENGTH - 2);
			}
		}

		// now, we execute the move on the board, but only if we are not in observe or analyze mode
		// in observemode, the user will provide all moves, in analyse mode the autothread drives the
		// game forward
		if (CBstate != OBSERVEGAME && CBstate != ANALYZEGAME && CBstate != ANALYZEPDN && !abortcalculation)
			memcpy(cbboard8, originalcopy, sizeof(cbboard8));

		// if we are in engine match mode and one of the engines claims a win
		// or a loss or a draw we stop
		if (result != CB_UNKNOWN && (CBstate == ENGINEMATCH)) {
			if (cbgame.movesindex > 0)
				sprintf(cbgame.moves[cbgame.movesindex - 1].comment, "%s : gameover claimed", statusbar_txt);
			gameover = TRUE;
		}

		// got board8 & a copy before move was made
		if (CBstate != OBSERVEGAME && CBstate != ANALYZEGAME && CBstate != ANALYZEPDN && !abortcalculation) {

			// determine the move that was made: we only do this if gametype is GT_ENGLISH,
			//	else the engine must return the appropriate information in localmove
			if (gametype() == GT_ENGLISH) {
				if (cbcolor == CB_BLACK)
					nmoves = getmovelist(1, m, b8copy, &dummy);
				else
					nmoves = getmovelist(-1, m, b8copy, &dummy);
				cbmove = m[0];
				for (i = 0; i < nmoves; i++) {

					//put original board8 in b8copy, execute move and compare with returned board8...
					memcpy(b8copy, original8board, sizeof(b8copy));
					domove(m[i], b8copy);
					if (memcmp(cbboard8, b8copy, sizeof(cbboard8)) == 0) {
						move4tonotation(m[i], PDN);
						cbmove = m[i];
						found = 1;
						break;
					}
				}

				if (found == 0)
					memcpy(cbboard8, original8board, sizeof(cbboard8));
			}
			else {

				// gametype not GT_ENGLISH, not regular checkers, use the move of the engine
				cbmove = localmove;
				move4tonotation(localmove, PDN);
				memcpy(cbboard8, original8board, sizeof(cbboard8));
				found = 1;
			}
		}

		// ************* finished determining what move was made....
	}

	// now we execute the move, but only if we are not in the mode
	// ANALYZEGAME or OBSERVEGAME
	if ((CBstate != OBSERVEGAME) && (CBstate != ANALYZEGAME) && (CBstate != ANALYZEPDN) && found && !abortcalculation) {
		appendmovetolist(cbmove);

		// if sound is on we make a beep
		if (cboptions.sound)
			Beep(440, 5);
		CloseHandle(hAniThread);
		setanimationbusy(TRUE);			// this was missing in CB 1.65 which was the reason for the bug...
		hAniThread = CreateThread(NULL,
								  0,
								  (LPTHREAD_START_ROUTINE) AnimationThreadFunc,
								  (HWND) hwnd,
								  0,
								  &g_AniThreadId);
	}

	// if CBstate is ANALYZEGAME, we have to print the analysis to a logfile,
	// make the move played in the game & also print it into the logfile
	switch (CBstate) {
	case ANALYZEPDN:					// drop through to analyzegame
	case ANALYZEGAME:
		// don't add analysis if there is only one move
		if (nmoves == 1)
			break;

		if (cbgame.movesindex < (int)cbgame.moves.size())
			sprintf(cbgame.moves[cbgame.movesindex].analysis, "%s", statusbar_txt);
		break;

	case ENGINEMATCH:
		{
			filename[MAX_PATH];

			sprintf(filename, "%s\\matchlog%s.txt", cboptions.matchdirectory, g_app_instance_suffix);
			Lfp = fopen(filename, "a");
			enginename(Lstr);
			if (Lfp != NULL) {
				fprintf(Lfp, "\n%s played %s", Lstr, PDN);
				fprintf(Lfp, "\nanalysis: %s", statusbar_txt);
				fclose(Lfp);
			}
		}
		break;
	}

	reset_move_history = false;
	setenginebusy(FALSE);
	setenginestarting(FALSE);
	return 1;
}

int changeCBstate(int oldstate, int newstate)
{
	// changes the state of Checkerboard from old state to new state
	// does whatever is necessary to do this - checks/unchecks menu buttons
	// changes state of buttons in toolbar etc.
	// when we change the state, we first of all make sure that the
	// engine is not running
	PostMessage(hwnd, WM_COMMAND, ABORTENGINE, 0);

	// toolbar buttons
	if (oldstate == BOOKVIEW)
		SendMessage(tbwnd, TB_CHECKBUTTON, (WPARAM) BOOKMODE_VIEW, MAKELONG(0, 0));
	if (oldstate == BOOKADD)
		SendMessage(tbwnd, TB_CHECKBUTTON, (WPARAM) BOOKMODE_ADD, MAKELONG(0, 0));

	CBstate = (enum state)newstate;

	// toolbar buttons
	if (CBstate == BOOKVIEW)
		SendMessage(tbwnd, TB_CHECKBUTTON, (WPARAM) BOOKMODE_VIEW, MAKELONG(1, 0));
	if (CBstate == BOOKADD)
		SendMessage(tbwnd, TB_CHECKBUTTON, (WPARAM) BOOKMODE_ADD, MAKELONG(1, 0));

	// mode menu checks are taken care of in autothread function
	// set menu
	CheckMenuItem(hmenu, CM_NORMAL, MF_UNCHECKED);
	CheckMenuItem(hmenu, CM_ANALYSIS, MF_UNCHECKED);
	CheckMenuItem(hmenu, CM_AUTOPLAY, MF_UNCHECKED);
	CheckMenuItem(hmenu, CM_ENGINEMATCH, MF_UNCHECKED);
	CheckMenuItem(hmenu, CM_2PLAYER, MF_UNCHECKED);
	CheckMenuItem(hmenu, BOOKMODE_VIEW, MF_UNCHECKED);
	CheckMenuItem(hmenu, BOOKMODE_ADD, MF_UNCHECKED);

	/* Update animation state. */
	if (CBstate == ENGINEMATCH) {
		if (cboptions.use_incremental_time) {
			if (cboptions.initial_time / 30 + cboptions.time_increment <= 1.5)
				set_animation(false);
			else
				set_animation(true);
		}
		else {
			if (timelevel_to_time(cboptions.level) <= 1)
				set_animation(false);
			else
				set_animation(true);
		}
	}
	else
		set_animation(true);

	switch (CBstate) {
	case NORMAL:
		CheckMenuItem(hmenu, CM_NORMAL, MF_CHECKED);
		break;

	case OBSERVEGAME:
		CheckMenuItem(hmenu, CM_ANALYSIS, MF_CHECKED);
		break;

	case AUTOPLAY:
		CheckMenuItem(hmenu, CM_AUTOPLAY, MF_CHECKED);
		break;

	case ENGINEMATCH:
		CheckMenuItem(hmenu, CM_ENGINEMATCH, MF_CHECKED);
		break;

	case ENTERGAME:
		CheckMenuItem(hmenu, CM_2PLAYER, MF_CHECKED);
		break;

	case BOOKVIEW:
		CheckMenuItem(hmenu, BOOKMODE_VIEW, MF_CHECKED);
		break;

	case BOOKADD:
		CheckMenuItem(hmenu, BOOKMODE_ADD, MF_CHECKED);
		break;
	}

	// clear status bar
	sprintf(statusbar_txt, "");
	return 1;
}

DWORD AutoThreadFunc(LPVOID param)
{
	//	this thread drives autoplay,analyze game and engine match.
	//	it looks in what state CB is currently and
	//  then does the appropriate thing and sets CBstate to the new state.
	//  CBstate only changes inside this function or on the menu commands
	//  'analyze game', 'engine match', 'autoplay' and 'play (->normal)'
	//  all automatic changes are in here!
	//
	//  it uses the booleans enginebusy and animationbusy to
	//  detect if it is allowed to do anything
	char Lstr[256];
	char windowtitle[256];
	char analysisfilename[256];
	char testlogname[MAX_PATH];
	char testsetname[MAX_PATH];
	char statsfilename[MAX_PATH];
	static char matchlogstring[65536];	// large string which holds the output which we write to match_progress.txt
	static int oldengine;
	FILE *Lfp;
	static int gamenumber;
	static int movecount;
	int i;
	const int maxmovecount = 200;
	static int wins, draws, losses, unknowns;
	static int blackwins, blacklosses;	//wins as black of primary engine
	static char FEN[256];
	char engine1[256], engine2[256];	// holds engine names
	int matchcontinues = 0;
	static int iselevenman = 0;

	// autothread is started at startup, and keeps running until the program
	// terminates, that's what for(;;) is here for.
	for (;;) {

		// thread sleeps for AUTOSLEEPTIME (10ms) so that this loop runs at
		// approximately 100x per second
		Sleep(AUTOSLEEPTIME);

		// if CB is doing something else, wait for it to finish by dropping back to sleep command above
		if (getanimationbusy() || getenginebusy() || getenginestarting())
			continue;

		switch (CBstate) {
		case NORMAL:
			if (startengine) {

				/* after determining the user move startengine flag is set and
					the move is animated. */
				PostMessage(hwnd, (UINT) WM_COMMAND, (WPARAM) MOVESPLAY, (LPARAM) 0);
				setenginestarting(TRUE);
				startengine = FALSE;
			}
			break;

		case RUNTESTSET:
			// sleep for 0.2 seconds to allow handletimer() to update the testlog file
			Sleep(200);

			// create or update testlog file
			strcpy(testlogname, CBdocuments);
			PathAppend(testlogname, "testlog.txt");
			if (testset_number == 0) {

				// testset start: clear file testlog.txt
				Lfp = fopen(testlogname, "w");
				fclose(Lfp);
			}
			else
				// write analysis
				logtofile(testlogname, "\n\n", "a");

			// load the next position from the test set
			strcpy(testsetname, CBdocuments);
			PathAppend(testsetname, "testset.txt");
			Lfp = fopen(testsetname, "r");
			if (Lfp == NULL) {
				sprintf(statusbar_txt, "could not find %s", testsetname);
				break;
			}

			for (i = 0; i <= testset_number; i++) {
				fgets(FEN, 255, Lfp);
				if (feof(Lfp)) {
					changeCBstate(RUNTESTSET, NORMAL);
				}
			}

			fclose(Lfp);
			testset_number++;

			// write FEN in testlog
			sprintf(statusbar_txt, "#%i: %s", testset_number, FEN);
			logtofile(testlogname, statusbar_txt, "a");

			// convert position to internal board
			FENtoboard8(cbboard8, FEN, &cbcolor, cbgame.gametype);
			updateboardgraphics(hwnd);
			reset_move_history = true;
			newposition = TRUE;
			PostMessage(hwnd, WM_COMMAND, MOVESPLAY, 0);
			setenginestarting(TRUE);
			Sleep(SLEEPTIME);
			break;

		case AUTOPLAY:
			// check if game is over, if yes, go from autoplay to normal state
			if (gameover == TRUE) {
				gameover = FALSE;
				changeCBstate(CBstate, NORMAL);
				sprintf(statusbar_txt, "game over");
			}

			// else continue game by sending a play command
			else {
				if (getenginestarting() || getanimationbusy() || getenginebusy())
					break;
				PostMessage(hwnd, WM_COMMAND, MOVESPLAY, 0);
				setenginestarting(TRUE);

				// sleep a bit to allow engine to start
				Sleep(SLEEPTIME);
			}
			break;

		case ENGINEGAME:
			if (gameover == TRUE) {
				gameover = FALSE;
				changeCBstate(CBstate, NORMAL);
				setcurrentengine(1);
				break;
			}

			if (startmatch == TRUE)
				startmatch = FALSE;
			else {
				currentengine ^= 3;
				setcurrentengine(currentengine);
				enginename(Lstr);
				sprintf(statusbar_txt, "CheckerBoard%s: ", g_app_instance_suffix);
				strcat(statusbar_txt, Lstr);
				SetWindowText(hwnd, statusbar_txt);
			}

			PostMessage(hwnd, WM_COMMAND, MOVESPLAY, 0);
			setenginestarting(TRUE);

			break;

		case ANALYZEGAME:
			if (gameover == TRUE) {
				gameover = FALSE;
				changeCBstate(CBstate, NORMAL);
				sprintf(statusbar_txt, "Game analysis finished!");
				strcpy(analysisfilename, CBdocuments);
				PathAppend(analysisfilename, "analysis");
				PathAppend(analysisfilename, "analysis.htm");
				makeanalysisfile(analysisfilename);
				break;
			}

			if (currentengine != 1)
				setcurrentengine(1);
			PostMessage(hwnd, WM_COMMAND, MOVESFORWARDALL, 0);

			Sleep(SLEEPTIME);

			// start analysis logfile - overwrite anything old
			strcpy(analysisfilename, CBdocuments);
			PathAppend(analysisfilename, "analysis.txt");
			Lfp = fopen(analysisfilename, "w");
			fclose(Lfp);
			sprintf(statusbar_txt, "played in game: 1. %s", cbgame.moves[cbgame.movesindex - 1].PDN);
			logtofile(analysisfilename, statusbar_txt, "a");

			PostMessage(hwnd, WM_COMMAND, MOVESPLAY, 0);
			setenginestarting(TRUE);

			// go into a for loop until the game is completely analyzed
			for (;;) {
				Sleep(SLEEPTIME);
				if ((CBstate != ANALYZEGAME) || (gameover == TRUE))
					break;
				if (!getenginebusy() && !getanimationbusy()) {
					PostMessage(hwnd, WM_COMMAND, MOVESBACK, 0);
					if (CBstate == ANALYZEGAME && gameover == FALSE) {
						PostMessage(hwnd, WM_COMMAND, MOVESPLAY, 0);
						setenginestarting(TRUE);
					}
				}
			}
			break;

		case ANALYZEPDN:
			if (startmatch == TRUE) {

			// this is the case when the user chooses analyzepdn in the menu;
			// at this point it is true for the first and only time in the
			// analyzepdn mode.
				gamenumber = 1;
				startmatch = FALSE;
				strcpy(analysisfilename, CBdocuments);
				PathAppend(analysisfilename, "analysis");
				PathAppend(analysisfilename, "analysis1.htm");
			}

			if (gameover == TRUE) {
				gameover = FALSE;
				makeanalysisfile(analysisfilename);

				// get number of next game; loadnextgame returns 0 if
				// there is no further game.
				gamenumber = loadnextgame();
				sprintf(analysisfilename, "%s\\analysis\\analysis%i.htm", CBdocuments, gamenumber);
			}

			if (gamenumber == 0) {

			// we're done with the file
				changeCBstate(CBstate, NORMAL);
				sprintf(statusbar_txt, "PDN analysis finished!");
				break;
			}

			// this is the signal that we are at the start of the analysis of a game
			if (currentengine != 1)
				setcurrentengine(1);
			PostMessage(hwnd, WM_COMMAND, MOVESFORWARDALL, 0);
			PostMessage(hwnd, WM_COMMAND, MOVESPLAY, 0);
			setenginestarting(TRUE);

			// analyze entire game in this for loop
			for (;;) {
				Sleep(SLEEPTIME);
				if ((CBstate != ANALYZEPDN) || (gameover == TRUE))
					break;
				if (!getenginebusy() && !getanimationbusy()) {
					PostMessage(hwnd, WM_COMMAND, MOVESBACK, 0);

					// give the main thread some time to stop analysis if
					//we are at the end of the game
					if (CBstate == ANALYZEPDN && gameover == FALSE) {
						PostMessage(hwnd, WM_COMMAND, MOVESPLAY, 0);
						setenginestarting(TRUE);
					}
				}
			}
			break;

		case OBSERVEGAME:
			// select primary engine if this is not the case
			if (currentengine != 1)
				setcurrentengine(1);

			// start engine if we have a new position.
			if (newposition) {
				playnow = 0;
				PostMessage(hwnd, WM_COMMAND, MOVESPLAY, 0);
				setenginestarting(TRUE);
				newposition = 0;
			}
			break;

		case ENGINEMATCH:
			if (startmatch) {

				// a new match has been started, do some initializations
				sprintf(matchlogstring, "");
				gamenumber = 0;
				wins = 0;
				losses = 0;
				draws = 0;
				unknowns = 0;
				blackwins = 0;
				blacklosses = 0;

				// check to see if a stats.txt file is here, and if yes, continue the match
				sprintf(statsfilename, "%s\\stats%s.txt", cboptions.matchdirectory, g_app_instance_suffix);
				Lfp = fopen(statsfilename, "r");
				if (Lfp != NULL) {

					// stats.txt exists
					// read first line just to read second line, which holds the actual stats
					fgets(Lstr, 255, Lfp);
					fgets(Lstr, 255, Lfp);
					sscanf(Lstr,
						   "+:%i =:%i -:%i unknown:%i +B:%i -B:%i",
						   &wins,
						   &draws,
						   &losses,
						   &unknowns,
						   &blackwins,
						   &blacklosses);
					gamenumber = wins + losses + draws + unknowns;
					sprintf(statusbar_txt,
							"resuming match at game #%i, (+:%i -:%i =:%i unknown:%i)",
							gamenumber,
							wins,
							losses,
							draws,
							unknowns);
					fclose(Lfp);

					// read match-progress file 	// TODO: this should be superfluous, write directly to file...
					sprintf(statsfilename, "%s\\match_progress%s.txt", cboptions.matchdirectory, g_app_instance_suffix);
					Lfp = fopen(statsfilename, "r");
					if (Lfp != NULL) {
						while (!feof(Lfp)) {
							fgets(Lstr, 255, Lfp);
							strcat(matchlogstring, Lstr);
						}

						fclose(Lfp);
					}
				}

				// finally, display stats in window title
				enginecommand1("name", engine1);
				enginecommand2("name", engine2);
				sprintf(windowtitle, "%s - %s", engine1, engine2);
				sprintf(Lstr, ": W-L-D:%i-%i-%i", wins, losses, draws + unknowns);
				strcat(windowtitle, Lstr);
				SetWindowText(hwnd, windowtitle);

				// ask user whether this is regular match or 11-man-match
				iselevenman = MessageBox(hwnd,
										 "Play 3-move openings? Choose Yes for 3-move, No for 11-man",
										 "Choose Match Type",
										 MB_ICONQUESTION | MB_YESNO);
				if (iselevenman == IDYES)
					iselevenman = 0;
				else
					iselevenman = 1;
			}	// end if startmatch

			// stuff below is for regular games
			// stop games which have been going for too long
			if (movecount > maxmovecount)
				gameover = TRUE;

			// when a game is terminated, save result and save game
			if ((gameover || startmatch == TRUE)) {
				if (gameover) {

					// set white and black players
					if (gamenumber % 2) {
						sprintf(cbgame.black, "%s", engine1);
						sprintf(cbgame.white, "%s", engine2);
					}
					else {
						sprintf(cbgame.black, "%s", engine2);
						sprintf(cbgame.white, "%s", engine1);
					}

					sprintf(cbgame.resultstring, "?");
					if (!((gamenumber - 1) % 20)) {
						if (gamenumber != 1)
							strcat(matchlogstring, "\n");
					}

					if (gamenumber % 2) {
						if (iselevenman == 1)
							sprintf(Lstr, "%4i:", gamenumber / 2 + 1);
						else
							sprintf(Lstr, "%3i:", op + 1);
						strcat(matchlogstring, Lstr);
					}

					// check result
					dostats(result,
							movecount,
							gamenumber,
							&wins,
							&draws,
							&losses,
							&unknowns,
							&blackwins,
							&blacklosses,
							matchlogstring);

					// finally, display stats in window title
					sprintf(windowtitle, "%s - %s", engine1, engine2);
					sprintf(Lstr, ": W-L-D:%i-%i-%i", wins, losses, draws + unknowns);
					strcat(windowtitle, Lstr);
					SetWindowText(hwnd, windowtitle);
					if (!(gamenumber % 2))
						strcat(matchlogstring, "  ");

					// write match statistics
					sprintf(statsfilename, "%s\\stats%s.txt", cboptions.matchdirectory, g_app_instance_suffix);
					Lfp = fopen(statsfilename, "w");
					if (Lfp != NULL) {
						fprintf(Lfp, "%s - %s", engine1, engine2);
						fprintf(Lfp, " %s\n", Lstr);
						fprintf(Lfp,
								"+:%i =:%i -:%i unknown:%i +B:%i -B:%i",
								wins,
								draws,
								losses,
								unknowns,
								blackwins,
								blacklosses);
						fclose(Lfp);
					}

					// write match_progress.txt file
					sprintf(statsfilename, "%s\\match_progress%s.txt", cboptions.matchdirectory, g_app_instance_suffix);
					logtofile(statsfilename, matchlogstring, "w");

					// save the game
					if (iselevenman == 1)
						sprintf(cbgame.event, "11-man #%i", (gamenumber - 1) / 2 + 1);
					else
						sprintf(cbgame.event, "ACF #%i", op + 1);

					// dosave expects a fully initialized cbgame structure
					sprintf(filename, "%s\\match%s.pdn", cboptions.matchdirectory, g_app_instance_suffix);
					SendMessage(hwnd, WM_COMMAND, DOSAVE, 0);

					Sleep(SLEEPTIME);
				}

				// set startmatch to FALSE, it is only true when the match starts to initialize
				startmatch = FALSE;

				// get the opening for the gamenumber, and check whether the match is over
				if (iselevenman == 1)
					matchcontinues = start11man(gamenumber / 2);
				else {
					op = getthreeopening(gamenumber, &cboptions);
					if (op == -1)
						matchcontinues = 0;
					else
						matchcontinues = 1;
				}

				// move on to the next game
				movecount = 0;
				gameover = FALSE;
				gamenumber++;

				sprintf(statusbar_txt, "gamenumber is %i\n", gamenumber);

				if (matchcontinues == 0) {
					changeCBstate(CBstate, NORMAL);
					setcurrentengine(1);

					// write final result in window title bar
					sprintf(Lstr, "Final result of %s", windowtitle);
					SetWindowText(hwnd, Lstr);
					break;
				}

				// set color of engine to start playing
				if (gamenumber % 2)
					setcurrentengine(1);
				else
					setcurrentengine(2);

				// post message so that main thread handles the request
				if (CBstate != NORMAL) {
					if (iselevenman == 1)
						PostMessage(hwnd, WM_COMMAND, START11MAN, 0);
					else
						PostMessage(hwnd, WM_COMMAND, START3MOVE, 0);

					// give main thread some time to handle this message
					Sleep(SLEEPTIME);
				}
				break;
			}

			if (!gameover && CBstate == ENGINEMATCH) {

				// make next move in game
				movecount++;

				// set which engine
				if ((gamenumber + cbcolor) % 2)
					setcurrentengine(1);
				else
					setcurrentengine(2);

				PostMessage(hwnd, WM_COMMAND, MOVESPLAY, 0);
				setenginestarting(TRUE);

				// give main thread some time to handle this message
				Sleep(SLEEPTIME);
			}
			break;
		}		// end switch CBstate
	}			// end for(;;)
}

int dostats
	(
		int result,
		int movecount,
		int gamenumber,
		int *wins,
		int *draws,
		int *losses,
		int *unknowns,
		int *blackwins,
		int *blacklosses,
		char *matchlogstring
	)
{
	// handles statistics during an engine match
	const int maxmovecount = 200;

	if (movecount > maxmovecount) {
		(*unknowns)++;
		sprintf(cbgame.resultstring, "*");
		strcat(matchlogstring, "?");
	}
	else {
		switch (result) {
		case CB_WIN:
			if (currentengine == 1) {
				(*wins)++;
				strcat(matchlogstring, "+");
				if (gamenumber % 2) {
					(*blackwins)++;
					sprintf(cbgame.resultstring, "1-0");
				}
				else
					sprintf(cbgame.resultstring, "0-1");
			}
			else {
				(*losses)++;
				strcat(matchlogstring, "-");
				if (gamenumber % 2) {
					(*blacklosses)++;
					sprintf(cbgame.resultstring, "0-1");
				}
				else
					sprintf(cbgame.resultstring, "1-0");
			}
			break;

		case CB_DRAW:
			strcat(matchlogstring, "=");
			(*draws)++;
			sprintf(cbgame.resultstring, "1/2-1/2");
			break;

		case CB_LOSS:
			if (currentengine == 1) {
				(*losses)++;
				strcat(matchlogstring, "-");
				if (gamenumber % 2) {
					(*blacklosses)++;
					sprintf(cbgame.resultstring, "0-1");
				}
				else
					sprintf(cbgame.resultstring, "1-0");
			}
			else {
				(*wins)++;
				strcat(matchlogstring, "+");
				if (gamenumber % 2) {
					(*blackwins)++;
					sprintf(cbgame.resultstring, "1-0");
				}
				else
					sprintf(cbgame.resultstring, "0-1");
			}
			break;

		case CB_UNKNOWN:
			(*unknowns)++;
			strcat(matchlogstring, "?");
			sprintf(cbgame.resultstring, "*");
			break;
		}
	}

	return 1;
}

int CPUinfo(char *str)
{
	// print CPU info into str
	int CPUInfo[4] = { -1 };
	char CPUBrandString[0x40];

	// get processor info
	__cpuid(CPUInfo, 0x80000002);
	memcpy(CPUBrandString, CPUInfo, sizeof(CPUInfo));
	__cpuid(CPUInfo, 0x80000003);
	memcpy(CPUBrandString + 16, CPUInfo, sizeof(CPUInfo));
	__cpuid(CPUInfo, 0x80000004);
	memcpy(CPUBrandString + 32, CPUInfo, sizeof(CPUInfo));

	sprintf(str, "%s", CPUBrandString);

	return 1;
}

int makeanalysisfile(char *filename)
{
	// produce nice analysis output
	int i;
	char s[256];
	char titlestring[256];
	char c1[256] = "D84020";
	char c2[256] = "A0C0C0";
	char c3[256] = "444444";
	FILE *fp;
	char CPUinfostring[64];

	fp = fopen(filename, "w");
	if (fp == NULL) {
		MessageBox(hwnd, "Could not open analysisfile - is\nyour analysis directory missing?", "Error", MB_OK);
		return 0;
	}

	// print game info
	sprintf(titlestring, "%s - %s", cbgame.black, cbgame.white);

	// print HTML head
	fprintf(fp,
			"<HTML>\n<HEAD>\n<META name=\"GENERATOR\" content=\"CheckerBoard %s\">\n<TITLE>%s</TITLE></HEAD>",
			VERSION,
			titlestring);

	// print HTML body
	fprintf(fp, "<BODY><H3>");
	fprintf(fp, "%s - %s", cbgame.black, cbgame.white);
	fprintf(fp, "</H3>");
	fprintf(fp, "\n%s<BR>%s<BR>", cbgame.date, cbgame.event);
	fprintf(fp, "\nResult: %s<P>", cbgame.resultstring);

	// print hardware and level info
	enginename(s);

	CPUinfo(CPUinfostring);

	fprintf(fp, "\nAnalysis by %s at %.1fs/move on %s", s, timelevel_to_time(cboptions.level), CPUinfostring);
	fprintf(fp, "\n<BR>\ngenerated with <A HREF=\"http://www.fierz.ch/checkers.htm\">CheckerBoard %s</A><P>", VERSION);

	// print PDN and analysis
	fprintf(fp, "\n<TABLE cellspacing=\"0\" cellpadding=\"3\">");
	for (i = 0; i < (int)cbgame.moves.size(); ++i) {
		fprintf(fp, "<TR>\n");
		if (strcmp(cbgame.moves[i].analysis, "") == 0) {
			if (is_second_player(cbgame, i)) {
				fprintf(fp,
						"<TD></TD><TD bgcolor=\"%s\"></TD><TD>%s</TD><TD bgcolor=\"%s\"></TD>\n",
						c1,
						cbgame.moves[i].PDN,
						c2);
			}
			else {
				fprintf(fp,
						"<TD>%2i.</TD><TD bgcolor=\"%s\">%s</TD><TD></TD><TD bgcolor=\"%s\"></TD>\n",
						moveindex2movenum(cbgame, i),
						c1,
						cbgame.moves[i].PDN,
						c2);
			}
		}
		else {
			if (is_second_player(cbgame, i)) {
				fprintf(fp,
						"<TD></TD><TD bgcolor=\"%s\"></TD><TD>%s</TD><TD bgcolor=\"%s\">%s</TD>\n",
						c1,
						cbgame.moves[i].PDN,
						c2,
						cbgame.moves[i].analysis);
			}
			else {
				fprintf(fp,
						"<TD>%2i.</TD><TD bgcolor=\"%s\">%s</TD><TD></TD><TD bgcolor=\"%s\">%s</TD>\n",
						moveindex2movenum(cbgame, i),
						c1,
						cbgame.moves[i].PDN,
						c2,
						cbgame.moves[i].analysis);
			}
		}

		fprintf(fp, "</TR>\n");

		// add a delimiter line between moves
		fprintf(fp, "<tr><td></td><td bgcolor=\"%s\"></td><td></td><td bgcolor=\"%s\"></td></tr>\n", c1, c3);
	}

	fprintf(fp, "</TABLE></BODY></HTML>");
	fclose(fp);

	ShellExecute(NULL, "open", filename, NULL, NULL, SW_SHOW);

	return 1;
}

void setcurrentengine(int engineN)
{
	char s[256], windowtitle[256];

	// set the engine
	if (engineN == 1) {
		getmove = getmove1;
		islegal = islegal1;
	}

	if (engineN == 2) {
		getmove = getmove2;
		islegal = islegal2;
	}

	currentengine = engineN;

	if (CBstate != ENGINEMATCH) {
		enginename(s);
		sprintf(windowtitle, "CheckerBoard%s: ", g_app_instance_suffix);
		strcat(windowtitle, s);
		SetWindowText(hwnd, windowtitle);
	}

	toggleengine = currentengine;

	// get book state of current engine
	if (enginecommand("get book", s))
		togglebook = atoi(s);
}

int gametype(void)
{
	// returns the game type which the current engine plays
	// if the engine has no game type associated, it will return 21 for english checkers
	char reply[ENGINECOMMAND_REPLY_SIZE];
	char command[256];

	sprintf(reply, "");
	sprintf(command, "get gametype");

	if (enginecommand(command, reply))
		return atoi(reply);

	// return default game type
	return GT_ENGLISH;
}

int enginecommand(char command[256], char reply[ENGINECOMMAND_REPLY_SIZE])
// sends a command to the current engine, defined with the currentengine variable
// wraps a 'safety layer around calls to engine command by checking if this is supported */
{
	int result = 0;
	sprintf(reply, "");

	if (currentengine == 1 && enginecommand1 != 0)
		result = enginecommand1(command, reply);

	if (currentengine == 2 && enginecommand2 != 0)
		result = enginecommand2(command, reply);

	return result;
}

int enginename(char Lstr[256])
// returns the name of the current engine in Lstr
{
	// set a default
	sprintf(Lstr, "no engine found");

	if (currentengine == 1) {
		if (enginecommand1 != 0) {
			if ((enginecommand1) ("name", Lstr))
				return 1;
		}

		if (enginename1 != 0) {
			(enginename1) (Lstr);
			return 1;
		}
	}

	if (currentengine == 2) {
		if (enginecommand2 != 0) {
			if ((enginecommand2) ("name", Lstr))
				return 1;
		}

		if (enginename2 != 0) {
			(enginename2) (Lstr);
			return 1;
		}
	}

	return 0;
}

int domove(struct CBmove m, int b[8][8])
{
	// do move m on board b
	int i, x, y;

	x = m.from.x;
	y = m.from.y;
	b[x][y] = 0;
	x = m.to.x;
	y = m.to.y;
	b[x][y] = m.newpiece;

	for (i = 0; i < m.jumps; i++) {
		x = m.del[i].x;
		y = m.del[i].y;
		b[x][y] = 0;
	}

	return 1;
}

int undomove(struct CBmove m, int b[8][8])
{
	// take back move m on board b
	int i, x, y;

	x = m.to.x;
	y = m.to.y;
	b[x][y] = 0;

	x = m.from.x;
	y = m.from.y;
	b[x][y] = m.oldpiece;

	for (i = 0; i < m.jumps; i++) {
		x = m.del[i].x;
		y = m.del[i].y;
		b[x][y] = m.delpiece[i];
	}

	return 1;
}

void move4tonotation(struct CBmove m, char s[80])
// takes a move in coordinates, and transforms it to numbers.
{
	int from, to;
	char c = '-';
	int x1, y1, x2, y2;
	char Lstr[255];
	x1 = m.from.x;
	y1 = m.from.y;
	x2 = m.to.x;
	y2 = m.to.y;

	if (m.jumps)
		c = 'x';

	// for all versions of checkers
	from = coorstonumber(x1, y1, cbgame.gametype);
	to = coorstonumber(x2, y2, cbgame.gametype);

	sprintf(s, "%i", from);
	sprintf(Lstr, "%c", c);
	strcat(s, Lstr);
	sprintf(Lstr, "%i", to);
	strcat(s, Lstr);
}

void PDNgametoPDNstring(PDNgame &game, char *pdnstring, char *lf)
{
	// prints a formatted PDN in *pdnstring
	// uses lf as line feed; for the clipboard this should be \r\n, normally just \n
	// i have no idea why this is so!
	char s[256];
	size_t counter;
	int i;

	// I: print headers
	sprintf(pdnstring, "");
	sprintf(s, "[Event \"%s\"]", game.event);
	strcat(pdnstring, s);
	strcat(pdnstring, lf);

	sprintf(s, "[Date \"%s\"]", game.date);
	strcat(pdnstring, s);
	strcat(pdnstring, lf);

	sprintf(s, "[Black \"%s\"]", game.black);
	strcat(pdnstring, s);
	strcat(pdnstring, lf);

	sprintf(s, "[White \"%s\"]", game.white);
	strcat(pdnstring, s);
	strcat(pdnstring, lf);

	sprintf(s, "[Result \"%s\"]", game.resultstring);
	strcat(pdnstring, s);
	strcat(pdnstring, lf);

	// if this was after a setup, add FEN and setup header
	if (strcmp(game.setup, "") != 0) {
		sprintf(s, "[Setup \"%s\"]", game.setup);
		strcat(pdnstring, s);
		strcat(pdnstring, lf);

		sprintf(s, "[FEN \"%s\"]", game.FEN);
		strcat(pdnstring, s);
		strcat(pdnstring, lf);
	}

	// print PDN
	counter = 0;
	for (i = 0; i < (int)game.moves.size(); ++i) {
		move4tonotation(game.moves[i].move, game.moves[i].PDN);

		// print the move number
		if (!is_second_player(game, i)) {
			sprintf(s, "%i. ", moveindex2movenum(game, i));
			counter += strlen(s);
			if (counter > 79) {
				strcat(pdnstring, lf);
				counter = strlen(s);
			}

			strcat(pdnstring, s);
		}

		// print the move
		counter += strlen(game.moves[i].PDN);
		if (counter > 79) {
			strcat(pdnstring, lf);
			counter = strlen(game.moves[i].PDN);
		}

		sprintf(s, "%s ", game.moves[i].PDN);
		strcat(pdnstring, s);

		// if the move has a comment, print it too
		if (strcmp(game.moves[i].comment, "") != 0) {
			counter += strlen(game.moves[i].comment);
			if (counter > 79) {
				strcat(pdnstring, lf);
				counter = strlen(game.moves[i].comment);
			}

			strcat(pdnstring, "{");
			strcat(pdnstring, game.moves[i].comment);
			strcat(pdnstring, "} ");
		}
	}

	// add the game terminator
	sprintf(s, "*");	/* Game terminator is '*' as per PDN 3.0. See http://pdn.fmjd.org/ */
	counter += strlen(s);
	if (counter > 79)
		strcat(pdnstring, lf);

	strcat(pdnstring, s);

	strcat(pdnstring, lf);
	strcat(pdnstring, lf);
}

/*
 * Adds a move to the cbgame.moves vector, and fills in the PDN field.
 * Initializes the analysis and comment fields to an empty string.
 * The move is added after the current position into the moves list, cbgame.movesindex.
 * If this is not the end of moves[], delete all the entries starting at movesindex.
 */
void appendmovetolist(CBmove &move)
{
	int i;
	gamebody_entry entry;

	/* Delete entries in cbgames.moves[] from end back to movesindex. Do it in reverse order
	 * because it's more efficient to delete vector entries from the end than in the middle.
	 */
	for (i = (int)cbgame.moves.size() - 1; i >= cbgame.movesindex; --i)
		cbgame.moves.erase(cbgame.moves.begin() + i);

	entry.analysis[0] = 0;
	entry.comment[0] = 0;
	entry.move = move;
	move4tonotation(move, entry.PDN);
	try
	{
		cbgame.moves.push_back(entry);
	}
	catch(...) {
		char *msg = "could not allocate memory for CB movelist";
		CBlog(msg);
		strcpy(statusbar_txt, msg);
	}

	cbgame.movesindex = (int)cbgame.moves.size();
}

int getfilename(char filename[255], int what)
{
	OPENFILENAME of;
	char dir[MAX_PATH];

	sprintf(filename, "");
	(of).lStructSize = sizeof(OPENFILENAME);
	(of).hwndOwner = NULL;
	(of).hInstance = g_hInst;
	(of).lpstrFilter = "checkers databases *.pdn\0 *.pdn\0 all files *.*\0 *.*\0\0";
	(of).lpstrCustomFilter = NULL;
	(of).nMaxCustFilter = 0;
	(of).nFilterIndex = 0;
	(of).lpstrFile = filename;	// if user chooses something, it's printed in here!
	(of).nMaxFile = MAX_PATH;
	(of).lpstrFileTitle = NULL;
	(of).nMaxFileTitle = 0;
	(of).lpstrInitialDir = cboptions.userdirectory;
	(of).lpstrTitle = NULL;
	(of).Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
	(of).nFileOffset = 0;
	(of).nFileExtension = 0;
	(of).lpstrDefExt = NULL;
	(of).lCustData = 0;
	(of).lpfnHook = NULL;
	(of).lpTemplateName = NULL;

	if (what == OF_SAVEGAME) {

	/* save a game to disk */
		(of).lpstrTitle = "Select PDN database to save game to";
		if (GetSaveFileName(&of))
			return 1;
	}

	if (what == OF_LOADGAME) {

		/* load a game from disk */
		(of).lpstrTitle = "Select PDN database to load";
		if (GetOpenFileName(&of))
			return 1;
	}

	if (what == OF_SAVEASHTML) {

	// save game as html
		(of).lpstrTitle = "Select filename of HTML output";
		(of).lpstrFilter = "HTML files *.htm\0 *.htm\0 all files *.*\0 *.*\0\0";
		sprintf(dir, "%s\\games", CBdocuments);
		(of).lpstrInitialDir = dir;
		if (GetSaveFileName(&of))
			return 1;
	}

	if (what == OF_USERBOOK) {

	// select user book
		(of).lpstrTitle = "Select the user book to use";
		(of).lpstrFilter = "user book files *.bin\0 *.bin\0 all files *.*\0 *.*\0\0";
		(of).lpstrInitialDir = CBdocuments;
		if (GetSaveFileName(&of))
			return 1;
	}

	if (what == OF_BOOKFILE) {
		(of).lpstrTitle = "Select the opening book filename";
		(of).lpstrFilter = "user book files *.odb\0 *.odb\0 all files *.*\0 *.*\0\0";
		sprintf(dir, "%s\\engines", CBdirectory);
		(of).lpstrInitialDir = dir;
		if (GetOpenFileName(&of))
			return 1;
	}

	return 0;
}

void pdntogame(int startposition[8][8], int startcolor)
{
	/* pdntogame takes a starting position, a side to move next as parameters. 
	it uses cbgame, which has to be initialized with pdn-text to generate the CBmoves. */

	/* called by loadgame and gamepaste */
	int i, color;
	int b8[8][8];
	int from, to;
	CBmove legalmove;

	/* set the starting values */
	color = startcolor;
	memcpy(b8, startposition, sizeof(b8));
	for (i = 0; i < (int)cbgame.moves.size(); ++i) {
		PDNparseTokentonumbers(cbgame.moves[i].PDN, &from, &to);
		if (islegal(b8, color, from, to, &legalmove)) {
			cbgame.moves[i].move = legalmove;
			color = CB_CHANGECOLOR(color);
			domove(legalmove, b8);
		}
	}
}

int builtinislegal(int board8[8][8], int color, int from, int to, struct CBmove *move)
{
	// make all moves and try to find out if this move is legal
	int i, n;
	struct coor c;
	int Lfrom, Lto;
	int isjump;
	CBmove movelist[28];

	/* This color translation does not seem to agree with code in getmovelist (but it also seems to work!) */
	if (color == CB_BLACK)
		n = getmovelist(1, movelist, board8, &isjump);
	else
		n = getmovelist(-1, movelist, board8, &isjump);
	for (i = 0; i < n; i++) {
		c.x = movelist[i].from.x;
		c.y = movelist[i].from.y;
		Lfrom = coortonumber(c, cbgame.gametype);
		c.x = movelist[i].to.x;
		c.y = movelist[i].to.y;
		Lto = coortonumber(c, cbgame.gametype);
		if (Lfrom == from && Lto == to) {

			// we have found a legal move
			// todo: continue to see whether this move is ambiguous!
			*move = movelist[i];
			return 1;
		}
	}

	if (isjump) {
		sprintf(statusbar_txt, "illegal move - you must jump! for multiple jumps, click only from and to square");
	}
	else
		sprintf(statusbar_txt, "%i-%i not a legal move", from, to);
	return 0;
}

void newgame(void)
{
	InitCheckerBoard(cbboard8);
	reset_game(cbgame);
	newposition = TRUE;
	reset_move_history = true;
	cboptions.mirror = is_mirror_gametype(cbgame.gametype);
	cbcolor = get_startcolor(cbgame.gametype);
	updateboardgraphics(hwnd);
	reset_game_clocks();
}

void doload(PDNgame *game, char *gamestring, int *color, int board8[8][8])
{
	// game is in gamestring. use pdnparser routines to convert
	// it into a game
	// read headers
	char *p, *start;
	char header[256], token[1024];
	char headername[256], headervalue[256];
	int i;
	int issetup = 0;
	PDN_PARSE_STATE state;
	gamebody_entry entry;

	// gamestring may terminate in a move, i.e. "1. 11-15 21-17". in this
	// case the tokenizer will not find a space after "11-15 " and not
	// parse the move 21-17. therefore:
	strcat(gamestring, " ");

	reset_game(*game);
	p = gamestring;
	while (PDNparseGetnextheader(&p, header)) {

		/* parse headers */
		start = header;
		PDNparseGetnexttoken(&start, headername);
		PDNparseGetnexttag(&start, headervalue);

		/* make header lowercase, so that 'event' and 'Event' will be recognized */
		for (i = 0; i < (int)strlen(headername); i++)
			headername[i] = (char)tolower(headername[i]);

		if (strcmp(headername, "event") == 0)
			sprintf(game->event, "%s", headervalue);
		if (strcmp(headername, "site") == 0)
			sprintf(game->site, "%s", headervalue);
		if (strcmp(headername, "date") == 0)
			sprintf(game->date, "%s", headervalue);
		if (strcmp(headername, "round") == 0)
			sprintf(game->round, "%s", headervalue);
		if (strcmp(headername, "white") == 0)
			sprintf(game->white, "%s", headervalue);
		if (strcmp(headername, "black") == 0)
			sprintf(game->black, "%s", headervalue);
		if (strcmp(headername, "result") == 0) {
			sprintf(game->resultstring, "%s", headervalue);
			if (strcmp(headervalue, "1-0") == 0)
				game->result = CB_WIN;
			if (strcmp(headervalue, "0-1") == 0)
				game->result = CB_LOSS;
			if (strcmp(headervalue, "1/2-1/2") == 0)
				game->result = CB_DRAW;
			if (strcmp(headervalue, "*") == 0)
				game->result = CB_UNKNOWN;
		}

		if (strcmp(headername, "setup") == 0)
			sprintf(game->setup, "%s", headervalue);
		if (strcmp(headername, "fen") == 0) {
			sprintf(game->FEN, "%s", headervalue);
			sprintf(game->setup, "1");
			issetup = 1;
		}
	}

	/* set defaults */
	*color = get_startcolor(game->gametype);
	cboptions.mirror = is_mirror_gametype(game->gametype);

	InitCheckerBoard(board8);

	/* if its a setup load position */
	if (issetup)
		FENtoboard8(board8, game->FEN, color, game->gametype);

	/* ok, headers read, now parse PDN input:*/
	while ((state = (PDN_PARSE_STATE) PDNparseGetnextPDNtoken(&p, token))) {

		/* check for special tokens*/

		/* move number - discard */
		if (token[strlen(token) - 1] == '.')
			continue;

		/* game terminators */
		if
		(
			(strcmp(token, "*") == 0) ||
			(strcmp(token, "0-1") == 0) ||
			(strcmp(token, "1-0") == 0) ||
			(strcmp(token, "1/2-1/2") == 0)
		) {

			/* In PDN 3.0, the game terminator is '*'. Allow old style game result terminators, 
			 * but don't interpret them as results.
			 */
			break;
		}

		if (token[0] == '{' || state == PDN_FLUFF) {

			/* we found a comment */
			start = token;

			// remove the curly braces by moving pointer one forward, and trimming
			// last character
			if (state != PDN_FLUFF) {
				start++;
				token[strlen(token) - 1] = 0;
			}

			/* This comment is for the previous move. */
			if (game->moves.size() > 0)
				sprintf(game->moves[game->moves.size() - 1].comment, "%s", start);
			continue;
		}

#ifdef NEMESIS
		if (token[0] == '(') {

			/* we found a comment */

			/* write it to last move, because current entry is already the new move */
			start = token;
			start++;
			token[strlen(token) - 1] = 0;
			if (game->moves.size() > 0)
				sprintf(game->moves[game->moves.size() - 1].comment, "%s", start);
			continue;
		}
#endif

		// ok, it was just a move. Save just the move string now, and we will fill in
		// the move details when done reading the pdn.
		sprintf(entry.PDN, "%s", token);
		entry.analysis[0] = 0;
		entry.comment[0] = 0;
		memset(&entry.move, 0, sizeof(entry.move));
		game->moves.push_back(entry);
	}

	// fill in the move information.
	pdntogame(board8, *color);
	reset_move_history = true;
	newposition = TRUE;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

// initializations below:
void InitStatus(HWND hwnd)
{
	hStatusWnd = CreateWindow(STATUSCLASSNAME, "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, NULL, g_hInst, NULL);
}

void InitCheckerBoard(int b[8][8])
{
	// initialize board to starting position
	memset(b, 0, sizeof(cbboard8));
	b[0][0] = CB_BLACK | CB_MAN;
	b[2][0] = CB_BLACK | CB_MAN;
	b[4][0] = CB_BLACK | CB_MAN;
	b[6][0] = CB_BLACK | CB_MAN;
	b[1][1] = CB_BLACK | CB_MAN;
	b[3][1] = CB_BLACK | CB_MAN;
	b[5][1] = CB_BLACK | CB_MAN;
	b[7][1] = CB_BLACK | CB_MAN;
	b[0][2] = CB_BLACK | CB_MAN;
	b[2][2] = CB_BLACK | CB_MAN;
	b[4][2] = CB_BLACK | CB_MAN;
	b[6][2] = CB_BLACK | CB_MAN;

	b[1][7] = CB_WHITE | CB_MAN;
	b[3][7] = CB_WHITE | CB_MAN;
	b[5][7] = CB_WHITE | CB_MAN;
	b[7][7] = CB_WHITE | CB_MAN;
	b[0][6] = CB_WHITE | CB_MAN;
	b[2][6] = CB_WHITE | CB_MAN;
	b[4][6] = CB_WHITE | CB_MAN;
	b[6][6] = CB_WHITE | CB_MAN;
	b[1][5] = CB_WHITE | CB_MAN;
	b[3][5] = CB_WHITE | CB_MAN;
	b[5][5] = CB_WHITE | CB_MAN;
	b[7][5] = CB_WHITE | CB_MAN;
}

/*
 * Load an engine dll, and get pointers to the exported functions in the dll.
 * Return non-zero on error.
 */
int load_engine
	(
		HINSTANCE *lib,
		char *dllname,
		CB_ENGINECOMMAND *cmdfn,
		CB_GETSTRING *namefn,
		CB_GETMOVE *getmovefn,
		CB_ISLEGAL *islegalfn,
		char *pri_or_sec
	)
{
	char buf[256];

	// go to the right directory to load engines
	SetCurrentDirectory(CBdirectory);
	sprintf(buf, "engines\\%s", dllname);
	*lib = LoadLibrary(buf);

	// go back to the working dir
	SetCurrentDirectory(CBdirectory);

	// If the handle is valid, try to get the function addresses
	if (*lib != NULL) {
		*cmdfn = (CB_ENGINECOMMAND) GetProcAddress(*lib, "enginecommand");
		*namefn = (CB_GETSTRING) GetProcAddress(*lib, "enginename");
		*getmovefn = (CB_GETMOVE) GetProcAddress(*lib, "getmove");
		*islegalfn = (CB_ISLEGAL) GetProcAddress(*lib, "islegal");
		if (*islegalfn == NULL)
			*islegalfn = CBislegal;
		return(0);
	}
	else {
		sprintf(buf,
				"CheckerBoard could not find\nthe %s engine dll.\n\nPlease use the \n'Engine->Select..' command\nto select a new %s engine",
			pri_or_sec,
				pri_or_sec);
		MessageBox(hwnd, buf, "Error", MB_OK);
		*cmdfn = NULL;
		*namefn = NULL;
		*getmovefn = NULL;
		*islegalfn = NULL;
		return(1);
	}
}

void loadengines(char *pri_fname, char *sec_fname)
// sets the engines
// this is first called from WinMain on the WM_CREATE command.
// the global strings "primaryenginestring",
// "secondaryenginestring"  contain
// the filenames of these engines.
// TODO: need to unload engines properly, first unload, then load
// new engines. for this, however, CB needs to know which engines
// are loaded right now.
{
	int status;
	HMODULE primaryhandle, secondaryhandle;
	char Lstr[256];

	// set built in functions
	CBgametype = (CB_GETGAMETYPE) builtingametype;
	CBislegal = (CB_ISLEGAL) builtinislegal;

	// load engine dlls
	// first, primary engine
	// is there a way to check whether a module is already loaded?
	primaryhandle = GetModuleHandle(pri_fname);
	sprintf(Lstr, "handle = %i (primary engine)", PtrToLong(primaryhandle));
	CBlog(Lstr);
	secondaryhandle = GetModuleHandle(sec_fname);
	sprintf(Lstr, "secondaryhandle = %i (secondary engine)", PtrToLong(secondaryhandle));
	CBlog(Lstr);

	// now, if one of the two handles, primaryhandle or secondaryhandle is
	// != 0, then that engine is already loaded and doesn't need to be loaded
	// again.
	// however, if these handles are 0, then we have to unload the engine
	// that is currently loaded. or, put differently, if one of the
	// handles oldprimary/oldsecondary is not equal to one of the new
	// handles, then it has to be unloaded.
	// free up engine modules that are no longer used!
	// in fact, we should do this first, before loading the new engines!!

	/*
	 * If there was a primary engine loaded, and it is different from the new primary and secondary
	 * engine handles, then unload it.
	 */
	if (hinstLib1) {
		if (hinstLib1 != primaryhandle && hinstLib1 != secondaryhandle) {
			status = FreeLibrary(hinstLib1);
			hinstLib1 = 0;
			enginecommand1 = NULL;
			enginename1 = NULL;
			getmove1 = NULL;
			islegal1 = NULL;
		}
	}

	/* If there was a secondary engine loaded, and it is different from the new primary and secondary
	 * engine handles, then unload it.
	 */
	if (hinstLib2) {
		if (hinstLib2 != primaryhandle && hinstLib2 != secondaryhandle) {
			status = FreeLibrary(hinstLib2);
			hinstLib2 = 0;
			enginecommand2 = NULL;
			enginename2 = NULL;
			getmove2 = NULL;
			islegal2 = NULL;
		}
	}

	/* Load a new primary engine if there isn't one already loaded, or
	 * if the requested new engine filename is different from the one presently loaded (this happens
	 * if the presently loaded primary engine handle is same as the secondary engine handle).
	 */
	if (!hinstLib1 || strcmp(pri_fname, cboptions.primaryenginestring)) {
		status = load_engine(&hinstLib1, pri_fname, &enginecommand1, &enginename1, &getmove1, &islegal1, "primary");
		if (!status)
			strcpy(cboptions.primaryenginestring, pri_fname);	/* Success. */
		else {
			if (strcmp(pri_fname, cboptions.primaryenginestring)) {
				status = load_engine(&hinstLib1,
									 cboptions.primaryenginestring,
									 &enginecommand1,
									 &enginename1,
									 &getmove1,
									 &islegal1,
									 "primary");
				if (status)
					cboptions.primaryenginestring[0] = 0;
			}
		}
	}

	if (!hinstLib2 || strcmp(sec_fname, cboptions.secondaryenginestring)) {
		status = load_engine(&hinstLib2, sec_fname, &enginecommand2, &enginename2, &getmove2, &islegal2, "secondary");
		if (!status)
			strcpy(cboptions.secondaryenginestring, sec_fname); /* Success. */
		else {
			if (strcmp(sec_fname, cboptions.secondaryenginestring)) {
				status = load_engine(&hinstLib2,
									 cboptions.secondaryenginestring,
									 &enginecommand2,
									 &enginename2,
									 &getmove2,
									 &islegal2,
									 "secondary");
				if (status)
					cboptions.secondaryenginestring[0] = 0;
			}
		}
	}

	// set current engine
	setcurrentengine(1);

	// reset game if an engine of different game type was selected!
	if (gametype() != cbgame.gametype) {
		PostMessage(hwnd, (UINT) WM_COMMAND, (WPARAM) GAMENEW, (LPARAM) 0);
		PostMessage(hwnd, (UINT) WM_SIZE, (WPARAM) 0, (LPARAM) 0);
	}

	// reset the directory to the CB directory
	SetCurrentDirectory(CBdirectory);
}

void initengines(void)
{
	loadengines(cboptions.primaryenginestring, cboptions.secondaryenginestring);
}

// CreateAToolBar creates a toolbar and adds a set of buttons to it.
// The function returns the handle to the toolbar if successful,
// or NULL otherwise.

// hwndParent is the handle to the toolbar's parent window.
HWND CreateAToolBar(HWND hwndParent)
{
	HWND hwndTB;
	TBADDBITMAP tbab;
	INITCOMMONCONTROLSEX icex;
	int i;
	int id[NUMBUTTONS] =
	{
		15,
		0,
		NUMBUTTONS + STD_FILENEW,
		NUMBUTTONS + STD_FILESAVE,
		NUMBUTTONS + STD_FILEOPEN,
		NUMBUTTONS + STD_FIND,
		0,
		7,
		NUMBUTTONS + STD_UNDO,
		NUMBUTTONS + STD_REDOW,
		8,
		0,
		2,
		3,
		0,
		0,
		10,
		11,
		17,
		0,
		12,
		13,
		14
	};
	int command[NUMBUTTONS] =
	{
		HELPHOMEPAGE,
		0,
		GAMENEW,
		GAMESAVE,
		GAMELOAD,
		GAMEFIND,
		0,
		MOVESBACKALL,
		MOVESBACK,
		MOVESFORWARD,
		MOVESFORWARDALL,
		0,
		MOVESPLAY,
		TOGGLEBOOK,
		TOGGLEENGINE,
		0,
		SETUPCC,
		DISPLAYINVERT,
		TOGGLEMODE,
		0,
		BOOKMODE_VIEW,
		BOOKMODE_ADD,
		BOOKMODE_DELETE
	};
	int style[NUMBUTTONS] =
	{
		TBSTYLE_BUTTON,
		TBSTYLE_SEP,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_SEP,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_SEP,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_SEP,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_SEP,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON,
		TBSTYLE_BUTTON
	};

	// Ensure that the common control DLL is loaded.
	icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
	icex.dwICC = ICC_BAR_CLASSES;

	InitCommonControlsEx(&icex);

	// Create a toolbar.
	hwndTB = CreateWindowEx(0,
							TOOLBARCLASSNAME,
							(LPSTR) NULL,
							WS_CHILD | WS_BORDER | WS_VISIBLE | TBSTYLE_TOOLTIPS | CCS_ADJUSTABLE | TBSTYLE_FLAT,
							0,
							0,
							0,
							0,
							hwndParent,
							(HMENU) ID_TOOLBAR,
							g_hInst,
							NULL);

	// Send the TB_BUTTONSTRUCTSIZE message, which is required for backward compatibility.
	SendMessage(hwndTB, TB_BUTTONSTRUCTSIZE, (WPARAM) sizeof(TBBUTTON), 0);

	// Fill the TBBUTTON array with button information, and add the
	// buttons to the toolbar. The buttons on this toolbar have text
	// but do not have bitmap images.
	for (i = 0; i < NUMBUTTONS; i++) {
		tbButtons[i].dwData = 0L;
		tbButtons[i].fsState = TBSTATE_ENABLED;
		tbButtons[i].fsStyle = (BYTE) style[i];
		tbButtons[i].iBitmap = id[i];
		tbButtons[i].idCommand = command[i];
		tbButtons[i].iString = 0;	//"text";
	}

	// here's how toolbars work:
	// first, add bitmaps to the toolbar. the toolbar keeps a list of bitmaps.
	// then, add buttons to the toolbar, specifying the index of the bitmap you want to use
	// in this case, i first add custom bitmaps, NUMBUTTONS of them,
	// then i add the standard windows bitmaps.
	// so instead of using the index STD_FIND for the bitmap to find things,
	// i need to add NUMBUTTONS to that so that it works out!
	// add custom bitmaps
	tbab.hInst = g_hInst;
	tbab.nID = IDTB_BMP;
	SendMessage(hwndTB, TB_ADDBITMAP, NUMBUTTONS, (LPARAM) & tbab);

	// add default bitmaps
	tbab.hInst = HINST_COMMCTRL;
	tbab.nID = IDB_STD_SMALL_COLOR;

	//tbab.nID = IDB_VIEW_LARGE_COLOR;
	SendMessage(hwndTB, TB_ADDBITMAP, 0, (LPARAM) & tbab);

	// add buttons
	SendMessage(hwndTB, TB_ADDBUTTONS, (WPARAM) NUMBUTTONS, (LPARAM) (LPTBBUTTON) & tbButtons);

	// finally, resize
	SendMessage(hwndTB, TB_AUTOSIZE, 0, 0);

	ShowWindow(hwndTB, SW_SHOW);
	return hwndTB;
}

// synchronization functions below
int getenginebusy(void)
{
	int returnvalue;
	EnterCriticalSection(&engine_criticalsection);
	returnvalue = enginebusy;
	LeaveCriticalSection(&engine_criticalsection);
	return returnvalue;
}

/*
 * Tell engine to abort searching immediately.
 * Wait a maximum of 1 second for the engine to finish aborting.
 */
void abortengine()
{
	clock_t t0;

	if (!getenginebusy())
		return;

	abortcalculation = 1;
	playnow = 1;

	t0 = clock();
	while (getenginebusy()) {
		Sleep(10);
		if ((int)(clock() - t0) > 1000)
			break;
	}
}

int getanimationbusy(void)
{
	int returnvalue;
	EnterCriticalSection(&ani_criticalsection);
	returnvalue = animationbusy;
	LeaveCriticalSection(&ani_criticalsection);
	return returnvalue;
}

int getenginestarting(void)
{
	int returnvalue;
	returnvalue = enginestarting;
	return returnvalue;
}

int setenginebusy(int value)
{
	EnterCriticalSection(&engine_criticalsection);
	enginebusy = value;
	LeaveCriticalSection(&engine_criticalsection);
	return 1;
}

int setanimationbusy(int value)
{
	EnterCriticalSection(&ani_criticalsection);
	animationbusy = value;
	LeaveCriticalSection(&ani_criticalsection);
	return 1;
}

int setenginestarting(int value)
{
	enginestarting = value;
	return 1;
}
