#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define SHIM_VERSION "0.0.1"

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

struct editorConfig {
  int curr_x, curr_y; // screen pointer coordinates
  int screenrows;     // screen height
  int screencols;     // screen width
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

void editorMoveCursor(int key) {
  switch(key) {
    case ARROW_LEFT :
      if(E.curr_x != 0) E.curr_x--;
      break;
    case ARROW_RIGHT :
      if(E.curr_x != E.screencols - 1) E.curr_x++;
      break;
    case ARROW_UP :
      if(E.curr_y != 0) E.curr_y--;
      break;
    case ARROW_DOWN :
      if(E.curr_y != E.screenrows - 1) E.curr_y++;
      break;
  }
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
      E.curr_x = E.screencols - 1;
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
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

void editorDrawRows(A_BUF* ab) {
  int r;
  for(r = 0; r < E.screenrows; r++){
    if(r == E.screenrows / 3) {
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
    // escape sequence to clear one line(K) at a time
    abAppend(ab, "\x1b[0K", 4);
    if(r < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen() {
  A_BUF ab = A_BUF_INIT;

  // escape sequence to hide/reset(l) the cursor before refreshing the screen 
  abAppend(&ab, "\x1b[?25l", 6);
  // escape sequence to reposition(H) the cursor at top-left corner(1,1)
  abAppend(&ab, "\x1b[1;1H", 6);
  // draw tildes
  editorDrawRows(&ab);

  // escape sequence to reposition the cursor
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "\x1b[%d;%dH", E.curr_y + 1, E.curr_x + 1);
  abAppend(&ab, buffer, strlen(buffer));  

  // escape sequence to show/set(h) the cursor after refreshing the screen 
  abAppend(&ab, "\x1b[?25h", 6);  

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void initEditor() {
  E.curr_x = 0;
  E.curr_y = 0;
  
  if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main() {
  enableRawMode();
  initEditor();

  while(1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
