#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h> 
#include <time.h>
#include <unistd.h>
#include <signal.h>

#define SHIM_VERSION "0.0.1"
#define SHIM_TAB_STOP 8 // tabulation length
#define SHIM_QUIT_TIMES 3 // how many times Ctrl-Q must be pressed to exit

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
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

enum editorHighlight {
  HL_NORMAL = 0,
  HL_COMMENT, // to highlight single-line comments
  HL_MLCOMMENT, // to highlight multi-line comments
  HL_KEYWORD1, // to highlight keywords of type 1
  HL_KEYWORD2, // to hihjlight keywords of type 2
  HL_STRING, // to highlight strings
  HL_NUMBER, // to highlight numbers
  HL_MATCH, // to highlight search results
  HL_SPECIAL, // to highlight language-specific special tokens
  HL_ERROR, // to highlight syntax error
};

// highlight flags
#define HL_HIGHLIGHT_NUMBERS (1<<0) 
#define HL_HIGHLIGHT_STRINGS (1<<1)
#define HL_HIGHLIGHT_SPECIAL (1<<2)

// for syntax highlight style
#define RED(x)((x & 0xff0000) >> 16)
#define GREEN(x)(( x & 0xff00) >> 8)
#define BLUE(x)((x & 0xff))
#define IS_BOLD(x)(x & (1 << 24))
#define IS_ITALIC(x)(x & (1 << 25))

typedef struct editorSyntax {
  char* filetype;
  char** filematch;
  char** keywords;
  // for special elements like tags or preprocessor directives
  char** specials;
  char special_start;
  //char special_continue, special_end;
  char* singleline_comment_start;
  char* multiline_comment_start;
  char* multiline_comment_end;
  int flags;
} editorSyntax;

typedef struct editor_row {
  int idx;
  int size;
  int rsize;
  char* chars;
  char* render; // actual characters to drawn on the screen for that row of text
  unsigned char* hl; // highlight config
  int hl_open_comment;
} E_ROW;

struct editorConfig {
  int curr_x, curr_y; // cursor's position coordinates within the file
  int render_x;       // index into the row render field
  int rowoff;         // row offset for scrolling control
  int coloff;         // column offset for scrolling control
  int screenrows;     // screen height
  int screencols;     // screen width
  int numrows;        // number of rows in the source file
  int row_num_offset;
  E_ROW* row;
  int dirty;          // tell if a text buffer has been modified
  char* filename;
  char statusmsg[80];
  time_t statusmsg_time; // timestamp when set the status message
  struct editorSyntax* syntax; // current editorSyntax config
  struct termios orig_termios;
};

struct editorConfig E;

char* C_HL_extensions[] = {".c", ".h", ".cpp", ".hpp", ".cc", NULL};
char* C_HL_keywords[] = {
  "switch", "if", "do", "while", "for", "break", "continue", "return", "else", "goto", // statements
  "struct", "union", "typedef", "enum", "class", "case", "default", "sizeof",
  
  "int|", "long|", "double|", "float|", "short|", "char|", "unsigned|", "signed|", // types
  "const|", "static|", "void|", "auto|", "bool|", "register|", "extern|", "volatile|",
  "size_t|", "ptrdiff_t|", NULL
};

char* C_HL_specials[] = {
  "include", "define", "undef", "if", "ifdef", "ifndef", // preprocessor 
  "else", "elif", "endif", "pragma", NULL
};

