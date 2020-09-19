#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>


//Define the control key plus q to quit the operation
#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

//enumarator to give int to the keys pressed in the editor
enum editorKey{
  //move cursor left right up down
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  //move cursor to edges of the screen fn arrow left or right
  HOME_KEY,
  END_KEY,
  //move cursor top or botton fn arrow up or down
  PAGE_UP,
  PAGE_DOWN

};

//struct which wil contain the state of the editor
struct editorConfig
{
  //cursor positions
  int cx,cy;
  //terminal screen rows and colums
  int screenrows;
  int screencols;

  struct termios orig_termios;
};

struct editorConfig E;


// Prints an error message and exits the program and clears the screen
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

// Restores the state of the terminal
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;

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

//this function waits for keypresses and returns them
int editorReadKey(){
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }
  //check if key pressed had an escape sequence
  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          //make it possible to use page up and page down and also addes home key and end key
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      }else{
        //make it possible to use the arrow keys to move and not only wasd also added home key and end key
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    }else if (seq[0] == 'O')
    {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
    return '\x1b';
  }else{
    return c;
  }
}

//this function is used to get the cursor position in case which we will use to get the terminal window screen size
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '['){ 
    return -1;
  }
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2){
    return -1;
  }
  return 0;
}

//this function will place the number of colums and rows of the windows size on the struct winsize if it fails it will return -1
int getWindowSize(int *rows, int *cols){
  struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  }else{
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

//we need to make a buffer for the text that is being written so we dont do small writes but one big write
struct abuf
{
  char *b;
  int len;
};
//the append buffer consists of a pointer to the buffer and the lenght of it
#define ABUF_INIT {NULL, 0};

//this function is used to append strings to the buffer it works by getting a memory block of the size of the string plus the size that we are adding 
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL ){
    return;
  }
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

//frees the block of memory of a buffer
void abFree(struct abuf *ab){
  free(ab->b);
}

//function to process the cursor movement we will move with the wasd keys
void editorMoveCursor(int key){
  switch (key)
  {
  case ARROW_LEFT:
    if(E.cx != 0){
      E.cx--;
    }
    break;
    case ARROW_RIGHT:
    if (E.cx != E.screencols - 1) {
      E.cx++;
    }
    break;
    case ARROW_UP:
    if(E.cy != 0){
      E.cy--;
    }
    break;
    case ARROW_DOWN:
    if (E.cy != E.screenrows - 1) {
      E.cy++;
    }
    break;
  }
}

//waits for a keypress and then handles the key press its used to map different key combinations and special keys to different functions of the vim, and also to insert andy printable keys to the edited text
void editorProcessKeypress(){
  int c = editorReadKey();
  switch (c)
  {
    //quit
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
      case HOME_KEY:
        E.cx = 0;
      break;
      case END_KEY:
        E.cx = E.screencols - 1;
      break;
    //page up and page down
    case PAGE_UP:
      {
        int times = E.screenrows;
        while (times--)
        if (c == PAGE_UP)
        {
          editorMoveCursor(ARROW_UP);
        }
      }
    break;
    case PAGE_DOWN:
      {
        int times = E.screenrows;
        while (times--)
        if (c == PAGE_DOWN)
        {
          editorMoveCursor(ARROW_DOWN);
        }
      }
    break;
    //move cursor
    case ARROW_UP:
          editorMoveCursor(c);
    break;
    case ARROW_DOWN:
          editorMoveCursor(c);
    break;
    case ARROW_LEFT:
          editorMoveCursor(c);
    break;
    case ARROW_RIGHT:
      editorMoveCursor(c);
    break;
  }
}

//this function draws ~ on every row when the editor is called
//it also fisplays the name and version of the mini vim centered 1/3 down on the terminal screen
void editorDrawRows(struct abuf *ab){
  int i;
  for (i = 0; i < E.screenrows; i++)
  {
    if (i == E.screenrows / 3) {
      char welcome[80];
      int welcomelen = snprintf(welcome, sizeof(welcome),
        "Mini-Vim -- version %s", KILO_VERSION);
      if (welcomelen > E.screencols){
         welcomelen = E.screencols;
      }
      int padding = (E.screencols - welcomelen) / 2;
      if (padding) {
        abAppend(ab, "~", 1);
        padding--;
      }
      while (padding--) abAppend(ab, " ", 1);
      abAppend(ab, welcome, welcomelen);
    }else{
      abAppend(ab, "~", 1);
    }
    //this erases the whole line with help of the k command erase in line
    abAppend(ab, "\x1b[K", 3);
    if (i < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
  
}

//this function clears the screen when we render the editors screen
//the second write function is to reposition the cursor to the top left of the editor
void editorRefreshScreen(){
  struct abuf ab = ABUF_INIT;
  //hides the cursor
  abAppend(&ab, "\x1b[?25l", 6);

  abAppend(&ab, "\x1b[H", 3);
  editorDrawRows(&ab);

  //cursor position
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));

  //shows the cursor
  abAppend(&ab, "\x1b[?25l", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void initEditor(){
  E.cx = 0;
  E.cy = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1){
    die("getWindowSize");
  }
}

int main() {
  enableRawMode();
  initEditor();

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}