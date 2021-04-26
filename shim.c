#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SHIM_VERSION "0.0.1"
#define SHIM_TAB_STOP 8 // tabulation length

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  ARROW_LEFT  = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
};

typedef struct editor_row {
  int size;
  int rsize;
  char* chars;
  char* render; // actual characters to drawn on the screen for that row of text
} E_ROW;

struct editorConfig {
  int curr_x, curr_y; // cursor's position coordinates within the file
  int render_x;       // index into the row render field
  int rowoff;         // row offset for scrolling control
  int coloff;         // column offset for scrolling control
  int screenrows;     // screen height
  int screencols;     // screen width
  int numrows;        // number of rows in the source file
  E_ROW* row;
  char* filename;
  char statusmsg[80];
  time_t statusmsg_time; // timestamp when set the status message
  struct termios orig_termios;
};

struct editorConfig E;

// append buffer, a dynamic string for screen description
typedef struct abuf {
  char* b;
  int len;
} A_BUF;

#define A_BUF_INIT {NULL, 0} 

void abAppend(A_BUF* ab, const char* s, int len) {
  char* new = realloc(ab->b, ab->len + len);
  if(!new) return;

  // "append" s to the string buffer 
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(A_BUF* ab) {
  free(ab->b);
}

void die(const char* s) {
  // perror looks at global errno variable that contains the current error code
  perror(s); // prints 's' and also a descriptive error message for this error code
  exit(1); // exit with failure
}

void disableRawMode() {
  // clear the screen and reposition the cursor
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[1;1H", 6);

  // restore terminal flags
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1){
    die("tcsetattr");
  }
}

void enableRawMode() {
  if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;

  // disabling BRKINT disables break conditions to send a SIGINT signal
  // disabling ICRNL disables any translation of carriage returns to newlines
  // disabling INPCK disables parity checker, that doesn't seem to apply to modern terminal emulators
  // disabling ISTRIP disables the stripping of the 8th bit of each input byte to 0  
  // disabling IXON disables software flow control with XOFF(Ctrl-S) and XON(Ctrl-Q)
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // input flags

  // disabling OPOST disables the "\n" to "\r\n" translation
  raw.c_oflag &= ~(OPOST); // output flags

  // CS8 is a bit mask that sets the character size to 8 bits per byte 
  raw.c_cflag |= (CS8); // control flags

  // disabling ECHO disables terminal echoing 
  // disabling ICANON disables CANONICAL mode, 
  // disabling IEXTEN disables Ctrl-V and Ctrl-O, 
  // disabling ISIG disables SIGINT(Ctrl-C) and SIGTSTP(Ctrl-Z) signaling
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // local flags

  // control characters flags
  raw.c_cc[VMIN] = 0; // sets minimum number of bytes of input needed before read() can return
  raw.c_cc[VTIME] = 1; // sets the maximum ammount of time to wait before read() returns, in a tenths of a second

  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;

  // wait until a keypress occurr
  while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if(nread == -1 && errno != EAGAIN) die("read");
  }
  
  if(c == '\x1b') {
    // parse escape sequence
    char seq[3];
    
    if(read(STDIN_FILENO, &seq[0], 1) != 1) return c;
    if(read(STDIN_FILENO, &seq[1], 1) != 1) return c;

    if(seq[0] == '[') { 
      if(seq[1] >= '0' && seq[1] <= '9') {
        if(read(STDIN_FILENO, &seq[2], 1) != 1) return c;
        if(seq[2] == '~') {
          switch(seq[1]) {
            case '1' : return HOME_KEY;
            case '3' : return DEL_KEY;
            case '4' : return END_KEY;
            case '5' : return PAGE_UP;
            case '6' : return PAGE_DOWN;
            case '7' : return HOME_KEY;
            case '8' : return END_KEY;
          }
        }
      }
      else {
        switch(seq[1]) {
          case 'A' : return ARROW_UP;
          case 'B' : return ARROW_DOWN;
          case 'C' : return ARROW_RIGHT;
          case 'D' : return ARROW_LEFT;
          case 'H' : return HOME_KEY;
          case 'F' : return END_KEY;
        }
      }
    } else if(seq[0] == 'O') {
      switch(seq[1]) {
        case 'H' : return HOME_KEY;
        case 'F' : return END_KEY;
      }
    }
  }
  return c;
}