editorSyntax HLDB[] = { // highlight database
  {
    "c", // filetype
    C_HL_extensions, // filematch
    C_HL_keywords,   // keywords
    C_HL_specials,   // special tokens
    '#', // '\\', '\n',  // C preprocessor as special highlight
    "//", "/*", "*/", // syntax for comments
    HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_SPECIAL // flags
  },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

void editorSetStatusMessage(const char* fmt, ...);
void editorRefreshScreen();
char* editorPrompt(char* prompt, void (*callback)(char*, int));

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

int editorReadKey(int fd) {
  int nread;
  char c;

  // wait until a keypress occurr
  while((nread = read(fd, &c, 1)) != 1) {
    if(nread == -1 && errno != EAGAIN) die("read");
  }
  
  if(c == '\x1b') {
    // parse escape sequence
    char seq[3];
    
    if(read(fd, &seq[0], 1) != 1) return c;
    if(read(fd, &seq[1], 1) != 1) return c;

    if(seq[0] == '[') { 
      if(seq[1] >= '0' && seq[1] <= '9') {
        if(read(fd, &seq[2], 1) != 1) return c;
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

int is_separator(int c) {
  // checks if c is a separator character or not
  // strchr looks for c in the given string and return a pointer to the matching character
  return isspace(c) || c == '\0' || strchr(",.()+-/*!?=~%<>[]{}:;&|^\"\'\\", c) != NULL;
}

void editorUpdateSyntax(E_ROW* row) {
  row->hl = realloc(row->hl, row->rsize);
  memset(row->hl, HL_NORMAL, row->rsize);

  if(E.syntax == NULL) return; // if NULL, no syntax highlight should be done
  
  char** keywords = E.syntax->keywords;
  char** specials = E.syntax->specials;

  char* scs = E.syntax->singleline_comment_start;
  char* mcs = E.syntax->multiline_comment_start;
  char* mce = E.syntax->multiline_comment_end;
  
  int scs_len = scs ? strlen(scs) : 0;
  int mcs_len = mcs ? strlen(mcs) : 0;
  int mce_len = mce ? strlen(mce) : 0;

  // if the previous character was a separator
  int prev_sep = 1; // assume true with the beginning of the line as a separator
  int in_string = 0;
  int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);
  int in_special = 0;

  int i = 0;
  while(i < row->rsize) {
    char c = row->render[i];
    
    if(scs_len && !in_string && !in_comment) {
      if(!strncmp(&row->render[i], scs, scs_len)) {
        // if is starting a single-line comment
        memset(&row->hl[i], HL_COMMENT, row->rsize - i);
        break;
      }
    }
    
    if(mcs_len && mce_len && !in_string) {
      if(in_comment) {
        row->hl[i] = HL_MLCOMMENT;
        if(!strncmp(&row->render[i], mce, mce_len)) { // finishing ml comment
          memset(&row->hl[i], HL_MLCOMMENT, mce_len);
          i += mce_len;
          in_comment = 0;
          prev_sep = 1;
          continue;
        } else {
          i++; continue;
        }
      } else if(!strncmp(&row->render[i], mcs, mcs_len)) { // starting ml comment
        memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
        i += mcs_len;
        in_comment = 1; 
        continue;
      }
    }

    if(E.syntax->flags & HL_HIGHLIGHT_SPECIAL) {
      if(in_special) {
        while(i < row->rsize && !isspace(c = row->render[i])) {
          row->hl[i] = HL_SPECIAL;
          i++;
        }
      }
      else if(!in_string && c == E.syntax->special_start) {
        int idx = i++;
        while(isspace(c = row->render[i])) i++;
        
        for(int j = 0; specials[j]; j++){
          int slen = strlen(specials[j]);
          
          if(!strncmp(&row->render[i], specials[j], slen) && // match special token
             is_separator(row->render[i + slen])) {
             
            row->hl[idx] = HL_SPECIAL;
            memset(&row->hl[i], HL_SPECIAL, slen);
            in_special = 1; i += slen - 1;
            prev_sep = 0; continue;
          }
        }
      }
    }

    if(E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if(in_string) {
        row->hl[i++] = HL_STRING;
        if(c == '\\' && i+1 < row->rsize) { // could be a '\'' or '\"' escape character 
          row->hl[i] = HL_STRING;
          i += 1;
          continue;
        }
        if(c == in_string) in_string = 0; // closing quotes
        prev_sep = 1;
        continue;
      } else {
        if(c == '"' || c == '\'') {
          in_string = c;
          row->hl[i++] = HL_STRING;
          continue;
        }
      }
    }

    if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      
      if(prev_sep && (isdigit(c) || 
                     (c == '.' && i+1 < row->rsize && isdigit(row->render[i+1])))) {
        int start = i;
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL; // previous highlight type
        unsigned char curr_hl = HL_NORMAL;
        int err_flag = 0; // if error in the highlight logic
        int is_hexa = 0, is_octa = 0;
        int count = 0; // count decimal points

        do {
          if(err_flag) {
            i++; continue; // consume invalid input
          }

          if((is_octa && (c >= '0' && c <= '7')) ||
             (is_hexa && (isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))) {
            prev_hl = curr_hl = HL_NUMBER; prev_sep = 0;

          } else if (!is_octa && !is_hexa && (isdigit(c) && ((prev_sep && c != '0') || prev_hl == HL_NUMBER))) {
            prev_hl = curr_hl = HL_NUMBER; prev_sep = 0;

          } else if(!is_octa && !is_hexa && c == '.' && ++count == 1 && 
                    (prev_hl == HL_NUMBER || prev_hl == HL_NORMAL)) { // parse numbers with decimal points
            prev_hl = curr_hl = HL_NUMBER; prev_sep = 0;

          } else if(prev_sep && c == '0') {
		        
            if(!is_hexa && !is_octa && i+1 < row->rsize) {
            
              c = row->render[i+1];            

              if(isdigit(c)) { // octal 
                is_octa = 1;
              } else if(c == 'x' || c == 'X') { // hexadecimal
                is_hexa = 1; i++;
              }
            }
            prev_hl = curr_hl = HL_NUMBER; prev_sep = 0;

          } else {
            curr_hl = count > 1 ? HL_NORMAL : HL_ERROR;
            err_flag = 1;
          }
          i++; 

        } while(i < row->rsize && (!is_separator(c = row->render[i]) || c == '.')); // or not all separators except '.'
		
        int hl_size = (int)(i-start);
        memset(row->hl + start, curr_hl, hl_size);
      }
    }
    
    if(prev_sep) {
    
      int j;
      
      for(j = 0; keywords[j]; j++) {
        int kwlen = strlen(keywords[j]);
        int kw2 = keywords[j][kwlen - 1] == '|'; // if is of type keyword2
        if(kw2) kwlen--;

        if(!strncmp(&row->render[i], keywords[j], kwlen) && // match keyword
           is_separator(row->render[i + kwlen])) {
          memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, kwlen);
          i += kwlen - 1;
        }
      }
      if(keywords[j] != NULL) {
        // if isn't the end of the keywords array, so it matched one keyword
        prev_sep = 0; // a keyword isn't a separator
        continue;
      }
    }
    prev_sep = is_separator(c);
    i++;
  }
  int changed = (row->hl_open_comment != in_comment);
  row->hl_open_comment = in_comment;
  if(changed && row->idx + 1 < E.numrows)
    editorUpdateSyntax(&E.row[row->idx + 1]);
}

int editorSyntaxToStyle(int hl) {
  switch(hl) {
    // HL_NORMAL have already been handled
    // change foreground style
    case HL_COMMENT : 
    case HL_MLCOMMENT: return 0x020088ff; // italic sky blue
    case HL_KEYWORD1 : return 0x01ff9d00; // bold bright orange
    case HL_KEYWORD2 : return 0x0080ffbb; // normal teal blue
    case HL_SPECIAL : return 0x0180ffbb; // bold teal blue 
    case HL_NUMBER : return 0x00ff0044; // normal nail polish pink
    case HL_STRING : return 0x003ad900; // normal spring green
    // change background style
    case HL_MATCH : return 0x001e96c8; // RGB(30, 150, 200), blue
    case HL_ERROR : return 0x00820000; // normal dark maroon
    default: return 0x00ffffff; // RGB(255, 255, 255), white
  }
}

void editorSelectSyntaxHighlight() {
  // tries to match the current filename to one of the filematch fields in HLDB  

  E.syntax = NULL;
  if(E.filename == NULL) return;
  
  char* ext = strchr(E.filename, '.'); // get file extension

  for(unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    editorSyntax* s = &HLDB[j];
    
    unsigned int i = 0;
    while(s->filematch[i]) {
      int is_ext = (s->filematch[i][0] == '.');
      // check if the pattern in the filematch exists in the filename
      if((is_ext && ext && !strcmp(ext, s->filematch[i])) || // if the extension matches or
         (!is_ext && strstr(E.filename, s->filematch[i]))) { // if is a substring of filename
        E.syntax = s; // update highlight syntax for this file
        
         // must refactor syntax highlighting after updating it
        for(int filerow = 0; filerow < E.numrows; filerow++) {
          editorUpdateSyntax(&E.row[filerow]);
        }
        return;
      }
      i++;
    }
  }
}

int ndigits(int num) {
  int d = 0;
  
  do {
    ++d;
  } while(num /= 10);
  
  return d; 
}

int editorRowCxtoRx(E_ROW* row, int cx) {
  // convert a chars index into a render index
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

int editorRowRxtoCx(E_ROW* row, int rx) {
  // convert a render index into a chars index  

  int curr_rx = 0, cx;

  for(cx = 0; cx < row->size; cx++) {
    if(row->chars[cx] == '\t') {
      curr_rx += (SHIM_TAB_STOP - 1) - (curr_rx % SHIM_TAB_STOP);
    }
    curr_rx++;
    if(curr_rx > rx) return cx;
  }
  return cx;
}

void editorUpdateRow(E_ROW* row) {
  int j, tabs = 0;
  for(j = 0; j < row->size; j++){
    if(row->chars[j] == '\t') tabs++;
  }
  free(row->render);
  // allocate memory for render
  // the maximum number of characters needed for each tab is SHIM_TAB_STOP
  // row->size already counts 1 character for each tab 
  // so multiply tabs by (SHIM_TAB_STOP - 1) and add that to row->size
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

  editorUpdateSyntax(row);
}

void editorUpdateRowOffset() {
  //E.screencols += (E.row_num_offset + 1);
  E.row_num_offset = ndigits(E.numrows);
  //E.screencols -= (E.row_num_offset + 1);
}

void editorInsertRow(int at, char* s, size_t len, int leading_spaces) { 
  if(at < 0 || at > E.numrows) return;

  E.row = realloc(E.row, sizeof(E_ROW) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(E_ROW) * (E.numrows - at));
  for(int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

  E.row[at].idx = at;

  E.row[at].size = len + leading_spaces;
  E.row[at].chars = malloc(len + leading_spaces + 1);
  memset(E.row[at].chars, ' ', leading_spaces);
  memcpy(E.row[at].chars + leading_spaces, s, len);
  E.row[at].chars[len + leading_spaces] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;
  E.row[at].hl_open_comment = 0;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  editorUpdateRowOffset();
  E.dirty++;
}

void editorFreeRow(E_ROW* row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

void editorDelRow(int at) {
  if(at < 0 || at >= E.numrows) return;
  
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(E_ROW) * (E.numrows - at - 1));
  for(int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
  E.numrows--;
  editorUpdateRowOffset();
  E.dirty++;
}

void editorRowInsertChar(E_ROW* row, int at, char c) {
  if(at < 0 || at > row->size) at = row->size;

  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(E_ROW* row, char* s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  // move len bytes from s to row->chars starting at row->size
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(E_ROW* row, int at) {
  if(at < 0 || at >= row->size) return;
  
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

void editorInsertChar(int c) {
  if(E.curr_y == E.numrows) {
    // the cursor is on the tilde line after EOF
    editorInsertRow(E.numrows, "", 0, 0);
  }
  // insert char at the current cursor position
  editorRowInsertChar(&E.row[E.curr_y], E.curr_x, c);
  E.curr_x++;
}

int getLeadingSpaces(int at) {
  int count;
  for(count = 0; E.row[at].render[count] == ' '; ++count);
  
  return count;
}

void editorInsertNewLine() {
  int leading_spaces = getLeadingSpaces(E.curr_y);

  if(E.curr_x == 0) {
    // insert an empty line at curr_y
    editorInsertRow(E.curr_y, "", 0, leading_spaces);
  } else {
    // split line at curr_x
    E_ROW* row = &E.row[E.curr_y];
    // create a new row after the current one, with the characters to the right of the cursor
    editorInsertRow(E.curr_y + 1, &row->chars[E.curr_x], row->size - E.curr_x, leading_spaces);
    row = &E.row[E.curr_y];
    row->size = E.curr_x; // truncate the current line
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.curr_y++;
  E.curr_x = leading_spaces;
}

void editorDelChar() {
  if(E.curr_y == E.numrows) return; // cursor is past the EOF, nothing to delete
  if(E.curr_y == 0 && E.curr_x == 0) return; // cursor is at the beginning of the first line, nothing to do  

  E_ROW* row = &E.row[E.curr_y];
  if(E.curr_x > 0) { // there is a character to the left of the cursor
    editorRowDelChar(row, E.curr_x - 1);
    E.curr_x--;
  } else { // beginning of a line
    E.curr_x = E.row[E.curr_y - 1].size; // point cursor to the end of the previous line
    editorRowAppendString(&E.row[E.curr_y - 1], row->chars, row->size);
    editorDelRow(E.curr_y);
    E.curr_y--;
  }
}

char* editorRowsToString(int* buflen) {
  int j, totlen = 0;

  for(j = 0; j < E.numrows; j++) {
    totlen += E.row[j].size + 1; // line len + newline character
  }
  *buflen = totlen;

  char* buf = malloc(totlen);
  char* p = buf;
  for(j = 0; j < E.numrows; j++) {
    // copy content of the row to the end of the buffer
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size; *p = '\n'; p++;
  }
  return buf;
}

void editorOpen(const char* filename) {
  free(E.filename);
  E.filename = strdup(filename); // makes a copy of the string

  editorSelectSyntaxHighlight();

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
    editorInsertRow(E.numrows, line, linelen, 0);
  }
  editorUpdateRowOffset();
  free(line); fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if(E.filename == NULL) {
    // prompt the user to input a filename when saving a new file
    E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
    if(!E.filename) {
      editorSetStatusMessage("Save aborted");
      return;
    }
    editorSelectSyntaxHighlight();
  }
  int len;
  char* buf = editorRowsToString(&len);
  
  // open the file for input/output (O_RDWR)
  // and tell open to create a new file if it doesn't exist (O_CREAT)
  // 0644 is the permission mode that the new file should have
  // it gives to the owner of the file the read and write permissions
  // everyone else can only read the file
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if(fd != -1) {
    // truncate the file's size, to len bytes 
    if(ftruncate(fd, len) != -1) {
      if(write(fd, buf, len) == len) {
        close(fd); free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

void editorFindCallback(char* query, int key) {

  static int last_match = -1; // the index of the row that the last match was on
  static int direction = 1; // 1 for searching forward and -1 for searching backward

  static int saved_hl_line;
  static char* saved_hl = NULL;

  if(saved_hl) {
    // restore previous saved highlighted search result
    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl);
    saved_hl = NULL;
  }

  if(key == '\r' || key == '\x1b') {
    // pressed ENTER or Escape key
    // leaving search mode, reset the search states
    last_match = -1;
    direction = 1;
    return;
  } else if(key == ARROW_RIGHT || key == ARROW_DOWN) {
    // search for the next match
    direction = 1;
  } else if(key == ARROW_LEFT || key == ARROW_UP) {
    // search for the previous match
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if(last_match == -1) direction = 1;
  int current_row = last_match;

  int i;
  // loop through all the rows of the file 
  for(i = 0; i < E.numrows; i++) {
    current_row += direction;
    // allow a search to wrap around of the file
    if(current_row == -1) current_row = E.numrows - 1;
    else if(current_row == E.numrows) current_row = 0;   
 
    E_ROW* row = &E.row[current_row];
    // check if query is a substring of the current row
    char* match = strstr(row->render, query);
    if(match) {
      last_match = current_row;
      // move the cursor to the match
      E.curr_y = current_row;
      E.curr_x = editorRowRxtoCx(row, match - row->render);
      // force editorScroll to scroll upwards at the next screen refresh
      // the matching line will be at the very top of the screen
      E.rowoff = E.numrows;
    
      saved_hl_line = current_row;
      saved_hl = malloc(row->rsize);
      // save current row syntax highlight status
      memcpy(saved_hl, row->hl, row->rsize);
      // set the highlight code of the query substring to HL_MATCH, for colorful search results
      memset(&row->hl[match - row->render], HL_MATCH, strlen(query)); 
      break;
    }
  }
}

void editorFind() {
  int saved_curr_x = E.curr_x;
  int saved_curr_y = E.curr_y;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;

  char* query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
  
  if(query) {
    free(query);
  } else {
    // the search was cancelled, restore the cursor position
    E.curr_x = saved_curr_x;
    E.curr_y = saved_curr_y;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

char* editorPrompt(char* prompt, void (*callback)(char*, int)) {
  // displays a prompt in the status bar
  // it lets the user input a line of text after the prompt
 
  size_t bufsize = 128; // buffer capacity
  char* buf = malloc(bufsize);
  buf[0] = '\0'; // initialize as an empty string

  size_t buflen = 0;
  
  while(1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey(STDIN_FILENO);
    if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if(buflen != 0) buf[--buflen] = '\0'; // delete last inserted character
    }
    else if(c == '\x1b') { // pressed the Escape key
      editorSetStatusMessage(""); // exit from prompt
      if(callback) callback(buf, c);
      free(buf); return NULL;
    }
    else if(c == '\r') {  // pressed the ENTER key 
      if(buflen != 0) {
        editorSetStatusMessage(""); // exit from prompt
        if(callback) callback(buf, c);
        return buf; // return input
      }
    } else if(!iscntrl(c) && c < 128) { // is a printable character
      if(buflen == bufsize - 1) { // buflen has reached the maximum capacity 
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
    if(callback) callback(buf, c);
  }
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

void editorProcessKeypress(int fd) {
  static int quit_times = SHIM_QUIT_TIMES;
  int c = editorReadKey(fd);

  // handle a keypress
  switch(c) {
    case '\r': // ENTER
      editorInsertNewLine();
      break;

    case CTRL_KEY('q'):
      if(E.dirty && quit_times > 0) { // if modified since last file save or opening
        editorSetStatusMessage("WARNING!!! File has unsaved changes."
          "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--; return;
      }
      // clear the screen and reposition the cursor
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[1;1H", 6);
      exit(0); // exit without error
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY : 
      E.curr_x = 0;
      break;

    case END_KEY :
      if(E.curr_y < E.numrows) E.curr_x = E.row[E.curr_y].size;
      break;

    case CTRL_KEY('f'):
      editorFind();
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if(c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
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

    // ignore terminal refresh and Escape key presses
    case CTRL_KEY('l') :
    case '\x1b': 
      break;
    
    default:
      editorInsertChar(c);
      break;
  }
  // if pressed any other key than Ctrl-Q, then resets quit_times back
  quit_times = SHIM_QUIT_TIMES;
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
  if(E.render_x >= E.coloff - E.row_num_offset + E.screencols - 1) {
    E.coloff = E.render_x - E.screencols + E.row_num_offset + 2;
  }
}

void editorDrawRows(A_BUF* ab) {
  int r;

  for(r = 0; r < E.screenrows; r++){
    int filerow = r + E.rowoff; // absolute position in the file
    
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
      char linenum[32];
      int lnlen = snprintf(linenum, sizeof(linenum), "%*d ", E.row_num_offset, filerow + 1);
      abAppend(ab, linenum, lnlen);
      
      // subtract the number of characters that are to the left of the offset
      int j, len = E.row[filerow].rsize - E.coloff;
      if(len < 0) len = 0; // scrolled horizontally past the end of the line
      if(len > E.screencols - (E.row_num_offset + 1)) len = E.screencols - E.row_num_offset - 2; // truncate the line

      char* c = &E.row[filerow].render[E.coloff]; // get the render array
      unsigned char* hl = &E.row[filerow].hl[E.coloff]; // get the highlight array
      int curr_fgcolor = -1; // current foreground color
      int curr_bgcolor = -1; // current background color

      for(j = 0; j < len; j++){

        if(iscntrl(c[j])) {
          char sym = (c[j] <= 26) ? '@' + c[j] : '?';
          abAppend(ab, "\x1b[7m", 4); // invert colors
          abAppend(ab, &sym, 1);
          abAppend(ab, "\x1b[m", 3); // turn off inverted colors
          if(curr_fgcolor != -1) {
            char buf[32];
            int colorlen = snprintf(buf, sizeof(buf), "\x1b[38;2;%d;%d;%dm", 
                                    RED(curr_fgcolor), GREEN(curr_fgcolor), BLUE(curr_fgcolor));
            abAppend(ab, buf, colorlen);
          }
          continue;

        } else if(hl[j] == HL_NORMAL) {			
          if(curr_fgcolor != -1 || curr_bgcolor != -1) {
            abAppend(ab, "\x1b[0;39m", 7); // reset the text color to default
            curr_bgcolor = curr_fgcolor = -1;
          }
        } else if(hl[j] == HL_MATCH || hl[j] == HL_ERROR) {

          int bgcolor = editorSyntaxToStyle(hl[j]);

          if(bgcolor != curr_bgcolor) {
				    curr_fgcolor = -1; // reset foreground color
            curr_bgcolor = bgcolor;
				    abAppend(ab, "\x1b[0;39m", 7);            

            char buf[32]; // for color scape sequence string
            int colorlen = snprintf(buf, sizeof(buf), "\x1b[48;2;%d;%d;%d;1m", 
                                         RED(bgcolor), GREEN(bgcolor), BLUE(bgcolor));
            abAppend(ab, buf, colorlen); // set the text background color
          }
        } else {
          int fgcolor = editorSyntaxToStyle(hl[j]);
          
          if(fgcolor != curr_fgcolor) {
				    abAppend(ab, "\x1b[0;39m", 7);
            curr_fgcolor = fgcolor;
            curr_bgcolor = -1;

            char buf[32]; // for color scape sequence string
            
            int colorlen = snprintf(buf, sizeof(buf), "\x1b[38;2;%d;%d;%d%sm", 
                                    RED(fgcolor), GREEN(fgcolor), BLUE(fgcolor),
                                    IS_BOLD(fgcolor) ? ";1" : (IS_ITALIC(fgcolor) ? ";3" : ""));
                                    
            abAppend(ab, buf, colorlen); // set the text foreground color
          }
        }
        abAppend(ab, &c[j], 1);
      }
      abAppend(ab, "\x1b[0;39m", 7); // reset the text color
    }
    // escape sequence to clear one line(K) at a time
    abAppend(ab, "\x1b[0K", 4);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(A_BUF* ab){

  abAppend(ab, "\x1b[7m", 4); // reverse terminal colors to black text on a white background
  
  char status[80], row_status[80];
 
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", 
    E.filename ? E.filename : "[No Name]", E.numrows, 
    E.dirty ? "(modified)" : "");
    
  if(len > E.screencols) len = E.screencols;

  abAppend(ab, status, len);  

  int rlen = snprintf(row_status, sizeof(row_status), "%s | %d/%d",
    E.syntax ? E.syntax->filetype : "no ft", E.curr_y + 1, E.numrows);

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
                                                  (E.render_x - E.coloff + E.row_num_offset + 1) + 1);
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

void updateWindowSize() {
  if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2; // reserve the two last lines for the status bar and the message bar
}

void handleSigWinCh(int sig) {
  updateWindowSize();
  if(E.curr_y > E.screenrows + E.rowoff - 1) E.curr_y = E.screenrows + E.rowoff - 1;
  if(E.curr_x > (E.screencols + E.coloff - (E.row_num_offset + 1) - 1)) 
    E.curr_x = E.screencols + E.coloff - (E.row_num_offset + 1) - 1;
  editorRefreshScreen();
}

void initEditor() {
  E.curr_x = E.curr_y = 0;
  E.rowoff = E.coloff = 0;
  E.render_x = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;
  E.syntax = NULL;
  E.row_num_offset = 0;
  
  updateWindowSize();
  signal(SIGWINCH, handleSigWinCh);
}

int main(int argc, char* argv[]) {
  enableRawMode();
  initEditor();

  if(argc >= 2) {
    editorOpen(argv[1]);
    updateWindowSize();
  }

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

  while(1) {
    editorRefreshScreen();
    editorProcessKeypress(STDIN_FILENO);
  }

  return 0;
}
