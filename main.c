#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

struct termios orig_termios;

// Prints an error message and exits the program
void die(const char *s) {
  perror(s);
  exit(1);
}

// Restores the state of the terminal
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = orig_termios;

  // With ECHO disabled it stops the input from showing as typed (Like typing a password on sudo)
  // With ICANON disabled reads byte-by-byte instead by line
  // With ISIG disabled it disables CTRL+C and CTRL+Z
  // With IEXTEN disabled it disables CTRL+V and CTRL+O (on macOS)
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

  // With IXON disabled it disables CTRL+S and CTRL+Q
  // With ICRNL disabled it disables CTRL+M and stops translating the CR (Enter)
  // The rest are consider a "tradition" when trying to enamble raw mode
  raw.c_iflag &= ~(IXON | ICRNL | INPCK | ISTRIP | BRKINT);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);

  // Minimum number of bytes of input needed before read() returns
  // Maximum amount of time to wait before read() can return
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

int main() {
  enableRawMode();

  while (1) {
    char c = '\0';
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
      die("read");
    if (iscntrl(c)) {
      printf("%d\r\n", c);
    }
    else {
      printf("%d ('%c')\r\n", c, c);
    }
    if (c == 'q') break;
  }
  return 0;
}