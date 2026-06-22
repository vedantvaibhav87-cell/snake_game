/*
 * Snake Game — Console Edition  v2
 * Compile : g++ snake.cpp -o snake
 * Run     : ./snake
 * Controls: W A S D  or  Arrow keys
 * Keys    : P = pause   Q = quit
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>

/* ─── platform ─────────────────────────────────────────────── */
#ifdef _WIN32
  #include <windows.h>
  #include <conio.h>
  #define SLEEP_MS(ms) Sleep(ms)

  /* MinGW older SDKs may not define this — declare it manually */
  #ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
    #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
  #endif

  inline void set_raw() {
      HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
      DWORD m = 0; GetConsoleMode(h, &m);
      SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
  inline void restore_raw() {}
  inline int  nb_getch() { return _kbhit() ? _getch() : 0; }
#else
  #include <unistd.h>
  #include <termios.h>
  #include <fcntl.h>
  #define SLEEP_MS(ms) usleep((ms)*1000)
  static struct termios _orig;
  inline void set_raw() {
      tcgetattr(STDIN_FILENO, &_orig);
      struct termios r = _orig;
      r.c_lflag &= ~(ICANON|ECHO);
      r.c_cc[VMIN]=0; r.c_cc[VTIME]=0;
      tcsetattr(STDIN_FILENO, TCSANOW, &r);
      int f = fcntl(STDIN_FILENO, F_GETFL, 0);
      fcntl(STDIN_FILENO, F_SETFL, f | O_NONBLOCK);
  }
  inline void restore_raw() {
      tcsetattr(STDIN_FILENO, TCSANOW, &_orig);
      int f = fcntl(STDIN_FILENO, F_GETFL, 0);
      fcntl(STDIN_FILENO, F_SETFL, f & ~O_NONBLOCK);
  }
  inline int nb_getch() {
      unsigned char c; return (read(STDIN_FILENO,&c,1)==1)?(int)c:0;
  }
#endif

/* ─── ANSI helpers ─────────────────────────────────────────── */
#define RST   "\033[0m"
#define BOLD  "\033[1m"
#define HC    "\033[?25l"   /* hide cursor */
#define SC    "\033[?25h"   /* show cursor */

static void go(int r,int c){ printf("\033[%d;%df",r,c); }
static void clreol()        { printf("\033[K"); }

/* colours (256-colour) */
#define cBORDER  "\033[38;5;28m"    /* dark green    */
#define cHEAD    "\033[38;5;46m\033[48;5;22m"  /* bright green on dark */
#define cBODY    "\033[38;5;40m\033[48;5;22m"
#define cFOOD    "\033[38;5;196m"   /* red           */
#define cSCORE   "\033[38;5;226m"   /* yellow        */
#define cLABEL   "\033[38;5;244m"   /* grey label    */
#define cLVL     "\033[38;5;51m"    /* cyan level    */
#define cTITLE   "\033[38;5;46m"    /* green title   */
#define cDEAD    "\033[38;5;196m"   /* red dead      */
#define cMSG     "\033[38;5;255m"   /* white msg     */
#define cDIM     "\033[38;5;240m"   /* dim hint      */
#define cBG      "\033[48;5;16m"    /* near-black bg */

/* ─── layout ────────────────────────────────────────────────
   Screen looks like:

   Row 1 :  ╔══ SNAKE ══╗            <- top border
   Row 2 :  ║           ║            <- board rows  (BOARD_H)
   ...
   Row 21:  ╚═══════════╝            <- bottom border
   Row 22:  SCORE bar
   Row 23:  hint line
   ──────────────────────────────────────────────────────── */
#define BOARD_W   36
#define BOARD_H   18
#define CELL_W     2          /* each cell = 2 chars wide     */

/* where the top-left of the board sits on screen             */
#define BR        2           /* board top row  (1-based)     */
#define BC        3           /* board left col (1-based)     */

/* derived positions */
#define TOP_ROW   (BR-1)                  /* border top row   */
#define BOT_ROW   (BR+BOARD_H)            /* border bot row   */
#define SCORE_ROW (BR+BOARD_H+1)          /* score bar row    */
#define HINT_ROW  (BR+BOARD_H+2)          /* hint line row    */
#define INNER_W   (BOARD_W*CELL_W)        /* inner pixel width*/
#define LEFT_COL  (BC-1)                  /* border left col  */
#define RIGHT_COL (BC+INNER_W)            /* border right col */

enum Dir { UP, DOWN, LEFT, RIGHT };
struct Pt { int r,c; };

/* ─── state ─────────────────────────────────────────────── */
static std::deque<Pt> snake;
static Dir   dir=RIGHT, ndir=RIGHT;
static Pt    food;
static bool  board[BOARD_H][BOARD_W];
static int   score=0, best=0, level=1, spd=140;
static bool  paused=false, dead=false;

/* ─── draw primitives ───────────────────────────────────── */
static void cell(int r,int c,const char*col,const char*ch){
    go(BR+r, BC+c*CELL_W);
    printf("%s%s%s%s",cBG,col,ch,RST);
}
static void blank_cell(int r,int c){
    go(BR+r, BC+c*CELL_W);
    printf("%s  %s",cBG,RST);
}

static const char* head_ch(){
    if(dir==UP)    return "▲ ";
    if(dir==DOWN)  return "▼ ";
    if(dir==LEFT)  return "◀ ";
    return "▶ ";
}

/* ─── score bar ─────────────────────────────────────────────
   Drawn on its OWN row, never inside the border.
   Fixed-width fields so numbers never shift the layout.
   ────────────────────────────────────────────────────────── */
static void draw_score_bar(){
    go(SCORE_ROW, LEFT_COL);
    clreol();

    /* total width of bar = INNER_W + 2  (matches border) */

    /* build the bar string into a buffer for precise centering */
    char buf[256];
    snprintf(buf, sizeof buf,
        "%s SCORE %s%5d  %s BEST %s%5d  %s LEVEL %s%3d ",
        cLABEL, cSCORE, score,
        cLABEL, cSCORE, best,
        cLABEL, cLVL,   level);

    /* print left-padded to centre inside bar width */
    /* We just print it flush at LEFT_COL; it fits */
    printf("%s%s%s", cBG, buf, RST);
}

/* ─── border ────────────────────────────────────────────── */
static void draw_border(){
    printf(cBORDER BOLD);

    /* top */
    go(TOP_ROW, LEFT_COL);
    printf("╔");
    for(int i=0;i<INNER_W;i++) printf("═");
    printf("╗");

    /* sides */
    for(int r=0;r<BOARD_H;r++){
        go(BR+r, LEFT_COL);   printf("║");
        go(BR+r, RIGHT_COL);  printf("║");
    }

    /* bottom */
    go(BOT_ROW, LEFT_COL);
    printf("╚");
    for(int i=0;i<INNER_W;i++) printf("═");
    printf("╝");

    /* centred title inside top border */
    const char* title = " SNAKE ";
    int tlen = 7;
    int tpos = LEFT_COL + 1 + (INNER_W - tlen)/2;
    go(TOP_ROW, tpos);
    printf("%s%s%s%s", cTITLE, BOLD, title, RST);

    printf(RST);
}

static void draw_hint(){
    go(HINT_ROW, LEFT_COL);
    printf("%s  W/A/S/D or Arrows  |  P pause  |  Q quit%s", cDIM, RST);
    clreol();
}

/* ─── food ──────────────────────────────────────────────── */
static void draw_food(){
    cell(food.r, food.c, cFOOD, "● ");
}

/* ─── place food ────────────────────────────────────────── */
static void place_food(){
    Pt p;
    do { p.r=rand()%BOARD_H; p.c=rand()%BOARD_W; }
    while(board[p.r][p.c]);
    food=p;
}

/* ─── full redraw ───────────────────────────────────────── */
static void full_redraw(){
    /* clear whole screen with bg colour */
    printf("\033[2J");
    for(int r=1; r<=HINT_ROW+1; r++){
        go(r,1); printf("%s",cBG);
        for(int c=0;c<RIGHT_COL+4;c++) printf(" ");
        printf(RST);
    }
    draw_border();
    for(auto& p:snake) cell(p.r,p.c,cBODY,"■ ");
    cell(snake.front().r,snake.front().c,cHEAD,head_ch());
    draw_food();
    draw_score_bar();
    draw_hint();
    fflush(stdout);
}

/* ─── init ──────────────────────────────────────────────── */
static void init(){
    snake.clear();
    memset(board,0,sizeof board);
    dir=ndir=RIGHT;
    score=0; level=1; spd=140;
    dead=paused=false;

    int sr=BOARD_H/2;
    for(int i=3;i>=0;i--){
        Pt p={sr, BOARD_W/2-i};
        snake.push_back(p);
        board[p.r][p.c]=true;
    }
    place_food();
}

/* ─── input ─────────────────────────────────────────────── */
static void handle_input(){
    int ch;
#ifdef _WIN32
    while(_kbhit()){
        ch=_getch();
        if(ch==0||ch==224){
            int c2=_getch();
            if(c2==72&&dir!=DOWN)  ndir=UP;
            if(c2==80&&dir!=UP)    ndir=DOWN;
            if(c2==75&&dir!=RIGHT) ndir=LEFT;
            if(c2==77&&dir!=LEFT)  ndir=RIGHT;
            continue;
        }
#else
    while((ch=nb_getch())!=0){
        if(ch==27){
            int c2=nb_getch();
            if(c2=='['){
                int c3=nb_getch();
                if(c3=='A'&&dir!=DOWN)  ndir=UP;
                if(c3=='B'&&dir!=UP)    ndir=DOWN;
                if(c3=='D'&&dir!=RIGHT) ndir=LEFT;
                if(c3=='C'&&dir!=LEFT)  ndir=RIGHT;
            }
            continue;
        }
#endif
        switch(ch|32){
            case 'w': if(dir!=DOWN)  ndir=UP;    break;
            case 's': if(dir!=UP)    ndir=DOWN;  break;
            case 'a': if(dir!=RIGHT) ndir=LEFT;  break;
            case 'd': if(dir!=LEFT)  ndir=RIGHT; break;
            case 'p':
                paused=!paused;
                if(!paused) full_redraw();
                break;
            case 'q': dead=true; break;
        }
    }
}

/* ─── step ──────────────────────────────────────────────── */
static bool step(){
    dir=ndir;
    Pt h=snake.front();
    if(dir==UP)    h.r--;
    if(dir==DOWN)  h.r++;
    if(dir==LEFT)  h.c--;
    if(dir==RIGHT) h.c++;

    if(h.r<0||h.r>=BOARD_H||h.c<0||h.c>=BOARD_W) return false;
    if(board[h.r][h.c]) return false;

    bool ate=(h.r==food.r&&h.c==food.c);
    snake.push_front(h);
    board[h.r][h.c]=true;

    /* redraw old head as body, draw new head */
    if(snake.size()>1) cell(snake[1].r,snake[1].c,cBODY,"■ ");
    cell(h.r,h.c,cHEAD,head_ch());

    if(ate){
        score += 10*level;
        if(score>best) best=score;
        level = score/50 + 1;
        spd   = 140 - (level-1)*12;
        if(spd<50) spd=50;
        place_food();
        draw_food();
    } else {
        Pt t=snake.back();
        board[t.r][t.c]=false;
        snake.pop_back();
        blank_cell(t.r,t.c);
    }

    draw_score_bar();
    fflush(stdout);
    return true;
}

/* ─── pause overlay ─────────────────────────────────────── */
static void show_pause(){
    int mr = BR + BOARD_H/2 - 1;
    int mc = LEFT_COL + (INNER_W+2)/2 - 11;
    go(mr,   mc); printf("%s┌───────────────────┐%s",cMSG,RST);
    go(mr+1, mc); printf("%s│      PAUSED       │%s",cMSG,RST);
    go(mr+2, mc); printf("%s│  Press P to resume│%s",cMSG,RST);
    go(mr+3, mc); printf("%s└───────────────────┘%s",cMSG,RST);
    fflush(stdout);
}

/* ─── game-over screen ──────────────────────────────────── */
static bool show_game_over(){
    int mr = BR + BOARD_H/2 - 4;
    int mc = LEFT_COL + (INNER_W+2)/2 - 12;
    go(mr,   mc); printf("%s╔═════════════════════╗%s",cDEAD,RST);
    go(mr+1, mc); printf("%s║     GAME  OVER      ║%s",cDEAD,RST);
    go(mr+2, mc); printf("%s╠═════════════════════╣%s",cDEAD,RST);
    go(mr+3, mc); printf("%s║%s  Score  : %s%6d   %s║%s",cDEAD,cLABEL,cSCORE,score,cDEAD,RST);
    go(mr+4, mc); printf("%s║%s  Best   : %s%6d   %s║%s",cDEAD,cLABEL,cSCORE,best, cDEAD,RST);
    go(mr+5, mc); printf("%s║%s  Level  : %s%6d   %s║%s",cDEAD,cLABEL,cLVL,  level,cDEAD,RST);
    go(mr+6, mc); printf("%s╠═════════════════════╣%s",cDEAD,RST);
    go(mr+7, mc); printf("%s║  R = replay  Q = quit║%s",cMSG,RST);
    go(mr+8, mc); printf("%s╚═════════════════════╝%s",cDEAD,RST);
    fflush(stdout);

    /* wait for R or Q (blocking) */
#ifdef _WIN32
    for(;;){ if(!_kbhit()){Sleep(50);continue;} int c=_getch()|32; if(c=='r')return true; if(c=='q')return false; }
#else
    struct termios blk=_orig;
    blk.c_lflag&=~(ICANON|ECHO);
    blk.c_cc[VMIN]=1; blk.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO,TCSANOW,&blk);
    int fl=fcntl(STDIN_FILENO,F_GETFL,0);
    fcntl(STDIN_FILENO,F_SETFL,fl&~O_NONBLOCK);
    for(;;){
        unsigned char c=0;
        read(STDIN_FILENO,&c,1);
        if((c|32)=='r'){ set_raw(); return true;  }
        if((c|32)=='q'){ set_raw(); return false; }
    }
#endif
}

/* ─── main ──────────────────────────────────────────────── */
int main(){
    srand((unsigned)time(NULL));
    set_raw();
#ifndef _WIN32
    atexit(restore_raw);
#endif
    printf(HC);

    for(;;){
        init();
        full_redraw();

        while(!dead){
            handle_input();
            if(paused){ show_pause(); SLEEP_MS(100); continue; }
            if(!step()){ dead=true; break; }
            SLEEP_MS(spd);
        }

        if(!show_game_over()) break;
    }

    printf(SC RST "\033[2J");
    go(1,1);
    printf("%sThanks for playing Snake!%s\n",cTITLE,RST);
    return 0;
}
