#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

void die(const char* s) {
  // perror looks at global errno variable that contains the current error code
  perror(s); // prints 's' and also a descriptive error message for this error code
  exit(1); // exit with failure
}

void disableRawMode() {
  // restore terminal flags
  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1){
    die("tcsetattr");
  }
}

void enableRawMode() {
  if(tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = orig_termios;

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

int main() {
  enableRawMode();

  char c;

  while(1) {
    c = '\0';
    if(read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) die("read");

    if(iscntrl(c)) {
      // nonprintable character
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')\r\n", c, c);
    }
    if(c == 'q') break;
  }

  return 0;
}