int getCursorPosition(int* rows, int *cols) {
  char buffer[32];
  unsigned int i = 0;

  // use escape sequence of Device Status Report(n) to ask for the cursor position(6)
  if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
  // parse Cursor Position Report from input
  // a valid reply is in the form of '\x1b[24;80R', where 24 and 80 are screen height and width
  while (i < sizeof(buffer) - 1) {
    if(read(STDIN_FILENO, &buffer[i], 1) != 1) break;
    if (buffer[i] == 'R') break;
    i++;
  }
  buffer[i] = '\0';

  // invalid report
  if(buffer[0] != '\x1b' || buffer[1] != '[') return -1; 
  // read screen height and width from buffer
  if(sscanf(&buffer[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int getWindowSize(int* rows, int *cols) {
  struct winsize ws;

  // call ioctl with Terminal Input/Output Control Get WINdow SiZe request
  if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // failed to get window size, try by the hard way
    // use scape sequence to move cursor to the right (C) and down (B)
    // C and B commands stop the cursor from going past the edge of the screen
    // use 999 to ensures that it will reach bottom-right edge of the screen 
    if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  } 
}

int editorRowCxtoRx(E_ROW* row, int cx) {
  int j, rx = 0;
  // loop through all the characters to the left of cx
  // to figure out how many spaces each TAB takes up
  for(j = 0; j < cx; j++) {
    if(row->chars[j] == '\t'){
      // get rx just to the left of the next tab stop
      rx += (SHIM_TAB_STOP - 1) - (rx % SHIM_TAB_STOP);	  
    }
    rx++;
  }
  return rx;
}

void editorUpdateRow(E_ROW* row) {
  int j, tabs = 0;
  for(j = 0; j < row->size; j++){
    if(row->chars[j] == '\t') tabs++;
  }
  free(row->render);
  // allocate memory for render
  // the maximum number of characters needed for each tab is 8
  // row->size already counts 1 character for each tab 
  // so multiply tabs by 7 and add that to row->size
  row->render = malloc(row->size + tabs*(SHIM_TAB_STOP - 1) + 1);
  
  int idx = 0;
  for(j = 0; j < row->size; j++){
    if(row->chars[j] == '\t'){
      // render tabulation character as spaces
      row->render[idx++] = ' ';
      while(idx % SHIM_TAB_STOP != 0) row->render[idx++] = ' ';
    }
    else row->render[idx++] = row->chars[j];
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(char* s, size_t len) {

  E.row = realloc(E.row, sizeof(E_ROW) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
}

void editorOpen(const char* filename) {
  free(E.filename);
  E.filename = strdup(filename); // makes a copy of the string

  FILE* fp = fopen(filename, "r");
  if(!fp) die("fopen");
  
  char* line = NULL;
  size_t linecapacity = 0;
  int linelen;

  // getline allocates new memory for the next line it reads
  // it sets line to point to the memory allocated 
  // and sets linecapacity to how much memory it allocated
  while((linelen = getline(&line, &linecapacity, fp)) != -1) { // isn't EOF
    // strip off the newline or carriage return at the end of the line
    while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
      linelen--;
    }
    editorAppendRow(line, linelen);
  } 
  free(line);
  fclose(fp);
}

void editorMoveCursor(int key) {
  // check if the cursor is on an actual line of the source file or not
  // if it is, point row to the editor row (E_ROW structure) that the cursor is on
  E_ROW* row = (E.curr_y >= E.numrows) ? NULL : &E.row[E.curr_y];

  switch(key) {
    case ARROW_LEFT :
      if(E.curr_x != 0) E.curr_x--; // move the cursor left 
      else if(E.curr_y > 0) { // curr_x == 0 and isn't the first line
        // pressed <- (ARROW_LEFT) at the beginning of the line
        // move cursor to the end of the previous line
        E.curr_y--; E.curr_x = E.row[E.curr_y].size;
      }
      break;
    case ARROW_RIGHT :
      // curr_x can point to at most one character past the end of the line
      if(row && E.curr_x < row->size) E.curr_x++; // move the cursor right
      else if(row && E.curr_x == row->size){
        // pressed -> (ARROW_RIGHT) at the end of the line
        // move cursor to the beginning of the next line
        E.curr_y++; E.curr_x = 0;
      }
      break;
    case ARROW_UP :
      if(E.curr_y != 0) E.curr_y--; // move the cursor up
      break;
    case ARROW_DOWN :
      if(E.curr_y < E.numrows) E.curr_y++; // move the cursor down
      break;
  }
  row = (E.curr_y >= E.numrows) ? NULL : &E.row[E.curr_y];
  int rowlen = row ? row->size : 0;
  // set curr_x to the end of the line if it is to the right of the end of that line
  if(E.curr_x > rowlen) E.curr_x = rowlen;
}

void editorProcessKeypress() {
  int c = editorReadKey();

  // handle a keypress
  switch(c) {
    case CTRL_KEY('q'):
      // clear the screen and reposition the cursor
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[1;1H", 6);
      exit(0); // exit without error
      break;

    case HOME_KEY : 
      E.curr_x = 0;
      break;

    case END_KEY :
      if(E.curr_y < E.numrows) E.curr_x = E.row[E.curr_y].size;
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if(c == PAGE_UP){
          E.curr_y = E.rowoff;
        } else if(c == PAGE_DOWN){
          E.curr_y = E.rowoff + E.screenrows - 1;
          if(E.curr_y > E.numrows) E.curr_y = E.numrows;
        }
        int times = E.screenrows;
        while(times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;
  }
}

void editorScroll() {
  
  E.render_x = 0;
  if(E.curr_y < E.numrows) {
    E.render_x = editorRowCxtoRx(&E.row[E.curr_y], E.curr_x);  
  }
  // checks if the cursor is above the visible window
  if(E.curr_y < E.rowoff) {
    // scrolls up to where the cursor is
    E.rowoff = E.curr_y;
  }
  // checks if the cursor is past the bottom of the visible window
  if(E.curr_y >= E.rowoff + E.screenrows) {
    E.rowoff = E.curr_y - E.screenrows + 1;
  }
  // checks if the cursor is to the left of the visible window
  if(E.render_x < E.coloff) {
    E.coloff = E.render_x;
  }
  // checks if the cursor is past the right of the visible window
  if(E.render_x >= E.coloff + E.screencols) {
    E.coloff = E.render_x - E.screencols + 1;
  }
}

void editorDrawRows(A_BUF* ab) {
  int r;
  for(r = 0; r < E.screenrows; r++){
    int filerow = r + E.rowoff; // absolute file position
    
    if(filerow >= E.numrows) {
      if(E.numrows == 0 && r == E.screenrows / 3) {

        char welcome[80];
        int msg_len = snprintf(welcome, sizeof(welcome), "Shim editor -- version %s", SHIM_VERSION);
        if(msg_len > E.screencols) msg_len = E.screencols;
        int padding = (E.screencols - msg_len) / 2;
        if(padding != 0) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while(padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, msg_len);
      }
      else {
        abAppend(ab, "~", 1);
      }
    } else {
      // drawing a row that is part of the text buffer
      // subtract the number of characters that are to the left of the offset
      int len = E.row[filerow].rsize - E.coloff;
      if(len < 0) len = 0; // scrolled horizontally past the end of the line
      if(len > E.screencols) len = E.screencols; // truncate the line

      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }
    // escape sequence to clear one line(K) at a time
    abAppend(ab, "\x1b[0K", 4);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(A_BUF* ab){

  abAppend(ab, "\x1b[7m", 4); // reverse terminal colors to black text on a white background
  
  char status[80], row_status[80];
 
  int len = snprintf(status, sizeof(status), "%.20s - %d lines", 
    E.filename ? E.filename : "[No Name]", E.numrows);  
  if(len > E.screencols) len = E.screencols;

  abAppend(ab, status, len);  

  int rlen = snprintf(row_status, sizeof(row_status), "%d/%d",
    E.curr_y + 1, E.numrows);

  while(len < E.screencols) {
    if(E.screencols - len == rlen) {
      abAppend(ab, row_status, rlen); break;
    }
    else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[0m", 4); // switch back to normal formatting
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(A_BUF* ab){
  abAppend(ab, "\x1b[K", 3); // clear the line
  
  int msglen = strlen(E.statusmsg);
  if(msglen > E.screencols) msglen = E.screencols;
  if(msglen && time(NULL) - E.statusmsg_time < 5) 
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
  editorScroll();
 
  A_BUF ab = A_BUF_INIT;

  // escape sequence to hide/reset(l) the cursor before refreshing the screen 
  abAppend(&ab, "\x1b[?25l", 6);
  // escape sequence to reposition(H) the cursor at top-left corner(1,1)
  abAppend(&ab, "\x1b[1;1H", 6);
  
  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  // escape sequence to reposition the cursor
  char buffer[32];
  // point curr_x and curr_y back to its position on the screen
  // update the cursor position
  snprintf(buffer, sizeof(buffer), "\x1b[%d;%dH", (E.curr_y - E.rowoff) + 1, 
                                                  (E.render_x - E.coloff) + 1);
  abAppend(&ab, buffer, strlen(buffer));  

  // escape sequence to show/set(h) the cursor after refreshing the screen 
  abAppend(&ab, "\x1b[?25h", 6);  

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char* fmt, ...){
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

void initEditor() {
  E.curr_x = E.curr_y = 0;
  E.rowoff = E.coloff = 0;
  E.render_x = 0;
  E.numrows = 0;
  E.row = NULL;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  
  if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2; // reserve the two last lines for the status bar and the message bar
}

int main(int argc, char* argv[]) {
  enableRawMode();
  initEditor();

  if(argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-Q = quit");

  while(1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
